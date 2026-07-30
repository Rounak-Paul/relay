#include "relay/session.h"

#include "relay/platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if RELAY_PLATFORM_WINDOWS
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

enum {
    RELAY_SESSION_FORMAT_VERSION = 2,
    RELAY_SESSION_STATE_FORMAT_VERSION = 1,
    RELAY_SESSION_FILE_LIMIT = 16 * 1024 * 1024,
    RELAY_SESSION_NODE_LIMIT = 4096,
    RELAY_SESSION_CONNECTION_LIMIT = 16384
};

static const unsigned char relay_session_magic[8] = {
    'R', 'L', 'Y', 'S', 'L', 'O', 'T', '2'
};

static const unsigned char relay_session_state_magic[8] = {
    'R', 'L', 'Y', 'S', 'T', 'A', 'T', '1'
};

/** Growable bounded byte writer used by the versioned save codec. */
typedef struct Relay_SessionWriter {
    unsigned char *data;
    size_t size;
    size_t capacity;
    bool valid;
} Relay_SessionWriter;

/** Bounds-checked byte reader used before any candidate state is installed. */
typedef struct Relay_SessionReader {
    const unsigned char *data;
    size_t size;
    size_t offset;
    bool valid;
} Relay_SessionReader;

/** Deferred Blueprint metadata loaded before dependency-order compilation. */
typedef struct Relay_SessionBlueprintRecord {
    Relay_BlueprintId id;
    char name[RELAY_BLUEPRINT_NAME_CAPACITY];
    char source[RELAY_BLUEPRINT_SOURCE_CAPACITY];
    size_t source_size;
    char deployed_source[RELAY_BLUEPRINT_SOURCE_CAPACITY];
    size_t deployed_source_size;
    size_t cursor;
    size_t viewport_line;
    uint64_t revision;
    uint64_t compiled_revision;
    uint32_t source_checksum;
    uint32_t deployed_source_checksum;
    Relay_NodeId focused_node_id;
    bool workspace_open;
    bool editor_open;
    bool dirty;
} Relay_SessionBlueprintRecord;

static bool relay_session_node_state_valid(const Relay_Node *node,
    const Relay_ScriptStateSnapshot *script_state);
static bool relay_session_module_instances_valid(const Relay_Game *game);
static bool relay_session_blueprint_name_valid(const char *name);
static bool relay_session_game_valid(const Relay_Game *game,
    Relay_ScriptRuntime *runtime);

/** Update store and renderer-facing status without terminal output. */
static void relay_session_status(Relay_SessionStore *store, Relay_Game *game,
    const char *message)
{
    if (store != NULL) {
        (void)snprintf(store->status, sizeof(store->status), "%s", message);
    }
    if (game != NULL) {
        (void)snprintf(game->session_status, sizeof(game->session_status), "%s",
            message);
    }
}

/** Create one directory without treating an existing directory as failure. */
static bool relay_session_create_directory(const char *path)
{
#if RELAY_PLATFORM_WINDOWS
    DWORD attributes;
    int create_error;

    if (_mkdir(path) == 0) {
        return true;
    }
    create_error = errno;
    attributes = GetFileAttributesA(path);
    return create_error == EEXIST && attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat information;

    if (mkdir(path, 0700) == 0) {
        return true;
    }
    return errno == EEXIST && stat(path, &information) == 0 &&
        S_ISDIR(information.st_mode);
#endif
}

/** Return whether one filesystem path names a readable regular file. */
static bool relay_session_file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return false;
    }
    (void)fclose(file);
    return true;
}

/** Return whether one filesystem path names a directory. */
static bool relay_session_directory_exists(const char *path)
{
#if RELAY_PLATFORM_WINDOWS
    const DWORD attributes = GetFileAttributesA(path);

    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat information;

    return stat(path, &information) == 0 && S_ISDIR(information.st_mode);
#endif
}

/** Join two trusted path segments without truncating fixed storage. */
static bool relay_session_path_join(char *path, size_t capacity,
    const char *directory, const char *name)
{
    const int written = snprintf(path, capacity, "%s/%s", directory, name);

    return written >= 0 && (size_t)written < capacity;
}

/** Format one stable session identity as its canonical directory name. */
static bool relay_session_slot_name(uint64_t session_id,
    char name[RELAY_SESSION_SLOT_NAME_CAPACITY])
{
    return session_id != 0 &&
        snprintf(name, RELAY_SESSION_SLOT_NAME_CAPACITY, "%016llx",
            (unsigned long long)session_id) ==
            RELAY_SESSION_SLOT_NAME_CAPACITY - 1;
}

/** Parse one exact lowercase hexadecimal slot directory name. */
static bool relay_session_slot_id(const char *name, uint64_t *session_id)
{
    uint64_t value = 0;
    size_t index;

    if (name == NULL || session_id == NULL ||
        strlen(name) != RELAY_SESSION_SLOT_NAME_CAPACITY - 1) {
        return false;
    }
    for (index = 0; index < RELAY_SESSION_SLOT_NAME_CAPACITY - 1; index++) {
        unsigned digit;

        if (name[index] >= '0' && name[index] <= '9') {
            digit = (unsigned)(name[index] - '0');
        } else if (name[index] >= 'a' && name[index] <= 'f') {
            digit = (unsigned)(name[index] - 'a') + 10U;
        } else {
            return false;
        }
        value = (value << 4U) | digit;
    }
    if (value == 0) {
        return false;
    }
    *session_id = value;
    return true;
}

/** Recursively remove one store-owned staging, backup, or slot directory. */
static bool relay_session_remove_tree(const char *path)
{
#if RELAY_PLATFORM_WINDOWS
    WIN32_FIND_DATAA entry;
    HANDLE search;
    DWORD attributes;
    char pattern[RELAY_SESSION_PATH_CAPACITY];
    bool valid = true;

    attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();

        return error == ERROR_FILE_NOT_FOUND ||
            error == ERROR_PATH_NOT_FOUND;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return DeleteFileA(path) != 0;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return RemoveDirectoryA(path) != 0;
    }
    if (!relay_session_path_join(pattern, sizeof(pattern), path, "*")) {
        return false;
    }
    search = FindFirstFileA(pattern, &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            char child[RELAY_SESSION_PATH_CAPACITY];

            if (strcmp(entry.cFileName, ".") == 0 ||
                strcmp(entry.cFileName, "..") == 0) {
                continue;
            }
            if (!relay_session_path_join(child, sizeof(child), path,
                    entry.cFileName)) {
                valid = false;
                break;
            }
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                valid = relay_session_remove_tree(child);
            } else if ((entry.dwFileAttributes &
                    FILE_ATTRIBUTE_DIRECTORY) != 0) {
                valid = RemoveDirectoryA(child) != 0;
            } else {
                valid = DeleteFileA(child) != 0;
            }
        } while (valid && FindNextFileA(search, &entry));
        if (GetLastError() != ERROR_NO_MORE_FILES) {
            valid = false;
        }
        (void)FindClose(search);
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        valid = false;
    }
    return valid && RemoveDirectoryA(path) != 0;
#else
    struct stat root_information;
    DIR *directory;
    struct dirent *entry;
    bool valid = true;

    if (lstat(path, &root_information) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISDIR(root_information.st_mode)) {
        return unlink(path) == 0;
    }
    directory = opendir(path);
    if (directory == NULL) {
        return false;
    }
    while (valid && (entry = readdir(directory)) != NULL) {
        char child[RELAY_SESSION_PATH_CAPACITY];
        struct stat information;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!relay_session_path_join(child, sizeof(child), path,
                entry->d_name) ||
            lstat(child, &information) != 0) {
            valid = false;
        } else if (S_ISDIR(information.st_mode)) {
            valid = relay_session_remove_tree(child);
        } else {
            valid = unlink(child) == 0;
        }
    }
    if (closedir(directory) != 0) {
        valid = false;
    }
    return valid && rmdir(path) == 0;
#endif
}

/** Rename one store-owned directory without replacing an existing target. */
static bool relay_session_rename_directory(const char *source,
    const char *destination)
{
#if RELAY_PLATFORM_WINDOWS
    return MoveFileExA(source, destination, MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(source, destination) == 0;
#endif
}

/** Flush directory-entry mutations at one repository commit boundary. */
static bool relay_session_flush_directory(const char *path)
{
#if RELAY_PLATFORM_WINDOWS
    (void)path;
    return true;
#else
    const int descriptor = open(path, O_RDONLY);
    bool valid;

    if (descriptor < 0) {
        return false;
    }
    valid = fsync(descriptor) == 0;
    if (close(descriptor) != 0) {
        valid = false;
    }
    return valid;
#endif
}

/** Reserve bounded writer storage without integer overflow. */
static bool relay_session_writer_reserve(Relay_SessionWriter *writer,
    size_t amount)
{
    unsigned char *data;
    size_t capacity;

    if (!writer->valid || amount > RELAY_SESSION_FILE_LIMIT ||
        writer->size > RELAY_SESSION_FILE_LIMIT - amount) {
        writer->valid = false;
        return false;
    }
    if (writer->size + amount <= writer->capacity) {
        return true;
    }
    capacity = writer->capacity == 0 ? 4096 : writer->capacity;
    while (capacity < writer->size + amount) {
        if (capacity > RELAY_SESSION_FILE_LIMIT / 2) {
            capacity = RELAY_SESSION_FILE_LIMIT;
            break;
        }
        capacity *= 2;
    }
    if (capacity < writer->size + amount) {
        writer->valid = false;
        return false;
    }
    data = realloc(writer->data, capacity);
    if (data == NULL) {
        writer->valid = false;
        return false;
    }
    writer->data = data;
    writer->capacity = capacity;
    return true;
}

/** Append raw bytes to a bounded save payload. */
static void relay_session_write_bytes(Relay_SessionWriter *writer,
    const void *data, size_t size)
{
    if (!relay_session_writer_reserve(writer, size)) {
        return;
    }
    (void)memcpy(&writer->data[writer->size], data, size);
    writer->size += size;
}

/** Append one byte to a save payload. */
static void relay_session_write_u8(Relay_SessionWriter *writer, uint8_t value)
{
    relay_session_write_bytes(writer, &value, sizeof(value));
}

/** Append one little-endian 32-bit integer. */
static void relay_session_write_u32(Relay_SessionWriter *writer,
    uint32_t value)
{
    unsigned char bytes[4];
    size_t index;

    for (index = 0; index < sizeof(bytes); index++) {
        bytes[index] = (unsigned char)(value >> (index * 8));
    }
    relay_session_write_bytes(writer, bytes, sizeof(bytes));
}

/** Append one little-endian 64-bit integer. */
static void relay_session_write_u64(Relay_SessionWriter *writer,
    uint64_t value)
{
    unsigned char bytes[8];
    size_t index;

    for (index = 0; index < sizeof(bytes); index++) {
        bytes[index] = (unsigned char)(value >> (index * 8));
    }
    relay_session_write_bytes(writer, bytes, sizeof(bytes));
}

/** Append one signed integer through its exact two's-complement bits. */
static void relay_session_write_i64(Relay_SessionWriter *writer, int64_t value)
{
    uint64_t bits;

    (void)memcpy(&bits, &value, sizeof(bits));
    relay_session_write_u64(writer, bits);
}

/** Append a length-prefixed bounded string. */
static void relay_session_write_string(Relay_SessionWriter *writer,
    const char *value, size_t capacity)
{
    const char *end = memchr(value, '\0', capacity);
    const size_t size = end == NULL ? capacity : (size_t)(end - value);

    if (end == NULL || size > UINT32_MAX) {
        writer->valid = false;
        return;
    }
    relay_session_write_u32(writer, (uint32_t)size);
    relay_session_write_bytes(writer, value, size);
}

/** Consume raw bytes only when the payload contains the complete range. */
static bool relay_session_read_bytes(Relay_SessionReader *reader, void *data,
    size_t size)
{
    if (!reader->valid || size > reader->size ||
        reader->offset > reader->size - size) {
        reader->valid = false;
        return false;
    }
    if (data != NULL) {
        (void)memcpy(data, &reader->data[reader->offset], size);
    }
    reader->offset += size;
    return true;
}

/** Read one byte. */
static uint8_t relay_session_read_u8(Relay_SessionReader *reader)
{
    uint8_t value = 0;

    (void)relay_session_read_bytes(reader, &value, sizeof(value));
    return value;
}

/** Read one little-endian 32-bit integer. */
static uint32_t relay_session_read_u32(Relay_SessionReader *reader)
{
    unsigned char bytes[4] = {0};
    uint32_t value = 0;
    size_t index;

    if (!relay_session_read_bytes(reader, bytes, sizeof(bytes))) {
        return 0;
    }
    for (index = 0; index < sizeof(bytes); index++) {
        value |= (uint32_t)bytes[index] << (index * 8);
    }
    return value;
}

/** Read one little-endian 64-bit integer. */
static uint64_t relay_session_read_u64(Relay_SessionReader *reader)
{
    unsigned char bytes[8] = {0};
    uint64_t value = 0;
    size_t index;

    if (!relay_session_read_bytes(reader, bytes, sizeof(bytes))) {
        return 0;
    }
    for (index = 0; index < sizeof(bytes); index++) {
        value |= (uint64_t)bytes[index] << (index * 8);
    }
    return value;
}

/** Read one signed integer through its exact stored bits. */
static int64_t relay_session_read_i64(Relay_SessionReader *reader)
{
    const uint64_t bits = relay_session_read_u64(reader);
    int64_t value;

    (void)memcpy(&value, &bits, sizeof(value));
    return value;
}

/** Read one length-prefixed string into fixed project-owned storage. */
static bool relay_session_read_string(Relay_SessionReader *reader,
    char *value, size_t capacity)
{
    const uint32_t size = relay_session_read_u32(reader);

    if (!reader->valid || capacity == 0 || size >= capacity ||
        !relay_session_read_bytes(reader, value, size)) {
        reader->valid = false;
        return false;
    }
    value[size] = '\0';
    return true;
}

/** Compute a deterministic payload checksum. */
static uint32_t relay_session_checksum(const unsigned char *data, size_t size)
{
    uint32_t hash = UINT32_C(2166136261);
    size_t index;

    for (index = 0; index < size; index++) {
        hash ^= data[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

/** Flush one complete script source into a new staging directory. */
static bool relay_session_write_source_file(const char *path,
    const char *source, size_t size)
{
    FILE *file;
    bool valid;

    if (path == NULL || source == NULL || size == 0 ||
        size >= RELAY_BLUEPRINT_SOURCE_CAPACITY) {
        return false;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    valid = fwrite(source, 1, size, file) == size && fflush(file) == 0;
#if RELAY_PLATFORM_WINDOWS
    if (valid) {
        valid = _commit(_fileno(file)) == 0;
    }
#else
    if (valid) {
        valid = fsync(fileno(file)) == 0;
    }
#endif
    if (fclose(file) != 0) {
        valid = false;
    }
    return valid;
}

/** Read one exact checksummed Blueprint source from a slot directory. */
static bool relay_session_read_source_file(const char *path, char *source,
    size_t capacity, size_t expected_size, uint32_t expected_checksum)
{
    FILE *file;
    int trailing;

    if (source == NULL || capacity == 0 || expected_size == 0 ||
        expected_size >= capacity) {
        return false;
    }
    file = fopen(path, "rb");
    if (file == NULL ||
        fread(source, 1, expected_size, file) != expected_size) {
        if (file != NULL) {
            (void)fclose(file);
        }
        return false;
    }
    trailing = fgetc(file);
    if (fclose(file) != 0 || trailing != EOF ||
        relay_session_checksum((const unsigned char *)source,
            expected_size) != expected_checksum) {
        return false;
    }
    source[expected_size] = '\0';
    return true;
}

/** Serialize one FIFO in logical order. */
static void relay_session_write_queue(Relay_SessionWriter *writer,
    const Relay_ItemQueue *queue)
{
    size_t index;

    relay_session_write_u32(writer, (uint32_t)queue->count);
    for (index = 0; index < queue->count; index++) {
        const Relay_Item item = queue->items[
            (queue->head + index) % RELAY_ITEM_QUEUE_CAPACITY];

        relay_session_write_u64(writer, item.id);
        relay_session_write_u32(writer, (uint32_t)item.type);
    }
}

/** Deserialize one FIFO while enforcing its fixed capacity. */
static bool relay_session_read_queue(Relay_SessionReader *reader,
    Relay_ItemQueue *queue)
{
    const uint32_t count = relay_session_read_u32(reader);
    uint32_t index;

    *queue = (Relay_ItemQueue){0};
    if (!reader->valid || count > RELAY_ITEM_QUEUE_CAPACITY) {
        return false;
    }
    for (index = 0; index < count; index++) {
        Relay_Item item;

        item.id = relay_session_read_u64(reader);
        item.type = (Relay_NodePortType)relay_session_read_u32(reader);
        if (!reader->valid || !relay_item_queue_push(queue, item)) {
            return false;
        }
    }
    return true;
}

/** Serialize one deterministic Lua-independent persistent-state snapshot. */
static bool relay_session_write_script_state(Relay_SessionWriter *writer,
    Relay_ScriptRuntime *runtime, const Relay_ScriptInstanceState *instance)
{
    Relay_ScriptStateSnapshot snapshot;
    size_t index;

    if (!relay_script_instance_export(runtime, instance, &snapshot)) {
        return false;
    }
    relay_session_write_u8(writer, snapshot.initialized ? 1U : 0U);
    relay_session_write_u32(writer, (uint32_t)snapshot.count);
    for (index = 0; index < snapshot.count; index++) {
        const Relay_ScriptStateEntry *entry = &snapshot.entries[index];

        relay_session_write_string(writer, entry->key, sizeof(entry->key));
        relay_session_write_u32(writer, (uint32_t)entry->type);
        if (entry->type == RELAY_SCRIPT_STATE_STRING) {
            relay_session_write_string(writer, entry->string,
                sizeof(entry->string));
        } else {
            relay_session_write_i64(writer, entry->integer);
        }
    }
    return writer->valid;
}

/** Deserialize one bounded persistent-state snapshot. */
static bool relay_session_read_script_state(Relay_SessionReader *reader,
    Relay_ScriptStateSnapshot *snapshot)
{
    const uint8_t initialized = relay_session_read_u8(reader);
    const uint32_t count = relay_session_read_u32(reader);
    uint32_t index;

    *snapshot = (Relay_ScriptStateSnapshot){0};
    if (!reader->valid || initialized > 1 ||
        count > RELAY_SCRIPT_STATE_ENTRY_LIMIT) {
        return false;
    }
    snapshot->initialized = initialized != 0;
    snapshot->count = count;
    for (index = 0; index < count; index++) {
        Relay_ScriptStateEntry *entry = &snapshot->entries[index];

        if (!relay_session_read_string(reader, entry->key,
                sizeof(entry->key))) {
            return false;
        }
        entry->type =
            (Relay_ScriptStateValueType)relay_session_read_u32(reader);
        if (entry->type == RELAY_SCRIPT_STATE_STRING) {
            if (!relay_session_read_string(reader, entry->string,
                    sizeof(entry->string))) {
                return false;
            }
        } else if (entry->type == RELAY_SCRIPT_STATE_BOOLEAN ||
                entry->type == RELAY_SCRIPT_STATE_INTEGER) {
            entry->integer = relay_session_read_i64(reader);
        } else {
            return false;
        }
    }
    return reader->valid;
}

/** Serialize one complete runtime node without process-local pointers. */
static bool relay_session_write_node(Relay_SessionWriter *writer,
    Relay_ScriptRuntime *runtime, const Relay_Node *node)
{
    size_t index;

    relay_session_write_u64(writer, node->id);
    relay_session_write_u32(writer, node->definition_id);
    relay_session_write_u32(writer, (uint32_t)node->runtime_kind);
    relay_session_write_i64(writer, node->grid_x);
    relay_session_write_i64(writer, node->grid_y);
    relay_session_write_i64(writer, node->progress);
    relay_session_write_i64(writer, node->produced);
    relay_session_write_i64(writer, node->timer_interval_steps);
    relay_session_write_i64(writer, node->timer_elapsed_steps);
    for (index = 0; index < RELAY_NODE_MAX_PORTS; index++) {
        relay_session_write_i64(writer, node->output_values[index]);
        relay_session_write_i64(writer, node->previous_output_values[index]);
        relay_session_write_i64(writer, node->process_input_values[index]);
        relay_session_write_queue(writer, &node->input_queues[index]);
        relay_session_write_queue(writer, &node->output_queues[index]);
    }
    relay_session_write_u64(writer, node->blueprint_id);
    relay_session_write_u64(writer, node->origin_blueprint_id);
    relay_session_write_u64(writer, node->origin_node_id);
    relay_session_write_u64(writer, node->module_instance_id);
    relay_session_write_string(writer, node->local_key,
        sizeof(node->local_key));
    relay_session_write_u64(writer, node->process_activations);
    relay_session_write_u8(writer, node->enabled ? 1U : 0U);
    return relay_session_write_script_state(writer, runtime,
        &node->script_state);
}

/** Serialize the complete root runtime graph. */
static bool relay_session_write_world(Relay_SessionWriter *writer,
    Relay_ScriptRuntime *runtime, const Relay_NodeWorld *world)
{
    size_t index;

    if (world->count > UINT32_MAX || world->connection_count > UINT32_MAX ||
        world->module_input_binding_count > UINT32_MAX ||
        world->module_output_binding_count > UINT32_MAX ||
        !relay_node_world_items_valid(world)) {
        return false;
    }
    relay_session_write_u64(writer, world->next_id);
    relay_session_write_u64(writer, world->next_item_id);
    relay_session_write_u32(writer, (uint32_t)world->count);
    for (index = 0; index < world->count; index++) {
        if (!relay_session_write_node(writer, runtime, &world->nodes[index])) {
            return false;
        }
    }
    relay_session_write_u32(writer, (uint32_t)world->connection_count);
    for (index = 0; index < world->connection_count; index++) {
        const Relay_NodeConnection *connection = &world->connections[index];

        relay_session_write_u64(writer, connection->source_node_id);
        relay_session_write_u32(writer,
            (uint32_t)connection->source_port_index);
        relay_session_write_u64(writer, connection->destination_node_id);
        relay_session_write_u32(writer,
            (uint32_t)connection->destination_port_index);
    }
    relay_session_write_u32(writer,
        (uint32_t)world->module_input_binding_count);
    for (index = 0; index < world->module_input_binding_count; index++) {
        const Relay_NodeModuleInputBinding *binding =
            &world->module_input_bindings[index];

        relay_session_write_u64(writer, binding->module_node_id);
        relay_session_write_u32(writer, (uint32_t)binding->module_port_index);
        relay_session_write_u64(writer, binding->destination_node_id);
        relay_session_write_u32(writer,
            (uint32_t)binding->destination_port_index);
    }
    relay_session_write_u32(writer,
        (uint32_t)world->module_output_binding_count);
    for (index = 0; index < world->module_output_binding_count; index++) {
        const Relay_NodeModuleOutputBinding *binding =
            &world->module_output_bindings[index];

        relay_session_write_u64(writer, binding->module_node_id);
        relay_session_write_u32(writer, (uint32_t)binding->module_port_index);
        relay_session_write_u64(writer, binding->source_node_id);
        relay_session_write_u32(writer, (uint32_t)binding->source_port_index);
        relay_session_write_u32(writer,
            (uint32_t)binding->source_module_input_port_index);
        relay_session_write_u8(writer,
            binding->source_is_module_input ? 1U : 0U);
    }
    return writer->valid;
}

/** Serialize gameplay, Blueprint sources, and the root runtime graph. */
static bool relay_session_encode(Relay_SessionWriter *writer,
    Relay_Game *game, Relay_ScriptRuntime *runtime, uint64_t session_id,
    uint64_t save_revision, uint64_t saved_at_unix_seconds)
{
    size_t index;

    if (!relay_session_game_valid(game, runtime)) {
        return false;
    }
    writer->valid = true;
    relay_session_write_u64(writer, session_id);
    relay_session_write_u64(writer, save_revision);
    relay_session_write_u64(writer, game->simulation_step);
    relay_session_write_u64(writer, saved_at_unix_seconds);
    relay_session_write_u64(writer, game->currency);
    relay_session_write_u32(writer, (uint32_t)game->active_tab);
    relay_session_write_u32(writer, (uint32_t)game->workspace_mode);
    relay_session_write_u32(writer, (uint32_t)game->selected_offer);
    relay_session_write_u32(writer, (uint32_t)game->selected_blueprint);
    relay_session_write_u64(writer, game->focused_node_id);
    relay_session_write_u64(writer, game->root_focused_node_id);
    relay_session_write_u32(writer, (uint32_t)game->active_workspace);
    relay_session_write_u64(writer, game->editing_blueprint_id);
    relay_session_write_u64(writer, game->blueprints.next_id);
    relay_session_write_u32(writer, (uint32_t)game->blueprints.count);
    for (index = 0; index < game->blueprints.count; index++) {
        const Relay_Blueprint *blueprint = &game->blueprints.blueprints[index];

        relay_session_write_u64(writer, blueprint->id);
        relay_session_write_string(writer, blueprint->name,
            sizeof(blueprint->name));
        relay_session_write_u32(writer, (uint32_t)blueprint->source_size);
        relay_session_write_u32(writer,
            relay_session_checksum(
                (const unsigned char *)blueprint->source,
                blueprint->source_size));
        relay_session_write_u64(writer, blueprint->revision);
        relay_session_write_u32(writer,
            (uint32_t)blueprint->deployed_source_size);
        relay_session_write_u32(writer,
            relay_session_checksum(
                (const unsigned char *)blueprint->deployed_source,
                blueprint->deployed_source_size));
        relay_session_write_u64(writer, blueprint->compiled_revision);
        relay_session_write_u8(writer, blueprint->dirty ? 1U : 0U);
        relay_session_write_u32(writer, (uint32_t)blueprint->cursor);
        relay_session_write_u32(writer, (uint32_t)blueprint->viewport_line);
        relay_session_write_u64(writer, blueprint->focused_node_id);
        relay_session_write_u8(writer,
            blueprint->workspace_open ? 1U : 0U);
        relay_session_write_u8(writer, blueprint->editor_open ? 1U : 0U);
    }
    return writer->valid &&
        relay_session_write_world(writer, runtime, &game->nodes);
}

/** Return a dynamic definition owned by one loaded Blueprint when applicable. */
static const Relay_NodeDefinition *relay_session_node_definition(
    Relay_Game *game, Relay_NodeDefinitionId definition_id,
    Relay_NodeRuntimeKind runtime_kind, Relay_BlueprintId blueprint_id)
{
    Relay_Blueprint *blueprint;

    if (runtime_kind == RELAY_NODE_RUNTIME_ATOMIC) {
        return relay_node_definition_find(definition_id);
    }
    blueprint = relay_blueprint_library_find(&game->blueprints, blueprint_id);
    if (blueprint == NULL) {
        return NULL;
    }
    if (runtime_kind == RELAY_NODE_RUNTIME_BLUEPRINT_WRAPPER &&
        blueprint->definition.id == definition_id) {
        return &blueprint->definition;
    }
    if (runtime_kind == RELAY_NODE_RUNTIME_BLUEPRINT_PROCESS &&
        blueprint->process_definition.id == definition_id) {
        return &blueprint->process_definition;
    }
    return NULL;
}

/** Return whether one saved Timer interval belongs to its fixed data contract. */
static bool relay_session_timer_interval_valid(int64_t interval)
{
    size_t index;

    for (index = 0; index < relay_timer_interval_count(); index++) {
        if (relay_timer_interval_at(index) == interval) {
            return true;
        }
    }
    return false;
}

/** Validate saved node state against its immutable executable definition. */
static bool relay_session_node_state_valid(const Relay_Node *node,
    const Relay_ScriptStateSnapshot *script_state)
{
    const Relay_NodeDefinition *definition = relay_node_definition_for(node);
    size_t index;

    if (definition == NULL || node->produced < 0 || node->progress < 0 ||
        node->timer_interval_steps < 0 || node->timer_elapsed_steps < 0) {
        return false;
    }
    if (definition->simulation.behavior == RELAY_NODE_BEHAVIOR_TIMER) {
        if (node->progress != 0 ||
            !relay_session_timer_interval_valid(node->timer_interval_steps) ||
            node->timer_elapsed_steps >= node->timer_interval_steps) {
            return false;
        }
    } else if (definition->simulation.behavior ==
            RELAY_NODE_BEHAVIOR_FIXED_RATE_SOURCE) {
        if ((uint64_t)node->progress >
                definition->simulation.interval_steps ||
            node->timer_interval_steps != 0 ||
            node->timer_elapsed_steps != 0) {
            return false;
        }
    } else if (node->progress != 0 || node->produced != 0 ||
            node->timer_interval_steps != 0 ||
            node->timer_elapsed_steps != 0) {
        return false;
    }
    if (node->runtime_kind == RELAY_NODE_RUNTIME_ATOMIC) {
        if (node->blueprint_id != 0 || node->process_activations != 0 ||
            script_state->initialized ||
            (node->module_instance_id == 0 ?
                (node->origin_blueprint_id != 0 ||
                    node->origin_node_id != 0) :
                (node->origin_blueprint_id == 0 ||
                    node->origin_node_id == 0))) {
            return false;
        }
    } else if (node->runtime_kind ==
            RELAY_NODE_RUNTIME_BLUEPRINT_WRAPPER) {
        if (node->blueprint_id == 0 || node->module_instance_id != 0 ||
            node->origin_blueprint_id != 0 || node->origin_node_id != 0 ||
            node->process_activations != 0 || script_state->initialized) {
            return false;
        }
    } else if (node->runtime_kind ==
            RELAY_NODE_RUNTIME_BLUEPRINT_PROCESS) {
        if (node->blueprint_id == 0 || node->origin_blueprint_id == 0 ||
            node->module_instance_id == 0) {
            return false;
        }
    } else {
        return false;
    }
    for (index = 0; index < RELAY_NODE_MAX_PORTS; index++) {
        const bool input_exists = index < definition->input_count;
        const bool output_exists = index < definition->output_count;
        const Relay_NodePortType input_type = input_exists ?
            definition->inputs[index].type : RELAY_NODE_PORT_TYPE_INVALID;
        const Relay_NodePortType output_type = output_exists ?
            definition->outputs[index].type : RELAY_NODE_PORT_TYPE_INVALID;

        if ((!output_exists && (node->output_values[index] != 0 ||
                node->previous_output_values[index] != 0)) ||
            (!input_exists && node->process_input_values[index] != 0) ||
            (relay_node_port_type_is_item(output_type) &&
                (node->output_values[index] != 0 ||
                    node->previous_output_values[index] != 0)) ||
            (relay_node_port_type_is_item(input_type) &&
                node->process_input_values[index] != 0) ||
            (output_type == RELAY_NODE_PORT_TYPE_BOOLEAN &&
                ((node->output_values[index] != 0 &&
                    node->output_values[index] != 1) ||
                    (node->previous_output_values[index] != 0 &&
                        node->previous_output_values[index] != 1))) ||
            (input_type == RELAY_NODE_PORT_TYPE_BOOLEAN &&
                node->process_input_values[index] != 0 &&
                node->process_input_values[index] != 1)) {
            return false;
        }
    }
    return true;
}

/** Read and install one node after its dynamic definition is available. */
static bool relay_session_read_node(Relay_SessionReader *reader,
    Relay_Game *game, Relay_NodeWorld *world, Relay_ScriptRuntime *runtime)
{
    const Relay_NodeId id = relay_session_read_u64(reader);
    const Relay_NodeDefinitionId definition_id =
        relay_session_read_u32(reader);
    const Relay_NodeRuntimeKind runtime_kind =
        (Relay_NodeRuntimeKind)relay_session_read_u32(reader);
    const int64_t grid_x = relay_session_read_i64(reader);
    const int64_t grid_y = relay_session_read_i64(reader);
    const int64_t progress = relay_session_read_i64(reader);
    const int64_t produced = relay_session_read_i64(reader);
    const int64_t timer_interval_steps = relay_session_read_i64(reader);
    const int64_t timer_elapsed_steps = relay_session_read_i64(reader);
    int64_t output_values[RELAY_NODE_MAX_PORTS];
    int64_t previous_output_values[RELAY_NODE_MAX_PORTS];
    int64_t process_input_values[RELAY_NODE_MAX_PORTS];
    Relay_ItemQueue input_queues[RELAY_NODE_MAX_PORTS];
    Relay_ItemQueue output_queues[RELAY_NODE_MAX_PORTS];
    Relay_BlueprintId blueprint_id;
    Relay_BlueprintId origin_blueprint_id;
    Relay_NodeId origin_node_id;
    Relay_NodeId module_instance_id;
    char local_key[RELAY_NODE_LOCAL_KEY_CAPACITY];
    uint64_t process_activations;
    uint8_t enabled;
    Relay_ScriptStateSnapshot script_state;
    const Relay_NodeDefinition *definition;
    Relay_NodeId created_id;
    Relay_Node *node;
    size_t index;

    if (!reader->valid || id == 0 ||
        runtime_kind > RELAY_NODE_RUNTIME_BLUEPRINT_OUTPUT_BOUNDARY) {
        return false;
    }
    for (index = 0; index < RELAY_NODE_MAX_PORTS; index++) {
        output_values[index] = relay_session_read_i64(reader);
        previous_output_values[index] = relay_session_read_i64(reader);
        process_input_values[index] = relay_session_read_i64(reader);
        if (!relay_session_read_queue(reader, &input_queues[index]) ||
            !relay_session_read_queue(reader, &output_queues[index])) {
            return false;
        }
    }
    blueprint_id = relay_session_read_u64(reader);
    origin_blueprint_id = relay_session_read_u64(reader);
    origin_node_id = relay_session_read_u64(reader);
    module_instance_id = relay_session_read_u64(reader);
    if (!relay_session_read_string(reader, local_key, sizeof(local_key))) {
        return false;
    }
    process_activations = relay_session_read_u64(reader);
    enabled = relay_session_read_u8(reader);
    if (enabled > 1 ||
        !relay_session_read_script_state(reader, &script_state)) {
        return false;
    }
    definition = relay_session_node_definition(game, definition_id,
        runtime_kind, blueprint_id);
    if (definition == NULL ||
        (runtime_kind != RELAY_NODE_RUNTIME_BLUEPRINT_PROCESS &&
            script_state.initialized)) {
        return false;
    }
    created_id = relay_node_world_create_definition(world, definition,
        grid_x, grid_y);
    if (created_id != id) {
        return false;
    }
    node = relay_node_world_find(world, created_id);
    node->runtime_kind = runtime_kind;
    node->progress = progress;
    node->produced = produced;
    node->timer_interval_steps = timer_interval_steps;
    node->timer_elapsed_steps = timer_elapsed_steps;
    (void)memcpy(node->output_values, output_values, sizeof(output_values));
    (void)memcpy(node->previous_output_values, previous_output_values,
        sizeof(previous_output_values));
    (void)memcpy(node->process_input_values, process_input_values,
        sizeof(process_input_values));
    (void)memcpy(node->input_queues, input_queues, sizeof(input_queues));
    (void)memcpy(node->output_queues, output_queues, sizeof(output_queues));
    node->blueprint_id = blueprint_id;
    node->origin_blueprint_id = origin_blueprint_id;
    node->origin_node_id = origin_node_id;
    node->module_instance_id = module_instance_id;
    (void)snprintf(node->local_key, sizeof(node->local_key), "%s", local_key);
    node->process_activations = process_activations;
    node->enabled = enabled != 0;
    if (!relay_session_node_state_valid(node, &script_state)) {
        return false;
    }
    return relay_script_instance_import(runtime, &node->script_state,
        &script_state);
}

/** Validate every flattened runtime node against its compiled module plan. */
static bool relay_session_module_instances_valid(const Relay_Game *game)
{
    size_t wrapper_index;

    for (wrapper_index = 0; wrapper_index < game->nodes.count;
            wrapper_index++) {
        const Relay_Node *wrapper = &game->nodes.nodes[wrapper_index];
        const Relay_Blueprint *blueprint;
        bool *matched;
        size_t internal_count = 0;
        size_t node_index;
        size_t plan_index;

        if (wrapper->runtime_kind !=
                RELAY_NODE_RUNTIME_BLUEPRINT_WRAPPER) {
            if (wrapper->module_instance_id == 0 &&
                wrapper->runtime_kind != RELAY_NODE_RUNTIME_ATOMIC) {
                return false;
            }
            continue;
        }
        blueprint = relay_blueprint_library_find_const(&game->blueprints,
            wrapper->blueprint_id);
        if (wrapper->module_instance_id != 0 || blueprint == NULL ||
            !blueprint->plan.valid) {
            return false;
        }
        matched = blueprint->plan.node_count == 0 ? NULL :
            calloc(blueprint->plan.node_count, sizeof(*matched));
        if (matched == NULL && blueprint->plan.node_count > 0) {
            return false;
        }
        for (node_index = 0; node_index < game->nodes.count; node_index++) {
            const Relay_Node *node = &game->nodes.nodes[node_index];
            bool found = false;

            if (node->module_instance_id != wrapper->id) {
                continue;
            }
            internal_count++;
            for (plan_index = 0;
                    plan_index < blueprint->plan.node_count; plan_index++) {
                const Relay_BlueprintPlanNode *plan_node =
                    &blueprint->plan.nodes[plan_index];

                if (!matched[plan_index] &&
                    node->definition_id == plan_node->definition->id &&
                    node->runtime_kind == plan_node->runtime_kind &&
                    node->blueprint_id ==
                        plan_node->script_blueprint_id &&
                    node->origin_blueprint_id ==
                        plan_node->origin_blueprint_id &&
                    node->origin_node_id == plan_node->origin_node_id) {
                    matched[plan_index] = true;
                    found = true;
                    break;
                }
            }
            if (!found) {
                free(matched);
                return false;
            }
        }
        if (internal_count != blueprint->plan.node_count) {
            free(matched);
            return false;
        }
        for (plan_index = 0; plan_index < blueprint->plan.node_count;
                plan_index++) {
            if (!matched[plan_index]) {
                free(matched);
                return false;
            }
        }
        free(matched);
    }
    return true;
}

/** Validate all authoritative state before it may enter a durable save. */
static bool relay_session_game_valid(const Relay_Game *game,
    Relay_ScriptRuntime *runtime)
{
    size_t editor_count = 0;
    size_t index;

    if (game == NULL || runtime == NULL || game->script_runtime != runtime ||
        game->session_id == 0 ||
        game->active_tab > RELAY_GAME_PANEL_TAB_BLUEPRINTS ||
        game->workspace_mode > RELAY_GAME_WORKSPACE_MAP ||
        game->selected_offer >= relay_game_shop_offer_count() ||
        game->blueprints.count > RELAY_BLUEPRINT_CAPACITY ||
        game->blueprints.next_id == 0 ||
        game->blueprints.next_id <= game->blueprints.count ||
        game->active_workspace > game->blueprints.count ||
        (game->blueprints.count == 0 && game->selected_blueprint != 0) ||
        (game->blueprints.count > 0 &&
            game->selected_blueprint >= game->blueprints.count) ||
        (game->root_focused_node_id != 0 &&
            relay_node_world_find_const(&game->nodes,
                game->root_focused_node_id) == NULL) ||
        !relay_node_world_items_valid(&game->nodes) ||
        !relay_session_module_instances_valid(game)) {
        return false;
    }
    for (index = 0; index < game->blueprints.count; index++) {
        const Relay_Blueprint *blueprint = &game->blueprints.blueprints[index];

        if (blueprint->id != index + 1 ||
            !relay_session_blueprint_name_valid(blueprint->name) ||
            blueprint->source_size == 0 ||
            blueprint->source_size >= sizeof(blueprint->source) ||
            blueprint->source[blueprint->source_size] != '\0' ||
            blueprint->deployed_source_size == 0 ||
            blueprint->deployed_source_size >=
                sizeof(blueprint->deployed_source) ||
            blueprint->deployed_source[
                blueprint->deployed_source_size] != '\0' ||
            blueprint->revision == 0 || blueprint->compiled_revision == 0 ||
            blueprint->compiled_revision > blueprint->revision ||
            blueprint->cursor > blueprint->source_size ||
            !blueprint->artifact.installed || !blueprint->plan.valid ||
            blueprint->plan.revision != blueprint->compiled_revision ||
            (blueprint->focused_node_id != 0 &&
                relay_node_world_find_const(&blueprint->scene,
                    blueprint->focused_node_id) == NULL) ||
            !relay_node_world_items_valid(&blueprint->scene) ||
            (!blueprint->dirty &&
                (blueprint->revision != blueprint->compiled_revision ||
                    blueprint->source_size !=
                        blueprint->deployed_source_size ||
                    memcmp(blueprint->source, blueprint->deployed_source,
                        blueprint->source_size) != 0)) ||
            (blueprint->dirty &&
                blueprint->revision == blueprint->compiled_revision)) {
            return false;
        }
        if (blueprint->editor_open) {
            editor_count++;
            if (game->editing_blueprint_id != blueprint->id) {
                return false;
            }
        }
    }
    if ((game->editing_blueprint_id == 0 && editor_count != 0) ||
        (game->editing_blueprint_id != 0 && editor_count != 1)) {
        return false;
    }
    if (game->active_workspace == 0) {
        if (game->focused_node_id != game->root_focused_node_id) {
            return false;
        }
    } else {
        const Relay_Blueprint *active =
            &game->blueprints.blueprints[game->active_workspace - 1];

        if (!active->workspace_open ||
            game->focused_node_id != active->focused_node_id) {
            return false;
        }
    }
    for (index = 0; index < game->nodes.count; index++) {
        const Relay_Node *node = &game->nodes.nodes[index];
        Relay_ScriptStateSnapshot snapshot;

        if (!relay_script_instance_export(runtime, &node->script_state,
                &snapshot) ||
            !relay_session_node_state_valid(node, &snapshot)) {
            return false;
        }
    }
    return true;
}

/** Deserialize and validate the root runtime graph. */
static bool relay_session_read_world(Relay_SessionReader *reader,
    Relay_Game *game, Relay_ScriptRuntime *runtime)
{
    Relay_NodeWorld *world = &game->nodes;
    const Relay_NodeId next_id = relay_session_read_u64(reader);
    const Relay_ItemId next_item_id = relay_session_read_u64(reader);
    const uint32_t node_count = relay_session_read_u32(reader);
    uint32_t count;
    uint32_t index;

    if (!reader->valid || next_id == 0 || next_item_id == 0 ||
        node_count > RELAY_SESSION_NODE_LIMIT) {
        return false;
    }
    relay_node_world_shutdown(world);
    if (!relay_node_world_init(world)) {
        return false;
    }
    for (index = 0; index < node_count; index++) {
        if (!relay_session_read_node(reader, game, world, runtime)) {
            return false;
        }
    }
    if (next_id < world->next_id) {
        return false;
    }
    world->next_id = next_id;
    world->next_item_id = next_item_id;
    count = relay_session_read_u32(reader);
    if (!reader->valid || count > RELAY_SESSION_CONNECTION_LIMIT) {
        return false;
    }
    for (index = 0; index < count; index++) {
        const Relay_NodeId source = relay_session_read_u64(reader);
        const size_t source_port = relay_session_read_u32(reader);
        const Relay_NodeId destination = relay_session_read_u64(reader);
        const size_t destination_port = relay_session_read_u32(reader);

        if (!reader->valid || !relay_node_world_connect(world, source,
                source_port, destination, destination_port)) {
            return false;
        }
    }
    count = relay_session_read_u32(reader);
    if (!reader->valid || count > RELAY_SESSION_CONNECTION_LIMIT) {
        return false;
    }
    for (index = 0; index < count; index++) {
        Relay_NodeModuleInputBinding binding;

        binding.module_node_id = relay_session_read_u64(reader);
        binding.module_port_index = relay_session_read_u32(reader);
        binding.destination_node_id = relay_session_read_u64(reader);
        binding.destination_port_index = relay_session_read_u32(reader);
        if (!reader->valid ||
            !relay_node_world_bind_module_input(world, binding)) {
            return false;
        }
    }
    count = relay_session_read_u32(reader);
    if (!reader->valid || count > RELAY_SESSION_CONNECTION_LIMIT) {
        return false;
    }
    for (index = 0; index < count; index++) {
        Relay_NodeModuleOutputBinding binding;
        uint8_t source_is_input;

        binding.module_node_id = relay_session_read_u64(reader);
        binding.module_port_index = relay_session_read_u32(reader);
        binding.source_node_id = relay_session_read_u64(reader);
        binding.source_port_index = relay_session_read_u32(reader);
        binding.source_module_input_port_index =
            relay_session_read_u32(reader);
        source_is_input = relay_session_read_u8(reader);
        if (source_is_input > 1) {
            return false;
        }
        binding.source_is_module_input = source_is_input != 0;
        if (!reader->valid ||
            !relay_node_world_bind_module_output(world, binding)) {
            return false;
        }
    }
    if (!relay_node_world_items_valid(world) ||
        !relay_session_module_instances_valid(game)) {
        return false;
    }
    for (index = 0; index < world->count; index++) {
        Relay_Node *node = &world->nodes[index];

        if (node->module_instance_id != 0) {
            const Relay_Node *module = relay_node_world_find_const(world,
                node->module_instance_id);

            if (module == NULL ||
                module->runtime_kind !=
                    RELAY_NODE_RUNTIME_BLUEPRINT_WRAPPER) {
                return false;
            }
        }
        if (node->runtime_kind == RELAY_NODE_RUNTIME_BLUEPRINT_WRAPPER) {
            Relay_Blueprint *blueprint = relay_blueprint_library_find(
                &game->blueprints, node->blueprint_id);

            if (blueprint == NULL) {
                return false;
            }
            blueprint->instance_count++;
        }
    }
    return true;
}

/** Validate one canonical saved Blueprint name. */
static bool relay_session_blueprint_name_valid(const char *name)
{
    size_t index;
    bool underscore = false;

    if (name[0] < 'a' || name[0] > 'z') {
        return false;
    }
    for (index = 1; name[index] != '\0'; index++) {
        if (name[index] == '_') {
            if (underscore || name[index + 1] == '\0') {
                return false;
            }
            underscore = true;
        } else {
            if (!((name[index] >= 'a' && name[index] <= 'z') ||
                    (name[index] >= '0' && name[index] <= '9'))) {
                return false;
            }
            underscore = false;
        }
    }
    return true;
}

/** Create every Blueprint identity, then compile sources in dependency order. */
static bool relay_session_read_blueprints(Relay_SessionReader *reader,
    Relay_Game *game, const char *slot_directory)
{
    const Relay_BlueprintId next_id = relay_session_read_u64(reader);
    const uint32_t count = relay_session_read_u32(reader);
    Relay_SessionBlueprintRecord *records;
    bool compiled[RELAY_BLUEPRINT_CAPACITY] = {false};
    size_t compiled_count = 0;
    size_t index;
    size_t pass;

    if (!reader->valid || next_id == 0 || count > RELAY_BLUEPRINT_CAPACITY) {
        return false;
    }
    records = count == 0 ? NULL : calloc(count, sizeof(*records));
    if (records == NULL && count > 0) {
        return false;
    }
    for (index = 0; index < count; index++) {
        Relay_SessionBlueprintRecord *record = &records[index];
        char script_path[RELAY_SESSION_PATH_CAPACITY];
        char script_name[RELAY_BLUEPRINT_NAME_CAPACITY + 20];
        uint32_t source_size;
        uint32_t deployed_source_size;
        uint8_t workspace_open;
        uint8_t editor_open;
        uint8_t dirty;

        record->id = relay_session_read_u64(reader);
        if (!relay_session_read_string(reader, record->name,
                sizeof(record->name))) {
            free(records);
            return false;
        }
        source_size = relay_session_read_u32(reader);
        record->source_checksum = relay_session_read_u32(reader);
        record->source_size = source_size;
        record->revision = relay_session_read_u64(reader);
        deployed_source_size = relay_session_read_u32(reader);
        record->deployed_source_checksum = relay_session_read_u32(reader);
        record->deployed_source_size = deployed_source_size;
        record->compiled_revision = relay_session_read_u64(reader);
        dirty = relay_session_read_u8(reader);
        record->cursor = relay_session_read_u32(reader);
        record->viewport_line = relay_session_read_u32(reader);
        record->focused_node_id = relay_session_read_u64(reader);
        workspace_open = relay_session_read_u8(reader);
        editor_open = relay_session_read_u8(reader);
        if (!reader->valid || source_size == 0 ||
            source_size >= RELAY_BLUEPRINT_SOURCE_CAPACITY ||
            deployed_source_size == 0 ||
            deployed_source_size >= RELAY_BLUEPRINT_SOURCE_CAPACITY ||
            record->id != index + 1 ||
            !relay_session_blueprint_name_valid(record->name) ||
            record->revision == 0 || record->compiled_revision == 0 ||
            record->compiled_revision > record->revision ||
            record->cursor > record->source_size || dirty > 1 ||
            workspace_open > 1 || editor_open > 1 ||
            (!dirty && (record->revision != record->compiled_revision ||
                record->source_size != record->deployed_source_size ||
                memcmp(record->source, record->deployed_source,
                    record->source_size) != 0))) {
            free(records);
            return false;
        }
        if (snprintf(script_name, sizeof(script_name), "scripts/%s.lua",
                record->name) < 0 ||
            !relay_session_path_join(script_path, sizeof(script_path),
                slot_directory, script_name) ||
            !relay_session_read_source_file(script_path, record->source,
                sizeof(record->source), record->source_size,
                record->source_checksum) ||
            snprintf(script_name, sizeof(script_name),
                "scripts/%s.deployed.lua", record->name) < 0 ||
            !relay_session_path_join(script_path, sizeof(script_path),
                slot_directory, script_name) ||
            !relay_session_read_source_file(script_path,
                record->deployed_source, sizeof(record->deployed_source),
                record->deployed_source_size,
                record->deployed_source_checksum)) {
            free(records);
            return false;
        }
        record->workspace_open = workspace_open != 0;
        record->editor_open = editor_open != 0;
        record->dirty = dirty != 0;
    }
    for (index = 0; index < count; index++) {
        Relay_Blueprint *blueprint;

        if (relay_blueprint_library_create(&game->blueprints) !=
                records[index].id) {
            free(records);
            return false;
        }
        blueprint = &game->blueprints.blueprints[index];
        (void)snprintf(blueprint->name, sizeof(blueprint->name), "%s",
            records[index].name);
        (void)snprintf(blueprint->key, sizeof(blueprint->key), "script.%s",
            records[index].name);
        (void)memcpy(blueprint->source, records[index].deployed_source,
            records[index].deployed_source_size + 1);
        blueprint->source_size = records[index].deployed_source_size;
        blueprint->revision = records[index].compiled_revision;
        blueprint->dirty = true;
    }
    for (pass = 0; pass < count && compiled_count < count; pass++) {
        for (index = 0; index < count; index++) {
            if (!compiled[index] &&
                relay_blueprint_compile(&game->blueprints,
                    &game->blueprints.blueprints[index])) {
                compiled[index] = true;
                compiled_count++;
            }
        }
    }
    if (compiled_count != count || next_id < game->blueprints.next_id) {
        free(records);
        return false;
    }
    game->blueprints.next_id = next_id;
    for (index = 0; index < count; index++) {
        Relay_Blueprint *blueprint = &game->blueprints.blueprints[index];

        blueprint->cursor = records[index].cursor;
        blueprint->viewport_line = records[index].viewport_line;
        blueprint->focused_node_id = records[index].focused_node_id;
        blueprint->workspace_open = records[index].workspace_open;
        blueprint->editor_open = records[index].editor_open;
        (void)memcpy(blueprint->source, records[index].source,
            records[index].source_size + 1);
        blueprint->source_size = records[index].source_size;
        blueprint->revision = records[index].revision;
        blueprint->dirty = records[index].dirty;
    }
    free(records);
    return true;
}

/** Decode into a separate game and install only after full validation. */
static bool relay_session_decode(const unsigned char *data, size_t size,
    const char *slot_directory, uint64_t expected_session_id,
    Relay_Game *candidate, Relay_ScriptRuntime *runtime)
{
    Relay_SessionReader reader = {data, size, 0, true};
    uint32_t active_tab;
    uint32_t workspace_mode;
    uint32_t selected_offer;
    uint32_t selected_blueprint;
    uint32_t active_workspace;

    if (candidate == NULL || !relay_game_init(candidate, runtime)) {
        return false;
    }
    candidate->session_id = relay_session_read_u64(&reader);
    candidate->save_revision = relay_session_read_u64(&reader);
    candidate->simulation_step = relay_session_read_u64(&reader);
    (void)relay_session_read_u64(&reader);
    candidate->currency = relay_session_read_u64(&reader);
    active_tab = relay_session_read_u32(&reader);
    workspace_mode = relay_session_read_u32(&reader);
    selected_offer = relay_session_read_u32(&reader);
    selected_blueprint = relay_session_read_u32(&reader);
    candidate->focused_node_id = relay_session_read_u64(&reader);
    candidate->root_focused_node_id = relay_session_read_u64(&reader);
    active_workspace = relay_session_read_u32(&reader);
    candidate->editing_blueprint_id = relay_session_read_u64(&reader);
    if (!reader.valid || candidate->session_id != expected_session_id ||
        candidate->save_revision == 0 ||
        active_tab > RELAY_GAME_PANEL_TAB_BLUEPRINTS ||
        workspace_mode > RELAY_GAME_WORKSPACE_MAP ||
        selected_offer >= relay_game_shop_offer_count()) {
        relay_game_shutdown(candidate);
        return false;
    }
    candidate->active_tab = (Relay_GamePanelTab)active_tab;
    candidate->workspace_mode = (Relay_GameWorkspaceMode)workspace_mode;
    candidate->selected_offer = selected_offer;
    candidate->selected_blueprint = selected_blueprint;
    if (!relay_session_read_blueprints(&reader, candidate, slot_directory) ||
        active_workspace > candidate->blueprints.count ||
        (candidate->blueprints.count == 0 && selected_blueprint != 0) ||
        (candidate->blueprints.count > 0 &&
            selected_blueprint >= candidate->blueprints.count) ||
        (candidate->editing_blueprint_id != 0 &&
            relay_blueprint_library_find(&candidate->blueprints,
                candidate->editing_blueprint_id) == NULL) ||
        !relay_session_read_world(&reader, candidate, runtime) ||
        reader.offset != reader.size ||
        (candidate->root_focused_node_id != 0 &&
            relay_node_world_find(&candidate->nodes,
                candidate->root_focused_node_id) == NULL)) {
        relay_game_shutdown(candidate);
        return false;
    }
    candidate->active_workspace = active_workspace;
    if (candidate->active_workspace == 0) {
        candidate->focused_node_id = candidate->root_focused_node_id;
    } else {
        Relay_Blueprint *active =
            &candidate->blueprints.blueprints[
                candidate->active_workspace - 1];

        if (!active->workspace_open ||
            (active->focused_node_id != 0 &&
                relay_node_world_find(&active->scene,
                    active->focused_node_id) == NULL)) {
            relay_game_shutdown(candidate);
            return false;
        }
        candidate->focused_node_id = active->focused_node_id;
    }
    if (!relay_session_game_valid(candidate, runtime) ||
        !relay_blueprint_library_rebind_storage(&candidate->blueprints,
            &candidate->nodes)) {
        relay_game_shutdown(candidate);
        return false;
    }
    return true;
}

/** Write one checksummed versioned envelope and flush it to stable storage. */
static bool relay_session_write_envelope(const char *path,
    const unsigned char magic[8], uint32_t version,
    const Relay_SessionWriter *payload)
{
    unsigned char header[24] = {0};
    FILE *file;
    size_t index;
    bool valid;

    if (payload == NULL || !payload->valid || payload->size == 0) {
        return false;
    }
    (void)memcpy(header, magic, 8);
    for (index = 0; index < 4; index++) {
        header[8 + index] =
            (unsigned char)(version >> (index * 8));
        header[20 + index] = (unsigned char)(
            relay_session_checksum(payload->data, payload->size) >>
                (index * 8));
    }
    for (index = 0; index < 8; index++) {
        header[12 + index] = (unsigned char)(
            (uint64_t)payload->size >> (index * 8));
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    valid = fwrite(header, 1, sizeof(header), file) == sizeof(header) &&
        fwrite(payload->data, 1, payload->size, file) == payload->size &&
        fflush(file) == 0;
#if RELAY_PLATFORM_WINDOWS
    if (valid) {
        valid = _commit(_fileno(file)) == 0;
    }
#else
    if (valid) {
        valid = fsync(fileno(file)) == 0;
    }
#endif
    if (fclose(file) != 0) {
        valid = false;
    }
    return valid;
}

/** Read and validate one exact envelope before exposing its payload. */
static unsigned char *relay_session_read_envelope(const char *path,
    const unsigned char magic[8], uint32_t expected_version,
    size_t *payload_size)
{
    unsigned char header[24];
    unsigned char *payload;
    FILE *file = fopen(path, "rb");
    uint32_t version = 0;
    uint64_t size = 0;
    uint32_t checksum = 0;
    size_t index;
    int trailing;

    if (payload_size == NULL || file == NULL ||
        fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, magic, 8) != 0) {
        if (file != NULL) {
            (void)fclose(file);
        }
        return NULL;
    }
    for (index = 0; index < 4; index++) {
        version |= (uint32_t)header[8 + index] << (index * 8);
        checksum |= (uint32_t)header[20 + index] << (index * 8);
    }
    for (index = 0; index < 8; index++) {
        size |= (uint64_t)header[12 + index] << (index * 8);
    }
    if (version != expected_version || size == 0 ||
        size > RELAY_SESSION_FILE_LIMIT || size > SIZE_MAX) {
        (void)fclose(file);
        return NULL;
    }
    payload = malloc((size_t)size);
    if (payload == NULL ||
        fread(payload, 1, (size_t)size, file) != (size_t)size) {
        free(payload);
        (void)fclose(file);
        return NULL;
    }
    trailing = fgetc(file);
    (void)fclose(file);
    if (trailing != EOF ||
        relay_session_checksum(payload, (size_t)size) != checksum) {
        free(payload);
        return NULL;
    }
    *payload_size = (size_t)size;
    return payload;
}

/** Atomically replace one root metadata file after flushing its temporary. */
static bool relay_session_replace_file(const char *temporary_path,
    const char *path)
{
#if RELAY_PLATFORM_WINDOWS
    if (!MoveFileExA(temporary_path, path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        (void)remove(temporary_path);
        return false;
    }
#else
    if (rename(temporary_path, path) != 0) {
        (void)remove(temporary_path);
        return false;
    }
#endif
    return true;
}

/** Persist the last-played session identity through the root state file. */
static bool relay_session_write_state(const Relay_SessionStore *store,
    uint64_t session_id)
{
    Relay_SessionWriter payload = {0};
    bool valid;

    payload.valid = true;
    relay_session_write_u64(&payload, session_id);
    valid = relay_session_write_envelope(store->state_temporary_path,
        relay_session_state_magic, RELAY_SESSION_STATE_FORMAT_VERSION,
        &payload) &&
        relay_session_replace_file(store->state_temporary_path,
            store->state_path) &&
        relay_session_flush_directory(store->directory);
    free(payload.data);
    return valid;
}

/** Read the last-played identity without trusting it as a slot index. */
static uint64_t relay_session_read_state(const Relay_SessionStore *store)
{
    unsigned char *payload;
    Relay_SessionReader reader;
    size_t payload_size;
    uint64_t session_id;

    payload = relay_session_read_envelope(store->state_path,
        relay_session_state_magic, RELAY_SESSION_STATE_FORMAT_VERSION,
        &payload_size);
    if (payload == NULL) {
        return 0;
    }
    reader = (Relay_SessionReader){payload, payload_size, 0, true};
    session_id = relay_session_read_u64(&reader);
    if (!reader.valid || reader.offset != reader.size) {
        session_id = 0;
    }
    free(payload);
    return session_id;
}

/** Resolve final, staging, backup, scripts, and session paths for one slot. */
static bool relay_session_slot_paths(const Relay_SessionStore *store,
    uint64_t session_id, char *final_directory, char *staging_directory,
    char *backup_directory, char *scripts_directory, char *session_path)
{
    char name[RELAY_SESSION_SLOT_NAME_CAPACITY];
    char staging_name[RELAY_SESSION_SLOT_NAME_CAPACITY + 4];
    char backup_name[RELAY_SESSION_SLOT_NAME_CAPACITY + 4];

    if (!relay_session_slot_name(session_id, name) ||
        snprintf(staging_name, sizeof(staging_name), "%s.tmp", name) !=
            (int)sizeof(staging_name) - 1 ||
        snprintf(backup_name, sizeof(backup_name), "%s.bak", name) !=
            (int)sizeof(backup_name) - 1 ||
        !relay_session_path_join(final_directory,
            RELAY_SESSION_PATH_CAPACITY, store->sessions_directory, name) ||
        !relay_session_path_join(staging_directory,
            RELAY_SESSION_PATH_CAPACITY, store->sessions_directory,
            staging_name) ||
        !relay_session_path_join(backup_directory,
            RELAY_SESSION_PATH_CAPACITY, store->sessions_directory,
            backup_name) ||
        !relay_session_path_join(scripts_directory,
            RELAY_SESSION_PATH_CAPACITY, staging_directory, "scripts") ||
        !relay_session_path_join(session_path, RELAY_SESSION_PATH_CAPACITY,
            staging_directory, "session.rly")) {
        return false;
    }
    return true;
}

/** Write every Blueprint draft and deployed source into one staging slot. */
static bool relay_session_write_scripts(const Relay_Game *game,
    const char *scripts_directory)
{
    size_t index;

    if (!relay_session_create_directory(scripts_directory)) {
        return false;
    }
    for (index = 0; index < game->blueprints.count; index++) {
        const Relay_Blueprint *blueprint =
            &game->blueprints.blueprints[index];
        char name[RELAY_BLUEPRINT_NAME_CAPACITY + 20];
        char path[RELAY_SESSION_PATH_CAPACITY];
        int written;

        written = snprintf(name, sizeof(name), "%s.lua", blueprint->name);
        if (written < 0 || (size_t)written >= sizeof(name) ||
            !relay_session_path_join(path, sizeof(path), scripts_directory,
                name) ||
            !relay_session_write_source_file(path, blueprint->source,
                blueprint->source_size)) {
            return false;
        }
        written = snprintf(name, sizeof(name), "%s.deployed.lua",
            blueprint->name);
        if (written < 0 || (size_t)written >= sizeof(name) ||
            !relay_session_path_join(path, sizeof(path), scripts_directory,
                name) ||
            !relay_session_write_source_file(path,
                blueprint->deployed_source,
                blueprint->deployed_source_size)) {
            return false;
        }
    }
    return true;
}

/** Probe one slot envelope for browser metadata without loading gameplay. */
static bool relay_session_probe_slot(const Relay_SessionStore *store,
    uint64_t session_id, Relay_SessionSlot *slot)
{
    char final_directory[RELAY_SESSION_PATH_CAPACITY];
    char staging_directory[RELAY_SESSION_PATH_CAPACITY];
    char backup_directory[RELAY_SESSION_PATH_CAPACITY];
    char scripts_directory[RELAY_SESSION_PATH_CAPACITY];
    char session_path[RELAY_SESSION_PATH_CAPACITY];
    unsigned char *payload;
    Relay_SessionReader reader;
    size_t payload_size;

    if (!relay_session_slot_paths(store, session_id, final_directory,
            staging_directory, backup_directory, scripts_directory,
            session_path) ||
        !relay_session_path_join(session_path, sizeof(session_path),
            final_directory, "session.rly")) {
        return false;
    }
    *slot = (Relay_SessionSlot){0};
    slot->id = session_id;
    (void)relay_session_slot_name(session_id, slot->directory_name);
    payload = relay_session_read_envelope(session_path, relay_session_magic,
        RELAY_SESSION_FORMAT_VERSION, &payload_size);
    if (payload == NULL) {
        return false;
    }
    reader = (Relay_SessionReader){payload, payload_size, 0, true};
    if (relay_session_read_u64(&reader) != session_id) {
        reader.valid = false;
    }
    slot->save_revision = relay_session_read_u64(&reader);
    slot->simulation_step = relay_session_read_u64(&reader);
    slot->saved_at_unix_seconds = relay_session_read_u64(&reader);
    slot->valid = reader.valid && slot->save_revision != 0 &&
        slot->saved_at_unix_seconds != 0;
    free(payload);
    return slot->valid;
}

/** Compare slots by validity, recency, then stable identity. */
static int relay_session_slot_compare(const void *left, const void *right)
{
    const Relay_SessionSlot *left_slot = left;
    const Relay_SessionSlot *right_slot = right;

    if (left_slot->valid != right_slot->valid) {
        return left_slot->valid ? -1 : 1;
    }
    if (left_slot->saved_at_unix_seconds !=
            right_slot->saved_at_unix_seconds) {
        return left_slot->saved_at_unix_seconds >
            right_slot->saved_at_unix_seconds ? -1 : 1;
    }
    return left_slot->id > right_slot->id ? -1 :
        left_slot->id < right_slot->id ? 1 : 0;
}

/** Return whether a directory name is owned by the slot repository. */
static bool relay_session_entry_name_valid(const char *name)
{
    const size_t size = name == NULL ? 0 : strlen(name);
    char base[RELAY_SESSION_SLOT_NAME_CAPACITY];
    uint64_t session_id;

    if (size == RELAY_SESSION_SLOT_NAME_CAPACITY - 1) {
        return relay_session_slot_id(name, &session_id);
    }
    if (size != RELAY_SESSION_SLOT_NAME_CAPACITY + 3 ||
        (strcmp(&name[size - 4], ".bak") != 0 &&
            strcmp(&name[size - 4], ".tmp") != 0)) {
        return false;
    }
    (void)memcpy(base, name, RELAY_SESSION_SLOT_NAME_CAPACITY - 1);
    base[RELAY_SESSION_SLOT_NAME_CAPACITY - 1] = '\0';
    return relay_session_slot_id(base, &session_id);
}

/** Collect bounded directory entry names for recovery and slot discovery. */
static size_t relay_session_directory_names(const char *path,
    char names[][RELAY_SESSION_SLOT_NAME_CAPACITY + 4], size_t capacity,
    bool *complete)
{
    size_t count = 0;

    *complete = true;
#if RELAY_PLATFORM_WINDOWS
    WIN32_FIND_DATAA entry;
    HANDLE search;
    char pattern[RELAY_SESSION_PATH_CAPACITY];

    if (!relay_session_path_join(pattern, sizeof(pattern), path, "*")) {
        *complete = false;
        return 0;
    }
    search = FindFirstFileA(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) {
        *complete = GetLastError() == ERROR_FILE_NOT_FOUND;
        return 0;
    }
    do {
        const size_t size = strlen(entry.cFileName);

        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            strcmp(entry.cFileName, ".") == 0 ||
            strcmp(entry.cFileName, "..") == 0 ||
            !relay_session_entry_name_valid(entry.cFileName)) {
            continue;
        }
        if (count >= capacity || size >= sizeof(names[0])) {
            *complete = false;
            break;
        }
        (void)memcpy(names[count], entry.cFileName, size + 1);
        count++;
    } while (FindNextFileA(search, &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        *complete = false;
    }
    (void)FindClose(search);
#else
    DIR *directory = opendir(path);
    struct dirent *entry;

    if (directory == NULL) {
        *complete = false;
        return 0;
    }
    while ((entry = readdir(directory)) != NULL) {
        char child[RELAY_SESSION_PATH_CAPACITY];
        struct stat information;
        const size_t size = strlen(entry->d_name);

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!relay_session_path_join(child, sizeof(child), path,
                entry->d_name) ||
            lstat(child, &information) != 0) {
            *complete = false;
            break;
        }
        if (!S_ISDIR(information.st_mode)) {
            continue;
        }
        if (!relay_session_entry_name_valid(entry->d_name)) {
            continue;
        }
        if (count >= capacity || size >= sizeof(names[0])) {
            *complete = false;
            break;
        }
        (void)memcpy(names[count], entry->d_name, size + 1);
        count++;
    }
    if (closedir(directory) != 0) {
        *complete = false;
    }
#endif
    return count;
}

/** Recover interrupted directory commits before scanning visible slots. */
static bool relay_session_recover_directories(Relay_SessionStore *store)
{
    char names[RELAY_SESSION_SLOT_CAPACITY * 3]
        [RELAY_SESSION_SLOT_NAME_CAPACITY + 4];
    bool complete;
    const size_t count = relay_session_directory_names(
        store->sessions_directory, names,
        RELAY_SESSION_SLOT_CAPACITY * 3, &complete);
    size_t index;

    if (!complete) {
        return false;
    }
    for (index = 0; index < count; index++) {
        const size_t size = strlen(names[index]);
        uint64_t session_id;
        char base[RELAY_SESSION_SLOT_NAME_CAPACITY];
        char final_directory[RELAY_SESSION_PATH_CAPACITY];
        char entry_path[RELAY_SESSION_PATH_CAPACITY];

        if (size != RELAY_SESSION_SLOT_NAME_CAPACITY + 3 ||
            (strcmp(&names[index][size - 4], ".bak") != 0 &&
                strcmp(&names[index][size - 4], ".tmp") != 0)) {
            continue;
        }
        (void)memcpy(base, names[index],
            RELAY_SESSION_SLOT_NAME_CAPACITY - 1);
        base[RELAY_SESSION_SLOT_NAME_CAPACITY - 1] = '\0';
        if (!relay_session_slot_id(base, &session_id) ||
            !relay_session_path_join(final_directory,
                sizeof(final_directory), store->sessions_directory, base) ||
            !relay_session_path_join(entry_path, sizeof(entry_path),
                store->sessions_directory, names[index])) {
            return false;
        }
        if (strcmp(&names[index][size - 4], ".tmp") == 0) {
            if (!relay_session_remove_tree(entry_path)) {
                return false;
            }
        } else {
            Relay_SessionSlot slot;

            if (relay_session_directory_exists(final_directory) &&
                relay_session_probe_slot(store, session_id, &slot)) {
                if (!relay_session_remove_tree(entry_path)) {
                    return false;
                }
            } else {
                if (relay_session_directory_exists(final_directory) &&
                    !relay_session_remove_tree(final_directory)) {
                    return false;
                }
                if (!relay_session_rename_directory(entry_path,
                        final_directory)) {
                    return false;
                }
            }
        }
    }
    return true;
}

/** Rescan all canonical slot directories and resolve last-played availability. */
static bool relay_session_refresh(Relay_SessionStore *store)
{
    char names[RELAY_SESSION_SLOT_CAPACITY]
        [RELAY_SESSION_SLOT_NAME_CAPACITY + 4];
    bool complete;
    const size_t count = relay_session_directory_names(
        store->sessions_directory, names, RELAY_SESSION_SLOT_CAPACITY,
        &complete);
    size_t index;

    if (!complete) {
        return false;
    }
    store->slot_count = 0;
    for (index = 0; index < count; index++) {
        uint64_t session_id;
        Relay_SessionSlot *slot;

        if (!relay_session_slot_id(names[index], &session_id)) {
            continue;
        }
        if (store->slot_count >= RELAY_SESSION_SLOT_CAPACITY) {
            return false;
        }
        slot = &store->slots[store->slot_count++];
        if (!relay_session_probe_slot(store, session_id, slot)) {
            slot->id = session_id;
            (void)relay_session_slot_name(session_id,
                slot->directory_name);
            slot->valid = false;
        }
    }
    qsort(store->slots, store->slot_count, sizeof(store->slots[0]),
        relay_session_slot_compare);
    store->last_played_session_id = relay_session_read_state(store);
    store->continue_available = false;
    for (index = 0; index < store->slot_count; index++) {
        if (store->slots[index].valid &&
            store->slots[index].id == store->last_played_session_id) {
            store->continue_available = true;
            break;
        }
    }
    if (!store->continue_available && store->slot_count > 0 &&
        store->slots[0].valid) {
        store->last_played_session_id = store->slots[0].id;
        store->continue_available = true;
    }
    if (store->selected_slot >= store->slot_count) {
        store->selected_slot = 0;
    }
    return true;
}

/** Update the bounded slot index after a durable commit without rescanning. */
static bool relay_session_index_committed_slot(Relay_SessionStore *store,
    uint64_t session_id, uint64_t revision, uint64_t simulation_step,
    uint64_t saved_at_unix_seconds)
{
    Relay_SessionSlot *slot = NULL;
    size_t index;

    for (index = 0; index < store->slot_count; index++) {
        if (store->slots[index].id == session_id) {
            slot = &store->slots[index];
            break;
        }
    }
    if (slot == NULL) {
        if (store->slot_count >= RELAY_SESSION_SLOT_CAPACITY) {
            return false;
        }
        slot = &store->slots[store->slot_count++];
    }
    *slot = (Relay_SessionSlot){
        .id = session_id,
        .save_revision = revision,
        .simulation_step = simulation_step,
        .saved_at_unix_seconds = saved_at_unix_seconds,
        .valid = true
    };
    if (!relay_session_slot_name(session_id, slot->directory_name)) {
        return false;
    }
    qsort(store->slots, store->slot_count, sizeof(store->slots[0]),
        relay_session_slot_compare);
    store->selected_slot = 0;
    return true;
}

/** Return a fresh identity not present in any valid or invalid slot directory. */
static uint64_t relay_session_generate_id(const Relay_SessionStore *store)
{
    const time_t now = time(NULL);
    const clock_t ticks = clock();
    uint64_t candidate = (now == (time_t)-1 ? 1U : (uint64_t)now) *
        UINT64_C(1000003) +
        (uint64_t)(ticks == (clock_t)-1 ? 1 : ticks);
    size_t attempt;

    if (candidate == 0) {
        candidate = 1;
    }
    for (attempt = 0; attempt <= RELAY_SESSION_SLOT_CAPACITY; attempt++) {
        bool found = false;
        size_t index;

        for (index = 0; index < store->slot_count; index++) {
            if (store->slots[index].id == candidate) {
                found = true;
                break;
            }
        }
        if (!found) {
            return candidate;
        }
        candidate++;
        if (candidate == 0) {
            candidate = 1;
        }
    }
    return 0;
}

/** Build and transactionally install one complete slot directory snapshot. */
static bool relay_session_write_slot(Relay_SessionStore *store,
    Relay_Game *game, Relay_ScriptRuntime *runtime, uint64_t session_id,
    uint64_t revision, uint64_t saved_at_unix_seconds, bool overwrite)
{
    Relay_SessionWriter payload = {0};
    char final_directory[RELAY_SESSION_PATH_CAPACITY];
    char staging_directory[RELAY_SESSION_PATH_CAPACITY];
    char backup_directory[RELAY_SESSION_PATH_CAPACITY];
    char scripts_directory[RELAY_SESSION_PATH_CAPACITY];
    char session_path[RELAY_SESSION_PATH_CAPACITY];
    bool previous_moved = false;
    bool installed = false;
    bool valid;

    valid = relay_session_encode(&payload, game, runtime, session_id,
        revision, saved_at_unix_seconds) &&
        relay_session_slot_paths(store, session_id, final_directory,
            staging_directory, backup_directory, scripts_directory,
            session_path);
    if (!valid) {
        free(payload.data);
        return false;
    }
    if (relay_session_directory_exists(staging_directory) &&
        !relay_session_remove_tree(staging_directory)) {
        free(payload.data);
        return false;
    }
    if (!relay_session_create_directory(staging_directory) ||
        !relay_session_write_scripts(game, scripts_directory) ||
        !relay_session_write_envelope(session_path, relay_session_magic,
            RELAY_SESSION_FORMAT_VERSION, &payload) ||
        !relay_session_flush_directory(scripts_directory) ||
        !relay_session_flush_directory(staging_directory)) {
        (void)relay_session_remove_tree(staging_directory);
        free(payload.data);
        return false;
    }
    free(payload.data);
    if (overwrite) {
        if (!relay_session_directory_exists(final_directory) ||
            (relay_session_directory_exists(backup_directory) &&
                !relay_session_remove_tree(backup_directory)) ||
            !relay_session_rename_directory(final_directory,
                backup_directory)) {
            (void)relay_session_remove_tree(staging_directory);
            return false;
        }
        previous_moved = true;
    } else if (relay_session_directory_exists(final_directory)) {
        (void)relay_session_remove_tree(staging_directory);
        return false;
    }
    installed = relay_session_rename_directory(staging_directory,
        final_directory);
    if (installed) {
        installed = relay_session_flush_directory(
            store->sessions_directory);
    }
    if (installed) {
        installed = relay_session_write_state(store, session_id);
    }
    if (!installed) {
        if (relay_session_directory_exists(final_directory)) {
            (void)relay_session_remove_tree(final_directory);
        }
        if (previous_moved) {
            (void)relay_session_rename_directory(backup_directory,
                final_directory);
        }
        (void)relay_session_flush_directory(store->sessions_directory);
        (void)relay_session_remove_tree(staging_directory);
        return false;
    }
    if (previous_moved) {
        (void)relay_session_remove_tree(backup_directory);
        (void)relay_session_flush_directory(store->sessions_directory);
    }
    return true;
}

bool relay_session_store_init(Relay_SessionStore *store,
    const char *override_directory)
{
    const char *root = override_directory;
    char legacy_path[RELAY_SESSION_PATH_CAPACITY];
    int written;

    if (store == NULL) {
        return false;
    }
    *store = (Relay_SessionStore){0};
    if (root == NULL) {
#if RELAY_PLATFORM_WINDOWS
        root = getenv("USERPROFILE");
#else
        root = getenv("HOME");
#endif
        if (root == NULL || root[0] == '\0') {
            relay_session_status(store, NULL,
                "User data directory is unavailable");
            return false;
        }
        written = snprintf(store->directory, sizeof(store->directory),
            "%s/.relay", root);
    } else {
        written = snprintf(store->directory, sizeof(store->directory), "%s",
            root);
    }
    if (written < 0 || (size_t)written >= sizeof(store->directory) ||
        !relay_session_create_directory(store->directory) ||
        !relay_session_path_join(store->sessions_directory,
            sizeof(store->sessions_directory), store->directory,
            "sessions") ||
        !relay_session_create_directory(store->sessions_directory) ||
        !relay_session_path_join(store->state_path,
            sizeof(store->state_path), store->directory, "state.rly") ||
        !relay_session_path_join(store->state_temporary_path,
            sizeof(store->state_temporary_path), store->directory,
            "state.rly.tmp")) {
        relay_session_status(store, NULL,
            "Unable to initialize the Relay slot repository");
        return false;
    }
    if (!relay_session_path_join(legacy_path, sizeof(legacy_path),
            store->directory, "session.rly")) {
        return false;
    }
    if (relay_session_file_exists(legacy_path) && remove(legacy_path) != 0) {
        relay_session_status(store, NULL,
            "Unable to remove the obsolete session file");
        return false;
    }
    if (!relay_session_path_join(legacy_path, sizeof(legacy_path),
            store->directory, "session.rly.tmp")) {
        return false;
    }
    if (relay_session_file_exists(legacy_path) && remove(legacy_path) != 0) {
        relay_session_status(store, NULL,
            "Unable to remove the obsolete temporary session");
        return false;
    }
    if (relay_session_file_exists(store->state_temporary_path) &&
        remove(store->state_temporary_path) != 0) {
        relay_session_status(store, NULL,
            "Unable to remove interrupted root state");
        return false;
    }
    if (!relay_session_recover_directories(store) ||
        !relay_session_refresh(store)) {
        relay_session_status(store, NULL,
            "Unable to scan the Relay slot repository");
        return false;
    }
    store->initialized = true;
    relay_session_status(store, NULL, store->continue_available ?
        "Saved slots available" : "No saved slots");
    return true;
}

void relay_session_begin_new(Relay_SessionStore *store, Relay_Game *game)
{
    const uint64_t session_id =
        store == NULL ? 0 : relay_session_generate_id(store);

    if (store == NULL || game == NULL || session_id == 0) {
        return;
    }
    store->active_session_id = 0;
    game->session_id = session_id;
    game->save_revision = 0;
    relay_session_status(store, game, "New unsaved session");
}

bool relay_session_save(Relay_SessionStore *store, Relay_Game *game,
    Relay_ScriptRuntime *runtime, Relay_Logger *logger,
    Relay_SessionSaveMode mode)
{
    const bool overwrite = mode == RELAY_SESSION_SAVE_OVERWRITE;
    uint64_t session_id;
    uint64_t revision;
    uint64_t saved_at;
    const time_t now = time(NULL);

    if (store == NULL || !store->initialized || game == NULL ||
        runtime == NULL || game->session_id == 0 ||
        game->save_revision == UINT64_MAX ||
        (!overwrite &&
            store->slot_count >= RELAY_SESSION_SLOT_CAPACITY) ||
        (overwrite && (store->active_session_id == 0 ||
            store->active_session_id != game->session_id))) {
        return false;
    }
    session_id = overwrite ? store->active_session_id :
        store->active_session_id == 0 ? game->session_id :
            relay_session_generate_id(store);
    revision = overwrite ? game->save_revision + 1 : 1;
    saved_at = now == (time_t)-1 ? 1U : (uint64_t)now;
    if (session_id == 0 ||
        !relay_session_write_slot(store, game, runtime, session_id,
            revision, saved_at, overwrite)) {
        relay_session_status(store, game, "Session save failed");
        relay_logger_log(logger, RELAY_LOG_LEVEL_ERROR,
            "Session slot save failed.");
        return false;
    }
    if (!relay_session_refresh(store)) {
        (void)relay_session_index_committed_slot(store, session_id, revision,
            game->simulation_step, saved_at);
        relay_logger_log(logger, RELAY_LOG_LEVEL_WARNING,
            "Session %016llx saved, but repository rescan failed.",
            (unsigned long long)session_id);
    }
    game->session_id = session_id;
    game->save_revision = revision;
    store->active_session_id = session_id;
    store->last_played_session_id = session_id;
    store->continue_available = true;
    game->session_continue_available = true;
    relay_session_status(store, game, overwrite ?
        "Session slot overwritten" : "New session slot saved");
    relay_logger_log(logger, RELAY_LOG_LEVEL_INFO,
        "Session %016llx revision %llu saved.",
        (unsigned long long)session_id,
        (unsigned long long)revision);
    return true;
}

bool relay_session_load_slot(Relay_SessionStore *store, uint64_t session_id,
    Relay_Game *game, Relay_ScriptRuntime *runtime, Relay_Logger *logger)
{
    char final_directory[RELAY_SESSION_PATH_CAPACITY];
    char staging_directory[RELAY_SESSION_PATH_CAPACITY];
    char backup_directory[RELAY_SESSION_PATH_CAPACITY];
    char scripts_directory[RELAY_SESSION_PATH_CAPACITY];
    char session_path[RELAY_SESSION_PATH_CAPACITY];
    unsigned char *payload;
    Relay_Game candidate = {0};
    size_t payload_size;
    size_t index;
    bool listed = false;

    if (store == NULL || !store->initialized || game == NULL ||
        runtime == NULL || session_id == 0) {
        return false;
    }
    for (index = 0; index < store->slot_count; index++) {
        if (store->slots[index].id == session_id &&
            store->slots[index].valid) {
            listed = true;
            break;
        }
    }
    if (!listed ||
        !relay_session_slot_paths(store, session_id, final_directory,
            staging_directory, backup_directory, scripts_directory,
            session_path) ||
        !relay_session_path_join(session_path, sizeof(session_path),
            final_directory, "session.rly")) {
        relay_session_status(store, game, "Selected slot is invalid");
        return false;
    }
    payload = relay_session_read_envelope(session_path, relay_session_magic,
        RELAY_SESSION_FORMAT_VERSION, &payload_size);
    if (payload == NULL ||
        !relay_session_decode(payload, payload_size, final_directory,
            session_id, &candidate, runtime)) {
        free(payload);
        for (index = 0; index < store->slot_count; index++) {
            if (store->slots[index].id == session_id) {
                store->slots[index].valid = false;
                break;
            }
        }
        if (store->last_played_session_id == session_id) {
            store->continue_available = false;
            game->session_continue_available = false;
        }
        relay_session_status(store, game, "Selected slot is incompatible");
        relay_logger_log(logger, RELAY_LOG_LEVEL_ERROR,
            "Session %016llx validation failed.",
            (unsigned long long)session_id);
        return false;
    }
    free(payload);
    if (!relay_session_write_state(store, session_id)) {
        relay_game_shutdown(&candidate);
        relay_session_status(store, game,
            "Unable to update last-played state");
        return false;
    }
    {
        Relay_Game previous = *game;

        *game = candidate;
        game->script_runtime = runtime;
        if (!relay_blueprint_library_rebind_storage(&game->blueprints,
                &game->nodes)) {
            Relay_Game rejected = *game;

            *game = previous;
            relay_game_shutdown(&rejected);
            return false;
        }
        relay_game_shutdown(&previous);
    }
    store->active_session_id = session_id;
    store->last_played_session_id = session_id;
    store->continue_available = true;
    game->session_continue_available = true;
    relay_session_status(store, game, "Session slot continued");
    relay_logger_log(logger, RELAY_LOG_LEVEL_INFO,
        "Session %016llx revision %llu continued.",
        (unsigned long long)session_id,
        (unsigned long long)game->save_revision);
    return true;
}

bool relay_session_load_last(Relay_SessionStore *store, Relay_Game *game,
    Relay_ScriptRuntime *runtime, Relay_Logger *logger)
{
    if (store == NULL || !store->continue_available) {
        return false;
    }
    return relay_session_load_slot(store, store->last_played_session_id,
        game, runtime, logger);
}
