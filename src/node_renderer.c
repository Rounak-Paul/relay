#include "relay/node_renderer.h"

/** Select a stable terminal visual for a node definition. */
Relay_NodeVisual relay_node_renderer_visual(const Relay_Node *node)
{
    const Relay_NodeDefinition *definition = relay_node_definition_for(node);

    if (definition == NULL) {
        return (Relay_NodeVisual){"??", 1};
    }
    if (definition->id == RELAY_NODE_DEFINITION_CLOCK) {
        return (Relay_NodeVisual){definition->glyph, 3};
    }
    if (node->runtime_kind == RELAY_NODE_RUNTIME_BLUEPRINT_INPUT_BOUNDARY ||
        node->runtime_kind == RELAY_NODE_RUNTIME_BLUEPRINT_OUTPUT_BOUNDARY) {
        return (Relay_NodeVisual){definition->glyph, 6};
    }
    if (node->runtime_kind == RELAY_NODE_RUNTIME_BLUEPRINT_SCRIPT_CORE) {
        return (Relay_NodeVisual){definition->glyph, 5};
    }
    return (Relay_NodeVisual){definition->glyph, 2};
}

Relay_NodeVisual relay_node_renderer_port_visual(Relay_NodePortType type)
{
    switch (type) {
    case RELAY_NODE_PORT_TYPE_CLOCK:
        return (Relay_NodeVisual){"●", 3};
    case RELAY_NODE_PORT_TYPE_COAL:
        return (Relay_NodeVisual){"●", 1};
    case RELAY_NODE_PORT_TYPE_IRON_ORE:
        return (Relay_NodeVisual){"●", 2};
    case RELAY_NODE_PORT_TYPE_COPPER_ORE:
        return (Relay_NodeVisual){"●", 6};
    case RELAY_NODE_PORT_TYPE_STONE:
        return (Relay_NodeVisual){"●", 4};
    case RELAY_NODE_PORT_TYPE_BOOLEAN:
        return (Relay_NodeVisual){"●", 5};
    case RELAY_NODE_PORT_TYPE_INTEGER:
        return (Relay_NodeVisual){"●", 7};
    case RELAY_NODE_PORT_TYPE_TEXT:
        return (Relay_NodeVisual){"●", 8};
    case RELAY_NODE_PORT_TYPE_INVALID:
        return (Relay_NodeVisual){"?", 1};
    }
    return (Relay_NodeVisual){"?", 1};
}

Relay_NodeRenderCard relay_node_renderer_card(const Relay_Node *node)
{
    const Relay_NodeDefinition *definition = relay_node_definition_for(node);
    size_t port_rows;

    if (definition == NULL) {
        return (Relay_NodeRenderCard){{"??", 1}, NULL, 26, 5};
    }
    port_rows = definition->input_count > definition->output_count ?
        definition->input_count : definition->output_count;
    if (port_rows == 0) {
        port_rows = 1;
    }
    return (Relay_NodeRenderCard){relay_node_renderer_visual(node), definition,
        26, (int)port_rows + 5};
}
