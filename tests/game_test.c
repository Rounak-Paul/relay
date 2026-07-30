#include "relay/game.h"
#include "relay/icons.h"
#include "relay/node_renderer.h"

#include <string.h>

/** Verify bounded FIFO ordering, backpressure, identity, and material fan-out. */
static bool relay_test_physical_item_queues(void)
{
    static const Relay_NodePortDefinition source_outputs[] = {
        {"coal", "Coal", RELAY_NODE_PORT_TYPE_COAL}
    };
    static const Relay_NodePortDefinition destination_inputs[] = {
        {"coal", "Coal", RELAY_NODE_PORT_TYPE_COAL}
    };
    const Relay_NodePropertyDefinition *properties;
    Relay_NodeDefinition source_definition = {
        .id = 9001,
        .key = "test.source",
        .display_name = "Test Source",
        .glyph = "S",
        .description = "Queue test source.",
        .category = RELAY_NODE_CATEGORY_SOURCE,
        .outputs = source_outputs,
        .output_count = 1,
        .simulation = {0}
    };
    Relay_NodeDefinition destination_definition = {
        .id = 9002,
        .key = "test.destination",
        .display_name = "Test Destination",
        .glyph = "D",
        .description = "Queue test destination.",
        .category = RELAY_NODE_CATEGORY_PROCESSOR,
        .inputs = destination_inputs,
        .input_count = 1,
        .simulation = {0}
    };
    Relay_NodeWorld world = {0};
    Relay_ItemQueue source_queue = {0};
    Relay_ItemQueue destination_queue = {0};
    Relay_Item item;
    Relay_Item extra;
    Relay_NodeId source_id;
    Relay_NodeId destination_a;
    Relay_NodeId destination_b;
    size_t property_count;
    size_t index;
    bool valid = false;

    properties = relay_node_universal_properties(&property_count);
    source_definition.properties = properties;
    source_definition.property_count = property_count;
    destination_definition.properties = properties;
    destination_definition.property_count = property_count;
    if (!relay_node_world_init(&world)) {
        return false;
    }
    source_id = relay_node_world_create_definition(&world,
        &source_definition, 0, 0);
    destination_a = relay_node_world_create_definition(&world,
        &destination_definition, 10, 0);
    destination_b = relay_node_world_create_definition(&world,
        &destination_definition, 20, 0);
    if (source_id == 0 || destination_a == 0 || destination_b == 0 ||
        !relay_node_world_connect(&world, source_id, 0, destination_a, 0) ||
        relay_node_world_connect(&world, source_id, 0, destination_b, 0)) {
        goto cleanup;
    }
    for (index = 1; index <= RELAY_ITEM_QUEUE_CAPACITY; index++) {
        if (!relay_node_world_item_create(&world, RELAY_NODE_PORT_TYPE_COAL,
                &item) ||
            item.id != index || !relay_item_queue_push(&source_queue, item)) {
            goto cleanup;
        }
    }
    if (!relay_item_queue_full(&source_queue) ||
        !relay_node_world_item_create(&world, RELAY_NODE_PORT_TYPE_COAL,
            &extra) ||
        relay_item_queue_push(&source_queue, extra)) {
        goto cleanup;
    }
    for (index = 1; index <= RELAY_ITEM_QUEUE_CAPACITY / 2; index++) {
        if (!relay_item_queue_pop(&source_queue, &item) || item.id != index) {
            goto cleanup;
        }
    }
    for (index = 0; index < RELAY_ITEM_QUEUE_CAPACITY / 2; index++) {
        if (!relay_node_world_item_create(&world, RELAY_NODE_PORT_TYPE_COAL,
                &item) ||
            !relay_item_queue_push(&source_queue, item)) {
            goto cleanup;
        }
    }
    while (!relay_item_queue_empty(&source_queue)) {
        if (!relay_item_queue_transfer(&source_queue, &destination_queue,
                RELAY_NODE_PORT_TYPE_COAL)) {
            goto cleanup;
        }
    }
    if (!relay_item_queue_full(&destination_queue) ||
        !relay_item_queue_push(&source_queue, extra) ||
        relay_item_queue_transfer(&source_queue, &destination_queue,
            RELAY_NODE_PORT_TYPE_COAL) ||
        source_queue.count != 1 ||
        !relay_item_queue_pop(&source_queue, &item) || item.id != extra.id ||
        relay_item_queue_transfer(&destination_queue, &source_queue,
            RELAY_NODE_PORT_TYPE_IRON_ORE)) {
        goto cleanup;
    }
    for (index = RELAY_ITEM_QUEUE_CAPACITY / 2 + 1;
            index <= RELAY_ITEM_QUEUE_CAPACITY; index++) {
        if (!relay_item_queue_pop(&destination_queue, &item) ||
            item.id != index) {
            goto cleanup;
        }
    }
    if (!relay_item_queue_pop(&destination_queue, &item) ||
        item.id != extra.id + 1) {
        goto cleanup;
    }
    world.nodes[0].output_queues[0] = destination_queue;
    if (!relay_node_world_items_valid(&world) ||
        !relay_item_queue_peek(&destination_queue, &item) ||
        !relay_item_queue_push(&world.nodes[1].input_queues[0], item) ||
        relay_node_world_items_valid(&world) ||
        !relay_item_queue_pop(&world.nodes[1].input_queues[0], &item) ||
        !relay_node_world_items_valid(&world)) {
        goto cleanup;
    }
    valid = true;

cleanup:
    relay_node_world_shutdown(&world);
    return valid;
}

/** Verify one fixed-rate source definition and its universal script contract. */
static bool relay_test_source_definition(Relay_NodeDefinitionId id,
    const char *key, Relay_NodePortType output_type)
{
    const Relay_NodeDefinition *definition = relay_node_definition_find(id);
    Relay_NodeValue value;
    Relay_NodeValueType value_type;
    Relay_NodeWorld world = {0};
    Relay_Node *node;
    Relay_NodeId node_id;
    bool valid;

    if (definition == NULL || strcmp(definition->key, key) != 0 ||
        definition->input_count != 0 || definition->output_count != 1 ||
        definition->outputs[0].type != output_type ||
        definition->property_count != 1 ||
        strcmp(definition->properties[0].key, "node.enabled") != 0 ||
        definition->simulation.behavior !=
            RELAY_NODE_BEHAVIOR_FIXED_RATE_SOURCE ||
        definition->simulation.interval_steps !=
            RELAY_SOURCE_MINER_INTERVAL_STEPS ||
        definition->simulation.output_port_index != 0 ||
        definition->simulation.output_amount != 1 ||
        !relay_node_world_init(&world)) {
        return false;
    }
    node_id = relay_node_world_create(&world, id, 0, 0);
    node = relay_node_world_find(&world, node_id);
    valid = node != NULL &&
        relay_node_property_get(node, "node.enabled", &value, &value_type) &&
        value_type == RELAY_NODE_VALUE_BOOLEAN && value.boolean &&
        !relay_node_property_get(node, "process.progress", &value,
            &value_type) &&
        !relay_node_property_get(node, "resource.coal", &value, &value_type);
    relay_node_world_shutdown(&world);
    return valid;
}

/** Verify autonomous miners, Timer behavior, shop data, and typed wires. */
int relay_game_test(void)
{
    static const Relay_NodeDefinitionId miner_ids[] = {
        RELAY_NODE_DEFINITION_COAL_MINER,
        RELAY_NODE_DEFINITION_IRON_MINER,
        RELAY_NODE_DEFINITION_COPPER_MINER,
        RELAY_NODE_DEFINITION_STONE_MINER
    };
    static const char *miner_keys[] = {
        "source.coal_miner",
        "source.iron_miner",
        "source.copper_miner",
        "source.stone_miner"
    };
    static const Relay_NodePortType miner_types[] = {
        RELAY_NODE_PORT_TYPE_COAL,
        RELAY_NODE_PORT_TYPE_IRON_ORE,
        RELAY_NODE_PORT_TYPE_COPPER_ORE,
        RELAY_NODE_PORT_TYPE_STONE
    };
    Relay_Game game = {0};
    Relay_ScriptRuntime scripts = {0};
    Relay_Node *miner;
    Relay_Node *timer;
    Relay_NodeValue value;
    Relay_NodeValueType value_type;
    Relay_NodeRenderCard card;
    const Relay_NodeDefinition *timer_definition;
    const Relay_NodeDefinition *furnace_definition;
    const Relay_NodeDefinition *storage_definition;
    Relay_NodeVisual trigger_port_visual;
    Relay_NodeVisual coal_port_visual;
    Relay_NodeVisual iron_port_visual;
    Relay_NodeVisual copper_port_visual;
    Relay_NodeId source_node_ids[4] = {0};
    Relay_NodeId timer_id;
    size_t index;

    if (!relay_test_physical_item_queues()) {
        return 1;
    }
    timer_definition = relay_node_definition_find(RELAY_NODE_DEFINITION_TIMER);
    furnace_definition = relay_node_definition_find(
        RELAY_NODE_DEFINITION_STONE_FURNACE);
    storage_definition = relay_node_definition_find(
        RELAY_NODE_DEFINITION_STORAGE);
    trigger_port_visual = relay_node_renderer_port_visual(
        RELAY_NODE_PORT_TYPE_TRIGGER);
    coal_port_visual = relay_node_renderer_port_visual(RELAY_NODE_PORT_TYPE_COAL);
    iron_port_visual = relay_node_renderer_port_visual(
        RELAY_NODE_PORT_TYPE_IRON);
    copper_port_visual = relay_node_renderer_port_visual(
        RELAY_NODE_PORT_TYPE_COPPER);
    if (!relay_script_runtime_init(&scripts,
            RELAY_SCRIPT_RUNTIME_DEFAULT_MEMORY_LIMIT) ||
        !relay_game_init(&game, &scripts) || game.currency != 100 ||
        game.nodes.count != 1 || relay_node_definition_count() != 7 ||
        relay_game_shop_offer_count() != 7 || timer_definition == NULL ||
        strcmp(timer_definition->glyph, RELAY_ICON_TIMER) != 0 ||
        timer_definition->simulation.behavior != RELAY_NODE_BEHAVIOR_TIMER ||
        timer_definition->output_count != 1 ||
        timer_definition->outputs[0].type != RELAY_NODE_PORT_TYPE_TRIGGER ||
        strcmp(timer_definition->outputs[0].key, "trigger") != 0 ||
        furnace_definition == NULL ||
        strcmp(furnace_definition->key, "processor.stone_furnace") != 0 ||
        strcmp(furnace_definition->glyph, RELAY_ICON_FURNACE) != 0 ||
        furnace_definition->input_count != 3 ||
        furnace_definition->output_count != 2 ||
        furnace_definition->simulation.behavior !=
            RELAY_NODE_BEHAVIOR_ITEM_PROCESSOR ||
        furnace_definition->simulation.recipe_count != 2 ||
        storage_definition == NULL ||
        strcmp(storage_definition->key, "logistics.storage") != 0 ||
        strcmp(storage_definition->glyph, RELAY_ICON_STORAGE) != 0 ||
        storage_definition->category != RELAY_NODE_CATEGORY_LOGISTICS ||
        storage_definition->input_count != 6 ||
        storage_definition->output_count != 6 ||
        storage_definition->simulation.behavior !=
            RELAY_NODE_BEHAVIOR_ITEM_STORAGE ||
        storage_definition->simulation.route_count != 6 ||
        trigger_port_visual.color == coal_port_visual.color ||
        iron_port_visual.color == copper_port_visual.color ||
        !relay_node_port_type_is_item(RELAY_NODE_PORT_TYPE_IRON) ||
        !relay_node_port_type_is_item(RELAY_NODE_PORT_TYPE_COPPER)) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    if (relay_game_shop_offer_at(5) == NULL ||
        relay_game_shop_offer_at(5)->definition_id !=
            RELAY_NODE_DEFINITION_STONE_FURNACE ||
        relay_game_shop_offer_at(6) == NULL ||
        relay_game_shop_offer_at(6)->definition_id !=
            RELAY_NODE_DEFINITION_STORAGE) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    for (index = 0; index < sizeof(miner_ids) / sizeof(miner_ids[0]);
            index++) {
        const Relay_ShopOffer *offer = relay_game_shop_offer_at(index + 1);
        const Relay_NodeDefinition *definition =
            relay_node_definition_find(miner_ids[index]);

        if (!relay_test_source_definition(miner_ids[index],
                miner_keys[index], miner_types[index]) ||
            offer == NULL || offer->definition_id != miner_ids[index] ||
            definition == NULL ||
            strcmp(definition->glyph, RELAY_ICON_MINER) != 0) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
    }
    miner = &game.nodes.nodes[0];
    source_node_ids[0] = miner->id;
    card = relay_node_renderer_card(miner);
    if (card.definition == NULL || card.definition->input_count != 0 ||
        card.definition->output_count != 1 || card.height != 6 ||
        relay_game_handle_input(&game, RELAY_GAME_INPUT_CONFIRM) !=
            RELAY_GAME_ACTION_PURCHASED || game.nodes.count != 2) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    timer = &game.nodes.nodes[1];
    timer_id = timer->id;
    if (timer->definition_id != RELAY_NODE_DEFINITION_TIMER ||
        game.focused_node_id != timer->id ||
        relay_game_connect_nodes(&game, timer->id, 0, miner->id, 0) ||
        relay_game_connect_nodes(&game, miner->id, 0, miner->id, 0) ||
        !relay_node_property_get(timer, "timer.interval_steps", &value,
            &value_type) ||
        value_type != RELAY_NODE_VALUE_INTEGER ||
        value.integer != RELAY_TIMER_DEFAULT_INTERVAL_STEPS) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    value.boolean = false;
    if (!relay_node_property_set(miner, "node.enabled",
            RELAY_NODE_VALUE_BOOLEAN, value) ||
        !relay_game_step(&game) || miner->progress != 0) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    value.boolean = true;
    if (!relay_node_property_set(miner, "node.enabled",
            RELAY_NODE_VALUE_BOOLEAN, value)) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    for (index = 1; index < sizeof(miner_ids) / sizeof(miner_ids[0]);
            index++) {
        source_node_ids[index] = relay_node_world_create(&game.nodes,
            miner_ids[index], (int64_t)index * 30, 0);
        if (source_node_ids[index] == 0) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
    }
    miner = relay_node_world_find(&game.nodes, source_node_ids[0]);
    timer = relay_node_world_find(&game.nodes, timer_id);
    if (miner == NULL || timer == NULL) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    for (index = 0; index < RELAY_SOURCE_MINER_INTERVAL_STEPS; index++) {
        if (!relay_game_step(&game)) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
    }
    for (index = 0; index < sizeof(source_node_ids) /
            sizeof(source_node_ids[0]); index++) {
        const Relay_Node *source = relay_node_world_find_const(&game.nodes,
            source_node_ids[index]);

        Relay_Item item;

        if (source == NULL || source->produced != 1 ||
            source->progress != 0 || source->output_values[0] != 0 ||
            source->output_queues[0].count != 1 ||
            !relay_item_queue_peek(&source->output_queues[0], &item) ||
            item.type != miner_types[index] || item.id == 0) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
    }
    if (miner->produced != 1 || miner->progress != 0 ||
        miner->output_queues[0].count != 1 ||
        !relay_node_world_items_valid(&game.nodes) ||
        game.simulation_step != RELAY_SOURCE_MINER_INTERVAL_STEPS + 1 ||
        !relay_node_property_get(timer, "timer.triggers", &value,
            &value_type) || value.integer != 1 ||
        relay_game_connect_nodes(&game, miner->id, 0, timer->id, 0) ||
        !relay_game_step(&game) || miner->output_queues[0].count != 1 ||
        miner->progress != 1) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    if (relay_game_handle_input(&game,
            RELAY_GAME_INPUT_NEXT_TIMER_INTERVAL) !=
            RELAY_GAME_ACTION_NONE || timer->timer_interval_steps != 120 ||
        relay_game_handle_input(&game, RELAY_GAME_INPUT_TOGGLE_PANEL_TAB) !=
            RELAY_GAME_ACTION_NONE || game.active_tab !=
                RELAY_GAME_PANEL_TAB_INSPECTOR ||
        relay_game_handle_input(&game, RELAY_GAME_INPUT_TOGGLE_PANEL_TAB) !=
            RELAY_GAME_ACTION_NONE || game.active_tab !=
                RELAY_GAME_PANEL_TAB_BLUEPRINTS ||
        relay_game_handle_input(&game, RELAY_GAME_INPUT_TOGGLE_PANEL_TAB) !=
            RELAY_GAME_ACTION_NONE ||
        game.active_tab != RELAY_GAME_PANEL_TAB_SHOP ||
        !relay_game_focus_node(&game, miner->id) ||
        game.focused_node_id != miner->id ||
        game.active_tab != RELAY_GAME_PANEL_TAB_INSPECTOR ||
        relay_game_handle_input(&game, RELAY_GAME_INPUT_TOGGLE_MAP) !=
            RELAY_GAME_ACTION_NONE || !relay_game_back(&game) ||
        !relay_game_move_node(&game, miner->id, -9, -7) ||
        miner->grid_x != -9 || miner->grid_y != -7) {
        relay_game_shutdown(&game);
        relay_script_runtime_shutdown(&scripts);
        return 1;
    }
    {
        const Relay_NodeId furnace_id = relay_node_world_create(&game.nodes,
            RELAY_NODE_DEFINITION_STONE_FURNACE, 150, 0);
        const Relay_NodeId storage_id = relay_node_world_create(&game.nodes,
            RELAY_NODE_DEFINITION_STORAGE, 180, 0);
        Relay_Node *furnace = relay_node_world_find(&game.nodes, furnace_id);
        Relay_Node *storage = relay_node_world_find(&game.nodes, storage_id);
        Relay_Item coal_a;
        Relay_Item coal_b;
        Relay_Item iron_ore;
        Relay_Item copper_ore;
        Relay_Item iron;
        Relay_Item copper;
        Relay_ItemId iron_id;

        if (furnace == NULL || storage == NULL ||
            !relay_node_world_item_create(&game.nodes,
                RELAY_NODE_PORT_TYPE_COAL, &coal_a) ||
            !relay_node_world_item_create(&game.nodes,
                RELAY_NODE_PORT_TYPE_COAL, &coal_b) ||
            !relay_node_world_item_create(&game.nodes,
                RELAY_NODE_PORT_TYPE_IRON_ORE, &iron_ore) ||
            !relay_node_world_item_create(&game.nodes,
                RELAY_NODE_PORT_TYPE_COPPER_ORE, &copper_ore) ||
            !relay_item_queue_push(&furnace->input_queues[0], coal_a) ||
            !relay_item_queue_push(&furnace->input_queues[0], coal_b) ||
            !relay_item_queue_push(&furnace->input_queues[1], iron_ore) ||
            !relay_item_queue_push(&furnace->input_queues[2], copper_ore)) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
        for (index = 0; index < RELAY_STONE_FURNACE_INTERVAL_STEPS; index++) {
            if (!relay_game_step(&game)) {
                relay_game_shutdown(&game);
                relay_script_runtime_shutdown(&scripts);
                return 1;
            }
        }
        if (furnace->produced != 1 || furnace->progress != 0 ||
            furnace->input_queues[0].count != 1 ||
            !relay_item_queue_empty(&furnace->input_queues[1]) ||
            furnace->input_queues[2].count != 1 ||
            !relay_item_queue_peek(&furnace->output_queues[0], &iron) ||
            iron.type != RELAY_NODE_PORT_TYPE_IRON ||
            iron.id == coal_a.id || iron.id == iron_ore.id) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
        iron_id = iron.id;
        for (index = 0; index < RELAY_STONE_FURNACE_INTERVAL_STEPS; index++) {
            if (!relay_game_step(&game)) {
                relay_game_shutdown(&game);
                relay_script_runtime_shutdown(&scripts);
                return 1;
            }
        }
        if (furnace->produced != 2 ||
            !relay_item_queue_empty(&furnace->input_queues[0]) ||
            !relay_item_queue_empty(&furnace->input_queues[2]) ||
            !relay_item_queue_peek(&furnace->output_queues[1], &copper) ||
            copper.type != RELAY_NODE_PORT_TYPE_COPPER ||
            !relay_game_connect_nodes(&game, furnace_id, 0, storage_id, 4) ||
            !relay_game_step(&game) ||
            !relay_item_queue_empty(&furnace->output_queues[0]) ||
            !relay_item_queue_peek(&storage->output_queues[4], &iron) ||
            iron.type != RELAY_NODE_PORT_TYPE_IRON ||
            iron.id != iron_id ||
            !relay_node_world_items_valid(&game.nodes)) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
    }
    {
        const size_t queue_count = miner->output_queues[0].count;
        const int64_t produced = miner->produced;

        miner->progress = RELAY_SOURCE_MINER_INTERVAL_STEPS - 1;
        game.nodes.next_item_id = UINT64_MAX;
        if (relay_game_step(&game) ||
            miner->output_queues[0].count != queue_count ||
            miner->produced != produced ||
            game.nodes.next_item_id != UINT64_MAX) {
            relay_game_shutdown(&game);
            relay_script_runtime_shutdown(&scripts);
            return 1;
        }
    }
    relay_game_shutdown(&game);
    relay_script_runtime_shutdown(&scripts);
    return 0;
}
