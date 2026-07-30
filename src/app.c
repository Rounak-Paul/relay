#include "relay/app.h"

#include "relay/asset.h"

#include <stdio.h>

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
    if (app->overlay == RELAY_TERMINAL_OVERLAY_SESSION_MENU) {
        return;
    }
    if (app->overlay == RELAY_TERMINAL_OVERLAY_SLOT_SELECT) {
        app->overlay = RELAY_TERMINAL_OVERLAY_SESSION_MENU;
        return;
    }
    if (app->overlay == RELAY_TERMINAL_OVERLAY_EXIT_CONFIRM &&
        app->game.session_exit_from_menu) {
        app->game.session_exit_from_menu = false;
        app->overlay = RELAY_TERMINAL_OVERLAY_SESSION_MENU;
        return;
    }
    if (app->overlay == RELAY_TERMINAL_OVERLAY_SAVE_SELECT ||
        app->overlay == RELAY_TERMINAL_OVERLAY_SAVE_FAILED) {
        app->overlay = app->exit_after_save ?
            RELAY_TERMINAL_OVERLAY_EXIT_CONFIRM :
            RELAY_TERMINAL_OVERLAY_NONE;
        app->exit_after_save = false;
        app->game.session_exit_after_save = false;
        return;
    }
    if (app->overlay != RELAY_TERMINAL_OVERLAY_NONE) {
        app->overlay = RELAY_TERMINAL_OVERLAY_NONE;
    } else if (!relay_game_back(&app->game)) {
        app->game.session_exit_from_menu = false;
        app->overlay = RELAY_TERMINAL_OVERLAY_EXIT_CONFIRM;
    }
}

/** Save the current session at a main-thread simulation boundary. */
static bool relay_app_save(Relay_App *app, Relay_SessionSaveMode mode)
{
    return relay_session_save(&app->sessions, &app->game, &app->scripts,
        &app->logger, mode);
}

/** Apply one key event to the startup session menu. */
static void relay_app_handle_session_menu(Relay_App *app,
    const Relay_TerminalEvent *event)
{
    if (event->session_menu_index_plus_one > 0 &&
        event->session_menu_index_plus_one <=
            RELAY_GAME_SESSION_MENU_COUNT) {
        app->game.session_menu_selection =
            (Relay_GameSessionMenuItem)(
                event->session_menu_index_plus_one - 1);
    }
    if (event->key == RELAY_TERMINAL_KEY_UP ||
        event->character == 'k') {
        app->game.session_menu_selection =
            app->game.session_menu_selection ==
                RELAY_GAME_SESSION_MENU_CONTINUE ?
                RELAY_GAME_SESSION_MENU_EXIT :
                (Relay_GameSessionMenuItem)(
                    app->game.session_menu_selection - 1);
        return;
    }
    if (event->key == RELAY_TERMINAL_KEY_DOWN ||
        event->character == 'j') {
        app->game.session_menu_selection =
            (Relay_GameSessionMenuItem)(
                (app->game.session_menu_selection + 1) %
                    RELAY_GAME_SESSION_MENU_COUNT);
        return;
    }
    if (event->key != RELAY_TERMINAL_KEY_CONFIRM &&
        event->session_menu_index_plus_one == 0) {
        return;
    }
    if (app->game.session_menu_selection ==
        RELAY_GAME_SESSION_MENU_CONTINUE) {
        if (relay_session_load_last(&app->sessions, &app->game, &app->scripts,
                &app->logger)) {
            app->game.session_exit_from_menu = false;
            app->overlay = RELAY_TERMINAL_OVERLAY_NONE;
            app->simulation_accumulator = 0.0;
        } else {
            (void)snprintf(app->game.session_status,
                sizeof(app->game.session_status), "%s",
                "No valid session is available to continue");
        }
        return;
    }
    if (app->game.session_menu_selection == RELAY_GAME_SESSION_MENU_NEW) {
        relay_session_begin_new(&app->sessions, &app->game);
        app->game.session_exit_from_menu = false;
        app->overlay = RELAY_TERMINAL_OVERLAY_NONE;
        app->simulation_accumulator = 0.0;
        return;
    }
    if (app->game.session_menu_selection == RELAY_GAME_SESSION_MENU_SAVED) {
        app->sessions.selected_slot = 0;
        app->overlay = RELAY_TERMINAL_OVERLAY_SLOT_SELECT;
        return;
    }
    app->game.session_exit_from_menu = true;
    app->overlay = RELAY_TERMINAL_OVERLAY_EXIT_CONFIRM;
}

/** Apply one key event to the saved-slot browser. */
static void relay_app_handle_slot_select(Relay_App *app,
    const Relay_TerminalEvent *event)
{
    if (event->key == RELAY_TERMINAL_KEY_ESCAPE) {
        relay_app_back(app);
    } else if ((event->key == RELAY_TERMINAL_KEY_UP ||
            event->character == 'k') && app->sessions.slot_count > 0) {
        app->sessions.selected_slot =
            app->sessions.selected_slot == 0 ?
                app->sessions.slot_count - 1 :
                app->sessions.selected_slot - 1;
    } else if ((event->key == RELAY_TERMINAL_KEY_DOWN ||
            event->character == 'j') && app->sessions.slot_count > 0) {
        app->sessions.selected_slot =
            (app->sessions.selected_slot + 1) % app->sessions.slot_count;
    } else if (event->key == RELAY_TERMINAL_KEY_CONFIRM &&
        app->sessions.selected_slot < app->sessions.slot_count &&
        relay_session_load_slot(&app->sessions,
            app->sessions.slots[app->sessions.selected_slot].id,
            &app->game, &app->scripts, &app->logger)) {
        app->game.session_exit_from_menu = false;
        app->overlay = RELAY_TERMINAL_OVERLAY_NONE;
        app->simulation_accumulator = 0.0;
    }
}

/** Apply one key event to overwrite versus save-as-new selection. */
static void relay_app_handle_save_select(Relay_App *app,
    const Relay_TerminalEvent *event)
{
    const bool can_overwrite = app->sessions.active_session_id != 0;
    const bool can_create =
        app->sessions.slot_count < RELAY_SESSION_SLOT_CAPACITY;

    if (event->key == RELAY_TERMINAL_KEY_ESCAPE) {
        relay_app_back(app);
        return;
    }
    if (event->key == RELAY_TERMINAL_KEY_UP ||
        event->key == RELAY_TERMINAL_KEY_DOWN ||
        event->character == 'j' || event->character == 'k') {
        if (can_overwrite && can_create) {
            app->game.session_save_new_selected =
                !app->game.session_save_new_selected;
        } else if (can_create) {
            app->game.session_save_new_selected = true;
        } else {
            app->game.session_save_new_selected = false;
        }
        return;
    }
    if (event->key != RELAY_TERMINAL_KEY_CONFIRM) {
        return;
    }
    if ((!can_overwrite && !app->game.session_save_new_selected) ||
        (!can_create && app->game.session_save_new_selected)) {
        return;
    }
    app->pending_save_mode = app->game.session_save_new_selected ?
        RELAY_SESSION_SAVE_NEW_SLOT : RELAY_SESSION_SAVE_OVERWRITE;
    if (relay_app_save(app, app->pending_save_mode)) {
        app->overlay = RELAY_TERMINAL_OVERLAY_NONE;
        if (app->exit_after_save) {
            app->should_exit = true;
        }
        return;
    }
    app->overlay = RELAY_TERMINAL_OVERLAY_SAVE_FAILED;
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
        if (app->overlay != RELAY_TERMINAL_OVERLAY_EXIT_CONFIRM) {
            app->game.session_exit_from_menu =
                app->overlay == RELAY_TERMINAL_OVERLAY_SESSION_MENU ||
                app->overlay == RELAY_TERMINAL_OVERLAY_SLOT_SELECT;
        }
        app->overlay = RELAY_TERMINAL_OVERLAY_EXIT_CONFIRM;
    } else if (event->type == RELAY_EVENT_TERMINAL_RESIZED) {
        relay_logger_log(&app->logger, RELAY_LOG_LEVEL_DEBUG,
            "Terminal resized at simulation step %llu.",
            (unsigned long long)app->game.simulation_step);
        if (!relay_terminal_draw(&app->terminal, &app->game, &app->sessions,
                app->overlay)) {
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
    if (!relay_session_store_init(&app->sessions, NULL)) {
        relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
            "Session storage initialization failed: %s.",
            app->sessions.status);
        app->state = RELAY_APP_STATE_FAILED;
        return false;
    }
    app->game.session_continue_available =
        app->sessions.continue_available;
    (void)snprintf(app->game.session_status,
        sizeof(app->game.session_status), "%s", app->sessions.status);
    app->game.session_menu_selection =
        app->sessions.continue_available ?
            RELAY_GAME_SESSION_MENU_CONTINUE :
            RELAY_GAME_SESSION_MENU_NEW;
    app->overlay = RELAY_TERMINAL_OVERLAY_SESSION_MENU;
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
    if (!relay_terminal_draw(&app->terminal, &app->game, &app->sessions,
            app->overlay)) {
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

        if (!relay_terminal_poll(&app->terminal, &app->game, &app->sessions,
                app->overlay, &event)) {
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
        if (app->overlay == RELAY_TERMINAL_OVERLAY_NONE) {
            size_t catch_up_steps = 0;
            const double simulation_interval =
                1.0 / RELAY_GAME_SIMULATION_STEPS_PER_SECOND;

            app->simulation_accumulator += elapsed;
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
        } else {
            app->simulation_accumulator = 0.0;
            simulation_caught_up = true;
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

            if (app->overlay == RELAY_TERMINAL_OVERLAY_SESSION_MENU) {
                relay_app_handle_session_menu(app, &event);
            } else if (app->overlay == RELAY_TERMINAL_OVERLAY_SLOT_SELECT) {
                relay_app_handle_slot_select(app, &event);
            } else if (app->overlay == RELAY_TERMINAL_OVERLAY_SAVE_SELECT) {
                relay_app_handle_save_select(app, &event);
            } else if (app->overlay == RELAY_TERMINAL_OVERLAY_SAVE_FAILED) {
                if (event.key == RELAY_TERMINAL_KEY_CONFIRM) {
                    if (relay_app_save(app, app->pending_save_mode)) {
                        app->overlay = RELAY_TERMINAL_OVERLAY_NONE;
                        if (app->exit_after_save) {
                            app->should_exit = true;
                            continue;
                        }
                    }
                } else if (event.character == 'x' ||
                    event.character == 'X') {
                    if (!app->exit_after_save) {
                        continue;
                    }
                    relay_logger_log(&app->logger, RELAY_LOG_LEVEL_WARNING,
                        "Exiting without saving after explicit confirmation.");
                    app->should_exit = true;
                    continue;
                } else if (event.key == RELAY_TERMINAL_KEY_ESCAPE) {
                    relay_app_back(app);
                }
            } else if (app->game.editing_blueprint_id != 0 &&
                app->overlay == RELAY_TERMINAL_OVERLAY_NONE) {
                relay_app_handle_editor_input(app, &event);
            } else if (app->overlay == RELAY_TERMINAL_OVERLAY_EXIT_CONFIRM) {
                if (event.key == RELAY_TERMINAL_KEY_CONFIRM) {
                    if (app->game.session_exit_from_menu) {
                        app->should_exit = true;
                        continue;
                    } else {
                        app->exit_after_save = true;
                        app->game.session_exit_after_save = true;
                        app->game.session_save_new_selected =
                            app->sessions.active_session_id == 0;
                        app->overlay = RELAY_TERMINAL_OVERLAY_SAVE_SELECT;
                    }
                } else if (event.key == RELAY_TERMINAL_KEY_ESCAPE) {
                    relay_app_back(app);
                }
            } else if (event.key == RELAY_TERMINAL_KEY_ESCAPE) {
                relay_app_back(app);
            } else if (event.key == RELAY_TERMINAL_KEY_SAVE ||
                event.character == 's' || event.character == 'S') {
                app->exit_after_save = false;
                app->game.session_exit_after_save = false;
                app->game.session_save_new_selected =
                    app->sessions.active_session_id == 0;
                app->overlay = RELAY_TERMINAL_OVERLAY_SAVE_SELECT;
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
            if (!relay_terminal_draw(&app->terminal, &app->game,
                    &app->sessions, app->overlay)) {
                relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
                    "Terminal redraw failed.");
                app->state = RELAY_APP_STATE_FAILED;
                return 1;
            }
        } else if (event.type == RELAY_TERMINAL_EVENT_MOUSE &&
            app->overlay == RELAY_TERMINAL_OVERLAY_SESSION_MENU &&
            event.session_menu_index_plus_one != 0) {
            relay_app_handle_session_menu(app, &event);
            if (!relay_terminal_draw(&app->terminal, &app->game,
                    &app->sessions, app->overlay)) {
                relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
                    "Startup menu redraw failed.");
                app->state = RELAY_APP_STATE_FAILED;
                return 1;
            }
        } else if (event.type == RELAY_TERMINAL_EVENT_MOUSE &&
            app->overlay == RELAY_TERMINAL_OVERLAY_NONE) {
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
            if (!relay_terminal_draw(&app->terminal, &app->game,
                    &app->sessions, app->overlay)) {
                relay_logger_log(&app->logger, RELAY_LOG_LEVEL_ERROR,
                    "Terminal viewport redraw failed.");
                app->state = RELAY_APP_STATE_FAILED;
                return 1;
            }
        } else if (event.type == RELAY_TERMINAL_EVENT_NONE &&
            simulation_changed && simulation_caught_up &&
            !relay_terminal_draw(&app->terminal, &app->game, &app->sessions,
                app->overlay)) {
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
