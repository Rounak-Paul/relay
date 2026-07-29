#include "relay/script_runtime.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if LUA_VERSION_NUM != 505
#error "Relay requires the pinned Lua 5.5 API."
#endif

enum {
    RELAY_SCRIPT_RUNTIME_HASH_SEED = 0x52454C59U,
    RELAY_SCRIPT_RUNTIME_HOOK_GRANULARITY = 100,
    RELAY_SCRIPT_STATE_ENTRY_LIMIT = 64,
    RELAY_SCRIPT_STATE_STRING_LIMIT = 256
};

/** Schema being assembled by input/output declarations during compilation. */
typedef struct Relay_ScriptCompileContext {
    Relay_ScriptSchema schema;
    Relay_ScriptDiagnostic *diagnostic;
} Relay_ScriptCompileContext;

/** One capitalized Lua enum member mapped to an executable graph port type. */
typedef struct Relay_ScriptPortTypeEntry {
    const char *name;
    Relay_NodePortType type;
} Relay_ScriptPortTypeEntry;

static const Relay_ScriptPortTypeEntry relay_script_port_types[] = {
    {"TRIGGER", RELAY_NODE_PORT_TYPE_TRIGGER},
    {"COAL", RELAY_NODE_PORT_TYPE_COAL},
    {"IRON_ORE", RELAY_NODE_PORT_TYPE_IRON_ORE},
    {"COPPER_ORE", RELAY_NODE_PORT_TYPE_COPPER_ORE},
    {"STONE", RELAY_NODE_PORT_TYPE_STONE},
    {"BOOLEAN", RELAY_NODE_PORT_TYPE_BOOLEAN},
    {"INTEGER", RELAY_NODE_PORT_TYPE_INTEGER}
};

/** Allocate Lua memory while enforcing the owning runtime's hard limit. */
static void *relay_script_runtime_allocate(void *context, void *pointer,
    size_t old_size, size_t new_size)
{
    Relay_ScriptRuntime *runtime = context;
    const size_t accounted_old_size = pointer == NULL ? 0 : old_size;
    size_t retained_size;
    void *allocation;

    if (new_size == 0) {
        free(pointer);
        runtime->memory_used = accounted_old_size > runtime->memory_used ? 0 :
            runtime->memory_used - accounted_old_size;
        return NULL;
    }
    if (accounted_old_size > runtime->memory_used) {
        return NULL;
    }
    retained_size = runtime->memory_used - accounted_old_size;
    if (retained_size > runtime->memory_limit ||
        new_size > runtime->memory_limit - retained_size) {
        return NULL;
    }
    allocation = realloc(pointer, new_size);
    if (allocation != NULL) {
        runtime->memory_used = runtime->memory_used - accounted_old_size +
            new_size;
    }
    return allocation;
}

/** Remove a base-library global that violates the in-game sandbox contract. */
static void relay_script_runtime_remove_global(lua_State *state,
    const char *name)
{
    lua_pushnil(state);
    lua_setglobal(state, name);
}

/** Return whether a named global is unavailable to scripts. */
static bool relay_script_runtime_global_is_absent(lua_State *state,
    const char *name)
{
    const int type = lua_getglobal(state, name);
    const bool absent = type == LUA_TNIL;

    lua_pop(state, 1);
    return absent;
}

/** Open the deterministic-safe library subset and remove unsafe base APIs. */
static bool relay_script_runtime_open_sandbox(lua_State *state)
{
    static const char *const removed_globals[] = {
        "collectgarbage",
        "dofile",
        "getmetatable",
        "load",
        "loadfile",
        "next",
        "pairs",
        "print",
        "rawequal",
        "rawget",
        "rawlen",
        "rawset",
        "setmetatable",
        "tonumber",
        "warn"
    };
    static const char *const forbidden_libraries[] = {
        LUA_COLIBNAME,
        LUA_DBLIBNAME,
        LUA_IOLIBNAME,
        LUA_LOADLIBNAME,
        LUA_MATHLIBNAME,
        LUA_OSLIBNAME
    };
    size_t index;

    luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(state, 1);
    lua_getglobal(state, LUA_STRLIBNAME);
    lua_pushnil(state);
    lua_setfield(state, -2, "dump");
    lua_pushnil(state);
    lua_setfield(state, -2, "format");
    lua_pushnil(state);
    lua_setfield(state, -2, "pack");
    lua_pushnil(state);
    lua_setfield(state, -2, "unpack");
    lua_pop(state, 1);

    for (index = 0; index < sizeof(removed_globals) /
            sizeof(removed_globals[0]); index++) {
        relay_script_runtime_remove_global(state, removed_globals[index]);
    }
    for (index = 0; index < sizeof(forbidden_libraries) /
            sizeof(forbidden_libraries[0]); index++) {
        if (!relay_script_runtime_global_is_absent(state,
                forbidden_libraries[index])) {
            return false;
        }
    }
    for (index = 0; index < sizeof(removed_globals) /
            sizeof(removed_globals[0]); index++) {
        if (!relay_script_runtime_global_is_absent(state,
                removed_globals[index])) {
            return false;
        }
    }
    return true;
}

/** Store a bounded diagnostic without exposing Lua-owned string storage. */
static void relay_script_diagnostic_set(Relay_ScriptDiagnostic *diagnostic,
    const char *message)
{
    if (diagnostic == NULL) {
        return;
    }
    (void)snprintf(diagnostic->message, sizeof(diagnostic->message), "%s",
        message == NULL ? "Unknown script error." : message);
}

/** Reject an attempted write to a shared standard-library proxy. */
static int relay_script_read_only_error(lua_State *state)
{
    return luaL_error(state, "standard libraries are read-only");
}

/** Reject an attempted mutation of Relay's shared port-type enum. */
static int relay_script_type_read_only_error(lua_State *state)
{
    return luaL_error(state, "Type enum is read-only");
}

/** Reject an attempted write to the immutable tick input snapshot. */
static int relay_script_input_read_only_error(lua_State *state)
{
    return luaL_error(state, "input snapshots are read-only");
}

/** Reject runtime writes to a compiled module's sealed global environment. */
static int relay_script_environment_read_only_error(lua_State *state)
{
    return luaL_error(state, "module globals are read-only; use state");
}

/** Copy one safe global into a per-artifact environment. */
static void relay_script_environment_copy_global(lua_State *state,
    int environment_index, const char *name)
{
    environment_index = lua_absindex(state, environment_index);
    lua_getglobal(state, name);
    lua_setfield(state, environment_index, name);
}

/** Add a read-only proxy for one safe standard library to an environment. */
static void relay_script_environment_add_library(lua_State *state,
    int environment_index, const char *name)
{
    environment_index = lua_absindex(state, environment_index);
    lua_newtable(state);
    lua_newtable(state);
    lua_getglobal(state, name);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, relay_script_read_only_error);
    lua_setfield(state, -2, "__newindex");
    lua_pushliteral(state, "Relay protected library");
    lua_setfield(state, -2, "__metatable");
    (void)lua_setmetatable(state, -2);
    lua_setfield(state, environment_index, name);
}

/** Add the immutable capitalized port-type enum to a module environment. */
static void relay_script_environment_add_type_enum(lua_State *state,
    int environment_index)
{
    size_t index;

    environment_index = lua_absindex(state, environment_index);
    lua_newtable(state);
    lua_newtable(state);
    lua_newtable(state);
    for (index = 0; index < sizeof(relay_script_port_types) /
            sizeof(relay_script_port_types[0]); index++) {
        lua_pushinteger(state, relay_script_port_types[index].type);
        lua_setfield(state, -2, relay_script_port_types[index].name);
    }
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, relay_script_type_read_only_error);
    lua_setfield(state, -2, "__newindex");
    lua_pushliteral(state, "Relay Type enum");
    lua_setfield(state, -2, "__metatable");
    (void)lua_setmetatable(state, -2);
    lua_setfield(state, environment_index, "Type");
}

/** Return whether a script port key is stable and safe for lookup. */
static bool relay_script_port_key_is_valid(const char *key, size_t length)
{
    static const char *const reserved[] = {
        "and", "break", "do", "else", "elseif", "end", "false", "for",
        "function", "goto", "if", "in", "local", "nil", "not", "or",
        "repeat", "return", "then", "true", "until", "while"
    };
    size_t index;

    if (length == 0 || length >= RELAY_SCRIPT_PORT_KEY_CAPACITY ||
        !(isalpha((unsigned char)key[0]) || key[0] == '_')) {
        return false;
    }
    for (index = 1; index < length; index++) {
        if (!(isalnum((unsigned char)key[index]) || key[index] == '_')) {
            return false;
        }
    }
    for (index = 0; index < sizeof(reserved) / sizeof(reserved[0]); index++) {
        if (strlen(reserved[index]) == length &&
            memcmp(key, reserved[index], length) == 0) {
            return false;
        }
    }
    return true;
}

/** Validate one Lua enum value against the executable script type registry. */
static Relay_NodePortType relay_script_port_type_parse(lua_Integer value)
{
    size_t index;

    for (index = 0; index < sizeof(relay_script_port_types) /
            sizeof(relay_script_port_types[0]); index++) {
        if ((lua_Integer)relay_script_port_types[index].type == value) {
            return relay_script_port_types[index].type;
        }
    }
    return RELAY_NODE_PORT_TYPE_INVALID;
}

/** Return whether a port key was already declared in either direction. */
static bool relay_script_schema_has_key(const Relay_ScriptSchema *schema,
    const char *key)
{
    size_t index;

    for (index = 0; index < schema->input_count; index++) {
        if (strcmp(schema->inputs[index].key, key) == 0) {
            return true;
        }
    }
    for (index = 0; index < schema->output_count; index++) {
        if (strcmp(schema->outputs[index].key, key) == 0) {
            return true;
        }
    }
    return false;
}

/** Add an input or output declaration to the active compile schema. */
static int relay_script_declare_port(lua_State *state, bool output)
{
    Relay_ScriptCompileContext *context = lua_touserdata(state,
        lua_upvalueindex(1));
    size_t key_length;
    const char *key = luaL_checklstring(state, 1, &key_length);
    const lua_Integer type_value = luaL_checkinteger(state, 2);
    const Relay_NodePortType type = relay_script_port_type_parse(type_value);
    Relay_ScriptPort *port;
    size_t *count;

    if (!relay_script_port_key_is_valid(key, key_length)) {
        return luaL_error(state, "invalid port key '%s'", key);
    }
    if (type == RELAY_NODE_PORT_TYPE_INVALID) {
        return luaL_error(state, "unsupported Type enum value %lld",
            (long long)type_value);
    }
    if (relay_script_schema_has_key(&context->schema, key)) {
        return luaL_error(state, "duplicate port key '%s'", key);
    }
    count = output ? &context->schema.output_count :
        &context->schema.input_count;
    if (*count >= RELAY_NODE_MAX_PORTS) {
        return luaL_error(state, "a module supports at most %d %s ports",
            RELAY_NODE_MAX_PORTS, output ? "output" : "input");
    }
    port = output ? &context->schema.outputs[*count] :
        &context->schema.inputs[*count];
    (void)memcpy(port->key, key, key_length);
    port->key[key_length] = '\0';
    port->type = type;
    (*count)++;
    return 0;
}

/** Lua binding for one typed module input declaration. */
static int relay_script_declare_input(lua_State *state)
{
    return relay_script_declare_port(state, false);
}

/** Lua binding for one typed module output declaration. */
static int relay_script_declare_output(lua_State *state)
{
    return relay_script_declare_port(state, true);
}

/** Create an isolated environment with only Relay's safe Lua subset. */
static int relay_script_environment_create(lua_State *state,
    Relay_ScriptCompileContext *context)
{
    static const char *const safe_globals[] = {
        "assert", "error", "ipairs", "pcall", "select", "tostring", "type",
        "xpcall"
    };
    int environment_index;
    size_t index;

    lua_newtable(state);
    environment_index = lua_gettop(state);
    for (index = 0; index < sizeof(safe_globals) / sizeof(safe_globals[0]);
            index++) {
        relay_script_environment_copy_global(state, environment_index,
            safe_globals[index]);
    }
    relay_script_environment_add_library(state, environment_index,
        LUA_STRLIBNAME);
    relay_script_environment_add_library(state, environment_index,
        LUA_TABLIBNAME);
    relay_script_environment_add_library(state, environment_index,
        LUA_UTF8LIBNAME);
    relay_script_environment_add_type_enum(state, environment_index);
    lua_pushlightuserdata(state, context);
    lua_pushcclosure(state, relay_script_declare_input, 1);
    lua_setfield(state, environment_index, "input");
    lua_pushlightuserdata(state, context);
    lua_pushcclosure(state, relay_script_declare_output, 1);
    lua_setfield(state, environment_index, "output");
    lua_pushvalue(state, environment_index);
    lua_setfield(state, environment_index, "_G");
    return environment_index;
}

/** Seal compilation globals behind a proxy so runtime state stays instance-owned. */
static void relay_script_environment_seal(lua_State *state,
    int environment_index)
{
    int backing_index;

    environment_index = lua_absindex(state, environment_index);
    lua_newtable(state);
    backing_index = lua_gettop(state);

    lua_pushnil(state);
    while (lua_next(state, environment_index) != 0) {
        lua_pushvalue(state, -2);
        lua_pushvalue(state, -2);
        lua_settable(state, backing_index);
        lua_pop(state, 1);
    }
    lua_pushnil(state);
    while (lua_next(state, backing_index) != 0) {
        lua_pushvalue(state, -2);
        lua_pushnil(state);
        lua_settable(state, environment_index);
        lua_pop(state, 1);
    }

    lua_newtable(state);
    lua_pushvalue(state, backing_index);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, relay_script_environment_read_only_error);
    lua_setfield(state, -2, "__newindex");
    lua_pushliteral(state, "Relay module environment");
    lua_setfield(state, -2, "__metatable");
    (void)lua_setmetatable(state, environment_index);
    lua_pop(state, 1);
}

/** Abort a protected script call after its deterministic instruction budget. */
static void relay_script_instruction_hook(lua_State *state,
    lua_Debug *debug)
{
    void *context = NULL;
    Relay_ScriptRuntime *runtime;

    (void)debug;
    (void)lua_getallocf(state, &context);
    runtime = context;
    if (runtime->instruction_remaining <=
            RELAY_SCRIPT_RUNTIME_HOOK_GRANULARITY) {
        runtime->instruction_remaining = 0;
        (void)luaL_error(state, "instruction budget exhausted");
        return;
    }
    runtime->instruction_remaining -= RELAY_SCRIPT_RUNTIME_HOOK_GRANULARITY;
}

/** Run a protected Lua call with Relay's deterministic instruction limit. */
static int relay_script_runtime_pcall(Relay_ScriptRuntime *runtime,
    lua_State *state, int argument_count, int result_count)
{
    int result;

    runtime->instruction_remaining =
        RELAY_SCRIPT_RUNTIME_DEFAULT_INSTRUCTION_LIMIT;
    lua_sethook(state, relay_script_instruction_hook, LUA_MASKCOUNT,
        RELAY_SCRIPT_RUNTIME_HOOK_GRANULARITY);
    result = lua_pcall(state, argument_count, result_count, 0);
    lua_sethook(state, NULL, 0, 0);
    runtime->instruction_remaining = 0;
    return result;
}

/** Return whether one persistent-state value is bounded and serializable. */
static bool relay_script_state_value_is_valid(lua_State *state, int index)
{
    const int type = lua_type(state, index);

    return type == LUA_TBOOLEAN || lua_isinteger(state, index) ||
        (type == LUA_TSTRING &&
            lua_rawlen(state, index) <= RELAY_SCRIPT_STATE_STRING_LIMIT);
}

/** Copy and validate the flat project-owned persistent state table. */
static bool relay_script_state_clone(lua_State *state, int reference,
    int *clone_reference)
{
    size_t count = 0;
    int source_index;
    int clone_index;

    lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return false;
    }
    source_index = lua_gettop(state);
    lua_newtable(state);
    clone_index = lua_gettop(state);
    lua_pushnil(state);
    while (lua_next(state, source_index) != 0) {
        size_t key_length;

        if (lua_type(state, -2) != LUA_TSTRING ||
            lua_tolstring(state, -2, &key_length) == NULL ||
            key_length >= RELAY_SCRIPT_PORT_KEY_CAPACITY ||
            !relay_script_state_value_is_valid(state, -1) ||
            ++count > RELAY_SCRIPT_STATE_ENTRY_LIMIT) {
            lua_settop(state, source_index - 1);
            return false;
        }
        lua_pushvalue(state, -2);
        lua_pushvalue(state, -2);
        lua_settable(state, clone_index);
        lua_pop(state, 1);
    }
    lua_pushvalue(state, clone_index);
    *clone_reference = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_remove(state, source_index);
    return true;
}

/** Validate a mutated persistent-state table after a successful call. */
static bool relay_script_state_reference_is_valid(lua_State *state,
    int reference)
{
    size_t count = 0;
    bool valid = true;

    lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return false;
    }
    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        size_t key_length;

        if (lua_type(state, -2) != LUA_TSTRING ||
            lua_tolstring(state, -2, &key_length) == NULL ||
            key_length >= RELAY_SCRIPT_PORT_KEY_CAPACITY ||
            !relay_script_state_value_is_valid(state, -1) ||
            ++count > RELAY_SCRIPT_STATE_ENTRY_LIMIT) {
            valid = false;
            lua_pop(state, 2);
            break;
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return valid;
}

/** Validate Relay's deterministic integer-only Lua source subset. */
static bool relay_script_source_is_valid(const char *source, size_t size,
    Relay_ScriptDiagnostic *diagnostic)
{
    size_t index = 0;
    size_t line = 1;

    while (index < size) {
        const char character = source[index];

        if (character == '\n') {
            line++;
            index++;
        } else if ((character == '\'' || character == '"')) {
            const char quote = character;

            index++;
            while (index < size && source[index] != quote) {
                if (source[index] == '\\' && index + 1 < size) {
                    index += 2;
                } else {
                    if (source[index] == '\n') {
                        line++;
                    }
                    index++;
                }
            }
            if (index >= size) {
                relay_script_diagnostic_set(diagnostic,
                    "Unterminated string literal.");
                return false;
            }
            index++;
        } else if (character == '-' && index + 1 < size &&
            source[index + 1] == '-') {
            index += 2;
            while (index < size && source[index] != '\n') {
                index++;
            }
        } else if (character == '/' && index + 1 < size &&
            source[index + 1] == '/') {
            index += 2;
        } else if (character == '/') {
            char message[RELAY_SCRIPT_DIAGNOSTIC_CAPACITY];

            (void)snprintf(message, sizeof(message),
                "Line %zu: floating division '/' is forbidden; use '//'.",
                line);
            relay_script_diagnostic_set(diagnostic, message);
            return false;
        } else if (character == '^') {
            char message[RELAY_SCRIPT_DIAGNOSTIC_CAPACITY];

            (void)snprintf(message, sizeof(message),
                "Line %zu: exponentiation is forbidden.", line);
            relay_script_diagnostic_set(diagnostic, message);
            return false;
        } else if (isdigit((unsigned char)character)) {
            bool floating = false;
            size_t number_end = index + 1;

            while (number_end < size &&
                (isalnum((unsigned char)source[number_end]) ||
                    source[number_end] == '.' || source[number_end] == '_')) {
                if (source[number_end] == '.' || source[number_end] == 'e' ||
                    source[number_end] == 'E' || source[number_end] == 'p' ||
                    source[number_end] == 'P') {
                    floating = true;
                }
                number_end++;
            }
            if (floating) {
                char message[RELAY_SCRIPT_DIAGNOSTIC_CAPACITY];

                (void)snprintf(message, sizeof(message),
                    "Line %zu: floating-point literals are forbidden.", line);
                relay_script_diagnostic_set(diagnostic, message);
                return false;
            }
            index = number_end;
        } else {
            index++;
        }
    }
    return true;
}

bool relay_script_runtime_init(Relay_ScriptRuntime *runtime,
    size_t memory_limit)
{
    lua_State *state;

    if (runtime == NULL || runtime->state != NULL || memory_limit == 0) {
        return false;
    }
    runtime->memory_used = 0;
    runtime->memory_limit = memory_limit;
    state = lua_newstate(relay_script_runtime_allocate, runtime,
        RELAY_SCRIPT_RUNTIME_HASH_SEED);
    if (state == NULL) {
        runtime->memory_limit = 0;
        return false;
    }
    runtime->state = state;
    if (!relay_script_runtime_open_sandbox(state)) {
        relay_script_runtime_shutdown(runtime);
        return false;
    }
    return true;
}

const char *relay_script_runtime_version(void)
{
    return LUA_VERSION;
}

size_t relay_script_runtime_memory_used(const Relay_ScriptRuntime *runtime)
{
    return runtime == NULL ? 0 : runtime->memory_used;
}

bool relay_script_runtime_compile(Relay_ScriptRuntime *runtime,
    const char *source, size_t source_size, uint64_t revision,
    Relay_ScriptArtifact *artifact, Relay_ScriptSchema *schema,
    Relay_ScriptDiagnostic *diagnostic)
{
    Relay_ScriptCompileContext context = {0};
    Relay_ScriptArtifact candidate = {0};
    lua_State *state;
    int stack_base;
    int chunk_index;
    int environment_index;

    if (diagnostic != NULL) {
        *diagnostic = (Relay_ScriptDiagnostic){0};
    }
    if (runtime == NULL || runtime->state == NULL || source == NULL ||
        source_size == 0 || artifact == NULL || schema == NULL ||
        !relay_script_source_is_valid(source, source_size, diagnostic)) {
        if (diagnostic != NULL && diagnostic->message[0] == '\0') {
            relay_script_diagnostic_set(diagnostic,
                "Invalid script compilation request.");
        }
        return false;
    }
    state = runtime->state;
    stack_base = lua_gettop(state);
    if (luaL_loadbufferx(state, source, source_size, "relay.blueprint", "t") !=
            LUA_OK) {
        relay_script_diagnostic_set(diagnostic, lua_tostring(state, -1));
        lua_settop(state, stack_base);
        return false;
    }
    chunk_index = lua_gettop(state);
    environment_index = relay_script_environment_create(state, &context);
    lua_pushvalue(state, environment_index);
    if (lua_setupvalue(state, chunk_index, 1) == NULL) {
        relay_script_diagnostic_set(diagnostic,
            "Compiled chunk has no isolated environment.");
        lua_settop(state, stack_base);
        return false;
    }
    lua_pushvalue(state, chunk_index);
    if (relay_script_runtime_pcall(runtime, state, 0, 0) != LUA_OK) {
        relay_script_diagnostic_set(diagnostic, lua_tostring(state, -1));
        lua_settop(state, stack_base);
        return false;
    }
    lua_pushnil(state);
    lua_setfield(state, environment_index, "input");
    lua_pushnil(state);
    lua_setfield(state, environment_index, "output");
    lua_getfield(state, environment_index, "on_process");
    if (!lua_isfunction(state, -1)) {
        relay_script_diagnostic_set(diagnostic,
            "Module must define function on_process(inputs, state).");
        lua_settop(state, stack_base);
        return false;
    }
    candidate.on_process_reference = luaL_ref(state, LUA_REGISTRYINDEX);
    candidate.revision = revision;
    candidate.installed = true;
    relay_script_environment_seal(state, environment_index);
    lua_settop(state, stack_base);

    relay_script_artifact_shutdown(runtime, artifact);
    *artifact = candidate;
    *schema = context.schema;
    relay_script_diagnostic_set(diagnostic, "Compiled successfully.");
    return true;
}

bool relay_script_runtime_invoke(Relay_ScriptRuntime *runtime,
    const Relay_ScriptArtifact *artifact, Relay_ScriptInstanceState *instance,
    const Relay_ScriptSchema *schema, const int64_t *input_values,
    int64_t *output_values, Relay_ScriptDiagnostic *diagnostic)
{
    int64_t candidate_outputs[RELAY_NODE_MAX_PORTS] = {0};
    lua_State *state;
    int stack_base;
    int state_reference = LUA_NOREF;
    size_t index;

    if (diagnostic != NULL) {
        *diagnostic = (Relay_ScriptDiagnostic){0};
    }
    if (runtime == NULL || runtime->state == NULL || artifact == NULL ||
        !artifact->installed || instance == NULL || schema == NULL ||
        output_values == NULL ||
        (schema->input_count > 0 && input_values == NULL)) {
        relay_script_diagnostic_set(diagnostic,
            "Invalid script invocation request.");
        return false;
    }
    state = runtime->state;
    stack_base = lua_gettop(state);
    if (!instance->initialized) {
        lua_newtable(state);
        instance->runtime_reference = luaL_ref(state, LUA_REGISTRYINDEX);
        instance->initialized = true;
    }
    lua_rawgeti(state, LUA_REGISTRYINDEX, artifact->on_process_reference);
    if (!lua_isfunction(state, -1)) {
        relay_script_diagnostic_set(diagnostic,
            "Installed on_process function is unavailable.");
        lua_settop(state, stack_base);
        return false;
    }
    lua_createtable(state, 0, (int)schema->input_count);
    for (index = 0; index < schema->input_count; index++) {
        if (schema->inputs[index].type == RELAY_NODE_PORT_TYPE_BOOLEAN) {
            lua_pushboolean(state, input_values[index] != 0);
        } else {
            lua_pushinteger(state, (lua_Integer)input_values[index]);
        }
        lua_setfield(state, -2, schema->inputs[index].key);
    }
    lua_newtable(state);
    lua_newtable(state);
    lua_pushvalue(state, -3);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, relay_script_input_read_only_error);
    lua_setfield(state, -2, "__newindex");
    lua_pushliteral(state, "Relay input snapshot");
    lua_setfield(state, -2, "__metatable");
    (void)lua_setmetatable(state, -2);
    lua_remove(state, -2);
    if (!relay_script_state_clone(state, instance->runtime_reference,
            &state_reference)) {
        relay_script_diagnostic_set(diagnostic,
            "Persistent state is invalid.");
        lua_settop(state, stack_base);
        return false;
    }
    if (relay_script_runtime_pcall(runtime, state, 2, 1) != LUA_OK) {
        relay_script_diagnostic_set(diagnostic, lua_tostring(state, -1));
        luaL_unref(state, LUA_REGISTRYINDEX, state_reference);
        lua_settop(state, stack_base);
        return false;
    }
    if (!lua_isnil(state, -1) && !lua_istable(state, -1)) {
        relay_script_diagnostic_set(diagnostic,
            "on_process must return an output table or nil.");
        luaL_unref(state, LUA_REGISTRYINDEX, state_reference);
        lua_settop(state, stack_base);
        return false;
    }
    if (lua_istable(state, -1)) {
        for (index = 0; index < schema->output_count; index++) {
            lua_getfield(state, -1, schema->outputs[index].key);
            if (lua_isnil(state, -1)) {
                candidate_outputs[index] = 0;
            } else if (schema->outputs[index].type ==
                    RELAY_NODE_PORT_TYPE_BOOLEAN && lua_isboolean(state, -1)) {
                candidate_outputs[index] = lua_toboolean(state, -1) ? 1 : 0;
            } else if (schema->outputs[index].type !=
                    RELAY_NODE_PORT_TYPE_BOOLEAN && lua_isinteger(state, -1)) {
                candidate_outputs[index] = (int64_t)lua_tointeger(state, -1);
            } else {
                char message[RELAY_SCRIPT_DIAGNOSTIC_CAPACITY];

                (void)snprintf(message, sizeof(message),
                    "Output '%s' returned the wrong type.",
                    schema->outputs[index].key);
                relay_script_diagnostic_set(diagnostic, message);
                lua_pop(state, 1);
                luaL_unref(state, LUA_REGISTRYINDEX, state_reference);
                lua_settop(state, stack_base);
                return false;
            }
            lua_pop(state, 1);
        }
    }
    if (!relay_script_state_reference_is_valid(state, state_reference)) {
        relay_script_diagnostic_set(diagnostic,
            "Persistent state must contain only bounded scalar values.");
        luaL_unref(state, LUA_REGISTRYINDEX, state_reference);
        lua_settop(state, stack_base);
        return false;
    }
    luaL_unref(state, LUA_REGISTRYINDEX, instance->runtime_reference);
    instance->runtime_reference = state_reference;
    for (index = 0; index < schema->output_count; index++) {
        output_values[index] = candidate_outputs[index];
    }
    lua_settop(state, stack_base);
    relay_script_diagnostic_set(diagnostic, "Invocation completed.");
    return true;
}

void relay_script_artifact_shutdown(Relay_ScriptRuntime *runtime,
    Relay_ScriptArtifact *artifact)
{
    lua_State *state;

    if (artifact == NULL) {
        return;
    }
    if (runtime != NULL && runtime->state != NULL && artifact->installed) {
        state = runtime->state;
        luaL_unref(state, LUA_REGISTRYINDEX, artifact->on_process_reference);
    }
    *artifact = (Relay_ScriptArtifact){0};
}

void relay_script_instance_shutdown(Relay_ScriptRuntime *runtime,
    Relay_ScriptInstanceState *instance)
{
    if (instance == NULL) {
        return;
    }
    if (runtime != NULL && runtime->state != NULL && instance->initialized) {
        luaL_unref(runtime->state, LUA_REGISTRYINDEX,
            instance->runtime_reference);
    }
    *instance = (Relay_ScriptInstanceState){0};
}

void relay_script_runtime_shutdown(Relay_ScriptRuntime *runtime)
{
    if (runtime == NULL) {
        return;
    }
    if (runtime->state != NULL) {
        lua_close(runtime->state);
    }
    *runtime = (Relay_ScriptRuntime){0};
}
