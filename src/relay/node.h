#ifndef RELAY_NODE_H
#define RELAY_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Stable identifier of a node definition. */
typedef uint32_t Relay_NodeDefinitionId;

/** Stable identifiers for Relay's built-in source definitions. */
typedef enum Relay_BuiltinNodeDefinitionId {
    RELAY_NODE_DEFINITION_CLOCK = 1,
    RELAY_NODE_DEFINITION_COAL_MINER
} Relay_BuiltinNodeDefinitionId;

/** Stable identifier of a node instance. */
typedef uint64_t Relay_NodeId;

/** Categories used to group compatible automation nodes. */
typedef enum Relay_NodeCategory {
    RELAY_NODE_CATEGORY_SOURCE,
    RELAY_NODE_CATEGORY_PROCESSOR,
    RELAY_NODE_CATEGORY_MODULE
} Relay_NodeCategory;

/** Types exposed by node properties to future scripting and save systems. */
typedef enum Relay_NodeValueType {
    RELAY_NODE_VALUE_INTEGER,
    RELAY_NODE_VALUE_BOOLEAN,
    RELAY_NODE_VALUE_TEXT
} Relay_NodeValueType;

/** Value exchanged through a script-visible node property. */
typedef union Relay_NodeValue {
    int64_t integer;
    bool boolean;
    const char *text;
} Relay_NodeValue;

/** Immutable input or output port declared by a node definition. */
typedef struct Relay_NodePortDefinition {
    const char *key;
    const char *display_name;
    Relay_NodeValueType value_type;
} Relay_NodePortDefinition;

/** Schema entry for a node property. */
typedef struct Relay_NodePropertyDefinition {
    const char *key;
    Relay_NodeValueType value_type;
    bool writable;
    Relay_NodeValue default_value;
} Relay_NodePropertyDefinition;

/** Immutable, data-driven definition shared by all matching nodes. */
typedef struct Relay_NodeDefinition {
    Relay_NodeDefinitionId id;
    const char *key;
    const char *display_name;
    const char *glyph;
    const char *description;
    Relay_NodeCategory category;
    const Relay_NodePortDefinition *inputs;
    size_t input_count;
    const Relay_NodePortDefinition *outputs;
    size_t output_count;
    const Relay_NodePropertyDefinition *properties;
    size_t property_count;
} Relay_NodeDefinition;

/** Mutable node instance placed in the game's grid world. */
typedef struct Relay_Node {
    Relay_NodeId id;
    Relay_NodeDefinitionId definition_id;
    int64_t grid_x;
    int64_t grid_y;
    int64_t remaining_resource;
    int64_t progress;
    int64_t produced;
    int64_t clock_period;
    int64_t clock_phase;
    int64_t output_value;
    int64_t previous_output_value;
    int64_t fuel_coal;
    bool enabled;
    bool processing;
} Relay_Node;

/** A directed, typed wire between an output and an input port. */
typedef struct Relay_NodeConnection {
    Relay_NodeId source_node_id;
    size_t source_port_index;
    Relay_NodeId destination_node_id;
    size_t destination_port_index;
} Relay_NodeConnection;

/** Dynamic world-owned storage for node instances. */
typedef struct Relay_NodeWorld {
    Relay_Node *nodes;
    size_t count;
    size_t capacity;
    Relay_NodeId next_id;
    Relay_NodeConnection *connections;
    size_t connection_count;
    size_t connection_capacity;
} Relay_NodeWorld;

/** Initialize an empty node world. */
bool relay_node_world_init(Relay_NodeWorld *world);

/** Create a node from a registered definition at a grid position. */
Relay_NodeId relay_node_world_create(Relay_NodeWorld *world,
    Relay_NodeDefinitionId definition_id, int64_t grid_x, int64_t grid_y);

/** Find a mutable node instance by stable identifier. */
Relay_Node *relay_node_world_find(Relay_NodeWorld *world, Relay_NodeId id);

/** Find an immutable node instance by stable identifier. */
const Relay_Node *relay_node_world_find_const(const Relay_NodeWorld *world,
    Relay_NodeId id);

/** Move a node to any signed 64-bit grid coordinate. */
bool relay_node_world_move(Relay_NodeWorld *world, Relay_NodeId id,
    int64_t grid_x, int64_t grid_y);

/** Create or replace one valid typed input connection in a module graph. */
bool relay_node_world_connect(Relay_NodeWorld *world, Relay_NodeId source_node_id,
    size_t source_port_index, Relay_NodeId destination_node_id,
    size_t destination_port_index);

/** Return the wire feeding one input port, or NULL when it is unconnected. */
const Relay_NodeConnection *relay_node_world_connection_to(
    const Relay_NodeWorld *world, Relay_NodeId destination_node_id,
    size_t destination_port_index);

/** Release all node instances in a world. */
void relay_node_world_shutdown(Relay_NodeWorld *world);

/** Return the registered definition matching a stable identifier. */
const Relay_NodeDefinition *relay_node_definition_find(
    Relay_NodeDefinitionId id);

/** Return the registered definition matching a script-stable key. */
const Relay_NodeDefinition *relay_node_definition_find_key(const char *key);

/** Return the number of registered node definitions. */
size_t relay_node_definition_count(void);

/** Return a definition by registry index, or NULL when out of range. */
const Relay_NodeDefinition *relay_node_definition_at(size_t index);

/** Read a script-visible property from a node instance. */
bool relay_node_property_get(const Relay_Node *node, const char *key,
    Relay_NodeValue *value, Relay_NodeValueType *value_type);

/** Update a writable script-visible property on a node instance. */
bool relay_node_property_set(Relay_Node *node, const char *key,
    Relay_NodeValueType value_type, Relay_NodeValue value);

#endif
