#include "relay/game.h"

enum {
    RELAY_GAME_STARTING_CURRENCY = 100,
    RELAY_GAME_CLOCK_PRICE = 30,
    RELAY_GAME_COAL_REQUIRED_PULSES = 16,
    RELAY_GAME_NODE_SPACING_X = 30
};

/** Clock is the first purchased gameplay module. */
static const Relay_ShopOffer relay_shop_offers[] = {
    {RELAY_NODE_DEFINITION_CLOCK, RELAY_GAME_CLOCK_PRICE}
};

/** Return the adjacent editable clock period, wrapping through valid rates. */
static int64_t relay_game_next_clock_period(int64_t period, int direction)
{
    static const int64_t periods[] = {2, 4, 8, 16, 32, 64, 128};
    size_t index;

    for (index = 0; index < sizeof(periods) / sizeof(periods[0]); index++) {
        if (periods[index] == period) {
            const size_t count = sizeof(periods) / sizeof(periods[0]);
            return periods[(index + count + direction) % count];
        }
    }
    return periods[0];
}

/** Create a deterministic free grid position for a purchased gameplay module. */
static Relay_NodeId relay_game_create_node(Relay_Game *game,
    Relay_NodeDefinitionId definition_id)
{
    const size_t index = game->nodes.count;

    return relay_node_world_create(&game->nodes, definition_id,
        (int64_t)(index + 1) * RELAY_GAME_NODE_SPACING_X, 0);
}

/** Purchase the currently selected module when the player can afford it. */
static Relay_GameActionResult relay_game_purchase_selected(Relay_Game *game)
{
    const Relay_ShopOffer *offer = relay_game_shop_offer_at(game->selected_offer);

    if (offer == NULL) {
        return RELAY_GAME_ACTION_CREATION_FAILED;
    }
    if (game->currency < offer->price) {
        return RELAY_GAME_ACTION_INSUFFICIENT_CURRENCY;
    }
    game->focused_node_id = relay_game_create_node(game, offer->definition_id);
    if (game->focused_node_id == 0) {
        return RELAY_GAME_ACTION_CREATION_FAILED;
    }
    game->currency -= offer->price;
    return RELAY_GAME_ACTION_PURCHASED;
}

/** Update one clock output before consumers read this fixed gameplay tick. */
static void relay_game_step_clock(Relay_Node *node)
{
    node->output_value = 0;
    if (!node->enabled) {
        return;
    }
    node->clock_phase++;
    if (node->clock_phase >= node->clock_period) {
        node->clock_phase = 0;
        node->output_value = 1;
        node->produced++;
    }
}

/** Return a connected source value for a destination input in this tick. */
static int64_t relay_game_input_value(const Relay_Game *game,
    const Relay_Node *destination, size_t destination_port_index)
{
    const Relay_NodeConnection *connection = relay_node_world_connection_to(
        &game->nodes, destination->id, destination_port_index);
    const Relay_Node *source;

    if (connection == NULL) {
        return 0;
    }
    source = relay_node_world_find_const(&game->nodes,
        connection->source_node_id);
    if (source == NULL) {
        return 0;
    }
    return source == destination ? source->previous_output_value :
        source->output_value;
}

/** Update a coal miner from clock pulses and expose each completed coal output. */
static void relay_game_step_coal_miner(Relay_Game *game, Relay_Node *node)
{
    const int64_t fuel_input = relay_game_input_value(game, node, 1);

    node->output_value = 0;
    if (fuel_input > 0) {
        node->fuel_coal += fuel_input;
    }
    if (!node->enabled || relay_game_input_value(game, node, 0) <= 0) {
        return;
    }
    if (!node->processing) {
        if (node->fuel_coal <= 0) {
            return;
        }
        node->fuel_coal--;
        node->processing = true;
    }
    node->progress++;
    if (node->progress >= RELAY_GAME_COAL_REQUIRED_PULSES) {
        node->progress = 0;
        node->produced++;
        node->output_value = 1;
        node->processing = false;
    }
}

bool relay_game_init(Relay_Game *game)
{
    Relay_NodeId coal_miner_id;

    if (game == NULL || !relay_node_world_init(&game->nodes)) {
        return false;
    }
    coal_miner_id = relay_node_world_create(&game->nodes,
        RELAY_NODE_DEFINITION_COAL_MINER, 0, 0);
    if (coal_miner_id == 0) {
        relay_node_world_shutdown(&game->nodes);
        return false;
    }
    game->currency = RELAY_GAME_STARTING_CURRENCY;
    game->active_tab = RELAY_GAME_PANEL_TAB_SHOP;
    game->workspace_mode = RELAY_GAME_WORKSPACE_GRAPH;
    game->focused_node_id = coal_miner_id;
    return true;
}

Relay_GameActionResult relay_game_handle_input(Relay_Game *game,
    Relay_GameInput input)
{
    const size_t offer_count = relay_game_shop_offer_count();
    Relay_Node *focused;
    Relay_NodeValue value;

    if (game == NULL || offer_count == 0) {
        return RELAY_GAME_ACTION_NONE;
    }
    if (input == RELAY_GAME_INPUT_TOGGLE_PANEL_TAB) {
        game->active_tab = game->active_tab == RELAY_GAME_PANEL_TAB_SHOP ?
            RELAY_GAME_PANEL_TAB_INSPECTOR : RELAY_GAME_PANEL_TAB_SHOP;
        game->last_action = RELAY_GAME_ACTION_NONE;
    } else if (input == RELAY_GAME_INPUT_PREVIOUS &&
        game->active_tab == RELAY_GAME_PANEL_TAB_SHOP) {
        game->selected_offer = game->selected_offer == 0 ? offer_count - 1 :
            game->selected_offer - 1;
        game->last_action = RELAY_GAME_ACTION_NONE;
    } else if (input == RELAY_GAME_INPUT_NEXT &&
        game->active_tab == RELAY_GAME_PANEL_TAB_SHOP) {
        game->selected_offer = (game->selected_offer + 1) % offer_count;
        game->last_action = RELAY_GAME_ACTION_NONE;
    } else if (input == RELAY_GAME_INPUT_CONFIRM &&
        game->active_tab == RELAY_GAME_PANEL_TAB_SHOP) {
        game->last_action = relay_game_purchase_selected(game);
    } else if (input == RELAY_GAME_INPUT_TOGGLE_MAP) {
        game->workspace_mode = game->workspace_mode == RELAY_GAME_WORKSPACE_GRAPH ?
            RELAY_GAME_WORKSPACE_MAP : RELAY_GAME_WORKSPACE_GRAPH;
        game->last_action = RELAY_GAME_ACTION_NONE;
    } else if (input == RELAY_GAME_INPUT_PREVIOUS_CLOCK_RATE ||
        input == RELAY_GAME_INPUT_NEXT_CLOCK_RATE) {
        focused = relay_node_world_find(&game->nodes, game->focused_node_id);
        if (focused != NULL && focused->definition_id == RELAY_NODE_DEFINITION_CLOCK) {
            value.integer = relay_game_next_clock_period(focused->clock_period,
                input == RELAY_GAME_INPUT_NEXT_CLOCK_RATE ? 1 : -1);
            (void)relay_node_property_set(focused, "clock.period",
                RELAY_NODE_VALUE_INTEGER, value);
        }
        game->last_action = RELAY_GAME_ACTION_NONE;
    }
    return game->last_action;
}

bool relay_game_back(Relay_Game *game)
{
    if (game == NULL || game->workspace_mode == RELAY_GAME_WORKSPACE_GRAPH) {
        return false;
    }
    game->workspace_mode = RELAY_GAME_WORKSPACE_GRAPH;
    return true;
}

/** Add a drag delta without overflowing a signed world coordinate. */
static int64_t relay_game_add_delta(int64_t coordinate, int delta)
{
    if (delta > 0 && coordinate > INT64_MAX - delta) {
        return INT64_MAX;
    }
    if (delta < 0 && coordinate < INT64_MIN - delta) {
        return INT64_MIN;
    }
    return coordinate + delta;
}

bool relay_game_move_node(Relay_Game *game, Relay_NodeId id, int delta_x,
    int delta_y)
{
    Relay_Node *node;

    if (game == NULL) {
        return false;
    }
    node = relay_node_world_find(&game->nodes, id);
    return node != NULL && relay_node_world_move(&game->nodes, id,
        relay_game_add_delta(node->grid_x, delta_x),
        relay_game_add_delta(node->grid_y, delta_y));
}

bool relay_game_focus_node(Relay_Game *game, Relay_NodeId id)
{
    if (game == NULL || relay_node_world_find(&game->nodes, id) == NULL) {
        return false;
    }
    game->focused_node_id = id;
    game->active_tab = RELAY_GAME_PANEL_TAB_INSPECTOR;
    return true;
}

bool relay_game_connect_nodes(Relay_Game *game, Relay_NodeId source_node_id,
    size_t source_port_index, Relay_NodeId destination_node_id,
    size_t destination_port_index)
{
    if (game == NULL || !relay_node_world_connect(&game->nodes, source_node_id,
            source_port_index, destination_node_id, destination_port_index)) {
        return false;
    }
    return true;
}

bool relay_game_step(Relay_Game *game)
{
    size_t index;

    if (game == NULL) {
        return false;
    }
    for (index = 0; index < game->nodes.count; index++) {
        Relay_Node *node = &game->nodes.nodes[index];

        node->previous_output_value = node->output_value;
        node->output_value = 0;

        if (node->definition_id == RELAY_NODE_DEFINITION_CLOCK) {
            relay_game_step_clock(node);
        }
    }
    for (index = 0; index < game->nodes.count; index++) {
        Relay_Node *node = &game->nodes.nodes[index];

        if (node->definition_id == RELAY_NODE_DEFINITION_COAL_MINER) {
            relay_game_step_coal_miner(game, node);
        }
    }
    game->simulation_tick++;
    return true;
}

size_t relay_game_shop_offer_count(void)
{
    return sizeof(relay_shop_offers) / sizeof(relay_shop_offers[0]);
}

const Relay_ShopOffer *relay_game_shop_offer_at(size_t index)
{
    return index < relay_game_shop_offer_count() ? &relay_shop_offers[index] : NULL;
}

const Relay_NodeDefinition *relay_game_shop_offer_definition(
    const Relay_ShopOffer *offer)
{
    return offer == NULL ? NULL : relay_node_definition_find(offer->definition_id);
}

const char *relay_game_action_result_label(Relay_GameActionResult result)
{
    switch (result) {
    case RELAY_GAME_ACTION_NONE: return "";
    case RELAY_GAME_ACTION_PURCHASED: return "Module added";
    case RELAY_GAME_ACTION_INSUFFICIENT_CURRENCY: return "Insufficient currency";
    case RELAY_GAME_ACTION_CREATION_FAILED: return "Unable to create module";
    case RELAY_GAME_ACTION_CONNECTION_REJECTED: return "Connection rejected";
    }
    return "Unknown action";
}

void relay_game_shutdown(Relay_Game *game)
{
    if (game != NULL) {
        relay_node_world_shutdown(&game->nodes);
        *game = (Relay_Game){0};
    }
}
