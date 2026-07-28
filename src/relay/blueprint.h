#ifndef RELAY_BLUEPRINT_H
#define RELAY_BLUEPRINT_H

#include "relay/node.h"
#include "relay/script_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    RELAY_BLUEPRINT_CAPACITY = 12,
    RELAY_BLUEPRINT_NAME_CAPACITY = 32,
    RELAY_BLUEPRINT_KEY_CAPACITY = 64,
    RELAY_BLUEPRINT_SOURCE_CAPACITY = 8192,
    RELAY_BLUEPRINT_DEFINITION_ID_BASE = 1000,
    RELAY_BLUEPRINT_INPUT_DEFINITION_ID_BASE = 2000,
    RELAY_BLUEPRINT_PROCESS_DEFINITION_ID_BASE = 3000,
    RELAY_BLUEPRINT_OUTPUT_DEFINITION_ID_BASE = 4000
};

/** Stable identifier for one player-authored reusable blueprint. */
typedef uint64_t Relay_BlueprintId;

/** Modal editing states for Relay's terminal-native source editor. */
typedef enum Relay_BlueprintEditorMode {
    RELAY_BLUEPRINT_EDITOR_NORMAL,
    RELAY_BLUEPRINT_EDITOR_INSERT,
    RELAY_BLUEPRINT_EDITOR_COMMAND
} Relay_BlueprintEditorMode;

/** One flattened atomic or Lua-core node in an immutable module plan. */
typedef struct Relay_BlueprintPlanNode {
    const Relay_NodeDefinition *definition;
    Relay_BlueprintId script_blueprint_id;
    Relay_BlueprintId origin_blueprint_id;
    Relay_NodeId origin_node_id;
    Relay_NodeRuntimeKind runtime_kind;
} Relay_BlueprintPlanNode;

/** One ordinary typed edge between flattened plan nodes. */
typedef struct Relay_BlueprintPlanConnection {
    size_t source_node_index;
    size_t source_port_index;
    size_t destination_node_index;
    size_t destination_port_index;
} Relay_BlueprintPlanConnection;

/** One public input fan-out destination in a flattened module plan. */
typedef struct Relay_BlueprintPlanInputBinding {
    size_t module_port_index;
    size_t destination_node_index;
    size_t destination_port_index;
} Relay_BlueprintPlanInputBinding;

/** One public output source in a flattened module plan. */
typedef struct Relay_BlueprintPlanOutputBinding {
    size_t module_port_index;
    size_t source_node_index;
    size_t source_port_index;
    size_t source_module_input_port_index;
    bool source_is_module_input;
} Relay_BlueprintPlanOutputBinding;

/** Transactionally compiled flattened implementation of one Blueprint. */
typedef struct Relay_BlueprintPlan {
    Relay_BlueprintPlanNode *nodes;
    size_t node_count;
    size_t node_capacity;
    Relay_BlueprintPlanConnection *connections;
    size_t connection_count;
    size_t connection_capacity;
    Relay_BlueprintPlanInputBinding *input_bindings;
    size_t input_binding_count;
    size_t input_binding_capacity;
    Relay_BlueprintPlanOutputBinding *output_bindings;
    size_t output_binding_count;
    size_t output_binding_capacity;
    uint64_t revision;
    bool valid;
} Relay_BlueprintPlan;

/** One code-defined module plus its visual composition workspace. */
typedef struct Relay_Blueprint {
    Relay_BlueprintId id;
    char name[RELAY_BLUEPRINT_NAME_CAPACITY];
    char key[RELAY_BLUEPRINT_KEY_CAPACITY];
    char source[RELAY_BLUEPRINT_SOURCE_CAPACITY];
    size_t source_size;
    size_t cursor;
    size_t viewport_line;
    uint64_t revision;
    uint64_t compiled_revision;
    Relay_ScriptArtifact artifact;
    Relay_ScriptSchema schema;
    Relay_NodeDefinition definition;
    Relay_NodeDefinition input_boundary_definition;
    Relay_NodeDefinition process_definition;
    Relay_NodeDefinition output_boundary_definition;
    Relay_NodePortDefinition inputs[RELAY_NODE_MAX_PORTS];
    Relay_NodePortDefinition outputs[RELAY_NODE_MAX_PORTS];
    Relay_NodePortDefinition input_boundary_outputs[RELAY_NODE_MAX_PORTS];
    Relay_NodePortDefinition process_inputs[RELAY_NODE_MAX_PORTS];
    Relay_NodePortDefinition process_outputs[RELAY_NODE_MAX_PORTS];
    Relay_NodePortDefinition output_boundary_inputs[RELAY_NODE_MAX_PORTS];
    Relay_NodeWorld scene;
    Relay_BlueprintPlan plan;
    Relay_NodeId input_boundary_node_id;
    Relay_NodeId output_boundary_node_id;
    Relay_NodeId focused_node_id;
    size_t instance_count;
    Relay_ScriptDiagnostic diagnostic;
    char editor_command[32];
    size_t editor_command_size;
    Relay_BlueprintEditorMode editor_mode;
    bool editor_open;
    bool workspace_open;
    bool dirty;
} Relay_Blueprint;

/** Game-owned registry of stable player-authored blueprint definitions. */
typedef struct Relay_BlueprintLibrary {
    Relay_ScriptRuntime *runtime;
    Relay_Blueprint blueprints[RELAY_BLUEPRINT_CAPACITY];
    size_t count;
    Relay_BlueprintId next_id;
} Relay_BlueprintLibrary;

/** Initialize an empty blueprint registry against an owned script runtime. */
bool relay_blueprint_library_init(Relay_BlueprintLibrary *library,
    Relay_ScriptRuntime *runtime);

/** Create and compile a reusable pass-through script blueprint. */
Relay_BlueprintId relay_blueprint_library_create(
    Relay_BlueprintLibrary *library);

/** Find a mutable blueprint by stable identifier. */
Relay_Blueprint *relay_blueprint_library_find(Relay_BlueprintLibrary *library,
    Relay_BlueprintId id);

/** Find an immutable blueprint by stable identifier. */
const Relay_Blueprint *relay_blueprint_library_find_const(
    const Relay_BlueprintLibrary *library, Relay_BlueprintId id);

/** Return a blueprint by stable registry order. */
Relay_Blueprint *relay_blueprint_library_at(Relay_BlueprintLibrary *library,
    size_t index);

/** Compile edited source transactionally and preserve the prior artifact on failure. */
bool relay_blueprint_compile(Relay_BlueprintLibrary *library,
    Relay_Blueprint *blueprint);

/** Recompile only the visual architecture against installed source artifacts. */
bool relay_blueprint_rebuild_plan(Relay_BlueprintLibrary *library,
    Relay_Blueprint *blueprint);

/** Expand one compiled Blueprint transactionally into a normal node world. */
bool relay_blueprint_instantiate(Relay_BlueprintLibrary *library,
    Relay_Blueprint *blueprint, Relay_NodeWorld *world, int64_t grid_x,
    int64_t grid_y, Relay_NodeId *module_node_id);

/** Release all blueprint artifacts, instance state, and visual scenes. */
void relay_blueprint_library_shutdown(Relay_BlueprintLibrary *library);

#endif
