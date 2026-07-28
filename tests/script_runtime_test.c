#include "relay/script_runtime.h"

#include <string.h>

/** Verify the pinned Lua runtime lifecycle and bounded allocator contract. */
int relay_script_runtime_test(void)
{
    static const char valid_source[] =
        "input('clock', 'clock')\n"
        "output('clock_out', 'clock')\n"
        "function tick(inputs, state)\n"
        "  state.calls = (state.calls or 0) + 1\n"
        "  return { clock_out = (inputs.clock * state.calls * 2) // 2 }\n"
        "end\n";
    static const char invalid_source[] =
        "input('coal', 'coal')\n"
        "output('clock', 'clock')\n"
        "function tick(inputs, state)\n"
        "  return { clock = inputs.coal / 2 }\n"
        "end\n";
    static const char mutating_source[] =
        "input('clock', 'clock')\n"
        "output('clock_out', 'clock')\n"
        "function tick(inputs, state)\n"
        "  inputs.clock = 0\n"
        "  return { clock_out = 1 }\n"
        "end\n";
    static const char runaway_source[] =
        "output('clock_out', 'clock')\n"
        "function tick(inputs, state)\n"
        "  while true do end\n"
        "end\n";
    static const char global_state_source[] =
        "output('count', 'integer')\n"
        "function tick(inputs, state)\n"
        "  leaked_count = (leaked_count or 0) + 1\n"
        "  return { count = leaked_count }\n"
        "end\n";
    Relay_ScriptRuntime runtime = {0};
    Relay_ScriptArtifact artifact = {0};
    Relay_ScriptInstanceState instance = {0};
    Relay_ScriptSchema schema = {0};
    Relay_ScriptDiagnostic diagnostic = {0};
    int64_t inputs[RELAY_NODE_MAX_PORTS] = {1};
    int64_t outputs[RELAY_NODE_MAX_PORTS] = {0};
    size_t memory_used;

    if (relay_script_runtime_init(&runtime, 1) ||
        !relay_script_runtime_init(&runtime,
            RELAY_SCRIPT_RUNTIME_DEFAULT_MEMORY_LIMIT) ||
        relay_script_runtime_init(&runtime,
            RELAY_SCRIPT_RUNTIME_DEFAULT_MEMORY_LIMIT) ||
        strcmp(relay_script_runtime_version(), "Lua 5.5") != 0) {
        relay_script_runtime_shutdown(&runtime);
        return 1;
    }
    memory_used = relay_script_runtime_memory_used(&runtime);
    if (memory_used == 0 ||
        memory_used > RELAY_SCRIPT_RUNTIME_DEFAULT_MEMORY_LIMIT) {
        relay_script_runtime_shutdown(&runtime);
        return 1;
    }
    if (!relay_script_runtime_compile(&runtime, valid_source,
            sizeof(valid_source) - 1, 1, &artifact, &schema, &diagnostic) ||
        schema.input_count != 1 || schema.output_count != 1 ||
        schema.inputs[0].type != RELAY_NODE_PORT_TYPE_CLOCK ||
        schema.outputs[0].type != RELAY_NODE_PORT_TYPE_CLOCK ||
        !relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            inputs, outputs, &diagnostic) || outputs[0] != 1 ||
        !relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            inputs, outputs, &diagnostic) || outputs[0] != 2 ||
        relay_script_runtime_compile(&runtime, invalid_source,
            sizeof(invalid_source) - 1, 2, &artifact, &schema, &diagnostic) ||
        artifact.revision != 1) {
        relay_script_artifact_shutdown(&runtime, &artifact);
        relay_script_instance_shutdown(&runtime, &instance);
        relay_script_runtime_shutdown(&runtime);
        return 1;
    }
    relay_script_instance_shutdown(&runtime, &instance);
    if (!relay_script_runtime_compile(&runtime, mutating_source,
            sizeof(mutating_source) - 1, 2, &artifact, &schema, &diagnostic) ||
        relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            inputs, outputs, &diagnostic) ||
        strstr(diagnostic.message, "read-only") == NULL ||
        !relay_script_runtime_compile(&runtime, runaway_source,
            sizeof(runaway_source) - 1, 3, &artifact, &schema, &diagnostic) ||
        relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            NULL, outputs, &diagnostic) ||
        strstr(diagnostic.message, "instruction budget") == NULL) {
        relay_script_artifact_shutdown(&runtime, &artifact);
        relay_script_instance_shutdown(&runtime, &instance);
        relay_script_runtime_shutdown(&runtime);
        return 1;
    }
    relay_script_instance_shutdown(&runtime, &instance);
    if (!relay_script_runtime_compile(&runtime, global_state_source,
            sizeof(global_state_source) - 1, 4, &artifact, &schema,
            &diagnostic) ||
        relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            NULL, outputs, &diagnostic) ||
        strstr(diagnostic.message, "use state") == NULL) {
        relay_script_artifact_shutdown(&runtime, &artifact);
        relay_script_instance_shutdown(&runtime, &instance);
        relay_script_runtime_shutdown(&runtime);
        return 1;
    }
    relay_script_instance_shutdown(&runtime, &instance);
    relay_script_artifact_shutdown(&runtime, &artifact);
    relay_script_runtime_shutdown(&runtime);
    relay_script_runtime_shutdown(&runtime);
    return relay_script_runtime_memory_used(&runtime) == 0 ? 0 : 1;
}
