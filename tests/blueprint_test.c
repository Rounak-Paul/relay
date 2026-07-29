#include "relay/game.h"

#include <string.h>

/** Return one visible node matching a Blueprint or built-in definition. */
static Relay_Node *relay_test_visible_node(Relay_NodeWorld *world,
    Relay_NodeDefinitionId definition_id, Relay_BlueprintId blueprint_id)
{
    size_t index;

    for (index = 0; index < world->count; index++) {
        Relay_Node *node = &world->nodes[index];

        if (node->module_instance_id == 0 &&
            node->definition_id == definition_id &&
            node->blueprint_id == blueprint_id) {
            return node;
        }
    }
    return NULL;
}

/** Verify scene synchronization keeps one canonical architecture separator. */
static bool relay_test_compact_architecture_source(void)
{
    static const char expected_created[] =
        "input(\"trigger\", Type.TRIGGER)\n"
        "output(\"trigger_out\", Type.TRIGGER)\n"
        "\n"
        "local n3 = instance(\"source.coal_miner\", { x = 90, y = 0 })\n"
        "\n"
        "function on_process(inputs, state)\n"
        "  state.activations = (state.activations or 0) + 1\n"
        "  return { trigger_out = inputs.trigger or 0 }\n"
        "end\n";
    static const char expected_moved[] =
        "input(\"trigger\", Type.TRIGGER)\n"
        "output(\"trigger_out\", Type.TRIGGER)\n"
        "\n"
        "local n3 = instance(\"source.coal_miner\", { x = 95, y = 0 })\n"
        "\n"
        "function on_process(inputs, state)\n"
        "  state.activations = (state.activations or 0) + 1\n"
        "  return { trigger_out = inputs.trigger or 0 }\n"
        "end\n";
    Relay_ScriptRuntime scripts = {0};
    Relay_Game game = {0};
    Relay_Blueprint *blueprint;
    Relay_NodeId miner_id;
    bool valid = false;

    if (!relay_script_runtime_init(&scripts,
            RELAY_SCRIPT_RUNTIME_DEFAULT_MEMORY_LIMIT) ||
        !relay_game_init(&game, &scripts) ||
        !relay_game_create_blueprint(&game) ||
        !relay_game_select_panel_tab(&game, RELAY_GAME_PANEL_TAB_SHOP) ||
        relay_game_handle_input(&game, RELAY_GAME_INPUT_NEXT) !=
            RELAY_GAME_ACTION_NONE ||
        relay_game_handle_input(&game, RELAY_GAME_INPUT_CONFIRM) !=
            RELAY_GAME_ACTION_PURCHASED) {
        goto cleanup;
    }
    blueprint = relay_game_active_blueprint(&game);
    miner_id = game.focused_node_id;
    if (blueprint == NULL || strcmp(blueprint->source, expected_created) != 0 ||
        !relay_game_move_node(&game, miner_id, 5, 0) ||
        strcmp(blueprint->source, expected_moved) != 0) {
        goto cleanup;
    }
    valid = true;

cleanup:
    relay_game_shutdown(&game);
    relay_script_runtime_shutdown(&scripts);
    return valid;
}

/** Verify nested visual port maps compile and execute as one typed module. */
int relay_blueprint_test(void)
{
    static const char incompatible_source[] =
        "input('coal', Type.COAL)\n"
        "output('coal_out', Type.COAL)\n"
        "function on_process(inputs, state)\n"
        "  return { coal_out = inputs.coal or 0 }\n"
        "end\n";
    Relay_ScriptRuntime scripts = {0};
    Relay_Game game = {0};
    Relay_Blueprint *child;
    Relay_Blueprint *parent;
    Relay_BlueprintId child_id;
    Relay_BlueprintId parent_id;
    Relay_NodeId child_component_id;
    Relay_NodeId module_one_id;
    Relay_NodeId module_two_id;
    Relay_NodeWorld *root;
    Relay_Node *miner;
    Relay_Node *module_one;
    Relay_Node *module_two;
    Relay_Node *timer;
    Relay_NodeValue enabled_value;
    uint64_t deployed_revision;
    size_t deployed_plan_nodes;
    size_t output_connection_index = SIZE_MAX;
    size_t index;
    size_t initialized_processes = 0;
    bool activation_counts_valid = true;

    if (!relay_test_compact_architecture_source() ||
        !relay_script_runtime_init(&scripts,
            RELAY_SCRIPT_RUNTIME_DEFAULT_MEMORY_LIMIT) ||
        !relay_game_init(&game, &scripts) ||
        !relay_game_create_blueprint(&game)) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    child = relay_game_active_blueprint(&game);
    child_id = child == NULL ? 0 : child->id;
    if (child == NULL || child->scene.count != 2 ||
        child->input_boundary_node_id == 0 ||
        child->output_boundary_node_id == 0 || !child->plan.valid ||
        child->plan.node_count != 1 ||
        child->plan.input_binding_count != 1 ||
        child->plan.output_binding_count != 1 ||
        !relay_game_create_blueprint(&game)) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    parent = relay_game_active_blueprint(&game);
    parent_id = parent == NULL ? 0 : parent->id;
    if (parent == NULL || !relay_game_back(&game) ||
        game.active_workspace != 0 ||
        !relay_game_activate_workspace(&game, 2) ||
        !relay_game_add_blueprint(&game, child_id)) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    child_component_id = game.focused_node_id;
    if (!relay_game_connect_nodes(&game, parent->input_boundary_node_id, 0,
            child_component_id, 0) ||
        !relay_game_connect_nodes(&game, child_component_id, 0,
            parent->output_boundary_node_id, 0) ||
        !parent->plan.valid || parent->plan.node_count != 2 ||
        parent->plan.input_binding_count != 2 ||
        parent->plan.output_binding_count != 1) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    if (strstr(parent->source,
            "local n3 = instance(\"blueprint.script_1\", { x = 90, y = 0 })") ==
            NULL ||
        strstr(parent->source,
            "connect(inputs.trigger, n3.inputs.trigger)") == NULL ||
        strstr(parent->source,
            "connect(n3.outputs.trigger_out, outputs.trigger_out)") == NULL ||
        !relay_game_move_node(&game, child_component_id, 5, 0) ||
        strstr(parent->source,
            "local n3 = instance(\"blueprint.script_1\", { x = 95, y = 0 })") ==
            NULL) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    {
        char *layout = strstr(parent->source, "x = 95, y = 0");

        if (layout == NULL) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
        layout[5] = '6';
        parent->revision++;
        parent->dirty = true;
        if (!relay_blueprint_compile(&game.blueprints, parent) ||
            relay_node_world_find(&parent->scene, child_component_id) == NULL ||
            relay_node_world_find(&parent->scene,
                child_component_id)->grid_x != 96 ||
            parent->scene.connection_count != 2) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
    }
    {
        char *port_map = strstr(parent->source,
            "connect(n3.outputs.trigger_out");
        const uint64_t valid_revision = parent->compiled_revision;

        if (port_map == NULL) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
        port_map[19] = 'x';
        parent->revision++;
        parent->dirty = true;
        if (relay_blueprint_compile(&game.blueprints, parent) ||
            parent->compiled_revision != valid_revision ||
            parent->scene.connection_count != 2 ||
            relay_node_world_find(&parent->scene,
                child_component_id)->grid_x != 96) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
        port_map[19] = 't';
        parent->revision++;
        if (!relay_blueprint_compile(&game.blueprints, parent)) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
    }
    if (!relay_game_connect_nodes(&game, parent->input_boundary_node_id, 0,
            parent->output_boundary_node_id, 0) ||
        !parent->plan.output_bindings[0].source_is_module_input ||
        parent->plan.output_bindings[0].source_module_input_port_index != 0 ||
        !relay_game_connect_nodes(&game, child_component_id, 0,
            parent->output_boundary_node_id, 0) ||
        parent->plan.output_bindings[0].source_is_module_input) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    deployed_plan_nodes = parent->plan.node_count;
    for (index = 0; index < parent->scene.connection_count; index++) {
        if (parent->scene.connections[index].destination_node_id ==
                parent->output_boundary_node_id) {
            output_connection_index = index;
            break;
        }
    }
    if (output_connection_index == SIZE_MAX) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    parent->scene.connections[
        output_connection_index].destination_node_id =
        0;
    if (relay_blueprint_rebuild_plan(&game.blueprints, parent) ||
        !parent->plan.valid || parent->plan.node_count != deployed_plan_nodes) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    parent->scene.connections[
        output_connection_index].destination_node_id =
        parent->output_boundary_node_id;
    if (!relay_blueprint_rebuild_plan(&game.blueprints, parent) ||
        !relay_game_activate_workspace(&game, 1)) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    {
        const size_t child_scene_count = child->scene.count;
        const Relay_NodeId child_scene_next_id = child->scene.next_id;

        if (relay_game_add_blueprint(&game, parent_id) ||
            child->scene.count != child_scene_count ||
            child->scene.next_id != child_scene_next_id) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
    }
    if (!relay_game_activate_workspace(&game, 0) ||
        !relay_game_add_blueprint(&game, parent_id) ||
        (module_one_id = game.focused_node_id) == 0 ||
        !relay_game_add_blueprint(&game, parent_id) ||
        (module_two_id = game.focused_node_id) == 0 ||
        module_one_id == module_two_id ||
        !relay_game_select_panel_tab(&game, RELAY_GAME_PANEL_TAB_SHOP) ||
        relay_game_handle_input(&game, RELAY_GAME_INPUT_CONFIRM) !=
            RELAY_GAME_ACTION_PURCHASED) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    root = &game.nodes;
    miner = relay_test_visible_node(root, RELAY_NODE_DEFINITION_COAL_MINER, 0);
    module_one = relay_node_world_find(root, module_one_id);
    module_two = relay_node_world_find(root, module_two_id);
    timer = relay_test_visible_node(root, RELAY_NODE_DEFINITION_TIMER, 0);
    if (miner == NULL || module_one == NULL || module_two == NULL ||
        timer == NULL || root->module_input_binding_count != 4 ||
        root->module_output_binding_count != 2 ||
        relay_game_connect_nodes(&game, miner->id, 0, module_one->id, 0) ||
        !relay_game_connect_nodes(&game, timer->id, 0, module_one->id, 0) ||
        !relay_game_connect_nodes(&game, timer->id, 0, module_two->id, 0) ||
        relay_game_connect_nodes(&game, module_one->id, 0, miner->id, 0) ||
        relay_game_connect_nodes(&game, miner->id, 0, miner->id, 0)) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    for (index = 0; index < 65; index++) {
        if (!relay_game_step(&game)) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
    }
    for (index = 0; index < root->count; index++) {
        if ((root->nodes[index].module_instance_id == module_one->id ||
                root->nodes[index].module_instance_id == module_two->id) &&
            root->nodes[index].runtime_kind ==
                RELAY_NODE_RUNTIME_BLUEPRINT_PROCESS &&
            root->nodes[index].script_state.initialized) {
            initialized_processes++;
            if (root->nodes[index].process_activations != 2) {
                activation_counts_valid = false;
            }
        }
    }
    if (miner->produced < 1 || initialized_processes != 4 ||
        !activation_counts_valid || module_one->output_values[0] != 0 ||
        module_two->output_values[0] != 0) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    enabled_value.boolean = false;
    if (!relay_node_property_set(module_one, "node.enabled",
            RELAY_NODE_VALUE_BOOLEAN, enabled_value)) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    for (index = 0; index < RELAY_TIMER_DEFAULT_INTERVAL_STEPS; index++) {
        if (!relay_game_step(&game)) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
    }
    for (index = 0; index < root->count; index++) {
        const Relay_Node *node = &root->nodes[index];

        if (node->runtime_kind != RELAY_NODE_RUNTIME_BLUEPRINT_PROCESS) {
            continue;
        }
        if ((node->module_instance_id == module_one->id &&
                (node->process_activations != 2 ||
                    node->output_values[0] != 0)) ||
            (node->module_instance_id == module_two->id &&
                node->process_activations != 3)) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
    }

    deployed_revision = parent->compiled_revision;
    (void)memcpy(parent->source, incompatible_source,
        sizeof(incompatible_source));
    parent->source_size = sizeof(incompatible_source) - 1;
    parent->cursor = parent->source_size;
    parent->revision++;
    parent->dirty = true;
    if (relay_blueprint_compile(&game.blueprints, parent) ||
        parent->compiled_revision != deployed_revision ||
        !parent->artifact.installed || !parent->dirty ||
        strstr(parent->diagnostic.message, "ports") == NULL) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    relay_game_shutdown(&game);
    relay_script_runtime_shutdown(&scripts);
    return relay_script_runtime_memory_used(&scripts) == 0 ? 0 : 1;
}
