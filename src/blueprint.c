#include "relay/blueprint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char relay_blueprint_default_source[] =
    "-- Declare the typed module interface.\n"
    "input(\"clock\", \"clock\")\n"
    "output(\"clock_out\", \"clock\")\n"
    "\n"
    "-- The Script Core is wired between the Blueprint boundaries by default.\n"
    "function tick(inputs, state)\n"
    "  state.pulses = (state.pulses or 0) + (inputs.clock or 0)\n"
    "  return { clock_out = inputs.clock or 0 }\n"
    "end\n";

/** Design-node mapping into one candidate flattened plan. */
typedef struct Relay_BlueprintFlattenMap {
    Relay_NodeId design_node_id;
    size_t plan_node_index;
    size_t child_plan_base;
    const Relay_Blueprint *child;
    Relay_NodeRuntimeKind runtime_kind;
} Relay_BlueprintFlattenMap;

/** Symbolic flattened source used while resolving hierarchical port maps. */
typedef struct Relay_BlueprintFlattenSource {
    size_t node_index;
    size_t port_index;
    size_t module_input_port_index;
    bool is_module_input;
    bool valid;
} Relay_BlueprintFlattenSource;

/** Release all heap storage owned by one immutable compiled plan. */
static void relay_blueprint_plan_shutdown(Relay_BlueprintPlan *plan)
{
    if (plan == NULL) {
        return;
    }
    free(plan->nodes);
    free(plan->connections);
    free(plan->input_bindings);
    free(plan->output_bindings);
    *plan = (Relay_BlueprintPlan){0};
}

/** Grow a plan-owned array without imposing a gameplay graph-size limit. */
static bool relay_blueprint_plan_reserve(void **storage, size_t *capacity,
    size_t count, size_t element_size)
{
    size_t next_capacity;
    void *allocation;

    if (count < *capacity) {
        return true;
    }
    if (*capacity > SIZE_MAX / 2 ||
        (*capacity != 0 && *capacity * 2 > SIZE_MAX / element_size)) {
        return false;
    }
    next_capacity = *capacity == 0 ? 16 : *capacity * 2;
    allocation = realloc(*storage, next_capacity * element_size);
    if (allocation == NULL) {
        return false;
    }
    *storage = allocation;
    *capacity = next_capacity;
    return true;
}

/** Append one node to a candidate flattened plan. */
static bool relay_blueprint_plan_add_node(Relay_BlueprintPlan *plan,
    Relay_BlueprintPlanNode node, size_t *index)
{
    if (!relay_blueprint_plan_reserve((void **)&plan->nodes,
            &plan->node_capacity, plan->node_count, sizeof(*plan->nodes))) {
        return false;
    }
    if (index != NULL) {
        *index = plan->node_count;
    }
    plan->nodes[plan->node_count++] = node;
    return true;
}

/** Append one normal typed connection to a candidate flattened plan. */
static bool relay_blueprint_plan_add_connection(Relay_BlueprintPlan *plan,
    Relay_BlueprintPlanConnection connection)
{
    if (!relay_blueprint_plan_reserve((void **)&plan->connections,
            &plan->connection_capacity, plan->connection_count,
            sizeof(*plan->connections))) {
        return false;
    }
    plan->connections[plan->connection_count++] = connection;
    return true;
}

/** Append one public input fan-out route to a candidate plan. */
static bool relay_blueprint_plan_add_input_binding(Relay_BlueprintPlan *plan,
    Relay_BlueprintPlanInputBinding binding)
{
    if (!relay_blueprint_plan_reserve((void **)&plan->input_bindings,
            &plan->input_binding_capacity, plan->input_binding_count,
            sizeof(*plan->input_bindings))) {
        return false;
    }
    plan->input_bindings[plan->input_binding_count++] = binding;
    return true;
}

/** Append one public output source to a candidate plan. */
static bool relay_blueprint_plan_add_output_binding(Relay_BlueprintPlan *plan,
    Relay_BlueprintPlanOutputBinding binding)
{
    size_t index;

    for (index = 0; index < plan->output_binding_count; index++) {
        if (plan->output_bindings[index].module_port_index ==
                binding.module_port_index) {
            return false;
        }
    }
    if (!relay_blueprint_plan_reserve((void **)&plan->output_bindings,
            &plan->output_binding_capacity, plan->output_binding_count,
            sizeof(*plan->output_bindings))) {
        return false;
    }
    plan->output_bindings[plan->output_binding_count++] = binding;
    return true;
}

/** Return whether two compiled public interfaces are exactly compatible. */
static bool relay_blueprint_schema_equal(const Relay_ScriptSchema *left,
    const Relay_ScriptSchema *right)
{
    size_t index;

    if (left->input_count != right->input_count ||
        left->output_count != right->output_count) {
        return false;
    }
    for (index = 0; index < left->input_count; index++) {
        if (left->inputs[index].type != right->inputs[index].type ||
            strcmp(left->inputs[index].key, right->inputs[index].key) != 0) {
            return false;
        }
    }
    for (index = 0; index < left->output_count; index++) {
        if (left->outputs[index].type != right->outputs[index].type ||
            strcmp(left->outputs[index].key, right->outputs[index].key) != 0) {
            return false;
        }
    }
    return true;
}

/** Rebuild all stable definition views from the installed public schema. */
static void relay_blueprint_definition_refresh(Relay_Blueprint *blueprint)
{
    size_t index;

    for (index = 0; index < blueprint->schema.input_count; index++) {
        const Relay_NodePortDefinition port = {
            blueprint->schema.inputs[index].key,
            blueprint->schema.inputs[index].key,
            blueprint->schema.inputs[index].type
        };

        blueprint->inputs[index] = port;
        blueprint->input_boundary_outputs[index] = port;
        blueprint->script_core_inputs[index] = port;
    }
    for (index = 0; index < blueprint->schema.output_count; index++) {
        const Relay_NodePortDefinition port = {
            blueprint->schema.outputs[index].key,
            blueprint->schema.outputs[index].key,
            blueprint->schema.outputs[index].type
        };

        blueprint->outputs[index] = port;
        blueprint->script_core_outputs[index] = port;
        blueprint->output_boundary_inputs[index] = port;
    }
    blueprint->definition = (Relay_NodeDefinition){
        (Relay_NodeDefinitionId)(RELAY_BLUEPRINT_DEFINITION_ID_BASE +
            blueprint->id),
        blueprint->key, blueprint->name, "λ",
        "Compiled hierarchical Blueprint module.", RELAY_NODE_CATEGORY_MODULE,
        blueprint->inputs, blueprint->schema.input_count, blueprint->outputs,
        blueprint->schema.output_count, NULL, 0
    };
    blueprint->input_boundary_definition = (Relay_NodeDefinition){
        (Relay_NodeDefinitionId)(RELAY_BLUEPRINT_INPUT_DEFINITION_ID_BASE +
            blueprint->id),
        "blueprint.boundary.inputs", "Module Inputs", "⇥",
        "Public inputs entering this Blueprint architecture.",
        RELAY_NODE_CATEGORY_MODULE, NULL, 0,
        blueprint->input_boundary_outputs, blueprint->schema.input_count,
        NULL, 0
    };
    blueprint->script_core_definition = (Relay_NodeDefinition){
        (Relay_NodeDefinitionId)(RELAY_BLUEPRINT_CORE_DEFINITION_ID_BASE +
            blueprint->id),
        "blueprint.script_core", "Lua Core", "λ",
        "The Blueprint's player-authored Lua process.",
        RELAY_NODE_CATEGORY_MODULE, blueprint->script_core_inputs,
        blueprint->schema.input_count, blueprint->script_core_outputs,
        blueprint->schema.output_count, NULL, 0
    };
    blueprint->output_boundary_definition = (Relay_NodeDefinition){
        (Relay_NodeDefinitionId)(RELAY_BLUEPRINT_OUTPUT_DEFINITION_ID_BASE +
            blueprint->id),
        "blueprint.boundary.outputs", "Module Outputs", "⇤",
        "Public outputs leaving this Blueprint architecture.",
        RELAY_NODE_CATEGORY_MODULE, blueprint->output_boundary_inputs,
        blueprint->schema.output_count, NULL, 0, NULL, 0
    };
}

/** Create boundary and Lua-core nodes with a complete default port map. */
static bool relay_blueprint_scene_create_system_nodes(
    Relay_Blueprint *blueprint, Relay_NodeWorld *scene)
{
    Relay_Node *node;
    size_t index;

    blueprint->input_boundary_node_id = relay_node_world_create_definition(scene,
        &blueprint->input_boundary_definition, -30, 0);
    blueprint->script_core_node_id = relay_node_world_create_definition(scene,
        &blueprint->script_core_definition, 0, 0);
    blueprint->output_boundary_node_id = relay_node_world_create_definition(scene,
        &blueprint->output_boundary_definition, 30, 0);
    if (blueprint->input_boundary_node_id == 0 ||
        blueprint->script_core_node_id == 0 ||
        blueprint->output_boundary_node_id == 0) {
        return false;
    }
    node = relay_node_world_find(scene, blueprint->input_boundary_node_id);
    node->runtime_kind = RELAY_NODE_RUNTIME_BLUEPRINT_INPUT_BOUNDARY;
    node->blueprint_id = blueprint->id;
    node = relay_node_world_find(scene, blueprint->script_core_node_id);
    node->runtime_kind = RELAY_NODE_RUNTIME_BLUEPRINT_SCRIPT_CORE;
    node->blueprint_id = blueprint->id;
    node = relay_node_world_find(scene, blueprint->output_boundary_node_id);
    node->runtime_kind = RELAY_NODE_RUNTIME_BLUEPRINT_OUTPUT_BOUNDARY;
    node->blueprint_id = blueprint->id;
    for (index = 0; index < blueprint->schema.input_count; index++) {
        if (!relay_node_world_connect(scene, blueprint->input_boundary_node_id,
                index, blueprint->script_core_node_id, index)) {
            return false;
        }
    }
    for (index = 0; index < blueprint->schema.output_count; index++) {
        if (!relay_node_world_connect(scene, blueprint->script_core_node_id,
                index, blueprint->output_boundary_node_id, index)) {
            return false;
        }
    }
    return true;
}

/** Return a flatten mapping by its design-scene node identifier. */
static const Relay_BlueprintFlattenMap *relay_blueprint_flatten_map_find(
    const Relay_BlueprintFlattenMap *maps, size_t map_count,
    Relay_NodeId design_node_id)
{
    size_t index;

    for (index = 0; index < map_count; index++) {
        if (maps[index].design_node_id == design_node_id) {
            return &maps[index];
        }
    }
    return NULL;
}

/** Resolve one design output through nested Blueprint output port maps. */
static Relay_BlueprintFlattenSource relay_blueprint_flatten_source(
    const Relay_Blueprint *blueprint, const Relay_BlueprintFlattenMap *maps,
    size_t map_count, Relay_NodeId node_id, size_t port_index, size_t depth)
{
    const Relay_BlueprintFlattenMap *map;

    if (depth > map_count + 1) {
        return (Relay_BlueprintFlattenSource){0};
    }
    if (node_id == blueprint->input_boundary_node_id &&
        port_index < blueprint->schema.input_count) {
        return (Relay_BlueprintFlattenSource){
            0, 0, port_index, true, true
        };
    }
    map = relay_blueprint_flatten_map_find(maps, map_count, node_id);
    if (map == NULL) {
        return (Relay_BlueprintFlattenSource){0};
    }
    if (map->runtime_kind != RELAY_NODE_RUNTIME_BLUEPRINT_WRAPPER) {
        return (Relay_BlueprintFlattenSource){
            map->plan_node_index, port_index, 0, false, true
        };
    }
    if (map->child != NULL) {
        size_t index;

        for (index = 0; index < map->child->plan.output_binding_count; index++) {
            const Relay_BlueprintPlanOutputBinding *binding =
                &map->child->plan.output_bindings[index];

            if (binding->module_port_index != port_index) {
                continue;
            }
            if (!binding->source_is_module_input) {
                return (Relay_BlueprintFlattenSource){
                    map->child_plan_base + binding->source_node_index,
                    binding->source_port_index, 0, false, true
                };
            }
            {
                const Relay_NodeConnection *connection =
                    relay_node_world_connection_to(&blueprint->scene, node_id,
                        binding->source_module_input_port_index);

                if (connection == NULL) {
                    return (Relay_BlueprintFlattenSource){0};
                }
                return relay_blueprint_flatten_source(blueprint, maps,
                    map_count, connection->source_node_id,
                    connection->source_port_index, depth + 1);
            }
        }
    }
    return (Relay_BlueprintFlattenSource){0};
}

/** Connect one symbolic source to one flattened node input. */
static bool relay_blueprint_flatten_connect(Relay_BlueprintPlan *plan,
    Relay_BlueprintFlattenSource source, size_t destination_node_index,
    size_t destination_port_index)
{
    if (!source.valid) {
        return true;
    }
    if (source.is_module_input) {
        return relay_blueprint_plan_add_input_binding(plan,
            (Relay_BlueprintPlanInputBinding){
                source.module_input_port_index, destination_node_index,
                destination_port_index
            });
    }
    return relay_blueprint_plan_add_connection(plan,
        (Relay_BlueprintPlanConnection){
            source.node_index, source.port_index, destination_node_index,
            destination_port_index
        });
}

/** Validate that the definition dependency graph contains no recursion. */
static bool relay_blueprint_validate_dependencies(
    Relay_BlueprintLibrary *library, Relay_Blueprint *blueprint,
    Relay_BlueprintId *stack, size_t depth)
{
    size_t index;
    size_t stack_index;

    for (stack_index = 0; stack_index < depth; stack_index++) {
        if (stack[stack_index] == blueprint->id) {
            (void)snprintf(blueprint->diagnostic.message,
                sizeof(blueprint->diagnostic.message),
                "Blueprint dependency cycle includes %s.", blueprint->name);
            return false;
        }
    }
    stack[depth++] = blueprint->id;
    for (index = 0; index < blueprint->scene.count; index++) {
        const Relay_Node *node = &blueprint->scene.nodes[index];
        Relay_Blueprint *child;

        if (node->runtime_kind != RELAY_NODE_RUNTIME_BLUEPRINT_WRAPPER) {
            continue;
        }
        child = relay_blueprint_library_find(library, node->blueprint_id);
        if (child == NULL || !child->plan.valid ||
            !relay_blueprint_validate_dependencies(library, child, stack,
                depth)) {
            if (blueprint->diagnostic.message[0] == '\0') {
                (void)snprintf(blueprint->diagnostic.message,
                    sizeof(blueprint->diagnostic.message),
                    "Blueprint dependency is unavailable.");
            }
            return false;
        }
    }
    return true;
}

/** Build one immutable flattened plan from a Blueprint architecture scene. */
static bool relay_blueprint_plan_build(Relay_BlueprintLibrary *library,
    Relay_Blueprint *blueprint, Relay_BlueprintPlan *plan)
{
    Relay_BlueprintFlattenMap *maps;
    Relay_BlueprintId dependency_stack[RELAY_BLUEPRINT_CAPACITY + 1] = {0};
    size_t map_count = 0;
    size_t index;
    bool success = false;

    if (!relay_blueprint_validate_dependencies(library, blueprint,
            dependency_stack, 0)) {
        return false;
    }
    maps = calloc(blueprint->scene.count, sizeof(*maps));
    if (maps == NULL && blueprint->scene.count > 0) {
        return false;
    }
    for (index = 0; index < blueprint->scene.count; index++) {
        const Relay_Node *node = &blueprint->scene.nodes[index];
        Relay_BlueprintFlattenMap *map;

        if (node->runtime_kind ==
                RELAY_NODE_RUNTIME_BLUEPRINT_INPUT_BOUNDARY ||
            node->runtime_kind ==
                RELAY_NODE_RUNTIME_BLUEPRINT_OUTPUT_BOUNDARY) {
            continue;
        }
        map = &maps[map_count++];
        map->design_node_id = node->id;
        map->runtime_kind = node->runtime_kind;
        if (node->runtime_kind == RELAY_NODE_RUNTIME_BLUEPRINT_WRAPPER) {
            const Relay_Blueprint *child =
                relay_blueprint_library_find_const(library,
                    node->blueprint_id);
            size_t child_index;

            if (child == NULL || !child->plan.valid) {
                goto cleanup;
            }
            map->child = child;
            map->child_plan_base = plan->node_count;
            for (child_index = 0; child_index < child->plan.node_count;
                    child_index++) {
                if (!relay_blueprint_plan_add_node(plan,
                        child->plan.nodes[child_index], NULL)) {
                    goto cleanup;
                }
            }
            for (child_index = 0;
                    child_index < child->plan.connection_count; child_index++) {
                Relay_BlueprintPlanConnection connection =
                    child->plan.connections[child_index];

                connection.source_node_index += map->child_plan_base;
                connection.destination_node_index += map->child_plan_base;
                if (!relay_blueprint_plan_add_connection(plan, connection)) {
                    goto cleanup;
                }
            }
        } else {
            if (!relay_blueprint_plan_add_node(plan,
                    (Relay_BlueprintPlanNode){
                        node->definition,
                        node->runtime_kind ==
                            RELAY_NODE_RUNTIME_BLUEPRINT_SCRIPT_CORE ?
                                node->blueprint_id : 0,
                        blueprint->id, node->id, node->runtime_kind
                    }, &map->plan_node_index)) {
                goto cleanup;
            }
        }
    }
    for (index = 0; index < blueprint->scene.connection_count; index++) {
        const Relay_NodeConnection *connection =
            &blueprint->scene.connections[index];
        const Relay_BlueprintFlattenMap *destination_map;
        Relay_BlueprintFlattenSource source = relay_blueprint_flatten_source(
            blueprint, maps, map_count, connection->source_node_id,
            connection->source_port_index, 0);

        if (connection->destination_node_id ==
                blueprint->output_boundary_node_id) {
            if (!source.valid ||
                !relay_blueprint_plan_add_output_binding(plan,
                    (Relay_BlueprintPlanOutputBinding){
                        connection->destination_port_index,
                        source.node_index, source.port_index,
                        source.module_input_port_index,
                        source.is_module_input
                    })) {
                goto cleanup;
            }
            continue;
        }
        destination_map = relay_blueprint_flatten_map_find(maps, map_count,
            connection->destination_node_id);
        if (destination_map == NULL) {
            goto cleanup;
        }
        if (destination_map->runtime_kind ==
                RELAY_NODE_RUNTIME_BLUEPRINT_WRAPPER) {
            size_t binding_index;

            for (binding_index = 0;
                    binding_index <
                        destination_map->child->plan.input_binding_count;
                    binding_index++) {
                const Relay_BlueprintPlanInputBinding *binding =
                    &destination_map->child->plan.input_bindings[binding_index];

                if (binding->module_port_index ==
                        connection->destination_port_index &&
                    !relay_blueprint_flatten_connect(plan, source,
                        destination_map->child_plan_base +
                            binding->destination_node_index,
                        binding->destination_port_index)) {
                    goto cleanup;
                }
            }
        } else if (!relay_blueprint_flatten_connect(plan, source,
                destination_map->plan_node_index,
                connection->destination_port_index)) {
            goto cleanup;
        }
    }
    if (plan->output_binding_count != blueprint->schema.output_count) {
        (void)snprintf(blueprint->diagnostic.message,
            sizeof(blueprint->diagnostic.message),
            "Every module output must be wired in the architecture.");
        goto cleanup;
    }
    plan->revision = blueprint->revision;
    plan->valid = true;
    success = true;

cleanup:
    free(maps);
    if (!success) {
        relay_blueprint_plan_shutdown(plan);
    }
    return success;
}

bool relay_blueprint_library_init(Relay_BlueprintLibrary *library,
    Relay_ScriptRuntime *runtime)
{
    if (library == NULL || runtime == NULL || runtime->state == NULL) {
        return false;
    }
    *library = (Relay_BlueprintLibrary){0};
    library->runtime = runtime;
    library->next_id = 1;
    return true;
}

Relay_BlueprintId relay_blueprint_library_create(
    Relay_BlueprintLibrary *library)
{
    Relay_Blueprint *blueprint;
    const size_t source_size = sizeof(relay_blueprint_default_source) - 1;

    if (library == NULL || library->runtime == NULL ||
        library->count >= RELAY_BLUEPRINT_CAPACITY ||
        library->next_id > UINT32_MAX -
            RELAY_BLUEPRINT_OUTPUT_DEFINITION_ID_BASE) {
        return 0;
    }
    blueprint = &library->blueprints[library->count];
    *blueprint = (Relay_Blueprint){0};
    blueprint->id = library->next_id++;
    (void)snprintf(blueprint->name, sizeof(blueprint->name), "Script %llu",
        (unsigned long long)blueprint->id);
    (void)snprintf(blueprint->key, sizeof(blueprint->key),
        "blueprint.script_%llu", (unsigned long long)blueprint->id);
    (void)memcpy(blueprint->source, relay_blueprint_default_source,
        source_size + 1);
    blueprint->source_size = source_size;
    blueprint->revision = 1;
    if (!relay_node_world_init(&blueprint->scene) ||
        !relay_blueprint_compile(library, blueprint)) {
        relay_node_world_shutdown(&blueprint->scene);
        relay_script_artifact_shutdown(library->runtime, &blueprint->artifact);
        relay_blueprint_plan_shutdown(&blueprint->plan);
        *blueprint = (Relay_Blueprint){0};
        return 0;
    }
    library->count++;
    return blueprint->id;
}

Relay_Blueprint *relay_blueprint_library_find(Relay_BlueprintLibrary *library,
    Relay_BlueprintId id)
{
    size_t index;

    if (library == NULL || id == 0) {
        return NULL;
    }
    for (index = 0; index < library->count; index++) {
        if (library->blueprints[index].id == id) {
            return &library->blueprints[index];
        }
    }
    return NULL;
}

const Relay_Blueprint *relay_blueprint_library_find_const(
    const Relay_BlueprintLibrary *library, Relay_BlueprintId id)
{
    size_t index;

    if (library == NULL || id == 0) {
        return NULL;
    }
    for (index = 0; index < library->count; index++) {
        if (library->blueprints[index].id == id) {
            return &library->blueprints[index];
        }
    }
    return NULL;
}

Relay_Blueprint *relay_blueprint_library_at(Relay_BlueprintLibrary *library,
    size_t index)
{
    return library != NULL && index < library->count ?
        &library->blueprints[index] : NULL;
}

bool relay_blueprint_rebuild_plan(Relay_BlueprintLibrary *library,
    Relay_Blueprint *blueprint)
{
    Relay_BlueprintPlan candidate = {0};

    if (library == NULL || blueprint == NULL ||
        !relay_blueprint_plan_build(library, blueprint, &candidate)) {
        return false;
    }
    relay_blueprint_plan_shutdown(&blueprint->plan);
    blueprint->plan = candidate;
    (void)snprintf(blueprint->diagnostic.message,
        sizeof(blueprint->diagnostic.message),
        "Architecture compiled successfully.");
    return true;
}

bool relay_blueprint_compile(Relay_BlueprintLibrary *library,
    Relay_Blueprint *blueprint)
{
    Relay_ScriptArtifact candidate_artifact = {0};
    Relay_ScriptSchema candidate_schema = {0};
    Relay_ScriptSchema old_schema;
    Relay_NodeWorld old_scene;
    Relay_BlueprintPlan candidate_plan = {0};
    bool interface_changed;
    bool replaced_scene = false;

    if (library == NULL || library->runtime == NULL || blueprint == NULL ||
        !relay_script_runtime_compile(library->runtime, blueprint->source,
            blueprint->source_size, blueprint->revision, &candidate_artifact,
            &candidate_schema, &blueprint->diagnostic)) {
        return false;
    }
    interface_changed = blueprint->artifact.installed &&
        !relay_blueprint_schema_equal(&blueprint->schema, &candidate_schema);
    if (interface_changed &&
        (blueprint->instance_count > 0 ||
            blueprint->architecture_reference_count > 0 ||
            blueprint->scene.count > 3)) {
        relay_script_artifact_shutdown(library->runtime, &candidate_artifact);
        (void)snprintf(blueprint->diagnostic.message,
            sizeof(blueprint->diagnostic.message),
            "Remove module references and instances before changing ports.");
        return false;
    }

    old_schema = blueprint->schema;
    old_scene = blueprint->scene;
    blueprint->schema = candidate_schema;
    relay_blueprint_definition_refresh(blueprint);
    if (!blueprint->artifact.installed || interface_changed) {
        Relay_NodeWorld candidate_scene = {0};

        if (!relay_node_world_init(&candidate_scene) ||
            !relay_blueprint_scene_create_system_nodes(blueprint,
                &candidate_scene)) {
            relay_node_world_shutdown(&candidate_scene);
            blueprint->schema = old_schema;
            relay_blueprint_definition_refresh(blueprint);
            relay_script_artifact_shutdown(library->runtime,
                &candidate_artifact);
            return false;
        }
        blueprint->scene = candidate_scene;
        replaced_scene = true;
    }
    if (!relay_blueprint_plan_build(library, blueprint, &candidate_plan)) {
        if (replaced_scene) {
            relay_node_world_shutdown(&blueprint->scene);
            blueprint->scene = old_scene;
        }
        blueprint->schema = old_schema;
        relay_blueprint_definition_refresh(blueprint);
        relay_script_artifact_shutdown(library->runtime, &candidate_artifact);
        return false;
    }
    if (replaced_scene) {
        relay_node_world_shutdown(&old_scene);
    }
    relay_script_artifact_shutdown(library->runtime, &blueprint->artifact);
    relay_blueprint_plan_shutdown(&blueprint->plan);
    blueprint->artifact = candidate_artifact;
    blueprint->plan = candidate_plan;
    blueprint->compiled_revision = blueprint->revision;
    blueprint->dirty = false;
    (void)snprintf(blueprint->diagnostic.message,
        sizeof(blueprint->diagnostic.message),
        "Source and architecture compiled successfully.");
    return true;
}

bool relay_blueprint_instantiate(Relay_BlueprintLibrary *library,
    Relay_Blueprint *blueprint, Relay_NodeWorld *world, int64_t grid_x,
    int64_t grid_y, Relay_NodeId *module_node_id)
{
    const size_t node_count = world == NULL ? 0 : world->count;
    const size_t connection_count = world == NULL ? 0 :
        world->connection_count;
    const size_t input_binding_count = world == NULL ? 0 :
        world->module_input_binding_count;
    const size_t output_binding_count = world == NULL ? 0 :
        world->module_output_binding_count;
    const Relay_NodeId next_id = world == NULL ? 0 : world->next_id;
    Relay_NodeId *node_ids = NULL;
    Relay_NodeId wrapper_id;
    size_t index;
    bool success = false;

    if (library == NULL || blueprint == NULL || world == NULL ||
        module_node_id == NULL || !blueprint->artifact.installed ||
        !blueprint->plan.valid) {
        return false;
    }
    wrapper_id = relay_node_world_create_definition(world,
        &blueprint->definition, grid_x, grid_y);
    if (wrapper_id == 0) {
        return false;
    }
    {
        Relay_Node *wrapper = relay_node_world_find(world, wrapper_id);

        wrapper->runtime_kind = RELAY_NODE_RUNTIME_BLUEPRINT_WRAPPER;
        wrapper->blueprint_id = blueprint->id;
    }
    node_ids = calloc(blueprint->plan.node_count, sizeof(*node_ids));
    if (node_ids == NULL && blueprint->plan.node_count > 0) {
        goto cleanup;
    }
    for (index = 0; index < blueprint->plan.node_count; index++) {
        const Relay_BlueprintPlanNode *plan_node =
            &blueprint->plan.nodes[index];
        Relay_Node *node;

        node_ids[index] = relay_node_world_create_definition(world,
            plan_node->definition, grid_x, grid_y);
        if (node_ids[index] == 0) {
            goto cleanup;
        }
        node = relay_node_world_find(world, node_ids[index]);
        node->runtime_kind = plan_node->runtime_kind;
        node->blueprint_id = plan_node->script_blueprint_id;
        node->origin_blueprint_id = plan_node->origin_blueprint_id;
        node->origin_node_id = plan_node->origin_node_id;
        node->module_instance_id = wrapper_id;
    }
    for (index = 0; index < blueprint->plan.connection_count; index++) {
        const Relay_BlueprintPlanConnection *connection =
            &blueprint->plan.connections[index];

        if (!relay_node_world_connect(world,
                node_ids[connection->source_node_index],
                connection->source_port_index,
                node_ids[connection->destination_node_index],
                connection->destination_port_index)) {
            goto cleanup;
        }
    }
    for (index = 0; index < blueprint->plan.input_binding_count; index++) {
        const Relay_BlueprintPlanInputBinding *binding =
            &blueprint->plan.input_bindings[index];

        if (!relay_node_world_bind_module_input(world,
                (Relay_NodeModuleInputBinding){
                    wrapper_id, binding->module_port_index,
                    node_ids[binding->destination_node_index],
                    binding->destination_port_index
                })) {
            goto cleanup;
        }
    }
    for (index = 0; index < blueprint->plan.output_binding_count; index++) {
        const Relay_BlueprintPlanOutputBinding *binding =
            &blueprint->plan.output_bindings[index];

        if (!relay_node_world_bind_module_output(world,
                (Relay_NodeModuleOutputBinding){
                    wrapper_id, binding->module_port_index,
                    binding->source_is_module_input ? 0 :
                        node_ids[binding->source_node_index],
                    binding->source_port_index,
                    binding->source_module_input_port_index,
                    binding->source_is_module_input
                })) {
            goto cleanup;
        }
    }
    blueprint->instance_count++;
    *module_node_id = wrapper_id;
    success = true;

cleanup:
    if (!success) {
        world->count = node_count;
        world->connection_count = connection_count;
        world->module_input_binding_count = input_binding_count;
        world->module_output_binding_count = output_binding_count;
        world->next_id = next_id;
    }
    free(node_ids);
    return success;
}

void relay_blueprint_library_shutdown(Relay_BlueprintLibrary *library)
{
    size_t blueprint_index;

    if (library == NULL) {
        return;
    }
    for (blueprint_index = 0; blueprint_index < library->count;
            blueprint_index++) {
        Relay_Blueprint *blueprint = &library->blueprints[blueprint_index];
        size_t node_index;

        for (node_index = 0; node_index < blueprint->scene.count; node_index++) {
            relay_script_instance_shutdown(library->runtime,
                &blueprint->scene.nodes[node_index].script_state);
        }
        relay_node_world_shutdown(&blueprint->scene);
        relay_script_artifact_shutdown(library->runtime,
            &blueprint->artifact);
        relay_blueprint_plan_shutdown(&blueprint->plan);
    }
    *library = (Relay_BlueprintLibrary){0};
}
