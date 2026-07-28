#ifndef RELAY_NODE_RENDERER_H
#define RELAY_NODE_RENDERER_H

#include "relay/node.h"

/** Terminal-agnostic visual selected for one node definition. */
typedef struct Relay_NodeVisual {
    const char *glyph;
    unsigned int color;
} Relay_NodeVisual;

/** Complete terminal presentation contract for one graph node card. */
typedef struct Relay_NodeRenderCard {
    Relay_NodeVisual visual;
    const Relay_NodeDefinition *definition;
    int width;
    int height;
} Relay_NodeRenderCard;

/** Return the renderer visual for a node instance. */
Relay_NodeVisual relay_node_renderer_visual(const Relay_Node *node);

/** Return the terminal-agnostic visual token for one fixed graph port type. */
Relay_NodeVisual relay_node_renderer_port_visual(Relay_NodePortType type);

/** Return aligned card dimensions and schema data for a graph node. */
Relay_NodeRenderCard relay_node_renderer_card(const Relay_Node *node);

#endif
