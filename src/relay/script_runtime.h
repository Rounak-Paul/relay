#ifndef RELAY_SCRIPT_RUNTIME_H
#define RELAY_SCRIPT_RUNTIME_H

#include "relay/node.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    RELAY_SCRIPT_RUNTIME_DEFAULT_MEMORY_LIMIT = 8 * 1024 * 1024,
    RELAY_SCRIPT_RUNTIME_DEFAULT_INSTRUCTION_LIMIT = 10000,
    RELAY_SCRIPT_PORT_KEY_CAPACITY = 32,
    RELAY_SCRIPT_DIAGNOSTIC_CAPACITY = 256
};

/** Application-owned sandboxed Lua runtime with bounded memory. */
typedef struct Relay_ScriptRuntime {
    void *state;
    size_t memory_used;
    size_t memory_limit;
    size_t instruction_remaining;
} Relay_ScriptRuntime;

/** Compiled script references owned by a runtime and never serialized. */
typedef struct Relay_ScriptArtifact {
    int tick_reference;
    uint64_t revision;
    bool installed;
} Relay_ScriptArtifact;

/** One script-declared port with project-owned stable storage. */
typedef struct Relay_ScriptPort {
    char key[RELAY_SCRIPT_PORT_KEY_CAPACITY];
    Relay_NodePortType type;
} Relay_ScriptPort;

/** Typed public interface declared by one compiled script module. */
typedef struct Relay_ScriptSchema {
    Relay_ScriptPort inputs[RELAY_NODE_MAX_PORTS];
    size_t input_count;
    Relay_ScriptPort outputs[RELAY_NODE_MAX_PORTS];
    size_t output_count;
} Relay_ScriptSchema;

/** Source-ranged message returned by compilation or invocation. */
typedef struct Relay_ScriptDiagnostic {
    char message[RELAY_SCRIPT_DIAGNOSTIC_CAPACITY];
} Relay_ScriptDiagnostic;

/** Initialize a Lua state with only Relay's deterministic-safe base libraries. */
bool relay_script_runtime_init(Relay_ScriptRuntime *runtime,
    size_t memory_limit);

/** Return the linked Lua implementation version. */
const char *relay_script_runtime_version(void);

/** Return the bytes currently owned by the Lua allocator. */
size_t relay_script_runtime_memory_used(const Relay_ScriptRuntime *runtime);

/** Compile and transactionally install one Lua module and its typed interface. */
bool relay_script_runtime_compile(Relay_ScriptRuntime *runtime,
    const char *source, size_t source_size, uint64_t revision,
    Relay_ScriptArtifact *artifact, Relay_ScriptSchema *schema,
    Relay_ScriptDiagnostic *diagnostic);

/** Invoke a compiled module for one immutable input snapshot. */
bool relay_script_runtime_invoke(Relay_ScriptRuntime *runtime,
    const Relay_ScriptArtifact *artifact, Relay_ScriptInstanceState *instance,
    const Relay_ScriptSchema *schema, const int64_t *input_values,
    int64_t *output_values, Relay_ScriptDiagnostic *diagnostic);

/** Release compiled Lua references while preserving the owning runtime. */
void relay_script_artifact_shutdown(Relay_ScriptRuntime *runtime,
    Relay_ScriptArtifact *artifact);

/** Release one placed module's persistent state. */
void relay_script_instance_shutdown(Relay_ScriptRuntime *runtime,
    Relay_ScriptInstanceState *instance);

/** Close the Lua state and release all runtime-owned memory. */
void relay_script_runtime_shutdown(Relay_ScriptRuntime *runtime);

#endif
