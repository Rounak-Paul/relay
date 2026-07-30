#include "relay/script_runtime.h"

#include <string.h>

/** Construct one complete caller-owned activation storage view. */
static Relay_ScriptInvocation relay_test_invocation(
    Relay_ItemQueue *input_queues, Relay_ItemQueue *output_queues,
    int64_t *inputs, int64_t *outputs)
{
    return (Relay_ScriptInvocation){
        input_queues, output_queues, inputs, outputs};
}

/** Verify scalar scripting and move-only transactional physical-item queues. */
int relay_script_runtime_test(void)
{
    static const char valid_source[] =
        "input('trigger', Type.TRIGGER)\n"
        "output('trigger_out', Type.TRIGGER)\n"
        "function on_process(state, inputs, outputs)\n"
        "  state.calls = (state.calls or 0) + 1\n"
        "  outputs.trigger_out = (inputs.trigger * state.calls * 2) // 2\n"
        "end\n";
    static const char invalid_source[] =
        "input('coal', Type.COAL)\n"
        "output('trigger', Type.TRIGGER)\n"
        "function on_process(state, inputs, outputs)\n"
        "  outputs.trigger = #inputs.coal / 2\n"
        "end\n";
    static const char missing_handler_source[] =
        "output('trigger_out', Type.TRIGGER)\n"
        "function on_tick(state, inputs, outputs)\n"
        "end\n";
    static const char mutating_source[] =
        "input('trigger', Type.TRIGGER)\n"
        "output('trigger_out', Type.TRIGGER)\n"
        "function on_process(state, inputs, outputs)\n"
        "  inputs.trigger = 0\n"
        "end\n";
    static const char runaway_source[] =
        "output('trigger_out', Type.TRIGGER)\n"
        "function on_process(state, inputs, outputs)\n"
        "  while true do end\n"
        "end\n";
    static const char global_state_source[] =
        "output('count', Type.INTEGER)\n"
        "function on_process(state, inputs, outputs)\n"
        "  leaked_count = (leaked_count or 0) + 1\n"
        "end\n";
    static const char all_types_source[] =
        "input('trigger_value', Type.TRIGGER)\n"
        "input('coal_value', Type.COAL)\n"
        "input('iron_value', Type.IRON_ORE)\n"
        "input('copper_value', Type.COPPER_ORE)\n"
        "input('stone_value', Type.STONE)\n"
        "input('boolean_value', Type.BOOLEAN)\n"
        "input('integer_value', Type.INTEGER)\n"
        "function on_process(state, inputs, outputs)\n"
        "end\n";
    static const char mutating_type_source[] =
        "Type.TRIGGER = Type.COAL\n"
        "function on_process(state, inputs, outputs)\n"
        "end\n";
    static const char string_type_source[] =
        "input('trigger_value', 'trigger')\n"
        "function on_process(state, inputs, outputs)\n"
        "end\n";
    static const char splitter_source[] =
        "input('coal', Type.COAL)\n"
        "output('coal_a', Type.COAL)\n"
        "output('coal_b', Type.COAL)\n"
        "function on_process(state, inputs, outputs)\n"
        "  if #inputs.coal == 0 then return end\n"
        "  local send_a = state.send_a ~= false\n"
        "  local target = send_a and outputs.coal_a or outputs.coal_b\n"
        "  if #target >= target.capacity then return end\n"
        "  target:push(inputs.coal:pop())\n"
        "  state.send_a = not send_a\n"
        "end\n";
    static const char duplicate_source[] =
        "input('coal', Type.COAL)\n"
        "output('coal_a', Type.COAL)\n"
        "output('coal_b', Type.COAL)\n"
        "function on_process(state, inputs, outputs)\n"
        "  local item = inputs.coal:pop()\n"
        "  local alias = item\n"
        "  outputs.coal_a:push(item)\n"
        "  outputs.coal_b:push(alias)\n"
        "end\n";
    static const char unresolved_source[] =
        "input('coal', Type.COAL)\n"
        "function on_process(state, inputs, outputs)\n"
        "  local item = inputs.coal:pop()\n"
        "end\n";
    static const char retained_item_source[] =
        "input('coal', Type.COAL)\n"
        "function on_process(state, inputs, outputs)\n"
        "  state.item = inputs.coal:pop()\n"
        "end\n";
    static const char wrong_type_source[] =
        "input('coal', Type.COAL)\n"
        "output('iron', Type.IRON_ORE)\n"
        "function on_process(state, inputs, outputs)\n"
        "  outputs.iron:push(inputs.coal:pop())\n"
        "end\n";
    static const char full_output_source[] =
        "input('coal', Type.COAL)\n"
        "output('coal_out', Type.COAL)\n"
        "function on_process(state, inputs, outputs)\n"
        "  state.attempts = (state.attempts or 0) + 1\n"
        "  outputs.coal_out:push(inputs.coal:pop())\n"
        "end\n";
    static const char stale_handle_source[] =
        "input('coal', Type.COAL)\n"
        "output('coal_out', Type.COAL)\n"
        "local retained\n"
        "function on_process(state, inputs, outputs)\n"
        "  if retained ~= nil then\n"
        "    outputs.coal_out:push(retained)\n"
        "    return\n"
        "  end\n"
        "  retained = inputs.coal:pop()\n"
        "  outputs.coal_out:push(retained)\n"
        "end\n";
    static const Relay_NodePortType expected_types[] = {
        RELAY_NODE_PORT_TYPE_TRIGGER,
        RELAY_NODE_PORT_TYPE_COAL,
        RELAY_NODE_PORT_TYPE_IRON_ORE,
        RELAY_NODE_PORT_TYPE_COPPER_ORE,
        RELAY_NODE_PORT_TYPE_STONE,
        RELAY_NODE_PORT_TYPE_BOOLEAN,
        RELAY_NODE_PORT_TYPE_INTEGER
    };
    Relay_ScriptRuntime runtime = {0};
    Relay_ScriptArtifact artifact = {0};
    Relay_ScriptInstanceState instance = {0};
    Relay_ScriptSchema schema = {0};
    Relay_ScriptDiagnostic diagnostic = {0};
    Relay_ItemQueue input_queues[RELAY_NODE_MAX_PORTS] = {0};
    Relay_ItemQueue output_queues[RELAY_NODE_MAX_PORTS] = {0};
    int64_t inputs[RELAY_NODE_MAX_PORTS] = {1};
    int64_t outputs[RELAY_NODE_MAX_PORTS] = {0};
    Relay_Item item;
    size_t memory_used;
    size_t index;

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
        !relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            relay_test_invocation(input_queues, output_queues, inputs, outputs),
            &diagnostic) || outputs[0] != 1 ||
        !relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            relay_test_invocation(input_queues, output_queues, inputs, outputs),
            &diagnostic) || outputs[0] != 2 ||
        relay_script_runtime_compile(&runtime, missing_handler_source,
            sizeof(missing_handler_source) - 1, 2, &artifact, &schema,
            &diagnostic) ||
        strstr(diagnostic.message, "on_process") == NULL ||
        relay_script_runtime_compile(&runtime, invalid_source,
            sizeof(invalid_source) - 1, 2, &artifact, &schema, &diagnostic) ||
        artifact.revision != 1) {
        goto failure;
    }
    relay_script_instance_shutdown(&runtime, &instance);
    if (!relay_script_runtime_compile(&runtime, mutating_source,
            sizeof(mutating_source) - 1, 2, &artifact, &schema, &diagnostic) ||
        relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            relay_test_invocation(input_queues, output_queues, inputs, outputs),
            &diagnostic) ||
        strstr(diagnostic.message, "read-only") == NULL ||
        !relay_script_runtime_compile(&runtime, runaway_source,
            sizeof(runaway_source) - 1, 3, &artifact, &schema, &diagnostic) ||
        relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            relay_test_invocation(input_queues, output_queues, inputs, outputs),
            &diagnostic) ||
        strstr(diagnostic.message, "instruction budget") == NULL) {
        goto failure;
    }
    relay_script_instance_shutdown(&runtime, &instance);
    if (!relay_script_runtime_compile(&runtime, global_state_source,
            sizeof(global_state_source) - 1, 4, &artifact, &schema,
            &diagnostic) ||
        relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            relay_test_invocation(input_queues, output_queues, inputs, outputs),
            &diagnostic) ||
        strstr(diagnostic.message, "use state") == NULL) {
        goto failure;
    }
    relay_script_instance_shutdown(&runtime, &instance);
    if (!relay_script_runtime_compile(&runtime, all_types_source,
            sizeof(all_types_source) - 1, 5, &artifact, &schema,
            &diagnostic) ||
        schema.input_count != sizeof(expected_types) /
            sizeof(expected_types[0]) ||
        schema.output_count != 0) {
        goto failure;
    }
    for (index = 0; index < sizeof(expected_types) /
            sizeof(expected_types[0]); index++) {
        if (schema.inputs[index].type != expected_types[index]) {
            goto failure;
        }
    }
    if (relay_script_runtime_compile(&runtime, mutating_type_source,
            sizeof(mutating_type_source) - 1, 6, &artifact, &schema,
            &diagnostic) ||
        strstr(diagnostic.message, "Type enum is read-only") == NULL ||
        artifact.revision != 5 ||
        relay_script_runtime_compile(&runtime, string_type_source,
            sizeof(string_type_source) - 1, 6, &artifact, &schema,
            &diagnostic) ||
        artifact.revision != 5) {
        goto failure;
    }

    relay_script_instance_shutdown(&runtime, &instance);
    if (!relay_script_runtime_compile(&runtime, splitter_source,
            sizeof(splitter_source) - 1, 7, &artifact, &schema, &diagnostic) ||
        !relay_item_queue_push(&input_queues[0],
            (Relay_Item){101, RELAY_NODE_PORT_TYPE_COAL}) ||
        !relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            relay_test_invocation(input_queues, output_queues, inputs, outputs),
            &diagnostic) ||
        input_queues[0].count != 0 || output_queues[0].count != 1 ||
        !relay_item_queue_peek(&output_queues[0], &item) || item.id != 101 ||
        !relay_item_queue_push(&input_queues[0],
            (Relay_Item){102, RELAY_NODE_PORT_TYPE_COAL}) ||
        !relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            relay_test_invocation(input_queues, output_queues, inputs, outputs),
            &diagnostic) ||
        output_queues[0].count != 1 || output_queues[1].count != 1 ||
        !relay_item_queue_peek(&output_queues[1], &item) || item.id != 102) {
        goto failure;
    }

    relay_script_instance_shutdown(&runtime, &instance);
    (void)memset(input_queues, 0, sizeof(input_queues));
    (void)memset(output_queues, 0, sizeof(output_queues));
    if (!relay_script_runtime_compile(&runtime, duplicate_source,
            sizeof(duplicate_source) - 1, 8, &artifact, &schema,
            &diagnostic) ||
        !relay_item_queue_push(&input_queues[0],
            (Relay_Item){201, RELAY_NODE_PORT_TYPE_COAL}) ||
        relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            relay_test_invocation(input_queues, output_queues, inputs, outputs),
            &diagnostic) ||
        strstr(diagnostic.message, "already moved") == NULL ||
        input_queues[0].count != 1 || output_queues[0].count != 0 ||
        output_queues[1].count != 0 || instance.initialized) {
        goto failure;
    }

    relay_script_instance_shutdown(&runtime, &instance);
    if (!relay_script_runtime_compile(&runtime, unresolved_source,
            sizeof(unresolved_source) - 1, 9, &artifact, &schema,
            &diagnostic) ||
        relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            relay_test_invocation(input_queues, output_queues, inputs, outputs),
            &diagnostic) ||
        strstr(diagnostic.message, "pushed exactly once") == NULL ||
        input_queues[0].count != 1 || instance.initialized) {
        goto failure;
    }

    relay_script_instance_shutdown(&runtime, &instance);
    if (!relay_script_runtime_compile(&runtime, retained_item_source,
            sizeof(retained_item_source) - 1, 10, &artifact, &schema,
            &diagnostic) ||
        relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            relay_test_invocation(input_queues, output_queues, inputs, outputs),
            &diagnostic) ||
        strstr(diagnostic.message, "bounded scalar") == NULL ||
        input_queues[0].count != 1 || instance.initialized) {
        goto failure;
    }

    relay_script_instance_shutdown(&runtime, &instance);
    if (!relay_script_runtime_compile(&runtime, wrong_type_source,
            sizeof(wrong_type_source) - 1, 11, &artifact, &schema,
            &diagnostic) ||
        relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            relay_test_invocation(input_queues, output_queues, inputs, outputs),
            &diagnostic) ||
        strstr(diagnostic.message, "does not match") == NULL ||
        input_queues[0].count != 1 || output_queues[0].count != 0 ||
        instance.initialized) {
        goto failure;
    }

    relay_script_instance_shutdown(&runtime, &instance);
    (void)memset(input_queues, 0, sizeof(input_queues));
    (void)memset(output_queues, 0, sizeof(output_queues));
    for (index = 0; index < RELAY_ITEM_QUEUE_CAPACITY; index++) {
        if (!relay_item_queue_push(&output_queues[0],
                (Relay_Item){300 + index, RELAY_NODE_PORT_TYPE_COAL})) {
            goto failure;
        }
    }
    if (!relay_item_queue_push(&input_queues[0],
            (Relay_Item){399, RELAY_NODE_PORT_TYPE_COAL}) ||
        !relay_script_runtime_compile(&runtime, full_output_source,
            sizeof(full_output_source) - 1, 12, &artifact, &schema,
            &diagnostic) ||
        relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            relay_test_invocation(input_queues, output_queues, inputs, outputs),
            &diagnostic) ||
        strstr(diagnostic.message, "full") == NULL ||
        input_queues[0].count != 1 ||
        output_queues[0].count != RELAY_ITEM_QUEUE_CAPACITY ||
        instance.initialized) {
        goto failure;
    }

    relay_script_instance_shutdown(&runtime, &instance);
    (void)memset(input_queues, 0, sizeof(input_queues));
    (void)memset(output_queues, 0, sizeof(output_queues));
    if (!relay_item_queue_push(&input_queues[0],
            (Relay_Item){401, RELAY_NODE_PORT_TYPE_COAL}) ||
        !relay_script_runtime_compile(&runtime, stale_handle_source,
            sizeof(stale_handle_source) - 1, 13, &artifact, &schema,
            &diagnostic) ||
        !relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            relay_test_invocation(input_queues, output_queues, inputs, outputs),
            &diagnostic) ||
        input_queues[0].count != 0 || output_queues[0].count != 1 ||
        relay_script_runtime_invoke(&runtime, &artifact, &instance, &schema,
            relay_test_invocation(input_queues, output_queues, inputs, outputs),
            &diagnostic) ||
        strstr(diagnostic.message, "expired") == NULL ||
        input_queues[0].count != 0 || output_queues[0].count != 1) {
        goto failure;
    }

    relay_script_artifact_shutdown(&runtime, &artifact);
    relay_script_instance_shutdown(&runtime, &instance);
    relay_script_runtime_shutdown(&runtime);
    relay_script_runtime_shutdown(&runtime);
    return relay_script_runtime_memory_used(&runtime) == 0 ? 0 : 1;

failure:
    relay_script_artifact_shutdown(&runtime, &artifact);
    relay_script_instance_shutdown(&runtime, &instance);
    relay_script_runtime_shutdown(&runtime);
    return 1;
}
