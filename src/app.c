#include "relay/app.h"

#include "relay/asset.h"

#if RELAY_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <time.h>
#endif

/** Return a monotonic wall-clock timestamp for fixed gameplay stepping. */
static double relay_app_now_seconds(void)
{
#if RELAY_PLATFORM_WINDOWS
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;

    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter)) {
        return 0.0;
    }
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec time;

    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        return 0.0;
    }
    return (double)time.tv_sec + (double)time.tv_nsec / 1000000000.0;
#endif
}

/** Convert a normalized terminal key into a game input. */
static Relay_GameInput relay_app_game_input(const Relay_TerminalEvent *event)
{
    if (event->key == RELAY_TERMINAL_KEY_UP || event->character == 'k') {
        return RELAY_GAME_INPUT_PREVIOUS;
    }
    if (event->key == RELAY_TERMINAL_KEY_DOWN || event->character == 'j') {
        return RELAY_GAME_INPUT_NEXT;
    }
    if (event->key == RELAY_TERMINAL_KEY_CONFIRM) {
        return RELAY_GAME_INPUT_CONFIRM;
    }
    if (event->character == 'm' || event->character == 'M') {
        return RELAY_GAME_INPUT_TOGGLE_MAP;
    }
    if (event->character == '[') {
        return RELAY_GAME_INPUT_PREVIOUS_CLOCK_RATE;
    }
    if (event->character == ']') {
        return RELAY_GAME_INPUT_NEXT_CLOCK_RATE;
    }
    return RELAY_GAME_INPUT_NONE;
}

/** Apply Escape as a universal back action before offering application exit. */
static void relay_app_back(Relay_App *app)
{
    if (app->overlay != RELAY_TERMINAL_OVERLAY_NONE) {
        app->overlay = RELAY_TERMINAL_OVERLAY_NONE;
    } else if (!relay_game_back(&app->game)) {
        app->overlay = RELAY_TERMINAL_OVERLAY_EXIT_CONFIRM;
    }
}

/** React to a main-thread event emitted by Relay runtime services. */
static void relay_app_on_event(const Relay_Event *event, void *context)
{
    Relay_App *app = context;

    if (event == NULL || app == NULL || app->state != RELAY_APP_STATE_RUNNING) {
        return;
    }
    if (event->type == RELAY_EVENT_QUIT_REQUESTED) {
        app->should_exit = true;
    } else if (event->type == RELAY_EVENT_TERMINAL_RESIZED) {
        relay_logger_log(&app->logger, RELAY_LOG_LEVEL_DEBUG,
            "Terminal resized on frame %llu.",
            (unsigned long long)app->frame_index);
        if (!relay_terminal_draw(&app->terminal, &app->game, app->overlay)) {
            relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
                "Terminal redraw failed.");
            app->state = RELAY_APP_STATE_FAILED;
        }
    }
}

/** Initialize Relay's lifecycle and runtime service ownership. */
bool relay_app_init(Relay_App *app)
{
    if (app == NULL || app->state != RELAY_APP_STATE_UNINITIALIZED) {
        return false;
    }

    app->state = RELAY_APP_STATE_INITIALIZING;
    if (!relay_logger_init(&app->logger)) {
        app->state = RELAY_APP_STATE_FAILED;
        return false;
    }

    relay_logger_log(&app->logger, RELAY_LOG_LEVEL_INFO,
        "Relay application initialization started.");
    if (relay_departure_mono_font()->size == 0 || !relay_game_init(&app->game)) {
        relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
            "Game state or embedded font initialization failed.");
        app->state = RELAY_APP_STATE_FAILED;
        return false;
    }
    if (!relay_event_bus_init(&app->events) ||
        relay_event_bus_subscribe(&app->events, RELAY_EVENT_QUIT_REQUESTED,
            relay_app_on_event, app) == 0 ||
        relay_event_bus_subscribe(&app->events, RELAY_EVENT_TERMINAL_RESIZED,
            relay_app_on_event, app) == 0) {
        relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
            "Event system initialization failed.");
        app->state = RELAY_APP_STATE_FAILED;
        return false;
    }
    if (!relay_job_system_init(&app->jobs, 0)) {
        relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
            "Job system initialization failed.");
        app->state = RELAY_APP_STATE_FAILED;
        return false;
    }
    if (!relay_terminal_init(&app->terminal)) {
        relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
            "Terminal initialization failed.");
        app->state = RELAY_APP_STATE_FAILED;
        return false;
    }
    if (!relay_terminal_draw(&app->terminal, &app->game, app->overlay)) {
        relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
            "Initial terminal rendering failed.");
        app->state = RELAY_APP_STATE_FAILED;
        return false;
    }

    app->state = RELAY_APP_STATE_RUNNING;
    app->last_frame_seconds = relay_app_now_seconds();
    relay_logger_log(&app->logger, RELAY_LOG_LEVEL_INFO,
        "Relay application initialization completed.");
    return true;
}

/** Run the terminal-driven game loop until the application should exit. */
int relay_app_run(Relay_App *app)
{
    Relay_TerminalEvent event;

    if (app == NULL || app->state != RELAY_APP_STATE_RUNNING) {
        return 1;
    }

    while (!app->should_exit) {
        Relay_Event app_event;
        double now;
        double elapsed;
        bool simulation_changed = false;

        if (!relay_terminal_poll(&app->terminal, &app->game, &event)) {
            relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
                "Terminal event polling failed.");
            app->state = RELAY_APP_STATE_FAILED;
            return 1;
        }

        now = relay_app_now_seconds();
        elapsed = now - app->last_frame_seconds;
        app->last_frame_seconds = now;
        if (elapsed < 0.0 || elapsed > 0.25) {
            elapsed = 0.0;
        }
        app->simulation_accumulator += elapsed;
        while (app->simulation_accumulator >= 1.0 / 60.0) {
            if (!relay_game_step(&app->game)) {
                relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
                    "Gameplay simulation step failed.");
                app->state = RELAY_APP_STATE_FAILED;
                return 1;
            }
            app->simulation_accumulator -= 1.0 / 60.0;
            simulation_changed = true;
            app->frame_index++;
        }
        if (event.type == RELAY_TERMINAL_EVENT_RESIZED ||
            event.type == RELAY_TERMINAL_EVENT_QUIT) {
            app_event.type = event.type == RELAY_TERMINAL_EVENT_RESIZED ?
                RELAY_EVENT_TERMINAL_RESIZED : RELAY_EVENT_QUIT_REQUESTED;
            app_event.sender = &app->terminal;
            app_event.data = NULL;
            relay_event_bus_emit(&app->events, &app_event);
            if (app->state == RELAY_APP_STATE_FAILED) {
                return 1;
            }
        } else if (event.type == RELAY_TERMINAL_EVENT_INPUT) {
            Relay_GameActionResult result = RELAY_GAME_ACTION_NONE;

            if (app->overlay == RELAY_TERMINAL_OVERLAY_EXIT_CONFIRM) {
                if (event.key == RELAY_TERMINAL_KEY_CONFIRM) {
                    app->should_exit = true;
                    continue;
                }
                if (event.key == RELAY_TERMINAL_KEY_ESCAPE) {
                    relay_app_back(app);
                }
            } else if (event.key == RELAY_TERMINAL_KEY_ESCAPE) {
                relay_app_back(app);
            } else {
                result = relay_game_handle_input(&app->game,
                    relay_app_game_input(&event));
                if (result == RELAY_GAME_ACTION_PURCHASED) {
                    relay_terminal_focus_node(&app->terminal, &app->game,
                        app->game.focused_node_id);
                }
            }

            if (result != RELAY_GAME_ACTION_NONE) {
                relay_logger_log(&app->logger, result == RELAY_GAME_ACTION_PURCHASED ?
                    RELAY_LOG_LEVEL_INFO : RELAY_LOG_LEVEL_WARNING, "%s.",
                    relay_game_action_result_label(result));
            }
            if (!relay_terminal_draw(&app->terminal, &app->game, app->overlay)) {
                relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
                    "Terminal redraw failed.");
                app->state = RELAY_APP_STATE_FAILED;
                return 1;
            }
        } else if (event.type == RELAY_TERMINAL_EVENT_MOUSE) {
            if (event.connection_source_node_id != 0 &&
                !relay_game_connect_nodes(&app->game,
                    event.connection_source_node_id,
                    event.connection_source_port_index,
                    event.connection_destination_node_id,
                    event.connection_destination_port_index)) {
                relay_logger_log(&app->logger, RELAY_LOG_LEVEL_WARNING,
                    "Graph connection was rejected.");
            }
            if (event.dragged_node_id != 0 &&
                !relay_game_move_node(&app->game, event.dragged_node_id,
                    event.grid_delta_x, event.grid_delta_y)) {
                relay_logger_log(&app->logger, RELAY_LOG_LEVEL_WARNING,
                    "Node drag movement was rejected.");
            }
            if (!relay_terminal_draw(&app->terminal, &app->game, app->overlay)) {
                relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
                    "Terminal viewport redraw failed.");
                app->state = RELAY_APP_STATE_FAILED;
                return 1;
            }
        } else if (event.type == RELAY_TERMINAL_EVENT_NONE && simulation_changed &&
            !relay_terminal_draw(&app->terminal, &app->game, app->overlay)) {
            relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
                "Simulation frame redraw failed.");
            app->state = RELAY_APP_STATE_FAILED;
            return 1;
        }
    }

    relay_logger_log(&app->logger, RELAY_LOG_LEVEL_INFO,
        "Relay game loop stopped after %llu frames.",
        (unsigned long long)app->frame_index);
    return 0;
}

/** Restore services in reverse ownership order and finalize application state. */
void relay_app_shutdown(Relay_App *app)
{
    if (app == NULL || app->state == RELAY_APP_STATE_STOPPED ||
        app->state == RELAY_APP_STATE_UNINITIALIZED) {
        return;
    }

    app->state = RELAY_APP_STATE_SHUTTING_DOWN;
    relay_terminal_shutdown(&app->terminal);
    relay_game_shutdown(&app->game);
    relay_job_system_shutdown(&app->jobs);
    relay_event_bus_shutdown(&app->events);
    relay_logger_log(&app->logger, RELAY_LOG_LEVEL_INFO,
        "Relay application shutdown completed.");
    relay_logger_shutdown(&app->logger);
    app->state = RELAY_APP_STATE_STOPPED;
}
