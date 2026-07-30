#include "relay/game.h"
#include "relay/script_language.h"

#include <stdio.h>
#include <string.h>

enum {
    RELAY_GAME_STARTING_CURRENCY = 100,
    RELAY_GAME_TIMER_PRICE = 30,
    RELAY_GAME_COAL_MINER_PRICE = 20,
    RELAY_GAME_IRON_MINER_PRICE = 30,
    RELAY_GAME_COPPER_MINER_PRICE = 30,
    RELAY_GAME_STONE_MINER_PRICE = 20,
    RELAY_GAME_STONE_FURNACE_PRICE = 40,
    RELAY_GAME_STORAGE_PRICE = 20,
    RELAY_GAME_NODE_SPACING_X = 30
};

/** Purchasable built-in definitions and their gameplay-balanced prices. */
static const Relay_ShopOffer relay_shop_offers[] = {
    {RELAY_NODE_DEFINITION_TIMER, RELAY_GAME_TIMER_PRICE},
    {RELAY_NODE_DEFINITION_COAL_MINER, RELAY_GAME_COAL_MINER_PRICE},
    {RELAY_NODE_DEFINITION_IRON_MINER, RELAY_GAME_IRON_MINER_PRICE},
    {RELAY_NODE_DEFINITION_COPPER_MINER, RELAY_GAME_COPPER_MINER_PRICE},
    {RELAY_NODE_DEFINITION_STONE_MINER, RELAY_GAME_STONE_MINER_PRICE},
    {RELAY_NODE_DEFINITION_STONE_FURNACE, RELAY_GAME_STONE_FURNACE_PRICE},
    {RELAY_NODE_DEFINITION_STORAGE, RELAY_GAME_STORAGE_PRICE}
};

/** Store focus in the active workspace and its shared renderer-facing field. */
static void relay_game_set_focused_node(Relay_Game *game, Relay_NodeId node_id)
{
    game->focused_node_id = node_id;
    if (game->active_workspace == 0) {
        game->root_focused_node_id = node_id;
    } else if (game->active_workspace <= game->blueprints.count) {
        game->blueprints.blueprints[
            game->active_workspace - 1].focused_node_id = node_id;
    }
}

/** Return the adjacent timer interval, wrapping through valid durations. */
static int64_t relay_game_next_timer_interval(int64_t interval, int direction)
{
    const size_t count = relay_timer_interval_count();
    size_t index;

    for (index = 0; index < count; index++) {
        if (relay_timer_interval_at(index) == interval) {
            const size_t next = direction < 0 ?
                (index == 0 ? count - 1 : index - 1) :
                (index + 1) % count;

            return relay_timer_interval_at(next);
        }
    }
    return relay_timer_interval_at(0);
}

/** Create a deterministic free grid position for a purchased gameplay module. */
static Relay_NodeId relay_game_create_node(Relay_Game *game,
    Relay_NodeDefinitionId definition_id)
{
    Relay_NodeWorld *world = relay_game_active_world(game);
    const size_t index = world == NULL ? 0 : world->count;

    return relay_node_world_create(world, definition_id,
        (int64_t)(index + 1) * RELAY_GAME_NODE_SPACING_X, 0);
}

/** Purchase the currently selected module when the player can afford it. */
static Relay_GameActionResult relay_game_purchase_selected(Relay_Game *game)
{
    const Relay_ShopOffer *offer = relay_game_shop_offer_at(game->selected_offer);
    Relay_NodeWorld *world;
    Relay_Blueprint *active_blueprint;
    size_t old_count;
    Relay_NodeId old_next_id;
    Relay_NodeId node_id;

    if (offer == NULL) {
        return RELAY_GAME_ACTION_CREATION_FAILED;
    }
    if (game->currency < offer->price) {
        return RELAY_GAME_ACTION_INSUFFICIENT_CURRENCY;
    }
    world = relay_game_active_world(game);
    active_blueprint = relay_game_active_blueprint(game);
    old_count = world->count;
    old_next_id = world->next_id;
    node_id = relay_game_create_node(game, offer->definition_id);
    if (node_id == 0) {
        return RELAY_GAME_ACTION_CREATION_FAILED;
    }
    if (active_blueprint != NULL &&
        !relay_blueprint_rebuild_plan(&game->blueprints, active_blueprint)) {
        world->count = old_count;
        world->next_id = old_next_id;
        return RELAY_GAME_ACTION_CREATION_FAILED;
    }
    relay_game_set_focused_node(game, node_id);
    game->currency -= offer->price;
    return RELAY_GAME_ACTION_PURCHASED;
}

/** Return whether a node and its owning flattened module are enabled. */
static bool relay_game_node_enabled(const Relay_NodeWorld *world,
    const Relay_Node *node)
{
    const Relay_Node *module;

    if (!node->enabled) {
        return false;
    }
    if (node->module_instance_id == 0) {
        return true;
    }
    module = relay_node_world_find_const(world, node->module_instance_id);
    return module != NULL && module->enabled;
}

/** Advance one optional timer and emit a one-step trigger at its interval. */
static void relay_game_step_timer(Relay_Node *node)
{
    const Relay_NodeSimulationDefinition *simulation =
        &node->definition->simulation;

    node->timer_elapsed_steps++;
    if (node->timer_elapsed_steps >= node->timer_interval_steps) {
        node->timer_elapsed_steps = 0;
        node->output_values[simulation->output_port_index] =
            simulation->output_amount;
        node->produced = node->produced >
            INT64_MAX - simulation->output_amount ? INT64_MAX :
            node->produced + simulation->output_amount;
    }
}

/** Advance one autonomous source without producing into a full item queue. */
static bool relay_game_step_fixed_rate_source(Relay_NodeWorld *world,
    Relay_Node *node)
{
    const Relay_NodeSimulationDefinition *simulation =
        &node->definition->simulation;
    const Relay_NodePortType output_type =
        simulation->output_port_index < node->definition->output_count ?
            node->definition->outputs[
                simulation->output_port_index].type :
            RELAY_NODE_PORT_TYPE_INVALID;
    Relay_ItemQueue *queue;
    size_t amount;
    size_t index;

    if (simulation->interval_steps == 0 ||
        simulation->output_port_index >= node->definition->output_count ||
        !relay_node_port_type_is_item(output_type) ||
        simulation->output_amount <= 0 ||
        (uint64_t)simulation->output_amount > RELAY_ITEM_QUEUE_CAPACITY) {
        return false;
    }
    if ((uint64_t)node->progress < simulation->interval_steps) {
        node->progress++;
    }
    if ((uint64_t)node->progress < simulation->interval_steps) {
        return true;
    }
    queue = &node->output_queues[simulation->output_port_index];
    amount = (size_t)simulation->output_amount;
    if (queue->count > RELAY_ITEM_QUEUE_CAPACITY - amount) {
        return true;
    }
    if (world->next_item_id > UINT64_MAX - amount) {
        return false;
    }
    for (index = 0; index < amount; index++) {
        Relay_Item item;

        if (!relay_node_world_item_create(world, output_type, &item) ||
            !relay_item_queue_push(queue, item)) {
            return false;
        }
    }
    node->progress = 0;
    node->produced = node->produced >
        INT64_MAX - simulation->output_amount ? INT64_MAX :
        node->produced + simulation->output_amount;
    return true;
}

/** Return whether one processor recipe can complete without blocking. */
static bool relay_game_recipe_ready(const Relay_Node *node,
    const Relay_NodeRecipeDefinition *recipe)
{
    size_t index;

    if (node == NULL || node->definition == NULL || recipe == NULL ||
        recipe->input_count > RELAY_NODE_RECIPE_INPUT_CAPACITY ||
        recipe->output_port_index >= node->definition->output_count ||
        recipe->output_amount == 0 ||
        recipe->output_amount > RELAY_ITEM_QUEUE_CAPACITY ||
        node->output_queues[recipe->output_port_index].count >
            RELAY_ITEM_QUEUE_CAPACITY - recipe->output_amount) {
        return false;
    }
    for (index = 0; index < recipe->input_count; index++) {
        if (recipe->inputs[index].port_index >=
                node->definition->input_count ||
            node->input_queues[recipe->inputs[index].port_index].count <
                recipe->inputs[index].amount) {
            return false;
        }
    }
    return true;
}

/** Select an available recipe fairly from immutable definition order. */
static const Relay_NodeRecipeDefinition *relay_game_processor_recipe(
    const Relay_Node *node)
{
    const Relay_NodeSimulationDefinition *simulation;
    size_t start;
    size_t offset;

    if (node == NULL || node->definition == NULL) {
        return NULL;
    }
    simulation = &node->definition->simulation;
    if (simulation->recipe_count == 0 || simulation->recipes == NULL) {
        return NULL;
    }
    start = (size_t)((uint64_t)node->produced % simulation->recipe_count);
    for (offset = 0; offset < simulation->recipe_count; offset++) {
        const size_t index = (start + offset) % simulation->recipe_count;

        if (relay_game_recipe_ready(node, &simulation->recipes[index])) {
            return &simulation->recipes[index];
        }
    }
    return NULL;
}

/** Commit one physical-item recipe atomically while preserving uniqueness. */
static bool relay_game_commit_recipe(Relay_NodeWorld *world, Relay_Node *node,
    const Relay_NodeRecipeDefinition *recipe)
{
    Relay_ItemQueue input_snapshots[RELAY_NODE_RECIPE_INPUT_CAPACITY];
    Relay_ItemQueue output_snapshot;
    const Relay_ItemId next_item_id = world->next_item_id;
    const Relay_NodePortType output_type =
        node->definition->outputs[recipe->output_port_index].type;
    size_t input_index;
    size_t amount_index;

    if (!relay_game_recipe_ready(node, recipe) ||
        world->next_item_id > UINT64_MAX - recipe->output_amount) {
        return false;
    }
    output_snapshot = node->output_queues[recipe->output_port_index];
    for (input_index = 0; input_index < recipe->input_count; input_index++) {
        input_snapshots[input_index] = node->input_queues[
            recipe->inputs[input_index].port_index];
    }
    for (input_index = 0; input_index < recipe->input_count; input_index++) {
        Relay_Item discarded;

        for (amount_index = 0;
            amount_index < recipe->inputs[input_index].amount;
            amount_index++) {
            if (!relay_item_queue_pop(&node->input_queues[
                    recipe->inputs[input_index].port_index], &discarded)) {
                goto rollback;
            }
        }
    }
    for (amount_index = 0; amount_index < recipe->output_amount;
            amount_index++) {
        Relay_Item output;

        if (!relay_node_world_item_create(world, output_type, &output) ||
            !relay_item_queue_push(
                &node->output_queues[recipe->output_port_index], output)) {
            goto rollback;
        }
    }
    return true;

rollback:
    world->next_item_id = next_item_id;
    node->output_queues[recipe->output_port_index] = output_snapshot;
    for (input_index = 0; input_index < recipe->input_count; input_index++) {
        node->input_queues[recipe->inputs[input_index].port_index] =
            input_snapshots[input_index];
    }
    return false;
}

/** Advance one data-driven item processor through its selected recipe. */
static bool relay_game_step_item_processor(Relay_NodeWorld *world,
    Relay_Node *node)
{
    const Relay_NodeSimulationDefinition *simulation =
        &node->definition->simulation;
    const Relay_NodeRecipeDefinition *recipe =
        relay_game_processor_recipe(node);

    if (simulation->interval_steps == 0) {
        return false;
    }
    if (recipe == NULL) {
        node->progress = 0;
        return true;
    }
    if ((uint64_t)node->progress < simulation->interval_steps) {
        node->progress++;
    }
    if ((uint64_t)node->progress < simulation->interval_steps) {
        return true;
    }
    if (!relay_game_commit_recipe(world, node, recipe)) {
        return false;
    }
    node->progress = 0;
    node->produced = node->produced >
        INT64_MAX - recipe->output_amount ? INT64_MAX :
        node->produced + recipe->output_amount;
    return true;
}

/** Move one item per immutable storage route without changing identity. */
static void relay_game_step_item_storage(Relay_Node *node)
{
    const Relay_NodeSimulationDefinition *simulation =
        &node->definition->simulation;
    size_t index;

    for (index = 0; index < simulation->route_count; index++) {
        const Relay_NodeItemRouteDefinition *route =
            &simulation->routes[index];
        const Relay_NodePortType type =
            node->definition->inputs[route->input_port_index].type;

        (void)relay_item_queue_transfer(
            &node->input_queues[route->input_port_index],
            &node->output_queues[route->output_port_index], type);
    }
}

/** Return a connected source value for a destination input in this tick. */
static int64_t relay_game_input_value_depth(const Relay_Game *game,
    const Relay_NodeWorld *world, const Relay_Node *destination,
    size_t destination_port_index, size_t depth)
{
    const Relay_NodeConnection *connection = relay_node_world_connection_to(
        world, destination->id, destination_port_index);
    const Relay_Node *source;
    size_t index;

    (void)game;
    if (depth > world->count + 1) {
        return 0;
    }
    if (connection != NULL) {
        source = relay_node_world_find_const(world, connection->source_node_id);
        if (source == NULL) {
            return 0;
        }
        return source->previous_output_values[
            connection->source_port_index];
    }
    for (index = 0; index < world->module_input_binding_count; index++) {
        const Relay_NodeModuleInputBinding *binding =
            &world->module_input_bindings[index];

        if (binding->destination_node_id == destination->id &&
            binding->destination_port_index == destination_port_index) {
            const Relay_Node *module = relay_node_world_find_const(world,
                binding->module_node_id);

            return module == NULL ? 0 : relay_game_input_value_depth(game,
                world, module, binding->module_port_index, depth + 1);
        }
    }
    return 0;
}

/** Return one connected or module-routed input value in this tick. */
static int64_t relay_game_input_value(const Relay_Game *game,
    const Relay_NodeWorld *world, const Relay_Node *destination,
    size_t destination_port_index)
{
    return relay_game_input_value_depth(game, world, destination,
        destination_port_index, 0);
}

bool relay_game_init(Relay_Game *game, Relay_ScriptRuntime *script_runtime)
{
    Relay_NodeId coal_miner_id;

    if (game == NULL || script_runtime == NULL) {
        return false;
    }
    if (!relay_node_world_init(&game->nodes)) {
        return false;
    }
    if (!relay_blueprint_library_init(&game->blueprints, script_runtime)) {
        relay_node_world_shutdown(&game->nodes);
        return false;
    }
    coal_miner_id = relay_node_world_create(&game->nodes,
        RELAY_NODE_DEFINITION_COAL_MINER, 0, 0);
    if (coal_miner_id == 0) {
        relay_node_world_shutdown(&game->nodes);
        relay_blueprint_library_shutdown(&game->blueprints);
        return false;
    }
    game->currency = RELAY_GAME_STARTING_CURRENCY;
    game->script_runtime = script_runtime;
    game->active_tab = RELAY_GAME_PANEL_TAB_SHOP;
    game->workspace_mode = RELAY_GAME_WORKSPACE_GRAPH;
    game->session_id = 1;
    (void)snprintf(game->session_status, sizeof(game->session_status),
        "New session");
    relay_game_set_focused_node(game, coal_miner_id);
    return true;
}

Relay_NodeWorld *relay_game_active_world(Relay_Game *game)
{
    Relay_Blueprint *blueprint = relay_game_active_blueprint(game);

    return game == NULL ? NULL :
        (blueprint == NULL ? &game->nodes : &blueprint->scene);
}

const Relay_NodeWorld *relay_game_active_world_const(const Relay_Game *game)
{
    if (game == NULL || game->active_workspace == 0) {
        return game == NULL ? NULL : &game->nodes;
    }
    if (game->active_workspace > game->blueprints.count) {
        return NULL;
    }
    return &game->blueprints.blueprints[game->active_workspace - 1].scene;
}

Relay_Blueprint *relay_game_active_blueprint(Relay_Game *game)
{
    if (game == NULL || game->active_workspace == 0 ||
        game->active_workspace > game->blueprints.count) {
        return NULL;
    }
    return &game->blueprints.blueprints[game->active_workspace - 1];
}

bool relay_game_create_blueprint(Relay_Game *game)
{
    Relay_BlueprintId id;

    if (game == NULL) {
        return false;
    }
    id = relay_blueprint_library_create(&game->blueprints);
    if (id == 0) {
        return false;
    }
    game->active_workspace = game->blueprints.count;
    game->blueprints.blueprints[game->active_workspace - 1].workspace_open =
        true;
    relay_game_set_focused_node(game,
        game->blueprints.blueprints[
            game->active_workspace - 1].input_boundary_node_id);
    game->active_tab = RELAY_GAME_PANEL_TAB_INSPECTOR;
    game->workspace_mode = RELAY_GAME_WORKSPACE_GRAPH;
    return true;
}

bool relay_game_switch_workspace(Relay_Game *game, int direction)
{
    size_t candidate;
    size_t attempts;

    if (game == NULL || direction == 0) {
        return false;
    }
    candidate = game->active_workspace;
    for (attempts = 0; attempts < game->blueprints.count + 1; attempts++) {
        if (direction > 0) {
            candidate = (candidate + 1) % (game->blueprints.count + 1);
        } else {
            candidate = candidate == 0 ? game->blueprints.count :
                candidate - 1;
        }
        if (candidate == 0 ||
            game->blueprints.blueprints[candidate - 1].workspace_open) {
            return relay_game_activate_workspace(game, candidate);
        }
    }
    return false;
}

bool relay_game_activate_workspace(Relay_Game *game, size_t workspace_index)
{
    if (game == NULL || workspace_index > game->blueprints.count ||
        (workspace_index != 0 &&
            !game->blueprints.blueprints[workspace_index - 1].workspace_open)) {
        return false;
    }
    if (game->editing_blueprint_id != 0 &&
        workspace_index != game->active_workspace) {
        Relay_Blueprint *editing = relay_game_editing_blueprint(game);

        if (editing != NULL) {
            editing->editor_open = false;
        }
        game->editing_blueprint_id = 0;
    }
    game->active_workspace = workspace_index;
    game->focused_node_id = workspace_index == 0 ?
        game->root_focused_node_id :
        game->blueprints.blueprints[workspace_index - 1].focused_node_id;
    game->workspace_mode = RELAY_GAME_WORKSPACE_GRAPH;
    return true;
}

bool relay_game_open_selected_blueprint(Relay_Game *game)
{
    Relay_Blueprint *blueprint;

    if (game == NULL || game->selected_blueprint >= game->blueprints.count) {
        return false;
    }
    blueprint = &game->blueprints.blueprints[game->selected_blueprint];
    blueprint->workspace_open = true;
    return relay_game_activate_workspace(game, game->selected_blueprint + 1);
}

bool relay_game_close_active_blueprint(Relay_Game *game)
{
    Relay_Blueprint *blueprint;

    if (game == NULL || game->active_workspace == 0 ||
        game->active_workspace > game->blueprints.count) {
        return false;
    }
    blueprint = &game->blueprints.blueprints[game->active_workspace - 1];
    blueprint->workspace_open = false;
    if (game->editing_blueprint_id == blueprint->id) {
        blueprint->editor_open = false;
        game->editing_blueprint_id = 0;
    }
    return relay_game_activate_workspace(game, 0);
}

bool relay_game_add_blueprint(Relay_Game *game, Relay_BlueprintId blueprint_id)
{
    Relay_Blueprint *blueprint;
    Relay_NodeWorld *world;
    Relay_NodeId node_id;
    Relay_Blueprint *active_blueprint;
    const size_t old_count = game == NULL ? 0 :
        relay_game_active_world(game)->count;
    const Relay_NodeId old_next_id = game == NULL ? 0 :
        relay_game_active_world(game)->next_id;

    if (game == NULL || (game->active_workspace != 0 &&
            game->blueprints.blueprints[game->active_workspace - 1].id ==
                blueprint_id)) {
        return false;
    }
    blueprint = relay_blueprint_library_find(&game->blueprints, blueprint_id);
    world = relay_game_active_world(game);
    active_blueprint = relay_game_active_blueprint(game);
    if (blueprint == NULL || world == NULL || !blueprint->artifact.installed) {
        return false;
    }
    if (active_blueprint == NULL) {
        if (!relay_blueprint_instantiate(&game->blueprints, blueprint, world,
                (int64_t)(world->count + 1) * RELAY_GAME_NODE_SPACING_X, 0,
                &node_id)) {
            return false;
        }
    } else {
        Relay_Node *node;

        node_id = relay_node_world_create_definition(world,
            &blueprint->definition,
            (int64_t)(world->count + 1) * RELAY_GAME_NODE_SPACING_X, 0);
        if (node_id == 0) {
            return false;
        }
        node = relay_node_world_find(world, node_id);
        node->runtime_kind = RELAY_NODE_RUNTIME_BLUEPRINT_WRAPPER;
        node->blueprint_id = blueprint_id;
        if (!relay_blueprint_rebuild_plan(&game->blueprints,
                active_blueprint)) {
            world->count = old_count;
            world->next_id = old_next_id;
            return false;
        }
    }
    relay_game_set_focused_node(game, node_id);
    game->active_tab = RELAY_GAME_PANEL_TAB_INSPECTOR;
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
        game->active_tab = (Relay_GamePanelTab)((game->active_tab + 1) % 3);
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
    } else if (input == RELAY_GAME_INPUT_PREVIOUS &&
        game->active_tab == RELAY_GAME_PANEL_TAB_BLUEPRINTS &&
        game->blueprints.count > 0) {
        game->selected_blueprint = game->selected_blueprint == 0 ?
            game->blueprints.count - 1 : game->selected_blueprint - 1;
    } else if (input == RELAY_GAME_INPUT_NEXT &&
        game->active_tab == RELAY_GAME_PANEL_TAB_BLUEPRINTS &&
        game->blueprints.count > 0) {
        game->selected_blueprint = (game->selected_blueprint + 1) %
            game->blueprints.count;
    } else if (input == RELAY_GAME_INPUT_CONFIRM &&
        game->active_tab == RELAY_GAME_PANEL_TAB_BLUEPRINTS &&
        game->selected_blueprint < game->blueprints.count) {
        game->last_action = relay_game_add_blueprint(game,
            game->blueprints.blueprints[game->selected_blueprint].id) ?
            RELAY_GAME_ACTION_PURCHASED : RELAY_GAME_ACTION_CREATION_FAILED;
    } else if (input == RELAY_GAME_INPUT_TOGGLE_MAP) {
        game->workspace_mode = game->workspace_mode == RELAY_GAME_WORKSPACE_GRAPH ?
            RELAY_GAME_WORKSPACE_MAP : RELAY_GAME_WORKSPACE_GRAPH;
        game->last_action = RELAY_GAME_ACTION_NONE;
    } else if (input == RELAY_GAME_INPUT_PREVIOUS_TIMER_INTERVAL ||
        input == RELAY_GAME_INPUT_NEXT_TIMER_INTERVAL) {
        focused = relay_node_world_find(relay_game_active_world(game),
            game->focused_node_id);
        if (focused != NULL &&
            focused->definition != NULL &&
            focused->definition->simulation.behavior ==
                RELAY_NODE_BEHAVIOR_TIMER) {
            value.integer = relay_game_next_timer_interval(
                focused->timer_interval_steps,
                input == RELAY_GAME_INPUT_NEXT_TIMER_INTERVAL ? 1 : -1);
            (void)relay_node_property_set(focused, "timer.interval_steps",
                RELAY_NODE_VALUE_INTEGER, value);
        }
        game->last_action = RELAY_GAME_ACTION_NONE;
    }
    return game->last_action;
}

bool relay_game_back(Relay_Game *game)
{
    Relay_Blueprint *editing;

    if (game != NULL && game->editing_blueprint_id != 0) {
        editing = relay_game_editing_blueprint(game);
        if (editing != NULL) {
            editing->editor_open = false;
        }
        game->editing_blueprint_id = 0;
        return true;
    }
    if (game == NULL) {
        return false;
    }
    if (game->workspace_mode != RELAY_GAME_WORKSPACE_GRAPH) {
        game->workspace_mode = RELAY_GAME_WORKSPACE_GRAPH;
        return true;
    }
    if (game->active_workspace != 0) {
        return relay_game_activate_workspace(game, 0);
    }
    return false;
}

Relay_Blueprint *relay_game_editing_blueprint(Relay_Game *game)
{
    return game == NULL ? NULL : relay_blueprint_library_find(
        &game->blueprints, game->editing_blueprint_id);
}

bool relay_game_open_editor(Relay_Game *game)
{
    Relay_Blueprint *blueprint = relay_game_active_blueprint(game);

    if (game == NULL) {
        return false;
    }
    if (blueprint == NULL && game->focused_node_id != 0) {
        const Relay_Node *node = relay_node_world_find_const(
            relay_game_active_world_const(game), game->focused_node_id);

        if (node != NULL && node->blueprint_id != 0) {
            blueprint = relay_blueprint_library_find(&game->blueprints,
                node->blueprint_id);
        }
    }
    if (blueprint == NULL) {
        return false;
    }
    blueprint->editor_open = true;
    blueprint->editor_mode = RELAY_BLUEPRINT_EDITOR_NORMAL;
    blueprint->editor_command_size = 0;
    blueprint->editor_command[0] = '\0';
    blueprint->completion_selection = 0;
    blueprint->completion_suppressed = false;
    game->editing_blueprint_id = blueprint->id;
    return true;
}

bool relay_game_editor_insert(Relay_Game *game, uint32_t character)
{
    Relay_Blueprint *blueprint = relay_game_editing_blueprint(game);

    if (blueprint == NULL || (character != '\n' &&
            (character < 32 || character > 126)) ||
        blueprint->revision == UINT64_MAX ||
        blueprint->source_size + 1 >= RELAY_BLUEPRINT_SOURCE_CAPACITY) {
        return false;
    }
    (void)memmove(&blueprint->source[blueprint->cursor + 1],
        &blueprint->source[blueprint->cursor],
        blueprint->source_size - blueprint->cursor + 1);
    blueprint->source[blueprint->cursor++] = (char)character;
    blueprint->source_size++;
    blueprint->revision++;
    blueprint->dirty = true;
    blueprint->completion_selection = 0;
    blueprint->completion_suppressed = false;
    return true;
}

bool relay_game_editor_backspace(Relay_Game *game)
{
    Relay_Blueprint *blueprint = relay_game_editing_blueprint(game);

    if (blueprint == NULL || blueprint->cursor == 0 ||
        blueprint->revision == UINT64_MAX) {
        return false;
    }
    (void)memmove(&blueprint->source[blueprint->cursor - 1],
        &blueprint->source[blueprint->cursor],
        blueprint->source_size - blueprint->cursor + 1);
    blueprint->cursor--;
    blueprint->source_size--;
    blueprint->revision++;
    blueprint->dirty = true;
    blueprint->completion_selection = 0;
    blueprint->completion_suppressed = false;
    return true;
}

bool relay_game_editor_delete(Relay_Game *game)
{
    Relay_Blueprint *blueprint = relay_game_editing_blueprint(game);

    if (blueprint == NULL || blueprint->cursor >= blueprint->source_size ||
        blueprint->revision == UINT64_MAX) {
        return false;
    }
    (void)memmove(&blueprint->source[blueprint->cursor],
        &blueprint->source[blueprint->cursor + 1],
        blueprint->source_size - blueprint->cursor);
    blueprint->source_size--;
    blueprint->revision++;
    blueprint->dirty = true;
    blueprint->completion_selection = 0;
    blueprint->completion_suppressed = false;
    return true;
}

bool relay_game_editor_move_horizontal(Relay_Game *game, int direction)
{
    Relay_Blueprint *blueprint = relay_game_editing_blueprint(game);

    if (blueprint == NULL || direction == 0 ||
        (direction < 0 && blueprint->cursor == 0) ||
        (direction > 0 && blueprint->cursor >= blueprint->source_size)) {
        return false;
    }
    blueprint->cursor = direction < 0 ? blueprint->cursor - 1 :
        blueprint->cursor + 1;
    blueprint->completion_selection = 0;
    blueprint->completion_suppressed = false;
    return true;
}

/** Return the byte offset and column of the line containing a cursor. */
static size_t relay_game_editor_line_start(const Relay_Blueprint *blueprint,
    size_t cursor, size_t *column)
{
    size_t start = cursor;

    while (start > 0 && blueprint->source[start - 1] != '\n') {
        start--;
    }
    *column = cursor - start;
    return start;
}

bool relay_game_editor_move_vertical(Relay_Game *game, int direction)
{
    Relay_Blueprint *blueprint = relay_game_editing_blueprint(game);
    size_t column;
    size_t line_start;
    size_t target_start;
    size_t target_end;

    if (blueprint == NULL || direction == 0) {
        return false;
    }
    line_start = relay_game_editor_line_start(blueprint, blueprint->cursor,
        &column);
    if (direction < 0) {
        if (line_start == 0) {
            return false;
        }
        target_end = line_start - 1;
        target_start = target_end;
        while (target_start > 0 &&
            blueprint->source[target_start - 1] != '\n') {
            target_start--;
        }
    } else {
        target_start = line_start;
        while (target_start < blueprint->source_size &&
            blueprint->source[target_start] != '\n') {
            target_start++;
        }
        if (target_start >= blueprint->source_size) {
            return false;
        }
        target_start++;
        target_end = target_start;
        while (target_end < blueprint->source_size &&
            blueprint->source[target_end] != '\n') {
            target_end++;
        }
    }
    blueprint->cursor = target_start +
        (column < target_end - target_start ? column : target_end - target_start);
    blueprint->completion_selection = 0;
    blueprint->completion_suppressed = false;
    return true;
}

bool relay_game_editor_move_line_boundary(Relay_Game *game, bool to_end)
{
    Relay_Blueprint *blueprint = relay_game_editing_blueprint(game);
    size_t column;
    size_t boundary;

    if (blueprint == NULL) {
        return false;
    }
    boundary = relay_game_editor_line_start(blueprint, blueprint->cursor,
        &column);
    if (to_end) {
        while (boundary < blueprint->source_size &&
            blueprint->source[boundary] != '\n') {
            boundary++;
        }
    }
    blueprint->cursor = boundary;
    blueprint->completion_selection = 0;
    blueprint->completion_suppressed = false;
    return true;
}

bool relay_game_editor_enter_insert(Relay_Game *game)
{
    Relay_Blueprint *blueprint = relay_game_editing_blueprint(game);

    if (blueprint == NULL) {
        return false;
    }
    blueprint->editor_mode = RELAY_BLUEPRINT_EDITOR_INSERT;
    blueprint->completion_selection = 0;
    blueprint->completion_suppressed = false;
    return true;
}

bool relay_game_editor_enter_command(Relay_Game *game)
{
    Relay_Blueprint *blueprint = relay_game_editing_blueprint(game);

    if (blueprint == NULL) {
        return false;
    }
    blueprint->editor_mode = RELAY_BLUEPRINT_EDITOR_COMMAND;
    blueprint->editor_command_size = 0;
    blueprint->editor_command[0] = '\0';
    return true;
}

bool relay_game_editor_leave_mode(Relay_Game *game)
{
    Relay_Blueprint *blueprint = relay_game_editing_blueprint(game);

    if (blueprint == NULL ||
        blueprint->editor_mode == RELAY_BLUEPRINT_EDITOR_NORMAL) {
        return false;
    }
    blueprint->editor_mode = RELAY_BLUEPRINT_EDITOR_NORMAL;
    blueprint->editor_command_size = 0;
    blueprint->editor_command[0] = '\0';
    blueprint->completion_selection = 0;
    blueprint->completion_suppressed = false;
    return true;
}

size_t relay_game_editor_completions(const Relay_Game *game,
    Relay_ScriptCompletion *items, size_t item_capacity,
    size_t *replacement_start)
{
    const Relay_Blueprint *blueprint;
    const char *script_names[RELAY_BLUEPRINT_CAPACITY];
    Relay_ScriptLanguageCatalog catalog;
    size_t index;
    size_t script_name_count = 0;

    if (game == NULL) {
        return 0;
    }
    blueprint = game->editing_blueprint_id == 0 ? NULL :
        relay_blueprint_library_find_const(&game->blueprints,
            game->editing_blueprint_id);
    if (blueprint == NULL ||
        blueprint->editor_mode != RELAY_BLUEPRINT_EDITOR_INSERT ||
        blueprint->completion_suppressed) {
        return 0;
    }
    for (index = 0; index < game->blueprints.count; index++) {
        const Relay_Blueprint *candidate =
            &game->blueprints.blueprints[index];

        if (candidate->id != blueprint->id) {
            script_names[script_name_count++] = candidate->name;
        }
    }
    catalog = (Relay_ScriptLanguageCatalog){
        script_names, script_name_count};
    return relay_script_language_complete(blueprint->source,
        blueprint->source_size, blueprint->cursor, &catalog, items,
        item_capacity, replacement_start);
}

bool relay_game_editor_completion_move(Relay_Game *game, int direction)
{
    Relay_Blueprint *blueprint = relay_game_editing_blueprint(game);
    Relay_ScriptCompletion items[RELAY_SCRIPT_LANGUAGE_MAX_COMPLETIONS];
    size_t replacement_start;
    const size_t count = relay_game_editor_completions(game, items,
        RELAY_SCRIPT_LANGUAGE_MAX_COMPLETIONS, &replacement_start);

    (void)replacement_start;
    if (count == 0 || direction == 0) {
        return false;
    }
    blueprint->completion_selection = direction < 0 ?
        (blueprint->completion_selection == 0 ? count - 1 :
            blueprint->completion_selection - 1) :
        (blueprint->completion_selection + 1) % count;
    return true;
}

bool relay_game_editor_completion_accept(Relay_Game *game)
{
    Relay_Blueprint *blueprint = relay_game_editing_blueprint(game);
    Relay_ScriptCompletion items[RELAY_SCRIPT_LANGUAGE_MAX_COMPLETIONS];
    size_t replacement_start;
    size_t count;
    size_t selected;
    size_t replacement_size;
    size_t insertion_size;

    count = relay_game_editor_completions(game, items,
        RELAY_SCRIPT_LANGUAGE_MAX_COMPLETIONS, &replacement_start);
    if (count == 0) {
        return false;
    }
    selected = blueprint->completion_selection % count;
    replacement_size = blueprint->cursor - replacement_start;
    insertion_size = strlen(items[selected].insert_text);
    if (blueprint->revision == UINT64_MAX ||
        blueprint->source_size - replacement_size + insertion_size + 1 >=
            RELAY_BLUEPRINT_SOURCE_CAPACITY) {
        return false;
    }
    (void)memmove(&blueprint->source[replacement_start + insertion_size],
        &blueprint->source[blueprint->cursor],
        blueprint->source_size - blueprint->cursor + 1);
    (void)memcpy(&blueprint->source[replacement_start],
        items[selected].insert_text, insertion_size);
    blueprint->source_size = blueprint->source_size - replacement_size +
        insertion_size;
    blueprint->cursor = replacement_start + insertion_size;
    blueprint->revision++;
    blueprint->dirty = true;
    blueprint->completion_selection = 0;
    blueprint->completion_suppressed = true;
    return true;
}

bool relay_game_editor_completion_dismiss(Relay_Game *game)
{
    Relay_Blueprint *blueprint = relay_game_editing_blueprint(game);
    Relay_ScriptCompletion items[RELAY_SCRIPT_LANGUAGE_MAX_COMPLETIONS];
    size_t replacement_start;

    if (relay_game_editor_completions(game, items,
            RELAY_SCRIPT_LANGUAGE_MAX_COMPLETIONS, &replacement_start) == 0) {
        return false;
    }
    blueprint->completion_selection = 0;
    blueprint->completion_suppressed = true;
    return true;
}

bool relay_game_editor_command_insert(Relay_Game *game, uint32_t character)
{
    Relay_Blueprint *blueprint = relay_game_editing_blueprint(game);

    if (blueprint == NULL ||
        blueprint->editor_mode != RELAY_BLUEPRINT_EDITOR_COMMAND ||
        character < 32 || character > 126 ||
        blueprint->editor_command_size + 1 >=
            sizeof(blueprint->editor_command)) {
        return false;
    }
    blueprint->editor_command[blueprint->editor_command_size++] =
        (char)character;
    blueprint->editor_command[blueprint->editor_command_size] = '\0';
    return true;
}

bool relay_game_editor_command_backspace(Relay_Game *game)
{
    Relay_Blueprint *blueprint = relay_game_editing_blueprint(game);

    if (blueprint == NULL ||
        blueprint->editor_mode != RELAY_BLUEPRINT_EDITOR_COMMAND ||
        blueprint->editor_command_size == 0) {
        return false;
    }
    blueprint->editor_command[--blueprint->editor_command_size] = '\0';
    return true;
}

Relay_GameEditorCommandResult relay_game_editor_command_execute(
    Relay_Game *game)
{
    Relay_Blueprint *blueprint = relay_game_editing_blueprint(game);
    bool close_after_save;

    if (blueprint == NULL ||
        blueprint->editor_mode != RELAY_BLUEPRINT_EDITOR_COMMAND) {
        return RELAY_GAME_EDITOR_COMMAND_NONE;
    }
    close_after_save = strcmp(blueprint->editor_command, "wq") == 0;
    if (strcmp(blueprint->editor_command, "w") == 0 || close_after_save) {
        blueprint->editor_mode = RELAY_BLUEPRINT_EDITOR_NORMAL;
        if (!relay_game_editor_save(game)) {
            return RELAY_GAME_EDITOR_COMMAND_FAILED;
        }
        if (close_after_save) {
            (void)relay_game_back(game);
            return RELAY_GAME_EDITOR_COMMAND_CLOSED;
        }
        return RELAY_GAME_EDITOR_COMMAND_SAVED;
    }
    if (strcmp(blueprint->editor_command, "q") == 0) {
        blueprint->editor_mode = RELAY_BLUEPRINT_EDITOR_NORMAL;
        (void)relay_game_back(game);
        return RELAY_GAME_EDITOR_COMMAND_CLOSED;
    }
    (void)snprintf(blueprint->diagnostic.message,
        sizeof(blueprint->diagnostic.message),
        "Unknown editor command: :%s", blueprint->editor_command);
    blueprint->editor_mode = RELAY_BLUEPRINT_EDITOR_NORMAL;
    return RELAY_GAME_EDITOR_COMMAND_FAILED;
}

bool relay_game_editor_save(Relay_Game *game)
{
    Relay_Blueprint *blueprint = relay_game_editing_blueprint(game);

    return blueprint != NULL &&
        relay_blueprint_compile(&game->blueprints, blueprint);
}

bool relay_game_select_panel_tab(Relay_Game *game, size_t tab_index)
{
    if (game == NULL || tab_index > RELAY_GAME_PANEL_TAB_BLUEPRINTS) {
        return false;
    }
    game->active_tab = (Relay_GamePanelTab)tab_index;
    game->last_action = RELAY_GAME_ACTION_NONE;
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
    Relay_NodeWorld *world;
    Relay_Blueprint *blueprint;
    Relay_Node *node;
    int64_t old_grid_x;
    int64_t old_grid_y;

    if (game == NULL) {
        return false;
    }
    world = relay_game_active_world(game);
    blueprint = relay_game_active_blueprint(game);
    node = relay_node_world_find(world, id);
    if (node == NULL) {
        return false;
    }
    old_grid_x = node->grid_x;
    old_grid_y = node->grid_y;
    if (!relay_node_world_move(world, id,
            relay_game_add_delta(old_grid_x, delta_x),
            relay_game_add_delta(old_grid_y, delta_y))) {
        return false;
    }
    if (blueprint != NULL &&
        !relay_blueprint_rebuild_plan(&game->blueprints, blueprint)) {
        (void)relay_node_world_move(world, id, old_grid_x, old_grid_y);
        return false;
    }
    return true;
}

bool relay_game_focus_node(Relay_Game *game, Relay_NodeId id)
{
    if (game == NULL || relay_node_world_find(relay_game_active_world(game), id) ==
            NULL) {
        return false;
    }
    relay_game_set_focused_node(game, id);
    game->active_tab = RELAY_GAME_PANEL_TAB_INSPECTOR;
    return true;
}

bool relay_game_connect_nodes(Relay_Game *game, Relay_NodeId source_node_id,
    size_t source_port_index, Relay_NodeId destination_node_id,
    size_t destination_port_index)
{
    Relay_NodeWorld *world;
    Relay_Blueprint *blueprint;
    Relay_NodeConnection previous = {0};
    size_t previous_index = 0;
    size_t previous_count;
    size_t index;
    bool replaced = false;

    if (game == NULL) {
        return false;
    }
    world = relay_game_active_world(game);
    blueprint = relay_game_active_blueprint(game);
    previous_count = world->connection_count;
    for (index = 0; index < previous_count; index++) {
        if (world->connections[index].destination_node_id ==
                destination_node_id &&
            world->connections[index].destination_port_index ==
                destination_port_index) {
            previous = world->connections[index];
            previous_index = index;
            replaced = true;
            break;
        }
    }
    if (!relay_node_world_connect(world,
            source_node_id,
            source_port_index, destination_node_id, destination_port_index)) {
        return false;
    }
    if (blueprint != NULL &&
        !relay_blueprint_rebuild_plan(&game->blueprints, blueprint)) {
        if (replaced) {
            world->connections[previous_index] = previous;
        } else {
            world->connection_count = previous_count;
        }
        return false;
    }
    return true;
}

/** Execute one compiled script module node from the world's input snapshot. */
static void relay_game_step_script(Relay_Game *game, Relay_NodeWorld *world,
    Relay_Node *node)
{
    Relay_Blueprint *blueprint = relay_blueprint_library_find(
        &game->blueprints, node->blueprint_id);
    int64_t inputs[RELAY_NODE_MAX_PORTS] = {0};
    int64_t outputs[RELAY_NODE_MAX_PORTS] = {0};
    bool should_process = !node->script_state.initialized;
    size_t index;

    if (blueprint == NULL) {
        return;
    }
    for (index = 0; index < blueprint->schema.input_count; index++) {
        const Relay_NodePortType type = blueprint->schema.inputs[index].type;

        if (relay_node_port_type_is_item(type)) {
            if (!relay_item_queue_empty(&node->input_queues[index])) {
                should_process = true;
            }
            continue;
        }
        inputs[index] = relay_game_input_value(game, world, node, index);
        if ((relay_node_port_type_is_transient(type) &&
                inputs[index] != 0) ||
            (!relay_node_port_type_is_transient(type) &&
                inputs[index] != node->process_input_values[index])) {
            should_process = true;
        }
        node->process_input_values[index] = inputs[index];
    }
    if (!should_process) {
        return;
    }
    (void)memcpy(outputs, node->output_values, sizeof(outputs));
    if (relay_script_runtime_invoke(game->script_runtime, &blueprint->artifact,
            &node->script_state, &blueprint->schema,
            (Relay_ScriptInvocation){
                node->input_queues, node->output_queues, inputs, outputs
            }, &blueprint->diagnostic)) {
        node->process_activations++;
        for (index = 0; index < blueprint->schema.output_count; index++) {
            node->output_values[index] = outputs[index];
        }
    }
}

/** Move physical items across ordinary graph connections in stable order. */
static void relay_game_step_item_connections(Relay_NodeWorld *world)
{
    size_t index;

    for (index = 0; index < world->connection_count; index++) {
        const Relay_NodeConnection *connection = &world->connections[index];
        Relay_Node *source = relay_node_world_find(world,
            connection->source_node_id);
        Relay_Node *destination = relay_node_world_find(world,
            connection->destination_node_id);
        const Relay_NodeDefinition *source_definition =
            relay_node_definition_for(source);
        const Relay_NodeDefinition *destination_definition =
            relay_node_definition_for(destination);
        Relay_NodePortType type;

        if (source_definition == NULL || destination_definition == NULL ||
            connection->source_port_index >= source_definition->output_count ||
            connection->destination_port_index >=
                destination_definition->input_count) {
            continue;
        }
        type = source_definition->outputs[
            connection->source_port_index].type;
        if (relay_node_port_type_is_item(type) &&
            relay_game_node_enabled(world, source) &&
            relay_game_node_enabled(world, destination)) {
            (void)relay_item_queue_transfer(
                &source->output_queues[connection->source_port_index],
                &destination->input_queues[
                    connection->destination_port_index], type);
        }
    }
}

/** Move physical items through flattened module input/output boundaries. */
static void relay_game_step_item_bindings(Relay_NodeWorld *world)
{
    size_t index;

    for (index = 0; index < world->module_input_binding_count; index++) {
        const Relay_NodeModuleInputBinding *binding =
            &world->module_input_bindings[index];
        Relay_Node *module = relay_node_world_find(world,
            binding->module_node_id);
        Relay_Node *destination = relay_node_world_find(world,
            binding->destination_node_id);
        const Relay_NodeDefinition *module_definition =
            relay_node_definition_for(module);

        if (module_definition != NULL && destination != NULL &&
            binding->module_port_index < module_definition->input_count &&
            relay_node_port_type_is_item(module_definition->inputs[
                binding->module_port_index].type) &&
            relay_game_node_enabled(world, module) &&
            relay_game_node_enabled(world, destination)) {
            (void)relay_item_queue_transfer(
                &module->input_queues[binding->module_port_index],
                &destination->input_queues[binding->destination_port_index],
                module_definition->inputs[binding->module_port_index].type);
        }
    }
    for (index = 0; index < world->module_output_binding_count; index++) {
        const Relay_NodeModuleOutputBinding *binding =
            &world->module_output_bindings[index];
        Relay_Node *module = relay_node_world_find(world,
            binding->module_node_id);
        const Relay_NodeDefinition *module_definition =
            relay_node_definition_for(module);
        Relay_ItemQueue *source_queue;
        Relay_NodePortType type;

        if (module_definition == NULL ||
            binding->module_port_index >= module_definition->output_count ||
            !relay_game_node_enabled(world, module)) {
            continue;
        }
        type = module_definition->outputs[binding->module_port_index].type;
        if (!relay_node_port_type_is_item(type)) {
            continue;
        }
        if (binding->source_is_module_input) {
            if (binding->source_module_input_port_index >=
                    module_definition->input_count) {
                continue;
            }
            source_queue = &module->input_queues[
                binding->source_module_input_port_index];
        } else {
            Relay_Node *source = relay_node_world_find(world,
                binding->source_node_id);

            if (source == NULL || !relay_game_node_enabled(world, source)) {
                continue;
            }
            source_queue =
                &source->output_queues[binding->source_port_index];
        }
        (void)relay_item_queue_transfer(source_queue,
            &module->output_queues[binding->module_port_index], type);
    }
}

/** Advance one node world through transport, sources, and processors. */
static bool relay_game_step_world(Relay_Game *game, Relay_NodeWorld *world)
{
    size_t index;

    relay_game_step_item_connections(world);
    relay_game_step_item_bindings(world);
    for (index = 0; index < world->count; index++) {
        Relay_Node *node = &world->nodes[index];
        const Relay_NodeDefinition *definition =
            relay_node_definition_for(node);
        const bool enabled = relay_game_node_enabled(world, node);
        size_t port_index;

        for (port_index = 0; port_index < RELAY_NODE_MAX_PORTS; port_index++) {
            node->previous_output_values[port_index] =
                node->output_values[port_index];
            if (definition == NULL ||
                port_index >= definition->output_count ||
                relay_node_port_type_is_transient(
                    definition->outputs[port_index].type)) {
                node->output_values[port_index] = 0;
            }
            if (!enabled) {
                node->output_values[port_index] = 0;
            }
        }

        if (enabled && definition != NULL &&
            definition->simulation.behavior == RELAY_NODE_BEHAVIOR_TIMER) {
            relay_game_step_timer(node);
        }
    }
    for (index = 0; index < world->count; index++) {
        Relay_Node *node = &world->nodes[index];

        if (relay_game_node_enabled(world, node) &&
            node->runtime_kind ==
                RELAY_NODE_RUNTIME_BLUEPRINT_PROCESS) {
            relay_game_step_script(game, world, node);
        }
    }
    for (index = 0; index < world->count; index++) {
        Relay_Node *node = &world->nodes[index];

        if (!relay_game_node_enabled(world, node) ||
            node->definition == NULL) {
            continue;
        }
        if (node->definition->simulation.behavior ==
                RELAY_NODE_BEHAVIOR_ITEM_PROCESSOR) {
            if (!relay_game_step_item_processor(world, node)) {
                return false;
            }
        } else if (node->definition->simulation.behavior ==
                RELAY_NODE_BEHAVIOR_ITEM_STORAGE) {
            relay_game_step_item_storage(node);
        }
    }
    for (index = 0; index < world->count; index++) {
        Relay_Node *node = &world->nodes[index];

        if (relay_game_node_enabled(world, node) &&
            node->definition != NULL &&
            node->definition->simulation.behavior ==
                RELAY_NODE_BEHAVIOR_FIXED_RATE_SOURCE) {
            if (!relay_game_step_fixed_rate_source(world, node)) {
                return false;
            }
        }
    }
    for (index = 0; index < world->module_output_binding_count; index++) {
        const Relay_NodeModuleOutputBinding *binding =
            &world->module_output_bindings[index];
        Relay_Node *module = relay_node_world_find(world,
            binding->module_node_id);

        if (module == NULL ||
            !relay_game_node_enabled(world, module)) {
            continue;
        }
        if (binding->source_is_module_input) {
            module->output_values[binding->module_port_index] =
                relay_game_input_value(game, world, module,
                    binding->source_module_input_port_index);
        } else {
            const Relay_Node *source = relay_node_world_find_const(world,
                binding->source_node_id);

            if (source != NULL) {
                module->output_values[binding->module_port_index] =
                    source->output_values[binding->source_port_index];
            }
        }
    }
    return relay_node_world_items_valid(world);
}

bool relay_game_step(Relay_Game *game)
{
    if (game == NULL || game->script_runtime == NULL) {
        return false;
    }
    if (!relay_game_step_world(game, &game->nodes)) {
        return false;
    }
    game->simulation_step++;
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
        size_t index;

        for (index = 0; index < game->nodes.count; index++) {
            relay_script_instance_shutdown(game->script_runtime,
                &game->nodes.nodes[index].script_state);
        }
        relay_node_world_shutdown(&game->nodes);
        relay_blueprint_library_shutdown(&game->blueprints);
        *game = (Relay_Game){0};
    }
}
