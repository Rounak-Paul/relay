#ifndef RELAY_APP_H
#define RELAY_APP_H

#include "relay/event.h"
#include "relay/game.h"
#include "relay/job_system.h"
#include "relay/logger.h"
#include "relay/script_runtime.h"
#include "relay/session.h"
#include "relay/terminal.h"

#include <stdbool.h>
#include <stdint.h>

/** Lifecycle states for the Relay game application. */
typedef enum Relay_AppState {
    RELAY_APP_STATE_UNINITIALIZED,
    RELAY_APP_STATE_INITIALIZING,
    RELAY_APP_STATE_RUNNING,
    RELAY_APP_STATE_SHUTTING_DOWN,
    RELAY_APP_STATE_STOPPED,
    RELAY_APP_STATE_FAILED
} Relay_AppState;

/** Root state for Relay's engine and future game systems. */
typedef struct Relay_App {
    Relay_AppState state;
    Relay_EventBus events;
    Relay_Logger logger;
    Relay_ScriptRuntime scripts;
    Relay_Game game;
    Relay_SessionStore sessions;
    Relay_JobSystem jobs;
    Relay_Terminal terminal;
    Relay_TerminalOverlay overlay;
    Relay_SessionSaveMode pending_save_mode;
    bool should_exit;
    bool exit_after_save;
    double last_frame_seconds;
    double simulation_accumulator;
} Relay_App;

/** Initialize the application and all owned runtime services. */
bool relay_app_init(Relay_App *app);

/** Run Relay's game loop until a quit request or runtime failure. */
int relay_app_run(Relay_App *app);

/** Shut down owned services; calling it repeatedly is safe. */
void relay_app_shutdown(Relay_App *app);

#endif
