#include "relay/game.h"
#include "relay/node_renderer.h"

#include <string.h>

/** Verify clock-driven coal production, typed wires, and script properties. */
int relay_game_test(void)
{
    Relay_Game game = {0};
    Relay_ScriptRuntime scripts = {0};
    Relay_Node *miner;
    Relay_Node *clock;
    Relay_NodeValue value;
    Relay_NodeValueType value_type;
    Relay_NodeRenderCard card;
    const Relay_NodeDefinition *clock_definition;
    Relay_NodeVisual clock_port_visual;
    Relay_NodeVisual coal_port_visual;
    size_t index;

    clock_definition = relay_node_definition_find(RELAY_NODE_DEFINITION_CLOCK);
    clock_port_visual = relay_node_renderer_port_visual(
        RELAY_NODE_PORT_TYPE_CLOCK);
    coal_port_visual = relay_node_renderer_port_visual(RELAY_NODE_PORT_TYPE_COAL);
    if (!relay_script_runtime_init(&scripts,
            RELAY_SCRIPT_RUNTIME_DEFAULT_MEMORY_LIMIT) ||
        !relay_game_init(&game, &scripts) || game.currency != 100 ||
        game.nodes.count != 1 ||
        relay_node_definition_find_key("process.coal_miner") == NULL ||
        clock_definition == NULL || clock_definition->output_count != 1 ||
        clock_definition->outputs[0].type != RELAY_NODE_PORT_TYPE_CLOCK ||
        strcmp(clock_definition->outputs[0].key, "clock") != 0 ||
        clock_port_visual.color == coal_port_visual.color) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    miner = &game.nodes.nodes[0];
    card = relay_node_renderer_card(miner);
    if (card.definition == NULL || card.definition->input_count != 2 ||
        card.definition->output_count != 1 || card.height != 7 ||
        relay_game_handle_input(&game, RELAY_GAME_INPUT_CONFIRM) !=
            RELAY_GAME_ACTION_PURCHASED || game.nodes.count != 2) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    clock = &game.nodes.nodes[1];
    if (clock->definition_id != RELAY_NODE_DEFINITION_CLOCK ||
        game.focused_node_id != clock->id ||
        relay_game_connect_nodes(&game, miner->id, 0, miner->id, 0) ||
        !relay_game_connect_nodes(&game,
            clock->id, 0, miner->id, 0) || !relay_game_connect_nodes(&game,
            miner->id, 0, miner->id, 1) || !relay_node_property_get(clock,
            "clock.period", &value, &value_type) ||
        value_type != RELAY_NODE_VALUE_INTEGER || value.integer != 2) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    for (index = 0; index < 34; index++) {
        if (!relay_game_step(&game)) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
    }
    if (!relay_node_property_get(miner, "resource.coal", &value, &value_type) ||
        value.integer != 1 || miner->progress != 0 || miner->fuel_coal != 1 ||
        relay_game_connect_nodes(&game, miner->id, 0, clock->id, 0)) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    if (relay_game_handle_input(&game, RELAY_GAME_INPUT_CONFIRM) !=
            RELAY_GAME_ACTION_PURCHASED || !relay_game_connect_nodes(&game,
            game.focused_node_id, 0, miner->id, 0) ||
        relay_node_world_connection_to(&game.nodes, miner->id, 0) == NULL ||
        relay_node_world_connection_to(&game.nodes, miner->id, 0)->source_node_id !=
            game.focused_node_id) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    if (relay_game_handle_input(&game, RELAY_GAME_INPUT_NEXT_CLOCK_RATE) !=
            RELAY_GAME_ACTION_NONE || relay_node_world_find(&game.nodes,
                game.focused_node_id)->clock_period != 4 ||
        relay_game_handle_input(&game, RELAY_GAME_INPUT_TOGGLE_PANEL_TAB) !=
            RELAY_GAME_ACTION_NONE || game.active_tab !=
                RELAY_GAME_PANEL_TAB_INSPECTOR ||
        relay_game_handle_input(&game, RELAY_GAME_INPUT_TOGGLE_PANEL_TAB) !=
            RELAY_GAME_ACTION_NONE || game.active_tab !=
                RELAY_GAME_PANEL_TAB_BLUEPRINTS ||
        relay_game_handle_input(&game, RELAY_GAME_INPUT_TOGGLE_PANEL_TAB) !=
            RELAY_GAME_ACTION_NONE || game.active_tab != RELAY_GAME_PANEL_TAB_SHOP ||
        !relay_game_focus_node(&game,
                    miner->id) || game.focused_node_id != miner->id ||
        game.active_tab != RELAY_GAME_PANEL_TAB_INSPECTOR ||
        relay_game_handle_input(&game, RELAY_GAME_INPUT_TOGGLE_PANEL_TAB) !=
            RELAY_GAME_ACTION_NONE || game.active_tab !=
                RELAY_GAME_PANEL_TAB_BLUEPRINTS ||
        relay_game_handle_input(&game, RELAY_GAME_INPUT_TOGGLE_PANEL_TAB) !=
            RELAY_GAME_ACTION_NONE || game.active_tab != RELAY_GAME_PANEL_TAB_SHOP ||
        relay_game_handle_input(&game, RELAY_GAME_INPUT_TOGGLE_MAP) !=
            RELAY_GAME_ACTION_NONE || !relay_game_back(&game) ||
        !relay_game_move_node(&game, miner->id, -9, -7) || miner->grid_x != -9 ||
        miner->grid_y != -7) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    relay_game_shutdown(&game);
    relay_script_runtime_shutdown(&scripts);
    return 0;
}
