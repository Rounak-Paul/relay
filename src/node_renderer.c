#include "relay/node_renderer.h"

/** Select a stable terminal visual for a node definition. */
Relay_NodeVisual relay_node_renderer_visual(const Relay_Node *node)
{
    const Relay_NodeDefinition *definition = node == NULL ? NULL :
        relay_node_definition_find(node->definition_id);

    if (definition == NULL) {
        return (Relay_NodeVisual){"??", 1};
    }
    if (definition->id == RELAY_NODE_DEFINITION_CLOCK) {
        return (Relay_NodeVisual){definition->glyph, 3};
    }
    return (Relay_NodeVisual){definition->glyph, 2};
}

Relay_NodeRenderCard relay_node_renderer_card(const Relay_Node *node)
{
    const Relay_NodeDefinition *definition = node == NULL ? NULL :
        relay_node_definition_find(node->definition_id);
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
