#ifndef RELAY_APP_H
#define RELAY_APP_H

#include "relay/event.h"
#include "relay/game.h"
#include "relay/job_system.h"
#include "relay/logger.h"
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
    Relay_Game game;
    Relay_JobSystem jobs;
    Relay_Logger logger;
    Relay_Terminal terminal;
    Relay_TerminalOverlay overlay;
    bool should_exit;
    uint64_t frame_index;
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
