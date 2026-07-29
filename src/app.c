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
    if (event->key == RELAY_TERMINAL_KEY_TAB) {
        return RELAY_GAME_INPUT_TOGGLE_PANEL_TAB;
    }
    if (event->character == 'm' || event->character == 'M') {
        return RELAY_GAME_INPUT_TOGGLE_MAP;
    }
    if (event->character == '[') {
        return RELAY_GAME_INPUT_PREVIOUS_TIMER_INTERVAL;
    }
    if (event->character == ']') {
        return RELAY_GAME_INPUT_NEXT_TIMER_INTERVAL;
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

/** Apply one normalized terminal event to the modal Blueprint source editor. */
static void relay_app_handle_editor_input(Relay_App *app,
    const Relay_TerminalEvent *event)
{
    Relay_Blueprint *blueprint = relay_game_editing_blueprint(&app->game);

    if (blueprint == NULL) {
        return;
    }
    if (blueprint->editor_mode == RELAY_BLUEPRINT_EDITOR_COMMAND) {
        if (event->key == RELAY_TERMINAL_KEY_ESCAPE) {
            (void)relay_game_editor_leave_mode(&app->game);
        } else if (event->key == RELAY_TERMINAL_KEY_BACKSPACE) {
            (void)relay_game_editor_command_backspace(&app->game);
        } else if (event->key == RELAY_TERMINAL_KEY_CONFIRM) {
            const Relay_GameEditorCommandResult result =
                relay_game_editor_command_execute(&app->game);

            if (result == RELAY_GAME_EDITOR_COMMAND_SAVED ||
                result == RELAY_GAME_EDITOR_COMMAND_FAILED) {
                relay_logger_log(&app->logger,
                    result == RELAY_GAME_EDITOR_COMMAND_SAVED ?
                        RELAY_LOG_LEVEL_INFO : RELAY_LOG_LEVEL_WARNING,
                    "%s", blueprint->diagnostic.message);
            }
        } else if (event->character >= 32 && event->character <= 126) {
            (void)relay_game_editor_command_insert(&app->game,
                event->character);
        }
        return;
    }
    if (blueprint->editor_mode == RELAY_BLUEPRINT_EDITOR_INSERT) {
        if (event->key == RELAY_TERMINAL_KEY_ESCAPE) {
            if (!relay_game_editor_completion_dismiss(&app->game)) {
                (void)relay_game_editor_leave_mode(&app->game);
            }
        } else if (event->key == RELAY_TERMINAL_KEY_UP ||
            event->key == RELAY_TERMINAL_KEY_DOWN) {
            const int direction =
                event->key == RELAY_TERMINAL_KEY_UP ? -1 : 1;

            if (!relay_game_editor_completion_move(&app->game, direction)) {
                (void)relay_game_editor_move_vertical(&app->game, direction);
            }
        } else if (event->key == RELAY_TERMINAL_KEY_TAB) {
            (void)relay_game_editor_completion_accept(&app->game);
        } else if (event->key == RELAY_TERMINAL_KEY_LEFT ||
            event->key == RELAY_TERMINAL_KEY_RIGHT) {
            (void)relay_game_editor_move_horizontal(&app->game,
                event->key == RELAY_TERMINAL_KEY_LEFT ? -1 : 1);
        } else if (event->key == RELAY_TERMINAL_KEY_HOME ||
            event->key == RELAY_TERMINAL_KEY_END) {
            (void)relay_game_editor_move_line_boundary(&app->game,
                event->key == RELAY_TERMINAL_KEY_END);
        } else if (event->key == RELAY_TERMINAL_KEY_BACKSPACE) {
            (void)relay_game_editor_backspace(&app->game);
        } else if (event->key == RELAY_TERMINAL_KEY_DELETE) {
            (void)relay_game_editor_delete(&app->game);
        } else if (event->key == RELAY_TERMINAL_KEY_CONFIRM) {
            (void)relay_game_editor_insert(&app->game, '\n');
        } else if (event->character >= 32 && event->character <= 126) {
            (void)relay_game_editor_insert(&app->game, event->character);
        }
        return;
    }

    if (event->key == RELAY_TERMINAL_KEY_ESCAPE) {
        relay_app_back(app);
    } else if (event->character == ':') {
        (void)relay_game_editor_enter_command(&app->game);
    } else if (event->character == 'i') {
        (void)relay_game_editor_enter_insert(&app->game);
    } else if (event->character == 'a') {
        if (blueprint->cursor < blueprint->source_size &&
            blueprint->source[blueprint->cursor] != '\n') {
            (void)relay_game_editor_move_horizontal(&app->game, 1);
        }
        (void)relay_game_editor_enter_insert(&app->game);
    } else if (event->character == 'I') {
        (void)relay_game_editor_move_line_boundary(&app->game, false);
        (void)relay_game_editor_enter_insert(&app->game);
    } else if (event->character == 'A') {
        (void)relay_game_editor_move_line_boundary(&app->game, true);
        (void)relay_game_editor_enter_insert(&app->game);
    } else if (event->character == 'o') {
        (void)relay_game_editor_move_line_boundary(&app->game, true);
        (void)relay_game_editor_insert(&app->game, '\n');
        (void)relay_game_editor_enter_insert(&app->game);
    } else if (event->character == 'x' ||
        event->key == RELAY_TERMINAL_KEY_DELETE) {
        (void)relay_game_editor_delete(&app->game);
    } else if (event->character == '0' ||
        event->key == RELAY_TERMINAL_KEY_HOME) {
        (void)relay_game_editor_move_line_boundary(&app->game, false);
    } else if (event->character == '$' ||
        event->key == RELAY_TERMINAL_KEY_END) {
        (void)relay_game_editor_move_line_boundary(&app->game, true);
    } else if (event->character == 'h' ||
        event->key == RELAY_TERMINAL_KEY_LEFT) {
        (void)relay_game_editor_move_horizontal(&app->game, -1);
    } else if (event->character == 'l' ||
        event->key == RELAY_TERMINAL_KEY_RIGHT) {
        (void)relay_game_editor_move_horizontal(&app->game, 1);
    } else if (event->character == 'k' ||
        event->key == RELAY_TERMINAL_KEY_UP) {
        (void)relay_game_editor_move_vertical(&app->game, -1);
    } else if (event->character == 'j' ||
        event->key == RELAY_TERMINAL_KEY_DOWN) {
        (void)relay_game_editor_move_vertical(&app->game, 1);
    } else if (event->key == RELAY_TERMINAL_KEY_SAVE) {
        const bool saved = relay_game_editor_save(&app->game);

        relay_logger_log(&app->logger,
            saved ? RELAY_LOG_LEVEL_INFO : RELAY_LOG_LEVEL_WARNING,
            "%s", blueprint->diagnostic.message);
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
            "Terminal resized at simulation step %llu.",
            (unsigned long long)app->game.simulation_step);
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
    if (relay_departure_mono_font()->size == 0) {
        relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
            "Embedded font initialization failed.");
        app->state = RELAY_APP_STATE_FAILED;
        return false;
    }
    if (!relay_script_runtime_init(&app->scripts,
            RELAY_SCRIPT_RUNTIME_DEFAULT_MEMORY_LIMIT)) {
        relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
            "Sandboxed Lua runtime initialization failed.");
        app->state = RELAY_APP_STATE_FAILED;
        return false;
    }
    relay_logger_log(&app->logger, RELAY_LOG_LEVEL_INFO,
        "%s scripting runtime initialized with a %zu-byte memory limit.",
        relay_script_runtime_version(), app->scripts.memory_limit);
    if (!relay_game_init(&app->game, &app->scripts)) {
        relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
            "Game state initialization failed.");
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
        bool simulation_caught_up;

        if (!relay_terminal_poll(&app->terminal, &app->game, &event)) {
            relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
                "Terminal event polling failed.");
            app->state = RELAY_APP_STATE_FAILED;
            return 1;
        }

        now = relay_app_now_seconds();
        elapsed = now - app->last_frame_seconds;
        app->last_frame_seconds = now;
        if (elapsed < 0.0) {
            elapsed = 0.0;
        }
        app->simulation_accumulator += elapsed;
        {
            size_t catch_up_steps = 0;
            const double simulation_interval =
                1.0 / RELAY_GAME_SIMULATION_STEPS_PER_SECOND;

            while (app->simulation_accumulator >= simulation_interval &&
                catch_up_steps < RELAY_GAME_MAX_CATCH_UP_STEPS) {
                if (!relay_game_step(&app->game)) {
                    relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
                        "Gameplay simulation step failed.");
                    app->state = RELAY_APP_STATE_FAILED;
                    return 1;
                }
                app->simulation_accumulator -= simulation_interval;
                simulation_changed = true;
                catch_up_steps++;
            }
            simulation_caught_up =
                app->simulation_accumulator < simulation_interval;
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

            if (app->game.editing_blueprint_id != 0 &&
                app->overlay == RELAY_TERMINAL_OVERLAY_NONE) {
                relay_app_handle_editor_input(app, &event);
            } else if (app->overlay == RELAY_TERMINAL_OVERLAY_EXIT_CONFIRM) {
                if (event.key == RELAY_TERMINAL_KEY_CONFIRM) {
                    app->should_exit = true;
                    continue;
                }
                if (event.key == RELAY_TERMINAL_KEY_ESCAPE) {
                    relay_app_back(app);
                }
            } else if (event.key == RELAY_TERMINAL_KEY_ESCAPE) {
                relay_app_back(app);
            } else if (event.character == 'n' || event.character == 'N') {
                if (!relay_game_create_blueprint(&app->game)) {
                    relay_logger_log(&app->logger, RELAY_LOG_LEVEL_WARNING,
                        "Blueprint creation failed.");
                } else {
                    relay_terminal_focus_node(&app->terminal, &app->game,
                        app->game.focused_node_id);
                }
            } else if (event.character == ',') {
                (void)relay_game_switch_workspace(&app->game, -1);
            } else if (event.character == '.') {
                (void)relay_game_switch_workspace(&app->game, 1);
            } else if (event.character == 'e' || event.character == 'E') {
                if (!relay_game_open_editor(&app->game)) {
                    relay_logger_log(&app->logger, RELAY_LOG_LEVEL_WARNING,
                        "No blueprint is available for editing.");
                }
            } else if ((event.character == 'o' || event.character == 'O') &&
                app->game.active_tab == RELAY_GAME_PANEL_TAB_BLUEPRINTS) {
                if (!relay_game_open_selected_blueprint(&app->game)) {
                    relay_logger_log(&app->logger, RELAY_LOG_LEVEL_WARNING,
                        "The selected Blueprint could not be opened.");
                }
            } else if (event.character == 'c' || event.character == 'C') {
                if (!relay_game_close_active_blueprint(&app->game) &&
                    app->game.active_workspace != 0) {
                    relay_logger_log(&app->logger, RELAY_LOG_LEVEL_WARNING,
                        "The active Blueprint tab could not be closed.");
                }
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
            if (event.workspace_tab_index_plus_one != 0) {
                (void)relay_game_activate_workspace(&app->game,
                    event.workspace_tab_index_plus_one - 1);
            }
            if (event.panel_tab_index_plus_one != 0) {
                (void)relay_game_select_panel_tab(&app->game,
                    event.panel_tab_index_plus_one - 1);
            }
            if (event.selected_node_id != 0 &&
                !relay_game_focus_node(&app->game, event.selected_node_id)) {
                relay_logger_log(&app->logger, RELAY_LOG_LEVEL_WARNING,
                    "Node selection was rejected.");
            }
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
        } else if (event.type == RELAY_TERMINAL_EVENT_NONE &&
            simulation_changed && simulation_caught_up &&
            !relay_terminal_draw(&app->terminal, &app->game, app->overlay)) {
            relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
                "Simulation frame redraw failed.");
            app->state = RELAY_APP_STATE_FAILED;
            return 1;
        }
    }

    relay_logger_log(&app->logger, RELAY_LOG_LEVEL_INFO,
        "Relay game loop stopped after %llu simulation steps.",
        (unsigned long long)app->game.simulation_step);
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
    relay_script_runtime_shutdown(&app->scripts);
    relay_job_system_shutdown(&app->jobs);
    relay_event_bus_shutdown(&app->events);
    relay_logger_log(&app->logger, RELAY_LOG_LEVEL_INFO,
        "Relay application shutdown completed.");
    relay_logger_shutdown(&app->logger);
    app->state = RELAY_APP_STATE_STOPPED;
}
