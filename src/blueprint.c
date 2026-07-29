#include "relay/blueprint.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char relay_blueprint_default_source[] =
    "input(\"trigger\", Type.TRIGGER)\n"
    "output(\"trigger_out\", Type.TRIGGER)\n"
    "\n"
    "function on_process(inputs, state)\n"
    "  state.activations = (state.activations or 0) + 1\n"
    "  return { trigger_out = inputs.trigger or 0 }\n"
    "end\n";

static bool relay_blueprint_scene_create_system_nodes(
    Relay_Blueprint *blueprint, Relay_NodeWorld *scene);

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

/** Append formatted text to a bounded Blueprint source candidate. */
static bool relay_blueprint_source_append(char *destination, size_t *size,
    const char *format, ...)
{
    va_list arguments;
    int written;

    if (*size >= RELAY_BLUEPRINT_SOURCE_CAPACITY) {
        return false;
    }
    va_start(arguments, format);
    written = vsnprintf(&destination[*size],
        RELAY_BLUEPRINT_SOURCE_CAPACITY - *size, format, arguments);
    va_end(arguments);
    if (written < 0 ||
        (size_t)written >= RELAY_BLUEPRINT_SOURCE_CAPACITY - *size) {
        return false;
    }
    *size += (size_t)written;
    return true;
}

/** Remove blank lines accumulated at the end of a generated source section. */
static void relay_blueprint_source_trim_blank_tail(char *source, size_t *size)
{
    while (*size > 0) {
        size_t line_end = *size;
        size_t line_start;
        size_t index;
        bool blank = true;

        if (source[line_end - 1] == '\n') {
            line_end--;
        }
        line_start = line_end;
        while (line_start > 0 && source[line_start - 1] != '\n') {
            line_start--;
        }
        for (index = line_start; index < line_end; index++) {
            if (!isspace((unsigned char)source[index])) {
                blank = false;
                break;
            }
        }
        if (!blank) {
            break;
        }
        *size = line_start;
    }
    source[*size] = '\0';
}

/** Return whether one source line is a declarative architecture statement. */
static bool relay_blueprint_line_is_architecture(const char *line, size_t size)
{
    size_t index;

    if (size >= 8 && strncmp(line, "connect(", 8) == 0) {
        return true;
    }
    if (size < 6 || strncmp(line, "local ", 6) != 0) {
        return false;
    }
    index = 6;
    while (index < size &&
        (isalnum((unsigned char)line[index]) || line[index] == '_')) {
        index++;
    }
    while (index < size && isspace((unsigned char)line[index])) {
        index++;
    }
    if (index >= size || line[index++] != '=') {
        return false;
    }
    while (index < size && isspace((unsigned char)line[index])) {
        index++;
    }
    return size - index >= 9 &&
        memcmp(&line[index], "instance(", 9) == 0;
}

/** Return whether one source line begins the deterministic module handler. */
static bool relay_blueprint_line_starts_handler(const char *line, size_t size)
{
    return size > 19 && strncmp(line, "function on_process", 19) == 0 &&
        (line[19] == '(' || isspace((unsigned char)line[19]));
}

/** Blank architecture declarations while preserving Lua diagnostic line numbers. */
static bool relay_blueprint_runtime_source(const char *source, size_t size,
    char *runtime_source)
{
    size_t line_start = 0;
    bool handler_started = false;

    (void)memcpy(runtime_source, source, size);
    runtime_source[size] = '\0';
    while (line_start < size) {
        size_t line_end = line_start;
        size_t index;

        while (line_end < size && source[line_end] != '\n') {
            line_end++;
        }
        if (!handler_started &&
            relay_blueprint_line_is_architecture(&source[line_start],
                line_end - line_start)) {
            for (index = line_start; index < line_end; index++) {
                runtime_source[index] = ' ';
            }
        }
        if (relay_blueprint_line_starts_handler(&source[line_start],
                line_end - line_start)) {
            handler_started = true;
        }
        line_start = line_end < size ? line_end + 1 : size;
    }
    return true;
}

/** Return one definition port key after validating its direction and index. */
static const char *relay_blueprint_port_key(const Relay_Node *node,
    size_t port_index, bool output)
{
    const Relay_NodeDefinition *definition = relay_node_definition_for(node);

    if (definition == NULL ||
        (output && port_index >= definition->output_count) ||
        (!output && port_index >= definition->input_count)) {
        return NULL;
    }
    return output ? definition->outputs[port_index].key :
        definition->inputs[port_index].key;
}

/** Append canonical Lua-shaped component declarations from the visual graph. */
static bool relay_blueprint_source_append_scene(
    const Relay_Blueprint *blueprint, char *candidate, size_t *candidate_size)
{
    size_t index;

    for (index = 0; index < blueprint->scene.count; index++) {
        const Relay_Node *node = &blueprint->scene.nodes[index];

        if (node->runtime_kind ==
                RELAY_NODE_RUNTIME_BLUEPRINT_INPUT_BOUNDARY ||
            node->runtime_kind ==
                RELAY_NODE_RUNTIME_BLUEPRINT_OUTPUT_BOUNDARY) {
            continue;
        }
        if (node->definition == NULL || node->local_key[0] == '\0' ||
            !relay_blueprint_source_append(candidate, candidate_size,
                "local %s = instance(%s, { x = %lld, y = %lld })\n",
                node->local_key, node->definition->key,
                (long long)node->grid_x, (long long)node->grid_y)) {
            return false;
        }
    }
    for (index = 0; index < blueprint->scene.connection_count; index++) {
        const Relay_NodeConnection *connection =
            &blueprint->scene.connections[index];
        const Relay_Node *source = relay_node_world_find_const(
            &blueprint->scene, connection->source_node_id);
        const Relay_Node *destination = relay_node_world_find_const(
            &blueprint->scene, connection->destination_node_id);
        const char *source_port = relay_blueprint_port_key(source,
            connection->source_port_index, true);
        const char *destination_port = relay_blueprint_port_key(destination,
            connection->destination_port_index, false);
        bool source_is_boundary;
        bool destination_is_boundary;

        if (source == NULL || destination == NULL || source_port == NULL ||
            destination_port == NULL) {
            return false;
        }
        source_is_boundary =
            source->id == blueprint->input_boundary_node_id;
        destination_is_boundary =
            destination->id == blueprint->output_boundary_node_id;
        if (source_is_boundary && destination_is_boundary) {
            if (!relay_blueprint_source_append(candidate, candidate_size,
                    "connect(inputs.%s, outputs.%s)\n", source_port,
                    destination_port)) {
                return false;
            }
        } else if (source_is_boundary) {
            if (!relay_blueprint_source_append(candidate, candidate_size,
                    "connect(inputs.%s, %s.inputs.%s)\n", source_port,
                    destination->local_key, destination_port)) {
                return false;
            }
        } else if (destination_is_boundary) {
            if (!relay_blueprint_source_append(candidate, candidate_size,
                    "connect(%s.outputs.%s, outputs.%s)\n",
                    source->local_key, source_port, destination_port)) {
                return false;
            }
        } else if (!relay_blueprint_source_append(candidate, candidate_size,
                "connect(%s.outputs.%s, %s.inputs.%s)\n",
                source->local_key, source_port, destination->local_key,
                destination_port)) {
            return false;
        }
    }
    return true;
}

/** Synchronize canonical top-level Lua declarations from the visual graph. */
static bool relay_blueprint_source_from_scene(const Relay_Blueprint *blueprint,
    char *candidate, size_t *candidate_size)
{
    size_t line_start = 0;
    bool inserted = false;

    *candidate_size = 0;
    while (line_start < blueprint->source_size) {
        size_t line_end = line_start;
        size_t line_size;

        while (line_end < blueprint->source_size &&
            blueprint->source[line_end] != '\n') {
            line_end++;
        }
        line_size = line_end - line_start;
        if (!inserted && relay_blueprint_line_starts_handler(
                &blueprint->source[line_start], line_size)) {
            const size_t declaration_start = *candidate_size;

            relay_blueprint_source_trim_blank_tail(candidate, candidate_size);
            if (*candidate_size > 0 &&
                candidate[*candidate_size - 1] != '\n' &&
                !relay_blueprint_source_append(candidate, candidate_size,
                    "\n")) {
                return false;
            }
            if (*candidate_size > 0 &&
                !relay_blueprint_source_append(candidate, candidate_size,
                    "\n")) {
                return false;
            }
            if (!relay_blueprint_source_append_scene(blueprint, candidate,
                    candidate_size)) {
                return false;
            }
            if (*candidate_size > declaration_start &&
                !relay_blueprint_source_append(candidate, candidate_size,
                    "\n")) {
                return false;
            }
            inserted = true;
        }
        if (inserted || !relay_blueprint_line_is_architecture(
                &blueprint->source[line_start], line_size)) {
            if (!relay_blueprint_source_append(candidate, candidate_size,
                    "%.*s%s", (int)line_size,
                    &blueprint->source[line_start],
                    line_end < blueprint->source_size ? "\n" : "")) {
                return false;
            }
        }
        line_start = line_end < blueprint->source_size ?
            line_end + 1 : blueprint->source_size;
    }
    if (!inserted) {
        relay_blueprint_source_trim_blank_tail(candidate, candidate_size);
        if (*candidate_size > 0 &&
            candidate[*candidate_size - 1] != '\n' &&
            !relay_blueprint_source_append(candidate, candidate_size, "\n")) {
            return false;
        }
        if (*candidate_size > 0 &&
            !relay_blueprint_source_append(candidate, candidate_size, "\n")) {
            return false;
        }
        if (!relay_blueprint_source_append_scene(blueprint, candidate,
                candidate_size)) {
            return false;
        }
    }
    candidate[*candidate_size] = '\0';
    return true;
}

/** Resolve a stable built-in or Blueprint definition key. */
static const Relay_NodeDefinition *relay_blueprint_definition_find_key(
    const Relay_BlueprintLibrary *library, const char *key,
    Relay_BlueprintId *blueprint_id)
{
    const Relay_NodeDefinition *definition = relay_node_definition_find_key(key);
    size_t index;

    *blueprint_id = 0;
    if (definition != NULL) {
        return definition;
    }
    for (index = 0; index < library->count; index++) {
        const Relay_Blueprint *candidate = &library->blueprints[index];

        if (strcmp(candidate->key, key) == 0) {
            *blueprint_id = candidate->id;
            return &candidate->definition;
        }
    }
    return NULL;
}

/** Find one visual component by its architecture-local stable key. */
static Relay_Node *relay_blueprint_scene_find_local(Relay_NodeWorld *scene,
    const char *local_key)
{
    size_t index;

    for (index = 0; index < scene->count; index++) {
        if (strcmp(scene->nodes[index].local_key, local_key) == 0) {
            return &scene->nodes[index];
        }
    }
    return NULL;
}

/** Return whether an architecture-local component key is script-stable. */
static bool relay_blueprint_local_key_is_valid(const char *key)
{
    static const char *const reserved[] = {
        "and", "break", "do", "else", "elseif", "end", "false", "for",
        "function", "goto", "if", "in", "local", "nil", "not", "or",
        "repeat", "return", "then", "true", "until", "while"
    };
    size_t index;

    if (key[0] == '\0' ||
        !(isalpha((unsigned char)key[0]) || key[0] == '_')) {
        return false;
    }
    for (index = 1; key[index] != '\0'; index++) {
        if (!(isalnum((unsigned char)key[index]) || key[index] == '_')) {
            return false;
        }
    }
    for (index = 0; index < sizeof(reserved) / sizeof(reserved[0]); index++) {
        if (strcmp(key, reserved[index]) == 0) {
            return false;
        }
    }
    return true;
}

/** Resolve a stable port key to its declared directional index. */
static bool relay_blueprint_port_index(const Relay_Node *node,
    const char *key, bool output, size_t *port_index)
{
    const Relay_NodeDefinition *definition = relay_node_definition_for(node);
    const Relay_NodePortDefinition *ports;
    size_t count;
    size_t index;

    if (definition == NULL) {
        return false;
    }
    ports = output ? definition->outputs : definition->inputs;
    count = output ? definition->output_count : definition->input_count;
    for (index = 0; index < count; index++) {
        if (strcmp(ports[index].key, key) == 0) {
            *port_index = index;
            return true;
        }
    }
    return false;
}

/** Trim surrounding ASCII whitespace from one mutable source token. */
static char *relay_blueprint_token_trim(char *token)
{
    char *end;

    while (isspace((unsigned char)*token)) {
        token++;
    }
    end = token + strlen(token);
    while (end > token && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return token;
}

/** Return whether one namespace or member is canonical lowercase snake_case. */
static bool relay_blueprint_snake_name_is_valid(const char *name,
    size_t length)
{
    size_t index;
    bool underscore = false;

    if (length == 0 || !islower((unsigned char)name[0])) {
        return false;
    }
    for (index = 1; index < length; index++) {
        if (name[index] == '_') {
            if (underscore || index + 1 == length) {
                return false;
            }
            underscore = true;
        } else {
            if (!islower((unsigned char)name[index]) &&
                !isdigit((unsigned char)name[index])) {
                return false;
            }
            underscore = false;
        }
    }
    return true;
}

/** Validate one canonical namespace.member component definition symbol. */
static bool relay_blueprint_definition_symbol_is_valid(const char *symbol)
{
    const char *dot = strchr(symbol, '.');

    return dot != NULL && dot != symbol && strchr(dot + 1, '.') == NULL &&
        relay_blueprint_snake_name_is_valid(symbol, (size_t)(dot - symbol)) &&
        relay_blueprint_snake_name_is_valid(dot + 1, strlen(dot + 1));
}

/** Parse one directional endpoint into a component key and port key. */
static bool relay_blueprint_endpoint_parse(const char *endpoint, bool source,
    char *instance, size_t instance_capacity, char *port,
    size_t port_capacity, bool *boundary)
{
    const char *first = strchr(endpoint, '.');
    const char *second = first == NULL ? NULL : strchr(first + 1, '.');
    const char *expected_boundary = source ? "inputs" : "outputs";
    const char *expected_direction = source ? "outputs" : "inputs";
    size_t instance_size;
    size_t port_size;

    if (first == NULL || first == endpoint) {
        return false;
    }
    instance_size = (size_t)(first - endpoint);
    if (second == NULL) {
        port_size = strlen(first + 1);
        if (instance_size != strlen(expected_boundary) ||
            memcmp(endpoint, expected_boundary, instance_size) != 0) {
            return false;
        }
        *boundary = true;
    } else {
        const size_t direction_size = (size_t)(second - first - 1);

        port_size = strlen(second + 1);
        if (strchr(second + 1, '.') != NULL ||
            direction_size != strlen(expected_direction) ||
            memcmp(first + 1, expected_direction, direction_size) != 0) {
            return false;
        }
        *boundary = false;
        first = second;
    }
    if (instance_size >= instance_capacity || port_size == 0 ||
        port_size >= port_capacity) {
        return false;
    }
    (void)memcpy(instance, endpoint, instance_size);
    instance[instance_size] = '\0';
    (void)memcpy(port, first + 1, port_size + 1);
    return relay_blueprint_local_key_is_valid(instance) &&
        relay_blueprint_local_key_is_valid(port);
}

/** Rehydrate one typed visual architecture from top-level declarations. */
static bool relay_blueprint_scene_from_source(Relay_BlueprintLibrary *library,
    Relay_Blueprint *blueprint, Relay_NodeWorld *scene)
{
    const char *cursor = blueprint->source;

    if (!relay_node_world_init(scene) ||
        !relay_blueprint_scene_create_system_nodes(blueprint, scene)) {
        return false;
    }
    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        const size_t line_size = line_end == NULL ? strlen(cursor) :
            (size_t)(line_end - cursor);
        char line[256];
        char *statement;

        if (line_size >= sizeof(line)) {
            if (relay_blueprint_line_is_architecture(cursor, line_size)) {
                return false;
            }
            cursor = line_end == NULL ? cursor + line_size : line_end + 1;
            continue;
        }
        (void)memcpy(line, cursor, line_size);
        line[line_size] = '\0';
        if (relay_blueprint_line_starts_handler(cursor, line_size)) {
            break;
        }
        statement = relay_blueprint_token_trim(line);
        if (relay_blueprint_line_is_architecture(cursor, line_size) &&
            strncmp(statement, "local ", 6) == 0) {
            char local_key[RELAY_NODE_LOCAL_KEY_CAPACITY];
            char definition_key[RELAY_BLUEPRINT_KEY_CAPACITY];
            long long grid_x;
            long long grid_y;
            int consumed = 0;
            Relay_BlueprintId child_id;
            const Relay_NodeDefinition *definition;
            Relay_NodeId node_id;
            Relay_Node *node;

            if (sscanf(statement,
                    "local %31s = instance(%63[a-z0-9_.], { x = %lld, y = %lld })%n",
                    local_key, definition_key, &grid_x, &grid_y,
                    &consumed) != 4 || statement[consumed] != '\0' ||
                !relay_blueprint_local_key_is_valid(local_key) ||
                !relay_blueprint_definition_symbol_is_valid(definition_key) ||
                strcmp(local_key, "inputs") == 0 ||
                strcmp(local_key, "outputs") == 0 ||
                strcmp(local_key, "input") == 0 ||
                strcmp(local_key, "output") == 0 ||
                strcmp(local_key, "instance") == 0 ||
                strcmp(local_key, "connect") == 0 ||
                strcmp(local_key, "on_process") == 0 ||
                relay_blueprint_scene_find_local(scene, local_key) != NULL) {
                return false;
            }
            definition = relay_blueprint_definition_find_key(library,
                definition_key, &child_id);
            if (definition == NULL || child_id == blueprint->id) {
                return false;
            }
            node_id = relay_node_world_create_definition(scene, definition,
                (int64_t)grid_x, (int64_t)grid_y);
            node = relay_node_world_find(scene, node_id);
            if (node == NULL) {
                return false;
            }
            (void)snprintf(node->local_key, sizeof(node->local_key), "%s",
                local_key);
            if (child_id != 0) {
                node->runtime_kind = RELAY_NODE_RUNTIME_BLUEPRINT_WRAPPER;
                node->blueprint_id = child_id;
            }
        } else if (relay_blueprint_line_is_architecture(cursor, line_size) &&
            strncmp(statement, "connect(", 8) == 0) {
            char source_endpoint[80];
            char destination_endpoint[80];
            char source_instance[RELAY_NODE_LOCAL_KEY_CAPACITY];
            char destination_instance[RELAY_NODE_LOCAL_KEY_CAPACITY];
            char source_port[RELAY_SCRIPT_PORT_KEY_CAPACITY];
            char destination_port[RELAY_SCRIPT_PORT_KEY_CAPACITY];
            int consumed = 0;
            Relay_Node *source;
            Relay_Node *destination;
            size_t source_port_index;
            size_t destination_port_index;
            size_t connection_index;
            bool source_is_boundary;
            bool destination_is_boundary;

            if (sscanf(statement, "connect(%79[^,], %79[^)])%n",
                    source_endpoint, destination_endpoint, &consumed) != 2 ||
                statement[consumed] != '\0' ||
                !relay_blueprint_endpoint_parse(
                    relay_blueprint_token_trim(source_endpoint), true,
                    source_instance, sizeof(source_instance), source_port,
                    sizeof(source_port), &source_is_boundary) ||
                !relay_blueprint_endpoint_parse(
                    relay_blueprint_token_trim(destination_endpoint), false,
                    destination_instance, sizeof(destination_instance),
                    destination_port, sizeof(destination_port),
                    &destination_is_boundary)) {
                return false;
            }
            source = source_is_boundary ?
                relay_node_world_find(scene,
                    blueprint->input_boundary_node_id) :
                relay_blueprint_scene_find_local(scene, source_instance);
            destination = destination_is_boundary ?
                relay_node_world_find(scene,
                    blueprint->output_boundary_node_id) :
                relay_blueprint_scene_find_local(scene,
                    destination_instance);
            if (source == NULL || destination == NULL ||
                !relay_blueprint_port_index(source, source_port, true,
                    &source_port_index) ||
                !relay_blueprint_port_index(destination, destination_port,
                    false, &destination_port_index)) {
                return false;
            }
            for (connection_index = 0;
                    connection_index < scene->connection_count;
                    connection_index++) {
                const Relay_NodeConnection *existing =
                    &scene->connections[connection_index];

                if (existing->destination_node_id == destination->id &&
                    existing->destination_port_index ==
                        destination_port_index) {
                    return false;
                }
            }
            if (!relay_node_world_connect(scene, source->id, source_port_index,
                    destination->id, destination_port_index)) {
                return false;
            }
        }
        cursor = line_end == NULL ? cursor + line_size : line_end + 1;
    }
    return true;
}

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

/** Return whether another visual architecture instantiates this Blueprint. */
static bool relay_blueprint_library_references(
    const Relay_BlueprintLibrary *library, Relay_BlueprintId blueprint_id)
{
    size_t blueprint_index;

    for (blueprint_index = 0; blueprint_index < library->count;
            blueprint_index++) {
        const Relay_NodeWorld *scene =
            &library->blueprints[blueprint_index].scene;
        size_t node_index;

        for (node_index = 0; node_index < scene->count; node_index++) {
            const Relay_Node *node = &scene->nodes[node_index];

            if (node->runtime_kind ==
                    RELAY_NODE_RUNTIME_BLUEPRINT_WRAPPER &&
                node->blueprint_id == blueprint_id) {
                return true;
            }
        }
    }
    return false;
}

/** Rebuild all stable definition views from the installed public schema. */
static void relay_blueprint_definition_refresh(Relay_Blueprint *blueprint)
{
    const Relay_NodePropertyDefinition *universal_properties;
    size_t universal_property_count;
    size_t index;

    universal_properties = relay_node_universal_properties(
        &universal_property_count);
    for (index = 0; index < blueprint->schema.input_count; index++) {
        const Relay_NodePortDefinition port = {
            blueprint->schema.inputs[index].key,
            blueprint->schema.inputs[index].key,
            blueprint->schema.inputs[index].type
        };

        blueprint->inputs[index] = port;
        blueprint->input_boundary_outputs[index] = port;
        blueprint->process_inputs[index] = port;
    }
    for (index = 0; index < blueprint->schema.output_count; index++) {
        const Relay_NodePortDefinition port = {
            blueprint->schema.outputs[index].key,
            blueprint->schema.outputs[index].key,
            blueprint->schema.outputs[index].type
        };

        blueprint->outputs[index] = port;
        blueprint->process_outputs[index] = port;
        blueprint->output_boundary_inputs[index] = port;
    }
    blueprint->definition = (Relay_NodeDefinition){
        (Relay_NodeDefinitionId)(RELAY_BLUEPRINT_DEFINITION_ID_BASE +
            blueprint->id),
        blueprint->key, blueprint->name, "λ",
        "Compiled hierarchical Blueprint module.", RELAY_NODE_CATEGORY_MODULE,
        blueprint->inputs, blueprint->schema.input_count, blueprint->outputs,
        blueprint->schema.output_count, universal_properties,
        universal_property_count, {RELAY_NODE_BEHAVIOR_NONE, 0, 0, 0}
    };
    blueprint->input_boundary_definition = (Relay_NodeDefinition){
        (Relay_NodeDefinitionId)(RELAY_BLUEPRINT_INPUT_DEFINITION_ID_BASE +
            blueprint->id),
        "blueprint.boundary.inputs", "Module Inputs", "⇥",
        "Public inputs entering this Blueprint architecture.",
        RELAY_NODE_CATEGORY_MODULE, NULL, 0,
        blueprint->input_boundary_outputs, blueprint->schema.input_count,
        universal_properties, universal_property_count,
        {RELAY_NODE_BEHAVIOR_NONE, 0, 0, 0}
    };
    blueprint->process_definition = (Relay_NodeDefinition){
        (Relay_NodeDefinitionId)(RELAY_BLUEPRINT_PROCESS_DEFINITION_ID_BASE +
            blueprint->id),
        "blueprint.process", "Module Process", "λ",
        "The Blueprint's implicit player-authored Lua process.",
        RELAY_NODE_CATEGORY_MODULE, blueprint->process_inputs,
        blueprint->schema.input_count, blueprint->process_outputs,
        blueprint->schema.output_count, universal_properties,
        universal_property_count, {RELAY_NODE_BEHAVIOR_NONE, 0, 0, 0}
    };
    blueprint->output_boundary_definition = (Relay_NodeDefinition){
        (Relay_NodeDefinitionId)(RELAY_BLUEPRINT_OUTPUT_DEFINITION_ID_BASE +
            blueprint->id),
        "blueprint.boundary.outputs", "Module Outputs", "⇤",
        "Public outputs leaving this Blueprint architecture.",
        RELAY_NODE_CATEGORY_MODULE, blueprint->output_boundary_inputs,
        blueprint->schema.output_count, NULL, 0, universal_properties,
        universal_property_count, {RELAY_NODE_BEHAVIOR_NONE, 0, 0, 0}
    };
}

/** Create the public input and output boundary nodes for one architecture. */
static bool relay_blueprint_scene_create_system_nodes(
    Relay_Blueprint *blueprint, Relay_NodeWorld *scene)
{
    Relay_Node *node;

    blueprint->input_boundary_node_id = relay_node_world_create_definition(scene,
        &blueprint->input_boundary_definition, -30, 0);
    blueprint->output_boundary_node_id = relay_node_world_create_definition(scene,
        &blueprint->output_boundary_definition, 30, 0);
    if (blueprint->input_boundary_node_id == 0 ||
        blueprint->output_boundary_node_id == 0) {
        return false;
    }
    node = relay_node_world_find(scene, blueprint->input_boundary_node_id);
    node->runtime_kind = RELAY_NODE_RUNTIME_BLUEPRINT_INPUT_BOUNDARY;
    node->blueprint_id = blueprint->id;
    node = relay_node_world_find(scene, blueprint->output_boundary_node_id);
    node->runtime_kind = RELAY_NODE_RUNTIME_BLUEPRINT_OUTPUT_BOUNDARY;
    node->blueprint_id = blueprint->id;
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
    if (!relay_blueprint_plan_add_node(plan,
            (Relay_BlueprintPlanNode){
                &blueprint->process_definition, blueprint->id,
                blueprint->id, 0,
                RELAY_NODE_RUNTIME_BLUEPRINT_PROCESS
            }, NULL)) {
        return false;
    }
    for (index = 0; index < blueprint->schema.input_count; index++) {
        if (!relay_blueprint_plan_add_input_binding(plan,
                (Relay_BlueprintPlanInputBinding){index, 0, index})) {
            relay_blueprint_plan_shutdown(plan);
            return false;
        }
    }
    for (index = 0; index < blueprint->schema.output_count; index++) {
        if (!relay_blueprint_plan_add_output_binding(plan,
                (Relay_BlueprintPlanOutputBinding){
                    index, 0, index, 0, false
                })) {
            relay_blueprint_plan_shutdown(plan);
            return false;
        }
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
                            RELAY_NODE_RUNTIME_BLUEPRINT_PROCESS ?
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
            Relay_BlueprintPlanOutputBinding *binding;

            if (!source.valid ||
                connection->destination_port_index >=
                    plan->output_binding_count) {
                goto cleanup;
            }
            binding = &plan->output_bindings[
                connection->destination_port_index];
            *binding = (Relay_BlueprintPlanOutputBinding){
                connection->destination_port_index,
                source.node_index, source.port_index,
                source.module_input_port_index, source.is_module_input
            };
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
    (void)snprintf(blueprint->name, sizeof(blueprint->name), "script_%llu",
        (unsigned long long)blueprint->id);
    (void)snprintf(blueprint->key, sizeof(blueprint->key),
        "script.script_%llu", (unsigned long long)blueprint->id);
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
    char candidate_source[RELAY_BLUEPRINT_SOURCE_CAPACITY];
    char runtime_source[RELAY_BLUEPRINT_SOURCE_CAPACITY];
    size_t candidate_source_size = 0;
    Relay_ScriptArtifact candidate_artifact = {0};
    Relay_ScriptSchema candidate_schema = {0};
    Relay_BlueprintPlan candidate = {0};
    const uint64_t candidate_revision =
        blueprint == NULL ? 0 : blueprint->revision + 1;

    if (library == NULL || blueprint == NULL ||
        !relay_blueprint_source_from_scene(blueprint, candidate_source,
            &candidate_source_size) ||
        !relay_blueprint_runtime_source(candidate_source,
            candidate_source_size, runtime_source) ||
        !relay_script_runtime_compile(library->runtime, runtime_source,
            candidate_source_size, candidate_revision, &candidate_artifact,
            &candidate_schema, &blueprint->diagnostic) ||
        !relay_blueprint_schema_equal(&blueprint->schema, &candidate_schema) ||
        !relay_blueprint_plan_build(library, blueprint, &candidate)) {
        relay_script_artifact_shutdown(library == NULL ? NULL :
            library->runtime, &candidate_artifact);
        return false;
    }
    relay_script_artifact_shutdown(library->runtime, &blueprint->artifact);
    relay_blueprint_plan_shutdown(&blueprint->plan);
    blueprint->artifact = candidate_artifact;
    blueprint->plan = candidate;
    (void)memcpy(blueprint->source, candidate_source,
        candidate_source_size + 1);
    blueprint->source_size = candidate_source_size;
    blueprint->revision = candidate_revision;
    blueprint->compiled_revision = candidate_revision;
    blueprint->dirty = false;
    (void)snprintf(blueprint->diagnostic.message,
        sizeof(blueprint->diagnostic.message),
        "Architecture synchronized and compiled successfully.");
    return true;
}

bool relay_blueprint_compile(Relay_BlueprintLibrary *library,
    Relay_Blueprint *blueprint)
{
    char runtime_source[RELAY_BLUEPRINT_SOURCE_CAPACITY];
    Relay_ScriptArtifact candidate_artifact = {0};
    Relay_ScriptSchema candidate_schema = {0};
    Relay_ScriptSchema old_schema;
    Relay_NodeWorld old_scene;
    Relay_NodeWorld candidate_scene = {0};
    Relay_BlueprintPlan candidate_plan = {0};
    Relay_NodeId old_input_boundary_node_id;
    Relay_NodeId old_output_boundary_node_id;
    bool interface_changed;

    if (library == NULL || library->runtime == NULL || blueprint == NULL ||
        !relay_blueprint_runtime_source(blueprint->source,
            blueprint->source_size, runtime_source) ||
        !relay_script_runtime_compile(library->runtime, runtime_source,
            blueprint->source_size, blueprint->revision, &candidate_artifact,
            &candidate_schema, &blueprint->diagnostic)) {
        return false;
    }
    interface_changed = blueprint->artifact.installed &&
        !relay_blueprint_schema_equal(&blueprint->schema, &candidate_schema);
    if (interface_changed &&
        (blueprint->instance_count > 0 ||
            relay_blueprint_library_references(library, blueprint->id) ||
            blueprint->scene.count > 2)) {
        relay_script_artifact_shutdown(library->runtime, &candidate_artifact);
        (void)snprintf(blueprint->diagnostic.message,
            sizeof(blueprint->diagnostic.message),
            "Remove module references and instances before changing ports.");
        return false;
    }

    old_schema = blueprint->schema;
    old_scene = blueprint->scene;
    old_input_boundary_node_id = blueprint->input_boundary_node_id;
    old_output_boundary_node_id = blueprint->output_boundary_node_id;
    blueprint->schema = candidate_schema;
    relay_blueprint_definition_refresh(blueprint);
    if (!relay_blueprint_scene_from_source(library, blueprint,
            &candidate_scene)) {
        relay_node_world_shutdown(&candidate_scene);
        blueprint->schema = old_schema;
        blueprint->input_boundary_node_id = old_input_boundary_node_id;
        blueprint->output_boundary_node_id = old_output_boundary_node_id;
        relay_blueprint_definition_refresh(blueprint);
        relay_script_artifact_shutdown(library->runtime, &candidate_artifact);
        (void)snprintf(blueprint->diagnostic.message,
            sizeof(blueprint->diagnostic.message),
            "Invalid Blueprint architecture declarations.");
        return false;
    }
    blueprint->scene = candidate_scene;
    if (!relay_blueprint_plan_build(library, blueprint, &candidate_plan)) {
        relay_node_world_shutdown(&blueprint->scene);
        blueprint->scene = old_scene;
        blueprint->schema = old_schema;
        blueprint->input_boundary_node_id = old_input_boundary_node_id;
        blueprint->output_boundary_node_id = old_output_boundary_node_id;
        relay_blueprint_definition_refresh(blueprint);
        relay_script_artifact_shutdown(library->runtime, &candidate_artifact);
        return false;
    }
    relay_node_world_shutdown(&old_scene);
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
