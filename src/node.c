#include "relay/node.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const int64_t relay_timer_intervals[] = {60, 120, 240, 480, 960};

/** Script schema implemented by every node instance. */
static const Relay_NodePropertyDefinition relay_universal_properties[] = {
    {"node.enabled", RELAY_NODE_VALUE_BOOLEAN, true, {.boolean = true}}
};

/** Script schema for the optional deterministic timer. */
static const Relay_NodePropertyDefinition relay_timer_properties[] = {
    {"node.enabled", RELAY_NODE_VALUE_BOOLEAN, true, {.boolean = true}},
    {"timer.interval_steps", RELAY_NODE_VALUE_INTEGER, true,
        {.integer = RELAY_TIMER_DEFAULT_INTERVAL_STEPS}},
    {"timer.triggers", RELAY_NODE_VALUE_INTEGER, false, {.integer = 0}}
};

/** Event output emitted by the optional deterministic timer. */
static const Relay_NodePortDefinition relay_timer_outputs[] = {
    {"trigger", "Trigger", RELAY_NODE_PORT_TYPE_TRIGGER}
};

static const Relay_NodePortDefinition relay_coal_miner_outputs[] = {
    {"coal", "Coal", RELAY_NODE_PORT_TYPE_COAL}
};

static const Relay_NodePortDefinition relay_iron_miner_outputs[] = {
    {"iron_ore", "Iron Ore", RELAY_NODE_PORT_TYPE_IRON_ORE}
};

static const Relay_NodePortDefinition relay_copper_miner_outputs[] = {
    {"copper_ore", "Copper Ore", RELAY_NODE_PORT_TYPE_COPPER_ORE}
};

static const Relay_NodePortDefinition relay_stone_miner_outputs[] = {
    {"stone", "Stone", RELAY_NODE_PORT_TYPE_STONE}
};

/** Immutable built-in module catalog with script-stable identifiers and keys. */
static const Relay_NodeDefinition relay_node_definitions[] = {
    {
        .id = RELAY_NODE_DEFINITION_TIMER,
        .key = "control.timer",
        .display_name = "Timer",
        .glyph = "◷",
        .description = "Emits an optional periodic trigger.",
        .category = RELAY_NODE_CATEGORY_SOURCE,
        .outputs = relay_timer_outputs,
        .output_count = sizeof(relay_timer_outputs) /
            sizeof(relay_timer_outputs[0]),
        .properties = relay_timer_properties,
        .property_count = sizeof(relay_timer_properties) /
            sizeof(relay_timer_properties[0]),
        .simulation = {RELAY_NODE_BEHAVIOR_TIMER,
            RELAY_TIMER_DEFAULT_INTERVAL_STEPS, 0, 1}
    },
    {
        .id = RELAY_NODE_DEFINITION_COAL_MINER,
        .key = "source.coal_miner",
        .display_name = "Coal Miner",
        .glyph = "◆",
        .description = "Produces one Coal each second while enabled.",
        .category = RELAY_NODE_CATEGORY_SOURCE,
        .outputs = relay_coal_miner_outputs,
        .output_count = sizeof(relay_coal_miner_outputs) /
            sizeof(relay_coal_miner_outputs[0]),
        .properties = relay_universal_properties,
        .property_count = sizeof(relay_universal_properties) /
            sizeof(relay_universal_properties[0]),
        .simulation = {RELAY_NODE_BEHAVIOR_FIXED_RATE_SOURCE,
            RELAY_SOURCE_MINER_INTERVAL_STEPS, 0, 1}
    },
    {
        .id = RELAY_NODE_DEFINITION_IRON_MINER,
        .key = "source.iron_miner",
        .display_name = "Iron Miner",
        .glyph = "◆",
        .description = "Produces one Iron Ore each second while enabled.",
        .category = RELAY_NODE_CATEGORY_SOURCE,
        .outputs = relay_iron_miner_outputs,
        .output_count = sizeof(relay_iron_miner_outputs) /
            sizeof(relay_iron_miner_outputs[0]),
        .properties = relay_universal_properties,
        .property_count = sizeof(relay_universal_properties) /
            sizeof(relay_universal_properties[0]),
        .simulation = {RELAY_NODE_BEHAVIOR_FIXED_RATE_SOURCE,
            RELAY_SOURCE_MINER_INTERVAL_STEPS, 0, 1}
    },
    {
        .id = RELAY_NODE_DEFINITION_COPPER_MINER,
        .key = "source.copper_miner",
        .display_name = "Copper Miner",
        .glyph = "◆",
        .description = "Produces one Copper Ore each second while enabled.",
        .category = RELAY_NODE_CATEGORY_SOURCE,
        .outputs = relay_copper_miner_outputs,
        .output_count = sizeof(relay_copper_miner_outputs) /
            sizeof(relay_copper_miner_outputs[0]),
        .properties = relay_universal_properties,
        .property_count = sizeof(relay_universal_properties) /
            sizeof(relay_universal_properties[0]),
        .simulation = {RELAY_NODE_BEHAVIOR_FIXED_RATE_SOURCE,
            RELAY_SOURCE_MINER_INTERVAL_STEPS, 0, 1}
    },
    {
        .id = RELAY_NODE_DEFINITION_STONE_MINER,
        .key = "source.stone_miner",
        .display_name = "Stone Miner",
        .glyph = "◆",
        .description = "Produces one Stone each second while enabled.",
        .category = RELAY_NODE_CATEGORY_SOURCE,
        .outputs = relay_stone_miner_outputs,
        .output_count = sizeof(relay_stone_miner_outputs) /
            sizeof(relay_stone_miner_outputs[0]),
        .properties = relay_universal_properties,
        .property_count = sizeof(relay_universal_properties) /
            sizeof(relay_universal_properties[0]),
        .simulation = {RELAY_NODE_BEHAVIOR_FIXED_RATE_SOURCE,
            RELAY_SOURCE_MINER_INTERVAL_STEPS, 0, 1}
    }
};

/** Grow node storage to accept one more world instance. */
static bool relay_node_world_grow(Relay_NodeWorld *world)
{
    Relay_Node *nodes;
    size_t capacity;

    if (world->count < world->capacity) {
        return true;
    }
    if (world->capacity > SIZE_MAX / 2) {
        return false;
    }
    capacity = world->capacity == 0 ? 16 : world->capacity * 2;
    nodes = realloc(world->nodes, capacity * sizeof(*nodes));
    if (nodes == NULL) {
        return false;
    }
    world->nodes = nodes;
    world->capacity = capacity;
    return true;
}

/** Grow graph connection storage to accept one additional wire. */
static bool relay_node_world_grow_connections(Relay_NodeWorld *world)
{
    Relay_NodeConnection *connections;
    size_t capacity;

    if (world->connection_count < world->connection_capacity) {
        return true;
    }
    if (world->connection_capacity > SIZE_MAX / 2) {
        return false;
    }
    capacity = world->connection_capacity == 0 ? 16 :
        world->connection_capacity * 2;
    connections = realloc(world->connections, capacity * sizeof(*connections));
    if (connections == NULL) {
        return false;
    }
    world->connections = connections;
    world->connection_capacity = capacity;
    return true;
}

/** Grow one dynamically allocated module-binding array. */
static bool relay_node_world_grow_bindings(void **storage, size_t *capacity,
    size_t count, size_t element_size)
{
    size_t next_capacity;
    void *bindings;

    if (count < *capacity) {
        return true;
    }
    if (*capacity > SIZE_MAX / 2 ||
        (*capacity != 0 && *capacity * 2 > SIZE_MAX / element_size)) {
        return false;
    }
    next_capacity = *capacity == 0 ? 16 : *capacity * 2;
    bindings = realloc(*storage, next_capacity * element_size);
    if (bindings == NULL) {
        return false;
    }
    *storage = bindings;
    *capacity = next_capacity;
    return true;
}

/** Find a definition property by its script-stable key. */
static const Relay_NodePropertyDefinition *relay_node_property_definition_find(
    const Relay_NodeDefinition *definition, const char *key)
{
    size_t index;

    if (definition == NULL || key == NULL) {
        return NULL;
    }
    for (index = 0; index < definition->property_count; index++) {
        if (strcmp(definition->properties[index].key, key) == 0) {
            return &definition->properties[index];
        }
    }
    return NULL;
}

/** Validate a definition's storage and executable simulation contract. */
static bool relay_node_definition_valid(const Relay_NodeDefinition *definition)
{
    const Relay_NodeSimulationDefinition *simulation;
    const Relay_NodePropertyDefinition *enabled_property;

    if (definition == NULL || definition->id == 0 || definition->key == NULL ||
        definition->display_name == NULL || definition->glyph == NULL ||
        definition->description == NULL ||
        definition->input_count > RELAY_NODE_MAX_PORTS ||
        definition->output_count > RELAY_NODE_MAX_PORTS ||
        (definition->input_count > 0 && definition->inputs == NULL) ||
        (definition->output_count > 0 && definition->outputs == NULL) ||
        (definition->property_count > 0 && definition->properties == NULL)) {
        return false;
    }
    enabled_property = relay_node_property_definition_find(definition,
        "node.enabled");
    if (enabled_property == NULL ||
        enabled_property->value_type != RELAY_NODE_VALUE_BOOLEAN ||
        !enabled_property->writable) {
        return false;
    }
    simulation = &definition->simulation;
    if (simulation->behavior == RELAY_NODE_BEHAVIOR_NONE) {
        return true;
    }
    return (simulation->behavior == RELAY_NODE_BEHAVIOR_TIMER ||
            simulation->behavior == RELAY_NODE_BEHAVIOR_FIXED_RATE_SOURCE) &&
        simulation->interval_steps > 0 &&
        simulation->output_port_index < definition->output_count &&
        simulation->output_amount > 0;
}

bool relay_node_world_init(Relay_NodeWorld *world)
{
    if (world == NULL) {
        return false;
    }
    *world = (Relay_NodeWorld){0};
    world->next_id = 1;
    world->next_item_id = 1;
    return true;
}

Relay_NodeId relay_node_world_create(Relay_NodeWorld *world,
    Relay_NodeDefinitionId definition_id, int64_t grid_x, int64_t grid_y)
{
    return relay_node_world_create_definition(world,
        relay_node_definition_find(definition_id), grid_x, grid_y);
}

Relay_NodeId relay_node_world_create_definition(Relay_NodeWorld *world,
    const Relay_NodeDefinition *definition, int64_t grid_x, int64_t grid_y)
{
    Relay_Node *node;
    Relay_NodeId id;

    if (world == NULL || world->next_id == 0 ||
        world->next_id == UINT64_MAX ||
        !relay_node_definition_valid(definition) ||
        !relay_node_world_grow(world)) {
        return 0;
    }
    id = world->next_id++;
    node = &world->nodes[world->count++];
    *node = (Relay_Node){0};
    node->id = id;
    node->definition_id = definition->id;
    node->definition = definition;
    (void)snprintf(node->local_key, sizeof(node->local_key), "n%llu",
        (unsigned long long)id);
    node->grid_x = grid_x;
    node->grid_y = grid_y;
    node->timer_interval_steps =
        definition->simulation.behavior == RELAY_NODE_BEHAVIOR_TIMER ?
        definition->simulation.interval_steps : 0;
    node->enabled = true;
    return id;
}

Relay_Node *relay_node_world_find(Relay_NodeWorld *world, Relay_NodeId id)
{
    size_t index;

    if (world == NULL || id == 0) {
        return NULL;
    }
    for (index = 0; index < world->count; index++) {
        if (world->nodes[index].id == id) {
            return &world->nodes[index];
        }
    }
    return NULL;
}

const Relay_Node *relay_node_world_find_const(const Relay_NodeWorld *world,
    Relay_NodeId id)
{
    size_t index;

    if (world == NULL || id == 0) {
        return NULL;
    }
    for (index = 0; index < world->count; index++) {
        if (world->nodes[index].id == id) {
            return &world->nodes[index];
        }
    }
    return NULL;
}

const Relay_NodeDefinition *relay_node_definition_for(const Relay_Node *node)
{
    return node == NULL ? NULL : node->definition;
}

bool relay_node_world_move(Relay_NodeWorld *world, Relay_NodeId id,
    int64_t grid_x, int64_t grid_y)
{
    Relay_Node *node = relay_node_world_find(world, id);

    if (node == NULL) {
        return false;
    }
    node->grid_x = grid_x;
    node->grid_y = grid_y;
    return true;
}

/** Return a stable display label for a fixed graph port type. */
const char *relay_node_port_type_name(Relay_NodePortType type)
{
    switch (type) {
    case RELAY_NODE_PORT_TYPE_TRIGGER: return "Trigger";
    case RELAY_NODE_PORT_TYPE_COAL: return "Coal";
    case RELAY_NODE_PORT_TYPE_IRON_ORE: return "Iron Ore";
    case RELAY_NODE_PORT_TYPE_COPPER_ORE: return "Copper Ore";
    case RELAY_NODE_PORT_TYPE_STONE: return "Stone";
    case RELAY_NODE_PORT_TYPE_BOOLEAN: return "Boolean";
    case RELAY_NODE_PORT_TYPE_INTEGER: return "Integer";
    case RELAY_NODE_PORT_TYPE_TEXT: return "Text";
    case RELAY_NODE_PORT_TYPE_INVALID: return "Invalid";
    }
    return "Invalid";
}

/** Check exact fixed-type compatibility for a graph connection. */
bool relay_node_port_types_compatible(Relay_NodePortType source_type,
    Relay_NodePortType destination_type)
{
    return source_type != RELAY_NODE_PORT_TYPE_INVALID &&
        source_type == destination_type;
}

bool relay_node_port_type_is_transient(Relay_NodePortType type)
{
    return type == RELAY_NODE_PORT_TYPE_TRIGGER;
}

bool relay_node_port_type_is_item(Relay_NodePortType type)
{
    return type == RELAY_NODE_PORT_TYPE_COAL ||
        type == RELAY_NODE_PORT_TYPE_IRON_ORE ||
        type == RELAY_NODE_PORT_TYPE_COPPER_ORE ||
        type == RELAY_NODE_PORT_TYPE_STONE;
}

bool relay_item_queue_empty(const Relay_ItemQueue *queue)
{
    return queue == NULL || queue->count == 0;
}

bool relay_item_queue_full(const Relay_ItemQueue *queue)
{
    return queue == NULL || queue->count >= RELAY_ITEM_QUEUE_CAPACITY;
}

bool relay_item_queue_peek(const Relay_ItemQueue *queue, Relay_Item *item)
{
    if (queue == NULL || item == NULL || queue->count == 0 ||
        queue->head >= RELAY_ITEM_QUEUE_CAPACITY) {
        return false;
    }
    *item = queue->items[queue->head];
    return true;
}

bool relay_item_queue_push(Relay_ItemQueue *queue, Relay_Item item)
{
    size_t tail;

    if (queue == NULL || item.id == 0 ||
        !relay_node_port_type_is_item(item.type) ||
        queue->head >= RELAY_ITEM_QUEUE_CAPACITY ||
        queue->count >= RELAY_ITEM_QUEUE_CAPACITY) {
        return false;
    }
    tail = (queue->head + queue->count) % RELAY_ITEM_QUEUE_CAPACITY;
    queue->items[tail] = item;
    queue->count++;
    return true;
}

bool relay_item_queue_pop(Relay_ItemQueue *queue, Relay_Item *item)
{
    if (!relay_item_queue_peek(queue, item)) {
        return false;
    }
    queue->items[queue->head] = (Relay_Item){0};
    queue->head = (queue->head + 1) % RELAY_ITEM_QUEUE_CAPACITY;
    queue->count--;
    if (queue->count == 0) {
        queue->head = 0;
    }
    return true;
}

bool relay_item_queue_transfer(Relay_ItemQueue *source,
    Relay_ItemQueue *destination, Relay_NodePortType expected_type)
{
    Relay_Item item;

    if (source == NULL || destination == NULL || source == destination ||
        relay_item_queue_full(destination) ||
        !relay_item_queue_peek(source, &item) || item.type != expected_type ||
        !relay_node_port_type_is_item(expected_type)) {
        return false;
    }
    if (!relay_item_queue_push(destination, item)) {
        return false;
    }
    (void)relay_item_queue_pop(source, &item);
    return true;
}

bool relay_node_world_item_create(Relay_NodeWorld *world,
    Relay_NodePortType type, Relay_Item *item)
{
    Relay_ItemId id;

    if (world == NULL || item == NULL ||
        !relay_node_port_type_is_item(type) || world->next_item_id == 0) {
        return false;
    }
    id = world->next_item_id++;
    if (world->next_item_id == 0) {
        world->next_item_id = id;
        return false;
    }
    *item = (Relay_Item){id, type};
    return true;
}

/** Compare stable item identities for deterministic duplicate validation. */
static int relay_node_item_id_compare(const void *left, const void *right)
{
    const Relay_ItemId left_id = *(const Relay_ItemId *)left;
    const Relay_ItemId right_id = *(const Relay_ItemId *)right;

    return (left_id > right_id) - (left_id < right_id);
}

/** Validate one queue and append its item identities to caller storage. */
static bool relay_node_queue_items_valid(const Relay_ItemQueue *queue,
    Relay_NodePortType type, Relay_ItemId *ids, size_t *id_count,
    size_t id_capacity, Relay_ItemId next_item_id)
{
    size_t index;

    if (queue->head >= RELAY_ITEM_QUEUE_CAPACITY ||
        queue->count > RELAY_ITEM_QUEUE_CAPACITY ||
        !relay_node_port_type_is_item(type)) {
        return false;
    }
    for (index = 0; index < queue->count; index++) {
        const Relay_Item item = queue->items[
            (queue->head + index) % RELAY_ITEM_QUEUE_CAPACITY];

        if (item.id == 0 || item.id >= next_item_id || item.type != type ||
            *id_count >= id_capacity) {
            return false;
        }
        ids[(*id_count)++] = item.id;
    }
    return true;
}

bool relay_node_world_items_valid(const Relay_NodeWorld *world)
{
    Relay_ItemId *ids;
    size_t id_count = 0;
    size_t id_capacity;
    size_t node_index;
    bool valid = true;

    if (world == NULL || world->next_item_id == 0 ||
        world->count > SIZE_MAX / (RELAY_NODE_MAX_PORTS * 2U) ||
        world->count * RELAY_NODE_MAX_PORTS * 2U >
            SIZE_MAX / RELAY_ITEM_QUEUE_CAPACITY) {
        return false;
    }
    id_capacity = world->count * RELAY_NODE_MAX_PORTS * 2U *
        RELAY_ITEM_QUEUE_CAPACITY;
    if (id_capacity > SIZE_MAX / sizeof(*ids)) {
        return false;
    }
    ids = id_capacity == 0 ? NULL : malloc(id_capacity * sizeof(*ids));
    if (ids == NULL && id_capacity > 0) {
        return false;
    }
    for (node_index = 0; node_index < world->count && valid; node_index++) {
        const Relay_Node *node = &world->nodes[node_index];
        const Relay_NodeDefinition *definition =
            relay_node_definition_for(node);
        size_t port_index;

        if (definition == NULL) {
            valid = false;
            break;
        }
        for (port_index = 0; port_index < RELAY_NODE_MAX_PORTS; port_index++) {
            const bool input_item = port_index < definition->input_count &&
                relay_node_port_type_is_item(
                    definition->inputs[port_index].type);
            const bool output_item = port_index < definition->output_count &&
                relay_node_port_type_is_item(
                    definition->outputs[port_index].type);

            if ((input_item &&
                    !relay_node_queue_items_valid(
                        &node->input_queues[port_index],
                        definition->inputs[port_index].type, ids, &id_count,
                        id_capacity, world->next_item_id)) ||
                (!input_item && node->input_queues[port_index].count != 0) ||
                (output_item &&
                    !relay_node_queue_items_valid(
                        &node->output_queues[port_index],
                        definition->outputs[port_index].type, ids, &id_count,
                        id_capacity, world->next_item_id)) ||
                (!output_item && node->output_queues[port_index].count != 0)) {
                valid = false;
                break;
            }
        }
    }
    if (valid && id_count > 1) {
        qsort(ids, id_count, sizeof(*ids), relay_node_item_id_compare);
        for (node_index = 1; node_index < id_count; node_index++) {
            if (ids[node_index - 1] == ids[node_index]) {
                valid = false;
                break;
            }
        }
    }
    free(ids);
    return valid;
}

bool relay_node_world_connect(Relay_NodeWorld *world, Relay_NodeId source_node_id,
    size_t source_port_index, Relay_NodeId destination_node_id,
    size_t destination_port_index)
{
    const Relay_Node *source;
    const Relay_Node *destination;
    const Relay_NodeDefinition *source_definition;
    const Relay_NodeDefinition *destination_definition;
    size_t index;

    if (world == NULL || source_node_id == 0 || destination_node_id == 0) {
        return false;
    }
    source = relay_node_world_find_const(world, source_node_id);
    destination = relay_node_world_find_const(world, destination_node_id);
    source_definition = relay_node_definition_for(source);
    destination_definition = relay_node_definition_for(destination);
    if (source_definition == NULL || destination_definition == NULL ||
        source->module_instance_id != destination->module_instance_id ||
        source_port_index >= source_definition->output_count ||
        destination_port_index >= destination_definition->input_count ||
        !relay_node_port_types_compatible(
            source_definition->outputs[source_port_index].type,
            destination_definition->inputs[destination_port_index].type)) {
        return false;
    }
    for (index = 0; index < world->module_input_binding_count; index++) {
        const Relay_NodeModuleInputBinding *binding =
            &world->module_input_bindings[index];

        if (binding->destination_node_id == destination_node_id &&
            binding->destination_port_index == destination_port_index) {
            return false;
        }
    }
    if (relay_node_port_type_is_item(
            source_definition->outputs[source_port_index].type)) {
        for (index = 0; index < world->module_output_binding_count; index++) {
            const Relay_NodeModuleOutputBinding *binding =
                &world->module_output_bindings[index];

            if (!binding->source_is_module_input &&
                binding->source_node_id == source_node_id &&
                binding->source_port_index == source_port_index) {
                return false;
            }
        }
    }
    for (index = 0; index < world->connection_count; index++) {
        Relay_NodeConnection *connection = &world->connections[index];

        if (relay_node_port_type_is_item(
                source_definition->outputs[source_port_index].type) &&
            connection->source_node_id == source_node_id &&
            connection->source_port_index == source_port_index &&
            (connection->destination_node_id != destination_node_id ||
                connection->destination_port_index !=
                    destination_port_index)) {
            return false;
        }
        if (connection->destination_node_id == destination_node_id &&
            connection->destination_port_index == destination_port_index) {
            *connection = (Relay_NodeConnection){source_node_id, source_port_index,
                destination_node_id, destination_port_index};
            return true;
        }
    }
    if (!relay_node_world_grow_connections(world)) {
        return false;
    }
    world->connections[world->connection_count++] = (Relay_NodeConnection){
        source_node_id, source_port_index, destination_node_id,
        destination_port_index};
    return true;
}

const Relay_NodeConnection *relay_node_world_connection_to(
    const Relay_NodeWorld *world, Relay_NodeId destination_node_id,
    size_t destination_port_index)
{
    size_t index;

    if (world == NULL || destination_node_id == 0) {
        return NULL;
    }
    for (index = 0; index < world->connection_count; index++) {
        const Relay_NodeConnection *connection = &world->connections[index];

        if (connection->destination_node_id == destination_node_id &&
            connection->destination_port_index == destination_port_index) {
            return connection;
        }
    }
    return NULL;
}

bool relay_node_world_bind_module_input(Relay_NodeWorld *world,
    Relay_NodeModuleInputBinding binding)
{
    const Relay_Node *module;
    const Relay_Node *destination;
    const Relay_NodeDefinition *module_definition;
    const Relay_NodeDefinition *destination_definition;
    size_t index;

    if (world == NULL) {
        return false;
    }
    module = relay_node_world_find_const(world, binding.module_node_id);
    destination = relay_node_world_find_const(world,
        binding.destination_node_id);
    module_definition = relay_node_definition_for(module);
    destination_definition = relay_node_definition_for(destination);
    if (module == NULL || destination == NULL ||
        module->runtime_kind != RELAY_NODE_RUNTIME_BLUEPRINT_WRAPPER ||
        module->module_instance_id != 0 ||
        destination->module_instance_id != module->id ||
        module_definition == NULL || destination_definition == NULL ||
        binding.module_port_index >= module_definition->input_count ||
        binding.destination_port_index >= destination_definition->input_count ||
        !relay_node_port_types_compatible(
            module_definition->inputs[binding.module_port_index].type,
            destination_definition->inputs[
                binding.destination_port_index].type) ||
        relay_node_world_connection_to(world, binding.destination_node_id,
            binding.destination_port_index) != NULL) {
        return false;
    }
    for (index = 0; index < world->module_input_binding_count; index++) {
        const Relay_NodeModuleInputBinding *existing =
            &world->module_input_bindings[index];

        if ((existing->destination_node_id == binding.destination_node_id &&
                existing->destination_port_index ==
                    binding.destination_port_index) ||
            (relay_node_port_type_is_item(module_definition->inputs[
                    binding.module_port_index].type) &&
                existing->module_node_id == binding.module_node_id &&
                existing->module_port_index == binding.module_port_index)) {
            return false;
        }
    }
    if (relay_node_port_type_is_item(module_definition->inputs[
            binding.module_port_index].type)) {
        for (index = 0; index < world->module_output_binding_count; index++) {
            const Relay_NodeModuleOutputBinding *existing =
                &world->module_output_bindings[index];

            if (existing->module_node_id == binding.module_node_id &&
                existing->source_is_module_input &&
                existing->source_module_input_port_index ==
                    binding.module_port_index) {
                return false;
            }
        }
    }
    if (!relay_node_world_grow_bindings(
            (void **)&world->module_input_bindings,
            &world->module_input_binding_capacity,
            world->module_input_binding_count,
            sizeof(*world->module_input_bindings))) {
        return false;
    }
    world->module_input_bindings[world->module_input_binding_count++] = binding;
    return true;
}

bool relay_node_world_bind_module_output(Relay_NodeWorld *world,
    Relay_NodeModuleOutputBinding binding)
{
    const Relay_Node *module;
    const Relay_NodeDefinition *module_definition;
    Relay_NodePortType source_type;
    size_t index;

    if (world == NULL) {
        return false;
    }
    module = relay_node_world_find_const(world, binding.module_node_id);
    module_definition = relay_node_definition_for(module);
    if (module == NULL ||
        module->runtime_kind != RELAY_NODE_RUNTIME_BLUEPRINT_WRAPPER ||
        module_definition == NULL ||
        binding.module_port_index >= module_definition->output_count) {
        return false;
    }
    if (binding.source_is_module_input) {
        if (binding.source_module_input_port_index >=
                module_definition->input_count) {
            return false;
        }
        source_type = module_definition->inputs[
            binding.source_module_input_port_index].type;
    } else {
        const Relay_Node *source = relay_node_world_find_const(world,
            binding.source_node_id);
        const Relay_NodeDefinition *source_definition =
            relay_node_definition_for(source);

        if (source_definition == NULL ||
            source->module_instance_id != module->id ||
            binding.source_port_index >= source_definition->output_count) {
            return false;
        }
        source_type = source_definition->outputs[
            binding.source_port_index].type;
    }
    if (!relay_node_port_types_compatible(source_type,
            module_definition->outputs[binding.module_port_index].type)) {
        return false;
    }
    for (index = 0; index < world->module_output_binding_count; index++) {
        const Relay_NodeModuleOutputBinding *existing =
            &world->module_output_bindings[index];

        if (existing->module_node_id == binding.module_node_id &&
            existing->module_port_index == binding.module_port_index) {
            return false;
        }
    }
    if (relay_node_port_type_is_item(source_type)) {
        if (binding.source_is_module_input) {
            for (index = 0; index < world->module_input_binding_count;
                    index++) {
                const Relay_NodeModuleInputBinding *existing =
                    &world->module_input_bindings[index];

                if (existing->module_node_id == binding.module_node_id &&
                    existing->module_port_index ==
                        binding.source_module_input_port_index) {
                    return false;
                }
            }
            for (index = 0; index < world->module_output_binding_count;
                    index++) {
                const Relay_NodeModuleOutputBinding *existing =
                    &world->module_output_bindings[index];

                if (existing->module_node_id == binding.module_node_id &&
                    existing->source_is_module_input &&
                    existing->source_module_input_port_index ==
                        binding.source_module_input_port_index) {
                    return false;
                }
            }
        } else {
            for (index = 0; index < world->connection_count; index++) {
                const Relay_NodeConnection *connection =
                    &world->connections[index];

                if (connection->source_node_id == binding.source_node_id &&
                    connection->source_port_index ==
                        binding.source_port_index) {
                    return false;
                }
            }
            for (index = 0; index < world->module_output_binding_count;
                    index++) {
                const Relay_NodeModuleOutputBinding *existing =
                    &world->module_output_bindings[index];

                if (!existing->source_is_module_input &&
                    existing->source_node_id == binding.source_node_id &&
                    existing->source_port_index ==
                        binding.source_port_index) {
                    return false;
                }
            }
        }
    }
    if (!relay_node_world_grow_bindings(
            (void **)&world->module_output_bindings,
            &world->module_output_binding_capacity,
            world->module_output_binding_count,
            sizeof(*world->module_output_bindings))) {
        return false;
    }
    world->module_output_bindings[world->module_output_binding_count++] =
        binding;
    return true;
}

void relay_node_world_shutdown(Relay_NodeWorld *world)
{
    if (world == NULL) {
        return;
    }
    free(world->nodes);
    free(world->connections);
    free(world->module_input_bindings);
    free(world->module_output_bindings);
    *world = (Relay_NodeWorld){0};
}

const Relay_NodeDefinition *relay_node_definition_find(Relay_NodeDefinitionId id)
{
    size_t index;

    for (index = 0; index < relay_node_definition_count(); index++) {
        if (relay_node_definitions[index].id == id) {
            return &relay_node_definitions[index];
        }
    }
    return NULL;
}

const Relay_NodeDefinition *relay_node_definition_find_key(const char *key)
{
    size_t index;

    if (key == NULL) {
        return NULL;
    }
    for (index = 0; index < relay_node_definition_count(); index++) {
        if (strcmp(relay_node_definitions[index].key, key) == 0) {
            return &relay_node_definitions[index];
        }
    }
    return NULL;
}

size_t relay_node_definition_count(void)
{
    return sizeof(relay_node_definitions) / sizeof(relay_node_definitions[0]);
}

const Relay_NodeDefinition *relay_node_definition_at(size_t index)
{
    return index < relay_node_definition_count() ? &relay_node_definitions[index] :
        NULL;
}

const Relay_NodePropertyDefinition *relay_node_universal_properties(
    size_t *count)
{
    if (count != NULL) {
        *count = sizeof(relay_universal_properties) /
            sizeof(relay_universal_properties[0]);
    }
    return relay_universal_properties;
}

size_t relay_timer_interval_count(void)
{
    return sizeof(relay_timer_intervals) / sizeof(relay_timer_intervals[0]);
}

int64_t relay_timer_interval_at(size_t index)
{
    return index < relay_timer_interval_count() ?
        relay_timer_intervals[index] : 0;
}

bool relay_node_property_get(const Relay_Node *node, const char *key,
    Relay_NodeValue *value, Relay_NodeValueType *value_type)
{
    const Relay_NodeDefinition *definition;
    const Relay_NodePropertyDefinition *property;

    if (node == NULL || key == NULL || value == NULL || value_type == NULL) {
        return false;
    }
    definition = relay_node_definition_for(node);
    property = relay_node_property_definition_find(definition, key);
    if (property == NULL) {
        return false;
    }
    *value_type = property->value_type;
    *value = property->default_value;
    if (strcmp(key, "node.enabled") == 0) {
        value->boolean = node->enabled;
    } else if (strcmp(key, "timer.interval_steps") == 0) {
        value->integer = node->timer_interval_steps;
    } else if (strcmp(key, "timer.triggers") == 0) {
        value->integer = node->produced;
    }
    return true;
}

bool relay_node_property_set(Relay_Node *node, const char *key,
    Relay_NodeValueType value_type, Relay_NodeValue value)
{
    const Relay_NodeDefinition *definition;
    const Relay_NodePropertyDefinition *property;
    size_t index;

    if (node == NULL || key == NULL) {
        return false;
    }
    definition = relay_node_definition_for(node);
    property = relay_node_property_definition_find(definition, key);
    if (property == NULL || !property->writable ||
        property->value_type != value_type) {
        return false;
    }
    if (strcmp(key, "node.enabled") == 0) {
        node->enabled = value.boolean;
        return true;
    }
    if (strcmp(key, "timer.interval_steps") == 0) {
        for (index = 0; index < relay_timer_interval_count(); index++) {
            if (value.integer == relay_timer_interval_at(index)) {
                node->timer_interval_steps = value.integer;
                node->timer_elapsed_steps = 0;
                return true;
            }
        }
    }
    return false;
}
