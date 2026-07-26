#include "relay/terminal.h"

#include "relay/game.h"
#include "relay/node_renderer.h"

#include <stddef.h>
#include <stdio.h>

enum {
    RELAY_TERMINAL_MIN_GAME_WIDTH = 12,
    RELAY_TERMINAL_MIN_PANEL_WIDTH = 18,
    RELAY_TERMINAL_MAX_PANEL_WIDTH = 28,
    RELAY_TERMINAL_MIN_HEIGHT = 10
};

/** One graph-port hit result used to start or finish mouse wiring. */
typedef struct Relay_TerminalPortHit {
    Relay_NodeId node_id;
    size_t port_index;
    bool is_output;
} Relay_TerminalPortHit;

/** Orthogonal viewport route joining two graph-port anchors. */
typedef struct Relay_TerminalWireRoute {
    int source_x;
    int source_y;
    int destination_x;
    int destination_y;
    int source_top;
    int source_bottom;
    int destination_top;
    int destination_bottom;
    int bend_y;
} Relay_TerminalWireRoute;

/** One terminal-space point in an orthogonal graph-wire path. */
typedef struct Relay_TerminalPoint {
    int x;
    int y;
} Relay_TerminalPoint;

/** Fully resolved directional path shared by persistent and live wires. */
typedef struct Relay_TerminalWirePath {
    Relay_TerminalPoint points[6];
    size_t count;
} Relay_TerminalWirePath;

/** Cardinal segment directions used to select correct rounded-corner glyphs. */
typedef enum Relay_TerminalDirection {
    RELAY_TERMINAL_DIRECTION_NONE,
    RELAY_TERMINAL_DIRECTION_LEFT,
    RELAY_TERMINAL_DIRECTION_RIGHT,
    RELAY_TERMINAL_DIRECTION_UP,
    RELAY_TERMINAL_DIRECTION_DOWN
} Relay_TerminalDirection;

/** Return the cardinal direction from one path point to its adjacent point. */
static Relay_TerminalDirection relay_terminal_direction_between(
    Relay_TerminalPoint from, Relay_TerminalPoint to)
{
    if (to.x > from.x) {
        return RELAY_TERMINAL_DIRECTION_RIGHT;
    }
    if (to.x < from.x) {
        return RELAY_TERMINAL_DIRECTION_LEFT;
    }
    if (to.y > from.y) {
        return RELAY_TERMINAL_DIRECTION_DOWN;
    }
    if (to.y < from.y) {
        return RELAY_TERMINAL_DIRECTION_UP;
    }
    return RELAY_TERMINAL_DIRECTION_NONE;
}

/** Return a rounded-corner Unicode code point for two orthogonal segments. */
static uint32_t relay_terminal_corner_glyph(Relay_TerminalDirection incoming,
    Relay_TerminalDirection outgoing)
{
    if ((incoming == RELAY_TERMINAL_DIRECTION_RIGHT &&
            outgoing == RELAY_TERMINAL_DIRECTION_DOWN) ||
        (incoming == RELAY_TERMINAL_DIRECTION_UP &&
            outgoing == RELAY_TERMINAL_DIRECTION_LEFT)) {
        return 0x256EU;
    }
    if ((incoming == RELAY_TERMINAL_DIRECTION_RIGHT &&
            outgoing == RELAY_TERMINAL_DIRECTION_UP) ||
        (incoming == RELAY_TERMINAL_DIRECTION_DOWN &&
            outgoing == RELAY_TERMINAL_DIRECTION_LEFT)) {
        return 0x256FU;
    }
    if ((incoming == RELAY_TERMINAL_DIRECTION_LEFT &&
            outgoing == RELAY_TERMINAL_DIRECTION_DOWN) ||
        (incoming == RELAY_TERMINAL_DIRECTION_UP &&
            outgoing == RELAY_TERMINAL_DIRECTION_RIGHT)) {
        return 0x256DU;
    }
    if ((incoming == RELAY_TERMINAL_DIRECTION_LEFT &&
            outgoing == RELAY_TERMINAL_DIRECTION_UP) ||
        (incoming == RELAY_TERMINAL_DIRECTION_DOWN &&
            outgoing == RELAY_TERMINAL_DIRECTION_RIGHT)) {
        return 0x2570U;
    }
    return 0;
}

/** Create a minimal directional path from an output anchor to an input anchor. */
static void relay_terminal_wire_path_create(int source_x, int source_y,
    int source_top, int source_bottom, int destination_x, int destination_y,
    int destination_top, int destination_bottom, Relay_TerminalWirePath *path)
{
    const int source_exit_x = source_x + 3;
    const int destination_exit_x = destination_x - 3;
    int channel_y;

    *path = (Relay_TerminalWirePath){0};
    path->points[path->count++] = (Relay_TerminalPoint){source_x, source_y};
    if (source_exit_x < destination_exit_x) {
        const int middle_x = source_exit_x +
            (destination_exit_x - source_exit_x) / 2;

        path->points[path->count++] = (Relay_TerminalPoint){middle_x, source_y};
        path->points[path->count++] = (Relay_TerminalPoint){middle_x,
            destination_y};
    } else {
        if (source_top > destination_bottom + 1) {
            channel_y = (source_top + destination_bottom) / 2;
        } else if (destination_top > source_bottom + 1) {
            channel_y = (destination_top + source_bottom) / 2;
        } else {
            channel_y = (source_bottom > destination_bottom ? source_bottom :
                destination_bottom) + 2;
        }
        path->points[path->count++] = (Relay_TerminalPoint){source_exit_x,
            source_y};
        path->points[path->count++] = (Relay_TerminalPoint){source_exit_x,
            channel_y};
        path->points[path->count++] = (Relay_TerminalPoint){destination_exit_x,
            channel_y};
        path->points[path->count++] = (Relay_TerminalPoint){destination_exit_x,
            destination_y};
    }
    path->points[path->count++] = (Relay_TerminalPoint){destination_x,
        destination_y};
}

/** Return a positive modulo result for grid coordinates and negative pans. */
static int relay_terminal_positive_modulo(int64_t value, int divisor)
{
    const int64_t remainder = value % divisor;

    return (int)(remainder < 0 ? remainder + divisor : remainder);
}

/** Add a viewport delta without overflowing a signed world coordinate. */
static int64_t relay_terminal_add_delta(int64_t coordinate, int64_t delta)
{
    if (delta > 0 && coordinate > INT64_MAX - delta) {
        return INT64_MAX;
    }
    if (delta < 0 && coordinate < INT64_MIN - delta) {
        return INT64_MIN;
    }
    return coordinate + delta;
}

/** Resolve one graph connection into an orthogonal terminal-space wire route. */
static bool relay_terminal_wire_route(const Relay_Game *game,
    const Relay_Terminal *terminal, const Relay_NodeConnection *connection,
    Relay_TerminalWireRoute *route)
{
    const Relay_Node *source;
    const Relay_Node *destination;
    Relay_NodeRenderCard source_card;
    Relay_NodeRenderCard destination_card;
    const int source_x = 2;
    const int source_y = 3;

    if (game == NULL || terminal == NULL || connection == NULL || route == NULL) {
        return false;
    }
    source = relay_node_world_find_const(&game->nodes, connection->source_node_id);
    destination = relay_node_world_find_const(&game->nodes,
        connection->destination_node_id);
    source_card = relay_node_renderer_card(source);
    destination_card = relay_node_renderer_card(destination);
    if (source == NULL || destination == NULL || source_card.definition == NULL ||
        destination_card.definition == NULL ||
        connection->source_port_index >= source_card.definition->output_count ||
        connection->destination_port_index >= destination_card.definition->input_count) {
        return false;
    }
    route->source_x = source_x + (int)source->grid_x +
        (int)terminal->grid_offset_x + source_card.width - 1;
    route->source_y = source_y + (int)source->grid_y +
        (int)terminal->grid_offset_y + 3 + (int)connection->source_port_index;
    route->destination_x = source_x + (int)destination->grid_x +
        (int)terminal->grid_offset_x;
    route->destination_y = source_y + (int)destination->grid_y +
        (int)terminal->grid_offset_y + 3 + (int)connection->destination_port_index;
    route->source_top = source_y + (int)source->grid_y +
        (int)terminal->grid_offset_y;
    route->source_bottom = route->source_top + source_card.height - 1;
    route->destination_top = source_y + (int)destination->grid_y +
        (int)terminal->grid_offset_y;
    route->destination_bottom = route->destination_top + destination_card.height - 1;
    if (route->source_top > route->destination_bottom + 1) {
        route->bend_y = (route->source_top + route->destination_bottom) / 2;
    } else if (route->destination_top > route->source_bottom + 1) {
        route->bend_y = (route->destination_top + route->source_bottom) / 2;
    } else {
        route->bend_y = (route->source_bottom > route->destination_bottom ?
            route->source_bottom : route->destination_bottom) + 2;
    }
    return true;
}

/** Resolve an output port's terminal anchor for a persistent or live wire. */
static bool relay_terminal_output_anchor(const Relay_Game *game,
    const Relay_Terminal *terminal, Relay_NodeId node_id, size_t port_index,
    int *x, int *y)
{
    const Relay_Node *node;
    Relay_NodeRenderCard card;

    if (game == NULL || terminal == NULL || x == NULL || y == NULL) {
        return false;
    }
    node = relay_node_world_find_const(&game->nodes, node_id);
    card = relay_node_renderer_card(node);
    if (node == NULL || card.definition == NULL ||
        port_index >= card.definition->output_count) {
        return false;
    }
    *x = 2 + (int)node->grid_x + (int)terminal->grid_offset_x +
        card.width - 1;
    *y = 3 + (int)node->grid_y + (int)terminal->grid_offset_y + 3 +
        (int)port_index;
    return true;
}

/** Calculate the divider position for Relay's playfield and side panel. */
static bool relay_terminal_split(int width, int height, int *divider_x)
{
    int panel_width;
    int game_width;

    if (divider_x == NULL || height < RELAY_TERMINAL_MIN_HEIGHT) {
        return false;
    }
    panel_width = width / 3;
    if (panel_width < RELAY_TERMINAL_MIN_PANEL_WIDTH) {
        panel_width = RELAY_TERMINAL_MIN_PANEL_WIDTH;
    } else if (panel_width > RELAY_TERMINAL_MAX_PANEL_WIDTH) {
        panel_width = RELAY_TERMINAL_MAX_PANEL_WIDTH;
    }
    game_width = width - panel_width - 1;
    if (game_width < RELAY_TERMINAL_MIN_GAME_WIDTH) {
        return false;
    }

    *divider_x = game_width;
    return true;
}

#if RELAY_PLATFORM_WINDOWS
#include <stdio.h>
#include <string.h>

/** Write a string to the active Windows console. */
static bool relay_terminal_write(HANDLE output, const char *text)
{
    DWORD written;
    const DWORD length = (DWORD)strlen(text);

    return WriteFile(output, text, length, &written, NULL) != 0 &&
        written == length;
}

/** Write text at a zero-based terminal coordinate using ANSI cursor movement. */
static bool relay_terminal_write_at(HANDLE output, int x, int y,
    const char *text)
{
    char sequence[256];
    const int length = snprintf(sequence, sizeof(sequence), "\x1b[%d;%dH%s",
        y + 1, x + 1, text);

    return length > 0 && (size_t)length < sizeof(sequence) &&
        relay_terminal_write(output, sequence);
}

/** Draw the pan-aware dot grid behind Relay's graph workspace. */
static bool relay_terminal_draw_grid(HANDLE output, int divider_x, int height,
    const Relay_Terminal *terminal)
{
    int x;
    int y;

    for (y = 2 + relay_terminal_positive_modulo(
        terminal->grid_offset_y - 2, 2); y < height; y += 2) {
        for (x = relay_terminal_positive_modulo(terminal->grid_offset_x, 4);
            x < divider_x; x += 4) {
            if (!relay_terminal_write_at(output, x, y, "\x1b[2m.\x1b[0m")) {
                return false;
            }
        }
    }
    return true;
}

/** Draw one clipped Windows-console wire segment. */
static bool relay_terminal_draw_wire_cell(HANDLE output, int x, int y,
    int divider_x, int height, const char *glyph)
{
    return x >= 0 && x < divider_x && y >= 2 && y < height ?
        relay_terminal_write_at(output, x, y, glyph) : true;
}

/** Return an UTF-8 glyph string for a shared terminal wire code point. */
static const char *relay_terminal_wire_glyph(uint32_t glyph)
{
    switch (glyph) {
    case 0x2500U: return "─";
    case 0x2502U: return "│";
    case 0x256DU: return "╭";
    case 0x256EU: return "╮";
    case 0x256FU: return "╯";
    default: return "╰";
    }
}

/** Render a shared directional wire path in the Windows terminal backend. */
static bool relay_terminal_draw_wire_path(HANDLE output,
    const Relay_TerminalWirePath *path, int divider_x, int height)
{
    size_t index;

    for (index = 1; index < path->count; index++) {
        const Relay_TerminalPoint from = path->points[index - 1];
        const Relay_TerminalPoint to = path->points[index];
        const Relay_TerminalDirection direction =
            relay_terminal_direction_between(from, to);
        const int step = direction == RELAY_TERMINAL_DIRECTION_LEFT ||
            direction == RELAY_TERMINAL_DIRECTION_UP ? -1 : 1;
        int coordinate = direction == RELAY_TERMINAL_DIRECTION_LEFT ||
            direction == RELAY_TERMINAL_DIRECTION_RIGHT ? from.x : from.y;
        const int end = direction == RELAY_TERMINAL_DIRECTION_LEFT ||
            direction == RELAY_TERMINAL_DIRECTION_RIGHT ? to.x : to.y;

        while (true) {
            const int x = direction == RELAY_TERMINAL_DIRECTION_LEFT ||
                direction == RELAY_TERMINAL_DIRECTION_RIGHT ? coordinate : from.x;
            const int y = direction == RELAY_TERMINAL_DIRECTION_LEFT ||
                direction == RELAY_TERMINAL_DIRECTION_RIGHT ? from.y : coordinate;

            if (!relay_terminal_draw_wire_cell(output, x, y, divider_x, height,
                    relay_terminal_wire_glyph(direction == RELAY_TERMINAL_DIRECTION_LEFT ||
                    direction == RELAY_TERMINAL_DIRECTION_RIGHT ? 0x2500U : 0x2502U))) {
                return false;
            }
            if (coordinate == end) {
                break;
            }
            coordinate += step;
        }
    }
    for (index = 1; index + 1 < path->count; index++) {
        const uint32_t glyph = relay_terminal_corner_glyph(
            relay_terminal_direction_between(path->points[index - 1],
                path->points[index]), relay_terminal_direction_between(
                path->points[index], path->points[index + 1]));

        if (glyph != 0 && !relay_terminal_draw_wire_cell(output,
                path->points[index].x, path->points[index].y, divider_x, height,
                relay_terminal_wire_glyph(glyph))) {
            return false;
        }
    }
    return true;
}

/** Draw persistent and live wires through the same routing implementation. */
static bool relay_terminal_draw_wires(HANDLE output, const Relay_Game *game,
    const Relay_Terminal *terminal, int divider_x, int height)
{
    size_t index;

    for (index = 0; index < game->nodes.connection_count; index++) {
        Relay_TerminalWireRoute route;
        Relay_TerminalWirePath path;

        if (relay_terminal_wire_route(game, terminal,
                &game->nodes.connections[index], &route)) {
            relay_terminal_wire_path_create(route.source_x, route.source_y,
                route.source_top, route.source_bottom, route.destination_x,
                route.destination_y, route.destination_top, route.destination_bottom,
                &path);
            if (!relay_terminal_draw_wire_path(output, &path, divider_x, height)) {
                return false;
            }
        }
    }
    if (terminal->wiring) {
        const Relay_Node *source = relay_node_world_find_const(&game->nodes,
            terminal->wiring_source_node_id);
        const Relay_NodeRenderCard card = relay_node_renderer_card(source);
        Relay_TerminalWirePath path;
        int source_x;
        int source_y;
        const int source_top = source == NULL ? 0 : 3 + (int)source->grid_y +
            (int)terminal->grid_offset_y;

        if (source != NULL && relay_terminal_output_anchor(game, terminal,
                source->id, terminal->wiring_source_port_index, &source_x,
                &source_y)) {
            relay_terminal_wire_path_create(source_x, source_y, source_top,
                source_top + card.height - 1, terminal->wiring_mouse_x,
                terminal->wiring_mouse_y, terminal->wiring_mouse_y,
                terminal->wiring_mouse_y, &path);
            if (!relay_terminal_draw_wire_path(output, &path, divider_x, height)) {
                return false;
            }
        }
    }
    return true;
}

/** Draw one source node as a graph card with input and output ports. */
static bool relay_terminal_draw_node_graph(HANDLE output, const Relay_Node *node,
    const Relay_Terminal *terminal, int divider_x, int height)
{
    const Relay_NodeRenderCard card = relay_node_renderer_card(node);
    const int x = 2 + node->grid_x + terminal->grid_offset_x;
    const int y = 3 + node->grid_y + terminal->grid_offset_y;
    char line[64];
    char content[32];
    size_t row;

    if (card.definition == NULL || x < 0 || y < 2 ||
        x + card.width - 1 >= divider_x || y + card.height - 1 >= height) {
        return true;
    }
    (void)snprintf(line, sizeof(line), "│ ◆ %-20.20s │",
        card.definition->display_name);
    if (!relay_terminal_write_at(output, x, y, "╭────────────────────────╮") ||
        !relay_terminal_write_at(output, x, y + 1, line) ||
        !relay_terminal_write_at(output, x, y + 2, "├────────────────────────┤")) {
        return false;
    }
    for (row = 0; row < (size_t)card.height - 5; row++) {
        const Relay_NodePortDefinition *input = row < card.definition->input_count ?
            &card.definition->inputs[row] : NULL;
        const Relay_NodePortDefinition *output = row < card.definition->output_count ?
            &card.definition->outputs[row] : NULL;

        (void)snprintf(content, sizeof(content), "%-10.10s%12.12s",
            input == NULL ? "" : input->display_name,
            output == NULL ? "" : output->display_name);
        (void)snprintf(line, sizeof(line), "%s %-22.22s %s",
            input == NULL ? "│" : "●", content,
            output == NULL ? "│" : "●");
        if (!relay_terminal_write_at(output, x, y + 3 + (int)row, line)) {
            return false;
        }
    }
    if (node->definition_id == RELAY_NODE_DEFINITION_COAL_MINER) {
        const int filled = (int)(node->progress * 10 / 16);

        (void)snprintf(content, sizeof(content), "F%lld [%.*s%.*s] %2lld/16",
            (long long)node->fuel_coal, filled, "##########", 10 - filled,
            "..........", (long long)node->progress);
    } else {
        (void)snprintf(content, sizeof(content), "period: %lld",
            (long long)node->clock_period);
    }
    (void)snprintf(line, sizeof(line), "│ %-22.22s │", content);
    if (!relay_terminal_write_at(output, x, y + card.height - 2, line)) {
        return false;
    }
    return relay_terminal_write_at(output, x, y + card.height - 1,
        "╰────────────────────────╯");
}

/** Draw one compact source label in the zoomed-out workspace map. */
static bool relay_terminal_draw_node_map(HANDLE output, const Relay_Node *node,
    const Relay_Terminal *terminal, int divider_x, int height)
{
    const Relay_NodeRenderCard card = relay_node_renderer_card(node);
    const int64_t world_x = node->grid_x + terminal->grid_offset_x;
    const int64_t world_y = node->grid_y + terminal->grid_offset_y;
    const int x = (int)(2 + world_x / 4);
    const int y = (int)(3 + world_y / 4);
    char line[64];

    if (card.definition == NULL || x < 0 || y < 2 || x >= divider_x ||
        y >= height) {
        return true;
    }
    (void)snprintf(line, sizeof(line), "◆ %s", card.definition->display_name);
    return relay_terminal_write_at(output, x, y, line);
}

/** Render Relay's tabbed graph workspace and right-side control panel. */
static bool relay_terminal_draw_split(HANDLE output, int width, int height,
    const Relay_Game *game, const Relay_Terminal *terminal)
{
    int divider_x;
    int y;
    char line[64];
    size_t index;

    if (!relay_terminal_split(width, height, &divider_x)) {
        return relay_terminal_write_at(output, 0, 0,
            "Terminal is too small for the Relay layout.");
    }
    for (y = 0; y < height; y++) {
        if (!relay_terminal_write_at(output, divider_x, y, "\x1b[90m|\x1b[0m")) {
            return false;
        }
    }
    if (!relay_terminal_write_at(output, 1, 0,
            game->workspace_mode == RELAY_GAME_WORKSPACE_MAP ?
                "\x1b[1;36m[ Relay ]  MAP\x1b[0m" :
                "\x1b[1;36m[ Relay ]\x1b[0m") ||
        !relay_terminal_write_at(output, divider_x + 2, 1,
            "\x1b[1;36m[ SHOP ]\x1b[0m")) {
        return false;
    }
    if (!relay_terminal_draw_grid(output, divider_x, height, terminal)) {
        return false;
    }
    if (game->workspace_mode == RELAY_GAME_WORKSPACE_GRAPH &&
        !relay_terminal_draw_wires(output, game, terminal, divider_x, height)) {
        return false;
    }
    (void)snprintf(line, sizeof(line), "◈ %llu", (unsigned long long)game->currency);
    if (!relay_terminal_write_at(output, divider_x + 2, 3, line)) {
        return false;
    }
    (void)snprintf(line, sizeof(line), "wires: %zu", game->nodes.connection_count);
    if (!relay_terminal_write_at(output, divider_x + 2, 4, line)) {
        return false;
    }
    for (index = 0; index < relay_game_shop_offer_count() && 6 + (int)index < height - 2;
        index++) {
        const Relay_ShopOffer *offer = relay_game_shop_offer_at(index);
        const Relay_NodeDefinition *definition = relay_game_shop_offer_definition(offer);

        (void)snprintf(line, sizeof(line), "%c %-7s %3llu", index == game->selected_offer ?
            '>' : ' ', definition->display_name, (unsigned long long)offer->price);
        if (!relay_terminal_write_at(output, divider_x + 2, 6 + (int)index, line)) {
            return false;
        }
    }
    for (index = 0; index < game->nodes.count; index++) {
        const Relay_Node *node = &game->nodes.nodes[index];
        if ((game->workspace_mode == RELAY_GAME_WORKSPACE_MAP &&
                !relay_terminal_draw_node_map(output, node, terminal, divider_x,
                    height)) ||
            (game->workspace_mode == RELAY_GAME_WORKSPACE_GRAPH &&
                !relay_terminal_draw_node_graph(output, node, terminal, divider_x,
                    height))) {
            return false;
        }
    }
    return relay_terminal_write_at(output, divider_x + 2, height - 3,
        "drag output to input") && relay_terminal_write_at(output,
            divider_x + 2, height - 2, "[ / ] clock rate");
}

bool relay_terminal_init(Relay_Terminal *terminal)
{
    if (terminal == NULL) {
        return false;
    }

    terminal->input = GetStdHandle(STD_INPUT_HANDLE);
    terminal->output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (terminal->input == INVALID_HANDLE_VALUE ||
        terminal->output == INVALID_HANDLE_VALUE ||
        !GetConsoleMode(terminal->input, &terminal->input_mode) ||
        !GetConsoleMode(terminal->output, &terminal->output_mode) ||
        !SetConsoleMode(terminal->output,
            terminal->output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) ||
        !SetConsoleMode(terminal->input,
            terminal->input_mode | ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT |
                ENABLE_MOUSE_INPUT) ||
        !relay_terminal_write(terminal->output, "\x1b[?1049h\x1b[2J\x1b[H")) {
        return false;
    }

    terminal->initialized = true;
    return true;
}

/** Draw the centered exit-confirmation dialog above the current workspace. */
static bool relay_terminal_draw_exit_overlay(HANDLE output, int width, int height)
{
    const int x = (width - 28) / 2;
    const int y = (height - 5) / 2;

    return x >= 0 && y >= 0 && y + 4 < height &&
        relay_terminal_write_at(output, x, y, "╭──────────────────────────╮") &&
        relay_terminal_write_at(output, x, y + 1, "│       Exit Relay?        │") &&
        relay_terminal_write_at(output, x, y + 2, "│ Enter: confirm           │") &&
        relay_terminal_write_at(output, x, y + 3, "│ Esc: cancel              │") &&
        relay_terminal_write_at(output, x, y + 4, "╰──────────────────────────╯");
}

bool relay_terminal_draw(const Relay_Terminal *terminal, const Relay_Game *game,
    Relay_TerminalOverlay overlay)
{
    CONSOLE_SCREEN_BUFFER_INFO information;
    int width;
    int height;

    if (terminal == NULL || game == NULL || !terminal->initialized ||
        !GetConsoleScreenBufferInfo(terminal->output, &information) ||
        !relay_terminal_write(terminal->output, "\x1b[2J\x1b[H")) {
        return false;
    }
    width = information.srWindow.Right - information.srWindow.Left + 1;
    height = information.srWindow.Bottom - information.srWindow.Top + 1;
    if (!relay_terminal_draw_split(terminal->output, width, height, game,
            terminal)) {
        return false;
    }
    return overlay != RELAY_TERMINAL_OVERLAY_EXIT_CONFIRM ||
        relay_terminal_draw_exit_overlay(terminal->output, width, height);
}

void relay_terminal_focus_node(Relay_Terminal *terminal, const Relay_Game *game,
    uint64_t node_id)
{
    CONSOLE_SCREEN_BUFFER_INFO information;
    const Relay_Node *node;
    Relay_NodeRenderCard card;
    int divider_x;
    int width;
    int height;
    int scale;

    if (terminal == NULL || game == NULL ||
        !GetConsoleScreenBufferInfo(terminal->output, &information)) {
        return;
    }
    width = information.srWindow.Right - information.srWindow.Left + 1;
    height = information.srWindow.Bottom - information.srWindow.Top + 1;
    if (!relay_terminal_split(width, height, &divider_x)) {
        return;
    }
    node = relay_node_world_find_const(&game->nodes, node_id);
    if (node == NULL) {
        return;
    }
    card = relay_node_renderer_card(node);
    scale = game->workspace_mode == RELAY_GAME_WORKSPACE_MAP ? 4 : 1;
    terminal->grid_offset_x = (int64_t)(divider_x / 2 - 2) * scale -
        node->grid_x - (int64_t)(card.width / 2) * scale;
    terminal->grid_offset_y = (int64_t)(height / 2 - 3) * scale -
        node->grid_y - (int64_t)(card.height / 2) * scale;
}

/** Return the topmost graph node card at a terminal mouse coordinate. */
static Relay_NodeId relay_terminal_node_at(const Relay_Game *game,
    const Relay_Terminal *terminal, int mouse_x, int mouse_y)
{
    CONSOLE_SCREEN_BUFFER_INFO information;
    int divider_x;
    int width;
    int height;
    size_t index;

    if (game->workspace_mode != RELAY_GAME_WORKSPACE_GRAPH ||
        !GetConsoleScreenBufferInfo(terminal->output, &information)) {
        return 0;
    }
    width = information.srWindow.Right - information.srWindow.Left + 1;
    height = information.srWindow.Bottom - information.srWindow.Top + 1;
    if (!relay_terminal_split(width, height, &divider_x) || mouse_x < 0 ||
        mouse_x >= divider_x || mouse_y < 2) {
        return 0;
    }
    for (index = game->nodes.count; index > 0; index--) {
        const Relay_Node *node = &game->nodes.nodes[index - 1];
        const Relay_NodeRenderCard card = relay_node_renderer_card(node);
        const int x = 2 + node->grid_x + terminal->grid_offset_x;
        const int y = 3 + node->grid_y + terminal->grid_offset_y;

        if (mouse_x > x && mouse_x < x + card.width - 1 &&
            mouse_y == y + 1) {
            return node->id;
        }
    }
    return 0;
}

/** Return the graph port under a Windows console mouse coordinate. */
static Relay_TerminalPortHit relay_terminal_port_at(const Relay_Game *game,
    const Relay_Terminal *terminal, int mouse_x, int mouse_y)
{
    CONSOLE_SCREEN_BUFFER_INFO information;
    int divider_x;
    int width;
    int height;
    size_t index;

    if (game->workspace_mode != RELAY_GAME_WORKSPACE_GRAPH ||
        !GetConsoleScreenBufferInfo(terminal->output, &information)) {
        return (Relay_TerminalPortHit){0};
    }
    width = information.srWindow.Right - information.srWindow.Left + 1;
    height = information.srWindow.Bottom - information.srWindow.Top + 1;
    if (!relay_terminal_split(width, height, &divider_x)) {
        return (Relay_TerminalPortHit){0};
    }
    for (index = game->nodes.count; index > 0; index--) {
        const Relay_Node *node = &game->nodes.nodes[index - 1];
        const Relay_NodeRenderCard card = relay_node_renderer_card(node);
        const int x = 2 + node->grid_x + terminal->grid_offset_x;
        const int y = 3 + node->grid_y + terminal->grid_offset_y;
        const int row = mouse_y - (y + 3);

        if (card.definition != NULL && row >= 0 && row < (int)card.definition->input_count &&
            mouse_x == x) {
            return (Relay_TerminalPortHit){node->id, (size_t)row, false};
        }
        if (card.definition != NULL && row >= 0 && row < (int)card.definition->output_count &&
            mouse_x == x + card.width - 1) {
            return (Relay_TerminalPortHit){node->id, (size_t)row, true};
        }
    }
    return (Relay_TerminalPortHit){0};
}

bool relay_terminal_poll(Relay_Terminal *terminal, const Relay_Game *game,
    Relay_TerminalEvent *event)
{
    INPUT_RECORD record;
    DWORD count;

    if (terminal == NULL || game == NULL || event == NULL || !terminal->initialized) {
        return false;
    }

    *event = (Relay_TerminalEvent){0};
    if (WaitForSingleObject(terminal->input, 16) == WAIT_TIMEOUT) {
        return true;
    }
    if (!ReadConsoleInputA(terminal->input, &record, 1, &count)) {
        return false;
    }

    if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
        event->type = RELAY_TERMINAL_EVENT_RESIZED;
    } else if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) {
        event->type = RELAY_TERMINAL_EVENT_INPUT;
        event->character = (uint32_t)record.Event.KeyEvent.uChar.UnicodeChar;
        if (record.Event.KeyEvent.wVirtualKeyCode == VK_UP) {
            event->key = RELAY_TERMINAL_KEY_UP;
        } else if (record.Event.KeyEvent.wVirtualKeyCode == VK_DOWN) {
            event->key = RELAY_TERMINAL_KEY_DOWN;
        } else if (record.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) {
            event->key = RELAY_TERMINAL_KEY_CONFIRM;
        } else if (record.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE) {
            event->key = RELAY_TERMINAL_KEY_ESCAPE;
        }
    } else if (record.EventType == MOUSE_EVENT) {
        const MOUSE_EVENT_RECORD *mouse = &record.Event.MouseEvent;
        const bool left_down = (mouse->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;

        event->type = RELAY_TERMINAL_EVENT_MOUSE;
        event->mouse_x = mouse->dwMousePosition.X;
        event->mouse_y = mouse->dwMousePosition.Y;
        if (left_down && !terminal->dragging_grid && !terminal->dragging_node &&
            !terminal->wiring) {
            const Relay_TerminalPortHit hit = relay_terminal_port_at(game, terminal,
                event->mouse_x, event->mouse_y);

            if (hit.node_id != 0 && hit.is_output) {
                terminal->wiring = true;
                terminal->wiring_source_node_id = hit.node_id;
                terminal->wiring_source_port_index = hit.port_index;
                terminal->wiring_mouse_x = event->mouse_x;
                terminal->wiring_mouse_y = event->mouse_y;
                return true;
            }
            terminal->dragged_node_id = relay_terminal_node_at(game, terminal,
                event->mouse_x, event->mouse_y);
            terminal->dragging_node = terminal->dragged_node_id != 0;
            terminal->dragging_grid = !terminal->dragging_node;
            terminal->drag_last_x = event->mouse_x;
            terminal->drag_last_y = event->mouse_y;
        } else if (left_down && terminal->wiring) {
            terminal->wiring_mouse_x = event->mouse_x;
            terminal->wiring_mouse_y = event->mouse_y;
        } else if (left_down) {
            const int delta_x = event->mouse_x - terminal->drag_last_x;
            const int delta_y = event->mouse_y - terminal->drag_last_y;

            if (terminal->dragging_node) {
                event->dragged_node_id = terminal->dragged_node_id;
                event->grid_delta_x = delta_x;
                event->grid_delta_y = delta_y;
            } else if (terminal->dragging_grid) {
                const int scale = game->workspace_mode ==
                    RELAY_GAME_WORKSPACE_MAP ? 4 : 1;

                terminal->grid_offset_x = relay_terminal_add_delta(
                    terminal->grid_offset_x, (int64_t)delta_x * scale);
                terminal->grid_offset_y = relay_terminal_add_delta(
                    terminal->grid_offset_y, (int64_t)delta_y * scale);
            }
            terminal->drag_last_x = event->mouse_x;
            terminal->drag_last_y = event->mouse_y;
        } else {
            if (terminal->wiring) {
                const Relay_TerminalPortHit hit = relay_terminal_port_at(game, terminal,
                    event->mouse_x, event->mouse_y);

                if (hit.node_id != 0 && !hit.is_output) {
                    event->connection_source_node_id = terminal->wiring_source_node_id;
                    event->connection_source_port_index = terminal->wiring_source_port_index;
                    event->connection_destination_node_id = hit.node_id;
                    event->connection_destination_port_index = hit.port_index;
                }
            }
            terminal->dragging_grid = false;
            terminal->dragging_node = false;
            terminal->wiring = false;
            terminal->dragged_node_id = 0;
        }
    }
    return true;
}

void relay_terminal_shutdown(Relay_Terminal *terminal)
{
    if (terminal == NULL || !terminal->initialized) {
        return;
    }

    (void)relay_terminal_write(terminal->output, "\x1b[?1049l");
    (void)SetConsoleMode(terminal->input, terminal->input_mode);
    (void)SetConsoleMode(terminal->output, terminal->output_mode);
    terminal->initialized = false;
}
#else
#include <errno.h>
#include <string.h>

#include <termbox2.h>

/** Draw text when its origin belongs to the active terminal bounds. */
static void relay_terminal_draw_text(int x, int y, uintattr_t foreground,
    const char *text)
{
    const int height = tb_height();

    if (x < 0 || y < 0 || y >= height) {
        return;
    }
    (void)tb_print(x, y, foreground, TB_DEFAULT, text);
}

/** Convert a node-renderer color token into a Termbox foreground attribute. */
static uintattr_t relay_terminal_node_color(unsigned int color)
{
    switch (color) {
    case 2:
        return TB_WHITE | TB_BOLD;
    case 3:
        return TB_YELLOW | TB_BOLD;
    case 4:
        return TB_WHITE;
    case 5:
        return TB_BLACK | TB_BOLD;
    default:
        return TB_RED;
    }
}

/** Draw the pan-aware dot grid behind Relay's graph workspace. */
static void relay_terminal_draw_grid(int divider_x, int height,
    const Relay_Terminal *terminal)
{
    int x;
    int y;

    for (y = 2; y < height; y++) {
        if (relay_terminal_positive_modulo(y - terminal->grid_offset_y, 2) != 0) {
            continue;
        }
        for (x = 0; x < divider_x; x++) {
            if (relay_terminal_positive_modulo(x - terminal->grid_offset_x, 4) == 0) {
                (void)tb_set_cell(x, y, '.', TB_WHITE | TB_DIM, TB_DEFAULT);
            }
        }
    }
}

/** Draw one clipped Termbox wire cell. */
static void relay_terminal_draw_wire_cell(int x, int y, int divider_x,
    int height, uint32_t glyph)
{
    if (x >= 0 && x < divider_x && y >= 2 && y < height) {
        (void)tb_set_cell(x, y, glyph, TB_CYAN | TB_BOLD, TB_DEFAULT);
    }
}

/** Render a directional wire path using the shared route and corner contract. */
static void relay_terminal_draw_wire_path(const Relay_TerminalWirePath *path,
    int divider_x, int height)
{
    size_t index;

    for (index = 1; index < path->count; index++) {
        const Relay_TerminalPoint from = path->points[index - 1];
        const Relay_TerminalPoint to = path->points[index];
        const Relay_TerminalDirection direction =
            relay_terminal_direction_between(from, to);
        const int step = direction == RELAY_TERMINAL_DIRECTION_LEFT ||
            direction == RELAY_TERMINAL_DIRECTION_UP ? -1 : 1;
        int coordinate = direction == RELAY_TERMINAL_DIRECTION_LEFT ||
            direction == RELAY_TERMINAL_DIRECTION_RIGHT ? from.x : from.y;
        const int end = direction == RELAY_TERMINAL_DIRECTION_LEFT ||
            direction == RELAY_TERMINAL_DIRECTION_RIGHT ? to.x : to.y;

        while (true) {
            relay_terminal_draw_wire_cell(direction == RELAY_TERMINAL_DIRECTION_LEFT ||
                direction == RELAY_TERMINAL_DIRECTION_RIGHT ? coordinate : from.x,
                direction == RELAY_TERMINAL_DIRECTION_LEFT ||
                direction == RELAY_TERMINAL_DIRECTION_RIGHT ? from.y : coordinate,
                divider_x, height, direction == RELAY_TERMINAL_DIRECTION_LEFT ||
                direction == RELAY_TERMINAL_DIRECTION_RIGHT ? 0x2500U : 0x2502U);
            if (coordinate == end) {
                break;
            }
            coordinate += step;
        }
    }
    for (index = 1; index + 1 < path->count; index++) {
        const uint32_t glyph = relay_terminal_corner_glyph(
            relay_terminal_direction_between(path->points[index - 1],
                path->points[index]), relay_terminal_direction_between(
                path->points[index], path->points[index + 1]));

        if (glyph != 0) {
            relay_terminal_draw_wire_cell(path->points[index].x,
                path->points[index].y, divider_x, height, glyph);
        }
    }
}

/** Draw persistent and live wires through the shared route builder. */
static void relay_terminal_draw_shared_wires(const Relay_Game *game,
    const Relay_Terminal *terminal, int divider_x, int height)
{
    size_t index;

    for (index = 0; index < game->nodes.connection_count; index++) {
        Relay_TerminalWireRoute route;
        Relay_TerminalWirePath path;

        if (relay_terminal_wire_route(game, terminal,
                &game->nodes.connections[index], &route)) {
            relay_terminal_wire_path_create(route.source_x, route.source_y,
                route.source_top, route.source_bottom, route.destination_x,
                route.destination_y, route.destination_top, route.destination_bottom,
                &path);
            relay_terminal_draw_wire_path(&path, divider_x, height);
        }
    }
    if (terminal->wiring) {
        const Relay_Node *source = relay_node_world_find_const(&game->nodes,
            terminal->wiring_source_node_id);
        const Relay_NodeRenderCard card = relay_node_renderer_card(source);
        Relay_TerminalWirePath path;
        int source_x;
        int source_y;
        int source_top;

        if (source != NULL && relay_terminal_output_anchor(game, terminal,
                source->id, terminal->wiring_source_port_index, &source_x,
                &source_y)) {
            source_top = 3 + (int)source->grid_y + (int)terminal->grid_offset_y;
            relay_terminal_wire_path_create(source_x, source_y, source_top,
                source_top + card.height - 1, terminal->wiring_mouse_x,
                terminal->wiring_mouse_y, terminal->wiring_mouse_y,
                terminal->wiring_mouse_y, &path);
            relay_terminal_draw_wire_path(&path, divider_x, height);
        }
    }
}

/** Draw all graph wires before cards cover their anchor crossings. */
static void relay_terminal_draw_wires(const Relay_Game *game,
    const Relay_Terminal *terminal, int divider_x, int height)
{
    relay_terminal_draw_shared_wires(game, terminal, divider_x, height);
    return;

    size_t index;

    for (index = 0; index < game->nodes.connection_count; index++) {
        const Relay_NodeConnection *connection = &game->nodes.connections[index];
        Relay_TerminalWireRoute route;
        int x;
        int y;
        int horizontal_step;
        int source_exit_x;
        int destination_exit_x;
        int middle_x;

        if (!relay_terminal_wire_route(game, terminal, connection, &route)) {
            continue;
        }
        source_exit_x = route.source_x + 3;
        destination_exit_x = route.destination_x - 3;
        if (source_exit_x < destination_exit_x) {
            middle_x = source_exit_x + (destination_exit_x - source_exit_x) / 2;
            for (x = route.source_x + 1; x <= middle_x; x++) {
                relay_terminal_draw_wire_cell(x, route.source_y, divider_x, height,
                    0x2500U);
            }
            for (y = route.source_y < route.destination_y ? route.source_y :
                    route.destination_y; y <= (route.source_y > route.destination_y ?
                    route.source_y : route.destination_y); y++) {
                relay_terminal_draw_wire_cell(middle_x, y, divider_x, height,
                    0x2502U);
            }
            for (x = middle_x; x <= route.destination_x - 1; x++) {
                relay_terminal_draw_wire_cell(x, route.destination_y, divider_x,
                    height, 0x2500U);
            }
            if (route.source_y < route.destination_y) {
                relay_terminal_draw_wire_cell(middle_x, route.source_y, divider_x,
                    height, 0x256EU);
                relay_terminal_draw_wire_cell(middle_x, route.destination_y,
                    divider_x, height, 0x2570U);
            } else if (route.source_y > route.destination_y) {
                relay_terminal_draw_wire_cell(middle_x, route.source_y, divider_x,
                    height, 0x256FU);
                relay_terminal_draw_wire_cell(middle_x, route.destination_y,
                    divider_x, height, 0x256DU);
            }
        } else {
            for (x = route.source_x + 1; x <= source_exit_x; x++) {
                relay_terminal_draw_wire_cell(x, route.source_y, divider_x, height,
                    0x2500U);
            }
            for (y = route.source_y < route.bend_y ? route.source_y : route.bend_y;
                    y <= (route.source_y > route.bend_y ? route.source_y :
                    route.bend_y); y++) {
                relay_terminal_draw_wire_cell(source_exit_x, y, divider_x, height,
                    0x2502U);
            }
            horizontal_step = source_exit_x <= destination_exit_x ? 1 : -1;
            for (x = source_exit_x;; x += horizontal_step) {
                relay_terminal_draw_wire_cell(x, route.bend_y, divider_x, height,
                    0x2500U);
                if (x == destination_exit_x) {
                    break;
                }
            }
            for (y = route.destination_y < route.bend_y ? route.destination_y :
                    route.bend_y; y <= (route.destination_y > route.bend_y ?
                    route.destination_y : route.bend_y); y++) {
                relay_terminal_draw_wire_cell(destination_exit_x, y, divider_x,
                    height, 0x2502U);
            }
            for (x = destination_exit_x; x <= route.destination_x - 1; x++) {
                relay_terminal_draw_wire_cell(x, route.destination_y, divider_x,
                    height, 0x2500U);
            }
            relay_terminal_draw_wire_cell(source_exit_x, route.bend_y, divider_x,
                height, route.bend_y > route.source_y ? 0x256EU : 0x256FU);
            relay_terminal_draw_wire_cell(destination_exit_x, route.bend_y,
                divider_x, height, route.destination_y > route.bend_y ?
                0x256EU : 0x256FU);
        }
    }
    if (terminal->wiring) {
        int source_x;
        int source_y;
        int bend_x;
        bool target_is_right;
        int x;
        int y;

        if (!relay_terminal_output_anchor(game, terminal,
                terminal->wiring_source_node_id,
                terminal->wiring_source_port_index, &source_x, &source_y)) {
            return;
        }
        bend_x = terminal->wiring_mouse_x > source_x + 3 ? source_x + 3 +
            (terminal->wiring_mouse_x - (source_x + 3)) / 2 : source_x + 6;
        target_is_right = terminal->wiring_mouse_x >= bend_x;
        for (x = source_x + 1; x <= bend_x; x++) {
            relay_terminal_draw_wire_cell(x, source_y, divider_x, height,
                0x2500U);
        }
        for (y = source_y < terminal->wiring_mouse_y ? source_y :
                terminal->wiring_mouse_y; y <= (source_y >
                terminal->wiring_mouse_y ? source_y : terminal->wiring_mouse_y); y++) {
            relay_terminal_draw_wire_cell(bend_x, y, divider_x, height,
                0x2502U);
        }
        for (x = bend_x < terminal->wiring_mouse_x ? bend_x :
                terminal->wiring_mouse_x; x <= (bend_x > terminal->wiring_mouse_x ?
                bend_x : terminal->wiring_mouse_x); x++) {
            relay_terminal_draw_wire_cell(x, terminal->wiring_mouse_y, divider_x,
                height, 0x2500U);
        }
        if (source_y != terminal->wiring_mouse_y) {
            relay_terminal_draw_wire_cell(bend_x, source_y, divider_x, height,
                source_y < terminal->wiring_mouse_y ? 0x256EU : 0x256FU);
            relay_terminal_draw_wire_cell(bend_x, terminal->wiring_mouse_y,
                divider_x, height, source_y < terminal->wiring_mouse_y ?
                (target_is_right ? 0x2570U : 0x256FU) :
                (target_is_right ? 0x256DU : 0x256EU));
        }
    }
}

/** Draw one source node as a graph card with input and output ports. */
static void relay_terminal_draw_node_graph(const Relay_Node *node,
    const Relay_Terminal *terminal, int divider_x, int height)
{
    const Relay_NodeRenderCard card = relay_node_renderer_card(node);
    const int x = 2 + node->grid_x + terminal->grid_offset_x;
    const int y = 3 + node->grid_y + terminal->grid_offset_y;
    char content[32];
    size_t row;

    if (card.definition == NULL || x < 0 || y < 2 ||
        x + card.width - 1 >= divider_x || y + card.height - 1 >= height) {
        return;
    }
    (void)tb_print(x, y, relay_terminal_node_color(card.visual.color),
        TB_DEFAULT, "╭────────────────────────╮");
    (void)tb_printf(x, y + 1, relay_terminal_node_color(card.visual.color),
        TB_DEFAULT, "│ ◆ %-20.20s │", card.definition->display_name);
    (void)tb_print(x, y + 2, relay_terminal_node_color(card.visual.color),
        TB_DEFAULT, "├────────────────────────┤");
    for (row = 0; row < (size_t)card.height - 5; row++) {
        const Relay_NodePortDefinition *input = row < card.definition->input_count ?
            &card.definition->inputs[row] : NULL;
        const Relay_NodePortDefinition *output = row < card.definition->output_count ?
            &card.definition->outputs[row] : NULL;

        (void)snprintf(content, sizeof(content), "%-10.10s%12.12s",
            input == NULL ? "" : input->display_name,
            output == NULL ? "" : output->display_name);
        (void)tb_printf(x, y + 3 + (int)row,
            relay_terminal_node_color(card.visual.color), TB_DEFAULT,
            "%s %-22.22s %s", input == NULL ? "│" : "●", content,
            output == NULL ? "│" : "●");
    }
    if (node->definition_id == RELAY_NODE_DEFINITION_COAL_MINER) {
        const int filled = (int)(node->progress * 10 / 16);

        (void)snprintf(content, sizeof(content), "F%lld [%.*s%.*s] %2lld/16",
            (long long)node->fuel_coal, filled, "##########", 10 - filled,
            "..........", (long long)node->progress);
    } else {
        (void)snprintf(content, sizeof(content), "period: %lld",
            (long long)node->clock_period);
    }
    (void)tb_printf(x, y + card.height - 2,
        relay_terminal_node_color(card.visual.color), TB_DEFAULT,
        "│ %-22.22s │", content);
    (void)tb_print(x, y + card.height - 1,
        relay_terminal_node_color(card.visual.color), TB_DEFAULT,
        "╰────────────────────────╯");
}

/** Draw one compact source label in the zoomed-out workspace map. */
static void relay_terminal_draw_node_map(const Relay_Node *node,
    const Relay_Terminal *terminal, int divider_x, int height)
{
    const Relay_NodeRenderCard card = relay_node_renderer_card(node);
    const int64_t world_x = node->grid_x + terminal->grid_offset_x;
    const int64_t world_y = node->grid_y + terminal->grid_offset_y;
    const int x = (int)(2 + world_x / 4);
    const int y = (int)(3 + world_y / 4);

    if (card.definition == NULL || x < 0 || y < 2 || x >= divider_x ||
        y >= height) {
        return;
    }
    (void)tb_printf(x, y, relay_terminal_node_color(card.visual.color),
        TB_DEFAULT, "◆ %s", card.definition->display_name);
}

/** Render Relay's playfield and right-side control panel. */
static void relay_terminal_draw_split(int width, int height,
    const Relay_Game *game, const Relay_Terminal *terminal)
{
    int divider_x;
    int y;
    size_t index;

    if (!relay_terminal_split(width, height, &divider_x)) {
        relay_terminal_draw_text(0, 0, TB_RED | TB_BOLD,
            "Terminal is too small for the Relay layout.");
        return;
    }
    for (y = 0; y < height; y++) {
        (void)tb_set_cell(divider_x, y, '|', TB_BLACK, TB_WHITE);
    }
    relay_terminal_draw_text(1, 0, TB_CYAN | TB_BOLD,
        game->workspace_mode == RELAY_GAME_WORKSPACE_MAP ? "[ Relay ]  MAP" :
        "[ Relay ]");
    relay_terminal_draw_grid(divider_x, height, terminal);
    if (game->workspace_mode == RELAY_GAME_WORKSPACE_GRAPH) {
        relay_terminal_draw_wires(game, terminal, divider_x, height);
    }
    relay_terminal_draw_text(divider_x + 2, 1, TB_CYAN | TB_BOLD, "[ SHOP ]");
    (void)tb_printf(divider_x + 2, 3, TB_YELLOW | TB_BOLD, TB_DEFAULT,
        "◈ %llu", (unsigned long long)game->currency);
    (void)tb_printf(divider_x + 2, 4, TB_WHITE, TB_DEFAULT, "wires: %zu",
        game->nodes.connection_count);
    for (index = 0; index < relay_game_shop_offer_count() &&
        6 + (int)index < height - 2; index++) {
        const Relay_ShopOffer *offer = relay_game_shop_offer_at(index);
        const Relay_NodeDefinition *definition = relay_game_shop_offer_definition(offer);

        (void)tb_printf(divider_x + 2, 6 + (int)index,
            index == game->selected_offer ? TB_GREEN | TB_BOLD : TB_WHITE,
            TB_DEFAULT, "%c %s %llu", index == game->selected_offer ? '>' : ' ',
            definition->display_name, (unsigned long long)offer->price);
    }
    for (index = 0; index < game->nodes.count; index++) {
        const Relay_Node *node = &game->nodes.nodes[index];
        if (game->workspace_mode == RELAY_GAME_WORKSPACE_MAP) {
            relay_terminal_draw_node_map(node, terminal, divider_x, height);
        } else {
            relay_terminal_draw_node_graph(node, terminal, divider_x, height);
        }
    }
    relay_terminal_draw_text(divider_x + 2, height - 3, TB_YELLOW,
        "drag output to input");
    relay_terminal_draw_text(divider_x + 2, height - 2, TB_YELLOW,
        "[ / ] clock rate");
}

/** Draw the centered exit-confirmation dialog above the current workspace. */
static void relay_terminal_draw_exit_overlay(int width, int height)
{
    const int x = (width - 28) / 2;
    const int y = (height - 5) / 2;

    if (x < 0 || y < 0 || y + 4 >= height) {
        return;
    }
    (void)tb_print(x, y, TB_CYAN | TB_BOLD, TB_DEFAULT,
        "╭──────────────────────────╮");
    (void)tb_print(x, y + 1, TB_WHITE | TB_BOLD, TB_DEFAULT,
        "│       Exit Relay?        │");
    (void)tb_print(x, y + 2, TB_WHITE, TB_DEFAULT,
        "│ Enter: confirm           │");
    (void)tb_print(x, y + 3, TB_WHITE, TB_DEFAULT,
        "│ Esc: cancel              │");
    (void)tb_print(x, y + 4, TB_CYAN | TB_BOLD, TB_DEFAULT,
        "╰──────────────────────────╯");
}

bool relay_terminal_init(Relay_Terminal *terminal)
{
    if (terminal == NULL || tb_init() != TB_OK ||
        tb_set_input_mode(TB_INPUT_ESC | TB_INPUT_MOUSE) != TB_OK) {
        if (terminal != NULL) {
            (void)tb_shutdown();
        }
        return false;
    }

    terminal->initialized = true;
    return true;
}

bool relay_terminal_draw(const Relay_Terminal *terminal, const Relay_Game *game,
    Relay_TerminalOverlay overlay)
{
    const int height = tb_height();

    if (terminal == NULL || game == NULL || !terminal->initialized) {
        return false;
    }

    if (tb_clear() != TB_OK) {
        return false;
    }
    relay_terminal_draw_split(tb_width(), height, game, terminal);
    if (overlay == RELAY_TERMINAL_OVERLAY_EXIT_CONFIRM) {
        relay_terminal_draw_exit_overlay(tb_width(), height);
    }
    return tb_present() == TB_OK;
}

void relay_terminal_focus_node(Relay_Terminal *terminal, const Relay_Game *game,
    uint64_t node_id)
{
    const Relay_Node *node;
    Relay_NodeRenderCard card;
    int divider_x;
    int scale;

    if (terminal == NULL || game == NULL ||
        !relay_terminal_split(tb_width(), tb_height(), &divider_x)) {
        return;
    }
    node = relay_node_world_find_const(&game->nodes, node_id);
    if (node == NULL) {
        return;
    }
    card = relay_node_renderer_card(node);
    scale = game->workspace_mode == RELAY_GAME_WORKSPACE_MAP ? 4 : 1;
    terminal->grid_offset_x = (int64_t)(divider_x / 2 - 2) * scale -
        node->grid_x - (int64_t)(card.width / 2) * scale;
    terminal->grid_offset_y = (int64_t)(tb_height() / 2 - 3) * scale -
        node->grid_y - (int64_t)(card.height / 2) * scale;
}

/** Return the topmost graph node card at a terminal mouse coordinate. */
static Relay_NodeId relay_terminal_node_at(const Relay_Game *game,
    const Relay_Terminal *terminal, int mouse_x, int mouse_y)
{
    int divider_x;
    size_t index;

    if (game->workspace_mode != RELAY_GAME_WORKSPACE_GRAPH ||
        !relay_terminal_split(tb_width(), tb_height(), &divider_x) ||
        mouse_x < 0 || mouse_x >= divider_x || mouse_y < 2) {
        return 0;
    }
    for (index = game->nodes.count; index > 0; index--) {
        const Relay_Node *node = &game->nodes.nodes[index - 1];
        const Relay_NodeRenderCard card = relay_node_renderer_card(node);
        const int x = 2 + node->grid_x + terminal->grid_offset_x;
        const int y = 3 + node->grid_y + terminal->grid_offset_y;

        if (mouse_x > x && mouse_x < x + card.width - 1 &&
            mouse_y == y + 1) {
            return node->id;
        }
    }
    return 0;
}

#if !RELAY_PLATFORM_WINDOWS
/** Return the graph port under a Termbox mouse coordinate. */
static Relay_TerminalPortHit relay_terminal_port_at(const Relay_Game *game,
    const Relay_Terminal *terminal, int mouse_x, int mouse_y)
{
    size_t index;

    if (game->workspace_mode != RELAY_GAME_WORKSPACE_GRAPH) {
        return (Relay_TerminalPortHit){0};
    }
    for (index = game->nodes.count; index > 0; index--) {
        const Relay_Node *node = &game->nodes.nodes[index - 1];
        const Relay_NodeRenderCard card = relay_node_renderer_card(node);
        const int x = 2 + node->grid_x + terminal->grid_offset_x;
        const int y = 3 + node->grid_y + terminal->grid_offset_y;
        const int row = mouse_y - (y + 3);

        if (card.definition != NULL && row >= 0 && row < (int)card.definition->input_count &&
            mouse_x == x) {
            return (Relay_TerminalPortHit){node->id, (size_t)row, false};
        }
        if (card.definition != NULL && row >= 0 && row < (int)card.definition->output_count &&
            mouse_x == x + card.width - 1) {
            return (Relay_TerminalPortHit){node->id, (size_t)row, true};
        }
    }
    return (Relay_TerminalPortHit){0};
}
#endif

bool relay_terminal_poll(Relay_Terminal *terminal, const Relay_Game *game,
    Relay_TerminalEvent *event)
{
    struct tb_event termbox_event;
    int result;

    if (terminal == NULL || game == NULL || event == NULL || !terminal->initialized) {
        return false;
    }
    result = tb_peek_event(&termbox_event, 16);
    if (result == TB_ERR_NO_EVENT) {
        *event = (Relay_TerminalEvent){0};
        return true;
    }
    if (result == TB_ERR_POLL && tb_last_errno() == EINTR) {
        *event = (Relay_TerminalEvent){0};
        return true;
    }
    if (result != TB_OK) {
        return false;
    }

    *event = (Relay_TerminalEvent){0};
    if (termbox_event.type == TB_EVENT_RESIZE) {
        event->type = RELAY_TERMINAL_EVENT_RESIZED;
    } else if (termbox_event.type == TB_EVENT_KEY) {
        event->type = RELAY_TERMINAL_EVENT_INPUT;
        event->character = termbox_event.ch;
        if (termbox_event.key == TB_KEY_ARROW_UP) {
            event->key = RELAY_TERMINAL_KEY_UP;
        } else if (termbox_event.key == TB_KEY_ARROW_DOWN) {
            event->key = RELAY_TERMINAL_KEY_DOWN;
        } else if (termbox_event.key == TB_KEY_ENTER) {
            event->key = RELAY_TERMINAL_KEY_CONFIRM;
        } else if (termbox_event.key == TB_KEY_ESC) {
            event->key = RELAY_TERMINAL_KEY_ESCAPE;
        }
    } else if (termbox_event.type == TB_EVENT_MOUSE) {
        event->type = RELAY_TERMINAL_EVENT_MOUSE;
        event->mouse_x = termbox_event.x;
        event->mouse_y = termbox_event.y;
        if (termbox_event.key == TB_KEY_MOUSE_LEFT && !terminal->dragging_grid &&
            !terminal->dragging_node && !terminal->wiring) {
            const Relay_TerminalPortHit hit = relay_terminal_port_at(game, terminal,
                event->mouse_x, event->mouse_y);

            if (hit.node_id != 0 && hit.is_output) {
                terminal->wiring = true;
                terminal->wiring_source_node_id = hit.node_id;
                terminal->wiring_source_port_index = hit.port_index;
                terminal->wiring_mouse_x = event->mouse_x;
                terminal->wiring_mouse_y = event->mouse_y;
                return true;
            }
            terminal->dragged_node_id = relay_terminal_node_at(game, terminal,
                event->mouse_x, event->mouse_y);
            terminal->dragging_node = terminal->dragged_node_id != 0;
            terminal->dragging_grid = !terminal->dragging_node;
            terminal->drag_last_x = event->mouse_x;
            terminal->drag_last_y = event->mouse_y;
        } else if (termbox_event.key == TB_KEY_MOUSE_LEFT && terminal->wiring) {
            terminal->wiring_mouse_x = event->mouse_x;
            terminal->wiring_mouse_y = event->mouse_y;
        } else if (termbox_event.key == TB_KEY_MOUSE_LEFT) {
            const int delta_x = event->mouse_x - terminal->drag_last_x;
            const int delta_y = event->mouse_y - terminal->drag_last_y;

            if (terminal->dragging_node) {
                event->dragged_node_id = terminal->dragged_node_id;
                event->grid_delta_x = delta_x;
                event->grid_delta_y = delta_y;
            } else if (terminal->dragging_grid) {
                const int scale = game->workspace_mode ==
                    RELAY_GAME_WORKSPACE_MAP ? 4 : 1;

                terminal->grid_offset_x = relay_terminal_add_delta(
                    terminal->grid_offset_x, (int64_t)delta_x * scale);
                terminal->grid_offset_y = relay_terminal_add_delta(
                    terminal->grid_offset_y, (int64_t)delta_y * scale);
            }
            terminal->drag_last_x = event->mouse_x;
            terminal->drag_last_y = event->mouse_y;
        } else if (termbox_event.key == TB_KEY_MOUSE_RELEASE) {
            if (terminal->wiring) {
                const Relay_TerminalPortHit hit = relay_terminal_port_at(game, terminal,
                    event->mouse_x, event->mouse_y);

                if (hit.node_id != 0 && !hit.is_output) {
                    event->connection_source_node_id = terminal->wiring_source_node_id;
                    event->connection_source_port_index = terminal->wiring_source_port_index;
                    event->connection_destination_node_id = hit.node_id;
                    event->connection_destination_port_index = hit.port_index;
                }
            }
            terminal->dragging_grid = false;
            terminal->dragging_node = false;
            terminal->wiring = false;
            terminal->dragged_node_id = 0;
        }
    }
    return true;
}

void relay_terminal_shutdown(Relay_Terminal *terminal)
{
    if (terminal == NULL || !terminal->initialized) {
        return;
    }

    (void)tb_shutdown();
    terminal->initialized = false;
}
#endif
