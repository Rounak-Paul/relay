#include "relay/game.h"
#include "relay/platform.h"
#include "relay/session.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if RELAY_PLATFORM_WINDOWS
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef RELAY_TEST_SESSION_DIRECTORY
#error "RELAY_TEST_SESSION_DIRECTORY must name the isolated test directory"
#endif

/** Recursively clear only the isolated CTest session repository. */
static bool relay_test_remove_tree(const char *path)
{
#if RELAY_PLATFORM_WINDOWS
    WIN32_FIND_DATAA entry;
    HANDLE search;
    char pattern[RELAY_SESSION_PATH_CAPACITY];
    bool valid = true;

    if (snprintf(pattern, sizeof(pattern), "%s/*", path) < 0) {
        return false;
    }
    search = FindFirstFileA(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();

        return error == ERROR_FILE_NOT_FOUND ||
            error == ERROR_PATH_NOT_FOUND;
    }
    do {
        char child[RELAY_SESSION_PATH_CAPACITY];

        if (strcmp(entry.cFileName, ".") == 0 ||
            strcmp(entry.cFileName, "..") == 0) {
            continue;
        }
        if (snprintf(child, sizeof(child), "%s/%s", path,
                entry.cFileName) < 0) {
            valid = false;
            break;
        }
        valid = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ?
            relay_test_remove_tree(child) && RemoveDirectoryA(child) != 0 :
            DeleteFileA(child) != 0;
    } while (valid && FindNextFileA(search, &entry));
    (void)FindClose(search);
    return valid;
#else
    DIR *directory = opendir(path);
    struct dirent *entry;
    bool valid = true;

    if (directory == NULL) {
        return errno == ENOENT;
    }
    while (valid && (entry = readdir(directory)) != NULL) {
        char child[RELAY_SESSION_PATH_CAPACITY];
        struct stat information;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(child, sizeof(child), "%s/%s", path,
                entry->d_name) < 0 ||
            lstat(child, &information) != 0) {
            valid = false;
        } else if (S_ISDIR(information.st_mode)) {
            valid = relay_test_remove_tree(child) && rmdir(child) == 0;
        } else {
            valid = unlink(child) == 0;
        }
    }
    if (closedir(directory) != 0) {
        valid = false;
    }
    return valid;
#endif
}

/** Resolve one file inside the active slot directory. */
static bool relay_test_slot_path(char *path, size_t capacity,
    const Relay_SessionStore *store, const char *relative)
{
    const int written = snprintf(path, capacity, "%s/sessions/%016llx/%s",
        store->directory, (unsigned long long)store->active_session_id,
        relative);

    return written >= 0 && (size_t)written < capacity;
}

/** Return whether one script file contains exactly the expected source. */
static bool relay_test_file_equals(const char *path, const char *expected)
{
    FILE *file = fopen(path, "rb");
    const size_t size = strlen(expected);
    char buffer[RELAY_BLUEPRINT_SOURCE_CAPACITY];
    int trailing;
    bool valid;

    if (file == NULL || size >= sizeof(buffer)) {
        if (file != NULL) {
            (void)fclose(file);
        }
        return false;
    }
    valid = fread(buffer, 1, size, file) == size;
    trailing = valid ? fgetc(file) : 0;
    if (fclose(file) != 0) {
        valid = false;
    }
    return valid && trailing == EOF &&
        memcmp(buffer, expected, size) == 0;
}

/** Create one isolated test directory. */
static bool relay_test_create_directory(const char *path)
{
#if RELAY_PLATFORM_WINDOWS
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0700) == 0 || errno == EEXIST;
#endif
}

/** Create one exact test-owned file with the supplied contents. */
static bool relay_test_create_file(const char *path, const char *contents)
{
    FILE *file;
    const size_t size = strlen(contents);
    bool valid;

    file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    valid = fwrite(contents, 1, size, file) == size;
    if (fclose(file) != 0) {
        valid = false;
    }
    return valid;
}

/** Return whether one obsolete or staging path no longer exists. */
static bool relay_test_path_missing(const char *path)
{
#if RELAY_PLATFORM_WINDOWS
    return GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES;
#else
    struct stat information;

    return lstat(path, &information) != 0 && errno == ENOENT;
#endif
}

/** Replace and compile one Blueprint source as a deployed revision. */
static bool relay_test_blueprint_source(Relay_Game *game,
    Relay_Blueprint *blueprint, const char *source)
{
    const size_t size = strlen(source);

    if (size == 0 || size >= sizeof(blueprint->source)) {
        return false;
    }
    (void)memcpy(blueprint->source, source, size + 1);
    blueprint->source_size = size;
    blueprint->cursor = size;
    blueprint->revision++;
    blueprint->dirty = true;
    return relay_blueprint_compile(&game->blueprints, blueprint);
}

/** Find one initialized process expanded from a nested Blueprint. */
static Relay_Node *relay_test_blueprint_process(Relay_Game *game,
    Relay_BlueprintId blueprint_id)
{
    size_t index;

    for (index = 0; index < game->nodes.count; index++) {
        Relay_Node *node = &game->nodes.nodes[index];

        if (node->runtime_kind == RELAY_NODE_RUNTIME_BLUEPRINT_PROCESS &&
            node->blueprint_id == blueprint_id &&
            node->script_state.initialized) {
            return node;
        }
    }
    return NULL;
}

/** Advance a game by an exact deterministic step count. */
static bool relay_test_step_game(Relay_Game *game, size_t count)
{
    size_t index;

    for (index = 0; index < count; index++) {
        if (!relay_game_step(game)) {
            return false;
        }
    }
    return true;
}

/** Read the first item emitted by a Blueprint wrapper. */
static bool relay_test_wrapper_item(const Relay_Node *wrapper,
    Relay_Item *item)
{
    return wrapper != NULL && wrapper->output_queues[0].count > 0 &&
        relay_item_queue_peek(&wrapper->output_queues[0], item);
}

/** Overwrite one byte of the isolated save file. */
static bool relay_test_write_byte(const char *path, long offset,
    unsigned char value)
{
    FILE *file = fopen(path, "r+b");
    bool valid;

    if (file == NULL || fseek(file, offset, SEEK_SET) != 0) {
        if (file != NULL) {
            (void)fclose(file);
        }
        return false;
    }
    valid = fwrite(&value, 1, 1, file) == 1;
    if (fclose(file) != 0) {
        valid = false;
    }
    return valid;
}

/** Write one little-endian integer into a test save buffer. */
static void relay_test_write_u64(unsigned char *data, uint64_t value)
{
    size_t index;

    for (index = 0; index < 8; index++) {
        data[index] = (unsigned char)(value >> (index * 8));
    }
}

/** Return the save codec's FNV-1a payload checksum. */
static uint32_t relay_test_session_checksum(const unsigned char *data,
    size_t size)
{
    uint32_t hash = UINT32_C(2166136261);
    size_t index;

    for (index = 0; index < size; index++) {
        hash ^= data[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

/** Craft a checksum-valid save containing two queues that claim one item ID. */
static bool relay_test_duplicate_saved_item(const char *path,
    Relay_ItemId retained_id, Relay_ItemId replaced_id)
{
    FILE *file = fopen(path, "rb");
    unsigned char *data;
    unsigned char pattern[28] = {0};
    long file_size;
    size_t index;
    size_t match = SIZE_MAX;
    uint32_t checksum;
    bool valid;

    if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
        (file_size = ftell(file)) < 36 || fseek(file, 0, SEEK_SET) != 0) {
        if (file != NULL) {
            (void)fclose(file);
        }
        return false;
    }
    data = malloc((size_t)file_size);
    if (data == NULL) {
        (void)fclose(file);
        return false;
    }
    valid = fread(data, 1, (size_t)file_size, file) == (size_t)file_size;
    if (fclose(file) != 0) {
        valid = false;
    }
    if (!valid) {
        free(data);
        return false;
    }
    pattern[0] = 2;
    relay_test_write_u64(&pattern[4], retained_id);
    pattern[12] = (unsigned char)RELAY_NODE_PORT_TYPE_COAL;
    relay_test_write_u64(&pattern[16], replaced_id);
    pattern[24] = (unsigned char)RELAY_NODE_PORT_TYPE_COAL;
    for (index = 24; index + sizeof(pattern) <= (size_t)file_size; index++) {
        if (memcmp(&data[index], pattern, sizeof(pattern)) == 0) {
            match = index;
            break;
        }
    }
    if (match == SIZE_MAX) {
        free(data);
        return false;
    }
    relay_test_write_u64(&data[match + 16], retained_id);
    checksum = relay_test_session_checksum(&data[24],
        (size_t)file_size - 24);
    for (index = 0; index < 4; index++) {
        data[20 + index] = (unsigned char)(checksum >> (index * 8));
    }
    file = fopen(path, "wb");
    valid = file != NULL &&
        fwrite(data, 1, (size_t)file_size, file) == (size_t)file_size;
    if (file != NULL && fclose(file) != 0) {
        valid = false;
    }
    free(data);
    return valid;
}

/** Read the first two physical item identities from one wrapper output FIFO. */
static bool relay_test_two_item_ids(const Relay_Node *wrapper,
    Relay_ItemId *first, Relay_ItemId *second)
{
    const Relay_ItemQueue *queue;

    if (wrapper == NULL || first == NULL || second == NULL) {
        return false;
    }
    queue = &wrapper->output_queues[0];
    if (queue->count < 2) {
        return false;
    }
    *first = queue->items[queue->head].id;
    *second = queue->items[
        (queue->head + 1) % RELAY_ITEM_QUEUE_CAPACITY].id;
    return *first != 0 && *second != 0 && *first != *second;
}

/** Truncate the isolated session to a deliberately incomplete envelope. */
static bool relay_test_truncate_session(const char *path)
{
    static const unsigned char magic[8] = {
        'R', 'L', 'Y', 'S', 'A', 'V', 'E', '1'
    };
    FILE *file = fopen(path, "wb");
    bool valid;

    if (file == NULL) {
        return false;
    }
    valid = fwrite(magic, 1, sizeof(magic), file) == sizeof(magic);
    if (fclose(file) != 0) {
        valid = false;
    }
    return valid;
}

/** Verify complete, deterministic, transactional session persistence. */
int relay_session_test(void)
{
    static const char child_source[] =
        "input('coal', Type.COAL)\n"
        "output('coal_out', Type.COAL)\n"
        "\n"
        "function on_process(state, inputs, outputs)\n"
        "  if #inputs.coal > 0 and #outputs.coal_out < outputs.coal_out.capacity then\n"
        "    state.moved = (state.moved or 0) + 1\n"
        "    outputs.coal_out:push(inputs.coal:pop())\n"
        "  end\n"
        "end\n";
    static const char parent_source[] =
        "input('coal', Type.COAL)\n"
        "output('coal_out', Type.COAL)\n"
        "\n"
        "local n3 = instance(script.script_1, { x = 90, y = 0 })\n"
        "connect(inputs.coal, n3.inputs.coal)\n"
        "connect(n3.outputs.coal_out, outputs.coal_out)\n"
        "\n"
        "function on_process(state, inputs, outputs)\n"
        "end\n";
    Relay_ScriptRuntime runtime = {0};
    Relay_Game game = {0};
    Relay_SessionStore store = {0};
    Relay_SessionStore reopened_store = {0};
    Relay_SessionStore recovered_store = {0};
    Relay_Blueprint *child;
    Relay_Blueprint *parent;
    Relay_Node *miner;
    Relay_Node *wrapper;
    Relay_Node *process;
    Relay_Item saved_item;
    Relay_Item continued_item;
    Relay_Item replay_item;
    Relay_ItemId saved_next_item_id;
    Relay_ItemId first_item_id;
    Relay_ItemId second_item_id;
    Relay_NodeId wrapper_id;
    Relay_BlueprintId child_id;
    Relay_BlueprintId parent_id;
    uint64_t saved_session_id;
    uint64_t forked_session_id;
    uint64_t replay_step;
    uint64_t replay_activations;
    int64_t saved_wrapper_x;
    int64_t saved_wrapper_y;
    char child_draft[RELAY_BLUEPRINT_SOURCE_CAPACITY];
    char session_path[RELAY_SESSION_PATH_CAPACITY];
    char child_source_path[RELAY_SESSION_PATH_CAPACITY];
    char child_deployed_path[RELAY_SESSION_PATH_CAPACITY];
    char renamed_source_path[RELAY_SESSION_PATH_CAPACITY];
    char legacy_path[RELAY_SESSION_PATH_CAPACITY];
    char final_slot_path[RELAY_SESSION_PATH_CAPACITY];
    char backup_slot_path[RELAY_SESSION_PATH_CAPACITY];
    char temporary_slot_path[RELAY_SESSION_PATH_CAPACITY];
    char unrelated_directory[RELAY_SESSION_PATH_CAPACITY];
    char state_temporary_path[RELAY_SESSION_PATH_CAPACITY];
    int child_draft_size;
    int result = 1;

    if (!relay_test_remove_tree(RELAY_TEST_SESSION_DIRECTORY)) {
        return 1;
    }
    if (!relay_test_create_directory(RELAY_TEST_SESSION_DIRECTORY) ||
        snprintf(legacy_path, sizeof(legacy_path), "%s/session.rly",
            RELAY_TEST_SESSION_DIRECTORY) < 0) {
        return 1;
    }
    {
        FILE *legacy = fopen(legacy_path, "wb");
        bool legacy_valid;

        if (legacy == NULL) {
            return 1;
        }
        legacy_valid = fwrite("legacy", 1, 6, legacy) == 6;
        if (fclose(legacy) != 0) {
            legacy_valid = false;
        }
        if (!legacy_valid) {
            return 1;
        }
    }
    if (!relay_script_runtime_init(&runtime,
            RELAY_SCRIPT_RUNTIME_DEFAULT_MEMORY_LIMIT) ||
        !relay_game_init(&game, &runtime) ||
        !relay_session_store_init(&store, RELAY_TEST_SESSION_DIRECTORY) ||
        !relay_test_path_missing(legacy_path)) {
        goto cleanup;
    }
    relay_session_begin_new(&store, &game);
    saved_session_id = game.session_id;
    if (!relay_game_create_blueprint(&game)) {
        goto cleanup;
    }
    child = relay_game_active_blueprint(&game);
    child_id = child == NULL ? 0 : child->id;
    if (child == NULL ||
        !relay_test_blueprint_source(&game, child, child_source) ||
        !relay_game_create_blueprint(&game)) {
        goto cleanup;
    }
    parent = relay_game_active_blueprint(&game);
    parent_id = parent == NULL ? 0 : parent->id;
    if (parent == NULL ||
        !relay_test_blueprint_source(&game, parent, parent_source) ||
        !relay_game_activate_workspace(&game, 0) ||
        !relay_game_add_blueprint(&game, parent_id)) {
        goto cleanup;
    }
    wrapper_id = game.focused_node_id;
    miner = relay_node_world_find(&game.nodes, 1);
    wrapper = relay_node_world_find(&game.nodes, wrapper_id);
    if (miner == NULL || wrapper == NULL ||
        !relay_game_connect_nodes(&game, miner->id, 0, wrapper->id, 0) ||
        !relay_game_move_node(&game, wrapper->id, -75, 33) ||
        !relay_test_step_game(&game, 70)) {
        goto cleanup;
    }
    wrapper = relay_node_world_find(&game.nodes, wrapper_id);
    process = relay_test_blueprint_process(&game, child_id);
    if (!relay_node_world_items_valid(&game.nodes) || process == NULL ||
        process->process_activations == 0 ||
        !relay_test_wrapper_item(wrapper, &saved_item)) {
        goto cleanup;
    }
    game.currency = 73;
    game.active_tab = RELAY_GAME_PANEL_TAB_INSPECTOR;
    child = relay_blueprint_library_find(&game.blueprints, child_id);
    child_draft_size = snprintf(child_draft, sizeof(child_draft),
        "%s\nbroken(", child_source);
    if (child == NULL || child_draft_size < 0 ||
        (size_t)child_draft_size >= sizeof(child_draft)) {
        goto cleanup;
    }
    (void)memcpy(child->source, child_draft, (size_t)child_draft_size + 1);
    child->source_size = (size_t)child_draft_size;
    child->cursor = child->source_size;
    child->revision++;
    child->dirty = true;
    saved_wrapper_x = wrapper->grid_x;
    saved_wrapper_y = wrapper->grid_y;
    saved_next_item_id = game.nodes.next_item_id;
    if (!relay_session_save(&store, &game, &runtime, NULL,
            RELAY_SESSION_SAVE_NEW_SLOT) ||
        game.save_revision != 1 || !store.continue_available) {
        goto cleanup;
    }
    if (store.active_session_id != saved_session_id ||
        store.slot_count != 1 ||
        !relay_test_slot_path(session_path, sizeof(session_path), &store,
            "session.rly") ||
        !relay_test_slot_path(child_source_path, sizeof(child_source_path),
            &store, "scripts/script_1.lua") ||
        !relay_test_slot_path(child_deployed_path,
            sizeof(child_deployed_path), &store,
            "scripts/script_1.deployed.lua") ||
        !relay_test_file_equals(child_source_path, child_draft) ||
        !relay_test_file_equals(child_deployed_path, child_source) ||
        !relay_session_store_init(&reopened_store,
            RELAY_TEST_SESSION_DIRECTORY) ||
        reopened_store.last_played_session_id != saved_session_id ||
        !reopened_store.continue_available) {
        goto cleanup;
    }

    game.currency = 999;
    if (!relay_test_step_game(&game, 70)) {
        goto cleanup;
    }
    wrapper = relay_node_world_find(&game.nodes, wrapper_id);
    process = relay_test_blueprint_process(&game, child_id);
    if (process == NULL || !relay_test_wrapper_item(wrapper, &replay_item)) {
        goto cleanup;
    }
    replay_step = game.simulation_step;
    replay_activations = process->process_activations;

    if (!relay_session_load_last(&store, &game, &runtime, NULL)) {
        goto cleanup;
    }
    wrapper = relay_node_world_find(&game.nodes, wrapper_id);
    process = relay_test_blueprint_process(&game, child_id);
    child = relay_blueprint_library_find(&game.blueprints, child_id);
    parent = relay_blueprint_library_find(&game.blueprints, parent_id);
    if (game.session_id != saved_session_id || game.save_revision != 1 ||
        game.currency != 73 || game.nodes.next_item_id != saved_next_item_id ||
        game.active_tab != RELAY_GAME_PANEL_TAB_INSPECTOR ||
        child == NULL || parent == NULL ||
        strcmp(child->source, child_draft) != 0 ||
        strcmp(child->deployed_source, child_source) != 0 || !child->dirty ||
        child->revision <= child->compiled_revision ||
        strcmp(parent->source, parent_source) != 0 ||
        wrapper == NULL || wrapper->grid_x != saved_wrapper_x ||
        wrapper->grid_y != saved_wrapper_y ||
        process == NULL || !relay_test_wrapper_item(wrapper, &continued_item) ||
        continued_item.id != saved_item.id ||
        continued_item.type != RELAY_NODE_PORT_TYPE_COAL ||
        !relay_node_world_items_valid(&game.nodes) ||
        !relay_test_step_game(&game, 70)) {
        goto cleanup;
    }
    wrapper = relay_node_world_find(&game.nodes, wrapper_id);
    process = relay_test_blueprint_process(&game, child_id);
    if (process == NULL || game.simulation_step != replay_step ||
        process->process_activations != replay_activations ||
        !relay_test_wrapper_item(wrapper, &continued_item) ||
        continued_item.id != replay_item.id ||
        !relay_node_world_items_valid(&game.nodes)) {
        goto cleanup;
    }

    if (!relay_test_slot_path(child_source_path, sizeof(child_source_path),
            &store, "scripts/script_1.lua") ||
        remove(child_source_path) != 0 ||
        relay_session_load_slot(&store, store.active_session_id, &game,
            &runtime, NULL) ||
        game.simulation_step != replay_step ||
        !relay_session_save(&store, &game, &runtime, NULL,
            RELAY_SESSION_SAVE_OVERWRITE) ||
        !relay_test_slot_path(child_deployed_path,
            sizeof(child_deployed_path), &store,
            "scripts/script_1.deployed.lua") ||
        !relay_test_write_byte(child_deployed_path, 0, 'X') ||
        relay_session_load_slot(&store, store.active_session_id, &game,
            &runtime, NULL) ||
        game.simulation_step != replay_step ||
        !relay_session_save(&store, &game, &runtime, NULL,
            RELAY_SESSION_SAVE_OVERWRITE) ||
        !relay_test_slot_path(child_source_path, sizeof(child_source_path),
            &store, "scripts/script_1.lua") ||
        !relay_test_slot_path(renamed_source_path,
            sizeof(renamed_source_path), &store,
            "scripts/script_1.moved.lua") ||
        rename(child_source_path, renamed_source_path) != 0 ||
        relay_session_load_slot(&store, store.active_session_id, &game,
            &runtime, NULL) ||
        game.simulation_step != replay_step ||
        !relay_session_save(&store, &game, &runtime, NULL,
            RELAY_SESSION_SAVE_OVERWRITE)) {
        goto cleanup;
    }

    wrapper = relay_node_world_find(&game.nodes, wrapper_id);
    if (!relay_test_two_item_ids(wrapper, &first_item_id,
            &second_item_id) ||
        !relay_session_save(&store, &game, &runtime, NULL,
            RELAY_SESSION_SAVE_OVERWRITE) ||
        !relay_test_slot_path(session_path, sizeof(session_path), &store,
            "session.rly") ||
        !relay_test_duplicate_saved_item(session_path, first_item_id,
            second_item_id) ||
        relay_session_load_slot(&store, store.active_session_id, &game,
            &runtime, NULL) ||
        game.simulation_step != replay_step ||
        !relay_node_world_items_valid(&game.nodes)) {
        goto cleanup;
    }
    if (!relay_session_save(&store, &game, &runtime, NULL,
            RELAY_SESSION_SAVE_OVERWRITE) ||
        !relay_test_slot_path(session_path, sizeof(session_path), &store,
            "session.rly") ||
        !relay_test_write_byte(session_path, 24, 0xff)) {
        goto cleanup;
    }
    replay_step = game.simulation_step;
    if (relay_session_load_slot(&store, store.active_session_id, &game,
            &runtime, NULL) ||
        game.simulation_step != replay_step ||
        game.currency != 73 || !relay_node_world_items_valid(&game.nodes)) {
        goto cleanup;
    }
    if (!relay_session_save(&store, &game, &runtime, NULL,
            RELAY_SESSION_SAVE_OVERWRITE) ||
        !relay_test_slot_path(session_path, sizeof(session_path), &store,
            "session.rly") ||
        !relay_test_truncate_session(session_path) ||
        relay_session_load_slot(&store, store.active_session_id, &game,
            &runtime, NULL) ||
        game.simulation_step != replay_step) {
        goto cleanup;
    }
    if (!relay_session_save(&store, &game, &runtime, NULL,
            RELAY_SESSION_SAVE_OVERWRITE) ||
        !relay_test_slot_path(session_path, sizeof(session_path), &store,
            "session.rly")) {
        goto cleanup;
    }
    {
        const uint64_t oversized = UINT64_C(16) * 1024U * 1024U + 1U;
        size_t byte;

        for (byte = 0; byte < 8; byte++) {
            if (!relay_test_write_byte(session_path, 12 + (long)byte,
                    (unsigned char)(oversized >> (byte * 8)))) {
                goto cleanup;
            }
        }
    }
    if (relay_session_load_slot(&store, store.active_session_id, &game,
            &runtime, NULL) ||
        game.simulation_step != replay_step) {
        goto cleanup;
    }
    if (!relay_session_save(&store, &game, &runtime, NULL,
            RELAY_SESSION_SAVE_OVERWRITE) ||
        !relay_test_slot_path(session_path, sizeof(session_path), &store,
            "session.rly") ||
        !relay_test_write_byte(session_path, 8, 3) ||
        relay_session_load_slot(&store, store.active_session_id, &game,
            &runtime, NULL) ||
        game.simulation_step != replay_step) {
        goto cleanup;
    }
    if (!relay_session_save(&store, &game, &runtime, NULL,
            RELAY_SESSION_SAVE_OVERWRITE) ||
        !relay_session_save(&store, &game, &runtime, NULL,
            RELAY_SESSION_SAVE_NEW_SLOT)) {
        goto cleanup;
    }
    forked_session_id = game.session_id;
    if (forked_session_id == saved_session_id || game.save_revision != 1 ||
        store.slot_count != 2 ||
        store.last_played_session_id != forked_session_id ||
        !relay_session_load_slot(&store, saved_session_id, &game, &runtime,
            NULL) ||
        game.session_id != saved_session_id ||
        !relay_session_store_init(&reopened_store,
            RELAY_TEST_SESSION_DIRECTORY) ||
        reopened_store.last_played_session_id != saved_session_id ||
        !relay_session_load_last(&reopened_store, &game, &runtime, NULL) ||
        game.session_id != saved_session_id) {
        goto cleanup;
    }
    if (!relay_test_write_byte(reopened_store.state_path, 24, 0xff) ||
        !relay_session_store_init(&recovered_store,
            RELAY_TEST_SESSION_DIRECTORY) ||
        recovered_store.slot_count != 2 ||
        !recovered_store.continue_available ||
        recovered_store.last_played_session_id !=
            recovered_store.slots[0].id ||
        snprintf(final_slot_path, sizeof(final_slot_path),
            "%s/sessions/%016llx", RELAY_TEST_SESSION_DIRECTORY,
            (unsigned long long)saved_session_id) < 0 ||
        snprintf(backup_slot_path, sizeof(backup_slot_path),
            "%s/sessions/%016llx.bak", RELAY_TEST_SESSION_DIRECTORY,
            (unsigned long long)saved_session_id) < 0 ||
        rename(final_slot_path, backup_slot_path) != 0 ||
        !relay_session_store_init(&recovered_store,
            RELAY_TEST_SESSION_DIRECTORY) ||
        !relay_session_load_slot(&recovered_store, saved_session_id, &game,
            &runtime, NULL) ||
        snprintf(temporary_slot_path, sizeof(temporary_slot_path),
            "%s/sessions/%016llx.tmp", RELAY_TEST_SESSION_DIRECTORY,
            (unsigned long long)saved_session_id) < 0 ||
        !relay_test_create_directory(temporary_slot_path) ||
        snprintf(state_temporary_path, sizeof(state_temporary_path),
            "%s/state.rly.tmp", RELAY_TEST_SESSION_DIRECTORY) < 0 ||
        !relay_test_create_file(state_temporary_path, "interrupted") ||
        snprintf(unrelated_directory, sizeof(unrelated_directory),
            "%s/sessions/player-notes", RELAY_TEST_SESSION_DIRECTORY) < 0 ||
        !relay_test_create_directory(unrelated_directory) ||
        !relay_session_store_init(&recovered_store,
            RELAY_TEST_SESSION_DIRECTORY) ||
        !relay_test_path_missing(temporary_slot_path) ||
        !relay_test_path_missing(state_temporary_path) ||
        relay_test_path_missing(unrelated_directory) ||
        recovered_store.slot_count != 2) {
        goto cleanup;
    }
    result = 0;

cleanup:
    relay_game_shutdown(&game);
    relay_script_runtime_shutdown(&runtime);
    if (relay_test_remove_tree(RELAY_TEST_SESSION_DIRECTORY)) {
#if RELAY_PLATFORM_WINDOWS
        (void)RemoveDirectoryA(RELAY_TEST_SESSION_DIRECTORY);
#else
        (void)rmdir(RELAY_TEST_SESSION_DIRECTORY);
#endif
    }
    return result;
}
