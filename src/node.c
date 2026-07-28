#include "relay/node.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/** Script schema for the player-controlled infinite clock. */
static const Relay_NodePropertyDefinition relay_clock_properties[] = {
    {"node.enabled", RELAY_NODE_VALUE_BOOLEAN, true, {.boolean = true}},
    {"clock.period", RELAY_NODE_VALUE_INTEGER, true, {.integer = 2}},
    {"clock.pulses", RELAY_NODE_VALUE_INTEGER, false, {.integer = 0}}
};

/** Script schema for a coal miner driven by a clock signal. */
static const Relay_NodePropertyDefinition relay_coal_miner_properties[] = {
    {"node.enabled", RELAY_NODE_VALUE_BOOLEAN, true, {.boolean = true}},
    {"process.required_clock", RELAY_NODE_VALUE_INTEGER, false, {.integer = 16}},
    {"process.progress", RELAY_NODE_VALUE_INTEGER, false, {.integer = 0}},
    {"fuel.coal", RELAY_NODE_VALUE_INTEGER, false, {.integer = 0}},
    {"resource.coal", RELAY_NODE_VALUE_INTEGER, false, {.integer = 0}}
};

/** Ports for the player-controlled infinite clock resource. */
static const Relay_NodePortDefinition relay_clock_outputs[] = {
    {"clock", "Clock", RELAY_NODE_PORT_TYPE_CLOCK}
};

/** Ports for a coal miner that consumes clock pulses. */
static const Relay_NodePortDefinition relay_coal_miner_inputs[] = {
    {"clock", "Clock", RELAY_NODE_PORT_TYPE_CLOCK},
    {"fuel", "Fuel", RELAY_NODE_PORT_TYPE_COAL}
};

static const Relay_NodePortDefinition relay_coal_miner_outputs[] = {
    {"coal", "Coal", RELAY_NODE_PORT_TYPE_COAL}
};

/** Immutable built-in module catalog with script-stable identifiers and keys. */
static const Relay_NodeDefinition relay_node_definitions[] = {
    {RELAY_NODE_DEFINITION_CLOCK, "source.clock", "Clock", "◷",
        "An infinite, configurable clock source.", RELAY_NODE_CATEGORY_SOURCE,
        NULL, 0, relay_clock_outputs, sizeof(relay_clock_outputs) /
            sizeof(relay_clock_outputs[0]), relay_clock_properties,
        sizeof(relay_clock_properties) / sizeof(relay_clock_properties[0])},
    {RELAY_NODE_DEFINITION_COAL_MINER, "process.coal_miner", "Coal Miner", "◆",
        "Consumes sixteen clock pulses to produce one coal.",
        RELAY_NODE_CATEGORY_PROCESSOR, relay_coal_miner_inputs,
        sizeof(relay_coal_miner_inputs) / sizeof(relay_coal_miner_inputs[0]),
        relay_coal_miner_outputs, sizeof(relay_coal_miner_outputs) /
            sizeof(relay_coal_miner_outputs[0]), relay_coal_miner_properties,
        sizeof(relay_coal_miner_properties) /
            sizeof(relay_coal_miner_properties[0])}
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

bool relay_node_world_init(Relay_NodeWorld *world)
{
    if (world == NULL) {
        return false;
    }
    *world = (Relay_NodeWorld){0};
    world->next_id = 1;
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

    if (world == NULL || definition == NULL ||
        definition->input_count > RELAY_NODE_MAX_PORTS ||
        definition->output_count > RELAY_NODE_MAX_PORTS ||
        !relay_node_world_grow(world)) {
        return 0;
    }
    id = world->next_id++;
    if (id == 0) {
        id = world->next_id++;
        if (id == 0) {
            return 0;
        }
    }
    node = &world->nodes[world->count++];
    *node = (Relay_Node){0};
    node->id = id;
    node->definition_id = definition->id;
    node->definition = definition;
    (void)snprintf(node->local_key, sizeof(node->local_key), "n%llu",
        (unsigned long long)id);
    node->grid_x = grid_x;
    node->grid_y = grid_y;
    node->clock_period = 2;
    node->enabled = true;
    node->fuel_coal = definition->id == RELAY_NODE_DEFINITION_COAL_MINER ? 1 : 0;
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
    case RELAY_NODE_PORT_TYPE_CLOCK: return "Clock";
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
        source_port_index >= source_definition->output_count ||
        destination_port_index >= destination_definition->input_count ||
        !relay_node_port_types_compatible(
            source_definition->outputs[source_port_index].type,
            destination_definition->inputs[destination_port_index].type)) {
        return false;
    }
    for (index = 0; index < world->connection_count; index++) {
        Relay_NodeConnection *connection = &world->connections[index];

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
        module_definition == NULL || destination_definition == NULL ||
        binding.module_port_index >= module_definition->input_count ||
        binding.destination_port_index >= destination_definition->input_count ||
        !relay_node_port_types_compatible(
            module_definition->inputs[binding.module_port_index].type,
            destination_definition->inputs[
                binding.destination_port_index].type) ||
        !relay_node_world_grow_bindings(
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
            binding.source_port_index >= source_definition->output_count) {
            return false;
        }
        source_type = source_definition->outputs[
            binding.source_port_index].type;
    }
    if (!relay_node_port_types_compatible(source_type,
            module_definition->outputs[binding.module_port_index].type) ||
        !relay_node_world_grow_bindings(
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
    } else if (strcmp(key, "clock.period") == 0) {
        value->integer = node->clock_period;
    } else if (strcmp(key, "clock.pulses") == 0 ||
        strcmp(key, "resource.coal") == 0) {
        value->integer = node->produced;
    } else if (strcmp(key, "process.progress") == 0) {
        value->integer = node->progress;
    } else if (strcmp(key, "fuel.coal") == 0) {
        value->integer = node->fuel_coal;
    }
    return true;
}

bool relay_node_property_set(Relay_Node *node, const char *key,
    Relay_NodeValueType value_type, Relay_NodeValue value)
{
    const Relay_NodeDefinition *definition;
    const Relay_NodePropertyDefinition *property;

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
    if (strcmp(key, "clock.period") == 0 && (value.integer == 2 ||
            value.integer == 4 || value.integer == 8 || value.integer == 16 ||
            value.integer == 32 || value.integer == 64 || value.integer == 128)) {
        node->clock_period = value.integer;
        node->clock_phase = 0;
        return true;
    }
    return false;
}
