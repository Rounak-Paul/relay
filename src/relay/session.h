#ifndef RELAY_SESSION_H
#define RELAY_SESSION_H

#include "relay/game.h"
#include "relay/logger.h"
#include "relay/script_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    RELAY_SESSION_PATH_CAPACITY = 1024,
    RELAY_SESSION_STATUS_CAPACITY = 160,
    RELAY_SESSION_SLOT_CAPACITY = 64,
    RELAY_SESSION_SLOT_NAME_CAPACITY = 17
};

/** Save operation chosen by the player at the application boundary. */
typedef enum Relay_SessionSaveMode {
    RELAY_SESSION_SAVE_OVERWRITE,
    RELAY_SESSION_SAVE_NEW_SLOT
} Relay_SessionSaveMode;

/** One discovered persistent slot ordered by most recent valid save. */
typedef struct Relay_SessionSlot {
    uint64_t id;
    uint64_t save_revision;
    uint64_t simulation_step;
    uint64_t saved_at_unix_seconds;
    char directory_name[RELAY_SESSION_SLOT_NAME_CAPACITY];
    bool valid;
} Relay_SessionSlot;

/** Application-owned durable slot repository and resolved platform paths. */
typedef struct Relay_SessionStore {
    char directory[RELAY_SESSION_PATH_CAPACITY];
    char sessions_directory[RELAY_SESSION_PATH_CAPACITY];
    char state_path[RELAY_SESSION_PATH_CAPACITY];
    char state_temporary_path[RELAY_SESSION_PATH_CAPACITY];
    char status[RELAY_SESSION_STATUS_CAPACITY];
    Relay_SessionSlot slots[RELAY_SESSION_SLOT_CAPACITY];
    size_t slot_count;
    size_t selected_slot;
    uint64_t last_played_session_id;
    uint64_t active_session_id;
    bool continue_available;
    bool initialized;
} Relay_SessionStore;

/** Resolve and create the platform data directory, optionally under a test root. */
bool relay_session_store_init(Relay_SessionStore *store,
    const char *override_directory);

/** Start a distinct unsaved session identity in an initialized game. */
void relay_session_begin_new(Relay_SessionStore *store, Relay_Game *game);

/** Persist by overwriting the active slot or atomically creating a new slot. */
bool relay_session_save(Relay_SessionStore *store, Relay_Game *game,
    Relay_ScriptRuntime *runtime, Relay_Logger *logger,
    Relay_SessionSaveMode mode);

/** Transactionally load the root-state file's last-played valid slot. */
bool relay_session_load_last(Relay_SessionStore *store, Relay_Game *game,
    Relay_ScriptRuntime *runtime, Relay_Logger *logger);

/** Transactionally load one discovered slot by its stable session identity. */
bool relay_session_load_slot(Relay_SessionStore *store, uint64_t session_id,
    Relay_Game *game, Relay_ScriptRuntime *runtime, Relay_Logger *logger);

#endif
