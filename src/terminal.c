#include "relay/terminal.h"

#include "relay/game.h"
#include "relay/node_renderer.h"
#include "relay/script_language.h"
#include "relay/session.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum {
    RELAY_TERMINAL_MIN_GAME_WIDTH = 12,
    RELAY_TERMINAL_MIN_PANEL_WIDTH = 30,
    RELAY_TERMINAL_MAX_PANEL_WIDTH = 42,
    RELAY_TERMINAL_MIN_HEIGHT = 10,
    RELAY_TERMINAL_START_WIDTH = 46,
    RELAY_TERMINAL_START_HEIGHT = 18,
    RELAY_TERMINAL_START_MENU_ROW = 10
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

/** Derived line/column window for rendering the active code editor. */
typedef struct Relay_TerminalEditorView {
    size_t first_line;
    size_t first_column;
    size_t cursor_line;
    size_t cursor_column;
} Relay_TerminalEditorView;

/** Resolve the centered title-screen origin for drawing and hit testing. */
static bool relay_terminal_start_layout(int width, int height, int *x, int *y)
{
    if (x == NULL || y == NULL || width < RELAY_TERMINAL_START_WIDTH ||
        height < RELAY_TERMINAL_START_HEIGHT) {
        return false;
    }
    *x = (width - RELAY_TERMINAL_START_WIDTH) / 2;
    *y = (height - RELAY_TERMINAL_START_HEIGHT) / 2;
    return true;
}

/** Return the one-based startup action under one terminal-space point. */
static size_t relay_terminal_session_menu_at(int width, int height,
    int mouse_x, int mouse_y)
{
    int x;
    int y;
    int row;

    if (!relay_terminal_start_layout(width, height, &x, &y)) {
        return 0;
    }
    row = mouse_y - (y + RELAY_TERMINAL_START_MENU_ROW);
    if (mouse_x <= x ||
        mouse_x >= x + RELAY_TERMINAL_START_WIDTH - 1 ||
        row < 0 || row >= RELAY_GAME_SESSION_MENU_COUNT) {
        return 0;
    }
    return (size_t)row + 1;
}

/** Format the Timer's supported user-facing intervals from its data contract. */
static void relay_terminal_timer_intervals(char *buffer, size_t capacity)
{
    size_t size = 0;
    size_t index;
    int written;

    if (capacity == 0) {
        return;
    }
    written = snprintf(buffer, capacity, "intervals:");
    if (written < 0 || (size_t)written >= capacity) {
        buffer[capacity - 1] = '\0';
        return;
    }
    size = (size_t)written;
    for (index = 0; index < relay_timer_interval_count(); index++) {
        written = snprintf(&buffer[size], capacity - size, " %lld",
            (long long)(relay_timer_interval_at(index) /
                RELAY_GAME_SIMULATION_STEPS_PER_SECOND));

        if (written < 0 || (size_t)written >= capacity - size) {
            buffer[capacity - 1] = '\0';
            return;
        }
        size += (size_t)written;
    }
    (void)snprintf(&buffer[size], capacity - size, " s");
}

/** Calculate a cursor-centered editor viewport without mutating game state. */
static Relay_TerminalEditorView relay_terminal_editor_view(
    const Relay_Blueprint *blueprint, size_t visible_lines,
    size_t visible_columns)
{
    Relay_TerminalEditorView view = {0};
    size_t index;
    size_t line_start = 0;

    for (index = 0; index < blueprint->cursor; index++) {
        if (blueprint->source[index] == '\n') {
            view.cursor_line++;
            line_start = index + 1;
        }
    }
    view.cursor_column = blueprint->cursor - line_start;
    if (visible_lines > 0 && view.cursor_line >= visible_lines) {
        view.first_line = view.cursor_line - visible_lines + 1;
    }
    if (visible_columns > 0 && view.cursor_column >= visible_columns) {
        view.first_column = view.cursor_column - visible_columns + 1;
    }
    return view;
}

/** Return the source byte range occupied by a numbered editor line. */
static bool relay_terminal_editor_line(const Relay_Blueprint *blueprint,
    size_t target_line, size_t *start, size_t *length)
{
    size_t line = 0;
    size_t index = 0;

    while (line < target_line && index < blueprint->source_size) {
        if (blueprint->source[index++] == '\n') {
            line++;
        }
    }
    if (line != target_line || index > blueprint->source_size) {
        return false;
    }
    *start = index;
    while (index < blueprint->source_size &&
        blueprint->source[index] != '\n') {
        index++;
    }
    *length = index - *start;
    return true;
}

/** Return the compact player-facing label for one editor mode. */
static const char *relay_terminal_editor_mode_label(
    Relay_BlueprintEditorMode mode)
{
    switch (mode) {
    case RELAY_BLUEPRINT_EDITOR_NORMAL: return "NORMAL";
    case RELAY_BLUEPRINT_EDITOR_INSERT: return "INSERT";
    case RELAY_BLUEPRINT_EDITOR_COMMAND: return "COMMAND";
    }
    return "UNKNOWN";
}

/** Return the semantic token covering one source byte, or the default class. */
static Relay_ScriptTokenKind relay_terminal_editor_token_at(
    const Relay_ScriptToken *tokens, size_t token_count, size_t offset)
{
    size_t low = 0;
    size_t high = token_count;

    while (low < high) {
        const size_t middle = low + (high - low) / 2;

        if (offset < tokens[middle].start) {
            high = middle;
        } else if (offset >= tokens[middle].start + tokens[middle].length) {
            low = middle + 1;
        } else {
            return tokens[middle].kind;
        }
    }
    return RELAY_SCRIPT_TOKEN_DEFAULT;
}

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

/** Return whether a node belongs on the current collapsed graph surface. */
static bool relay_terminal_node_is_visible(const Relay_Node *node)
{
    return node != NULL && node->module_instance_id == 0;
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
    source = relay_node_world_find_const(relay_game_active_world_const(game),
        connection->source_node_id);
    destination = relay_node_world_find_const(relay_game_active_world_const(game),
        connection->destination_node_id);
    source_card = relay_node_renderer_card(source);
    destination_card = relay_node_renderer_card(destination);
    if (!relay_terminal_node_is_visible(source) ||
        !relay_terminal_node_is_visible(destination) ||
        source_card.definition == NULL ||
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
    node = relay_node_world_find_const(relay_game_active_world_const(game),
        node_id);
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

/** Resolve an input port's terminal anchor for reverse-start live wiring. */
static bool relay_terminal_input_anchor(const Relay_Game *game,
    const Relay_Terminal *terminal, Relay_NodeId node_id, size_t port_index,
    int *x, int *y)
{
    const Relay_Node *node;
    Relay_NodeRenderCard card;

    if (game == NULL || terminal == NULL || x == NULL || y == NULL) {
        return false;
    }
    node = relay_node_world_find_const(relay_game_active_world_const(game),
        node_id);
    card = relay_node_renderer_card(node);
    if (node == NULL || card.definition == NULL ||
        port_index >= card.definition->input_count) {
        return false;
    }
    *x = 2 + (int)node->grid_x + (int)terminal->grid_offset_x;
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
    panel_width = width * 2 / 5;
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

/** Return the one-based right-panel tab index at a terminal coordinate. */
static size_t relay_terminal_panel_tab_at(int width, int height, int mouse_x,
    int mouse_y)
{
    int divider_x;
    int offset;

    if (!relay_terminal_split(width, height, &divider_x) || mouse_y != 1) {
        return 0;
    }
    offset = mouse_x - (divider_x + 2);
    if (offset >= 0 && offset <= 5) {
        return 1;
    }
    if (offset >= 7 && offset <= 15) {
        return 2;
    }
    if (offset >= 17 && offset <= 25) {
        return 3;
    }
    return 0;
}

/** Return the encoded workspace index under one top-tab mouse coordinate. */
static size_t relay_terminal_workspace_tab_at(const Relay_Game *game,
    int width, int height, int mouse_x, int mouse_y)
{
    int divider_x;
    int tab_x = 1;
    size_t index;
    size_t tab_width = strlen("Relay") + 2;

    if (game == NULL || mouse_y != 0 ||
        !relay_terminal_split(width, height, &divider_x)) {
        return 0;
    }
    if (mouse_x >= tab_x && mouse_x < tab_x + (int)tab_width) {
        return 1;
    }
    tab_x += (int)tab_width + 1;
    for (index = 0; index < game->blueprints.count; index++) {
        const Relay_Blueprint *blueprint = &game->blueprints.blueprints[index];

        if (!blueprint->workspace_open) {
            continue;
        }
        tab_width = strlen(blueprint->name) + 2;
        if (tab_x >= divider_x) {
            break;
        }
        if (mouse_x >= tab_x && mouse_x < tab_x + (int)tab_width) {
            return index + 2;
        }
        tab_x += (int)tab_width + 1;
    }
    return 0;
}

#if RELAY_PLATFORM_WINDOWS
#include <stdio.h>

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

/** Draw Relay and every open Blueprint as persistent Windows workspace tabs. */
static bool relay_terminal_draw_workspace_tabs(HANDLE output, int divider_x,
    const Relay_Game *game)
{
    int tab_x = 1;
    size_t index;
    char text[96];

    if (!relay_terminal_write_at(output, tab_x, 0,
            game->active_workspace == 0 ?
                "\x1b[1;36m[Relay]\x1b[0m" : "[Relay]")) {
        return false;
    }
    tab_x += (int)strlen("Relay") + 3;
    for (index = 0; index < game->blueprints.count; index++) {
        const Relay_Blueprint *blueprint = &game->blueprints.blueprints[index];
        const int tab_width = (int)strlen(blueprint->name) + 2;

        if (!blueprint->workspace_open) {
            continue;
        }
        if (tab_x + tab_width > divider_x) {
            break;
        }
        (void)snprintf(text, sizeof(text),
            game->active_workspace == index + 1 ?
                "\x1b[1;36m[%s]\x1b[0m" : "[%s]", blueprint->name);
        if (!relay_terminal_write_at(output, tab_x, 0, text)) {
            return false;
        }
        tab_x += tab_width + 1;
    }
    return true;
}

/** Return an ANSI-colored port glyph from the renderer-owned visual token. */
static const char *relay_terminal_port_glyph(Relay_NodePortType type)
{
    switch (relay_node_renderer_port_visual(type).color) {
    case 3: return "\x1b[1;36m●\x1b[0m";
    case 1: return "\x1b[1;33m●\x1b[0m";
    case 2: return "\x1b[1;37m●\x1b[0m";
    case 6: return "\x1b[1;31m●\x1b[0m";
    case 4: return "\x1b[2;37m●\x1b[0m";
    case 5: return "\x1b[1;35m●\x1b[0m";
    case 7: return "\x1b[1;34m●\x1b[0m";
    case 8: return "\x1b[1;32m●\x1b[0m";
    default: return "\x1b[1;31m?\x1b[0m";
    }
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

    for (index = 0; index <
            relay_game_active_world_const(game)->connection_count; index++) {
        Relay_TerminalWireRoute route;
        Relay_TerminalWirePath path;

        if (relay_terminal_wire_route(game, terminal,
                &relay_game_active_world_const(game)->connections[index],
                &route)) {
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
        const Relay_Node *source = relay_node_world_find_const(
            relay_game_active_world_const(game),
            terminal->wiring_source_node_id);
        const Relay_NodeRenderCard card = relay_node_renderer_card(source);
        Relay_TerminalWirePath path;
        int source_x;
        int source_y;
        const int source_top = source == NULL ? 0 : 3 + (int)source->grid_y +
            (int)terminal->grid_offset_y;

        if (source != NULL && terminal->wiring_origin_is_output &&
            relay_terminal_output_anchor(game, terminal,
                source->id, terminal->wiring_source_port_index, &source_x,
                &source_y)) {
            relay_terminal_wire_path_create(source_x, source_y, source_top,
                source_top + card.height - 1, terminal->wiring_mouse_x,
                terminal->wiring_mouse_y, terminal->wiring_mouse_y,
                terminal->wiring_mouse_y, &path);
            if (!relay_terminal_draw_wire_path(output, &path, divider_x, height)) {
                return false;
            }
        } else if (source != NULL && relay_terminal_input_anchor(game, terminal,
                source->id, terminal->wiring_source_port_index, &source_x,
                &source_y)) {
            relay_terminal_wire_path_create(terminal->wiring_mouse_x,
                terminal->wiring_mouse_y, terminal->wiring_mouse_y,
                terminal->wiring_mouse_y, source_x, source_y, source_top,
                source_top + card.height - 1, &path);
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
    char input_content[20];
    char output_content[20];
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

        if (input != NULL && relay_node_port_type_is_item(input->type)) {
            (void)snprintf(input_content, sizeof(input_content), "%.7s[%zu]",
                input->display_name, node->input_queues[row].count);
        } else {
            (void)snprintf(input_content, sizeof(input_content), "%s",
                input == NULL ? "" : input->display_name);
        }
        if (output != NULL && relay_node_port_type_is_item(output->type)) {
            (void)snprintf(output_content, sizeof(output_content), "%.9s[%zu]",
                output->display_name, node->output_queues[row].count);
        } else {
            (void)snprintf(output_content, sizeof(output_content), "%s",
                output == NULL ? "" : output->display_name);
        }
        (void)snprintf(content, sizeof(content), "%-10.10s%12.12s",
            input_content, output_content);
        (void)snprintf(line, sizeof(line), "%s %-22.22s %s",
            input == NULL ? "│" : "●", content,
            output == NULL ? "│" : "●");
        if (!relay_terminal_write_at(output, x, y + 3 + (int)row, line)) {
            return false;
        }
        if ((input != NULL && !relay_terminal_write_at(output, x,
                y + 3 + (int)row, relay_terminal_port_glyph(input->type))) ||
            (output != NULL && !relay_terminal_write_at(output,
                x + card.width - 1, y + 3 + (int)row,
                relay_terminal_port_glyph(output->type)))) {
            return false;
        }
    }
    if (card.definition->simulation.behavior ==
            RELAY_NODE_BEHAVIOR_FIXED_RATE_SOURCE) {
        const int64_t interval = card.definition->simulation.interval_steps;
        const int filled = (int)(node->progress * 10 /
            interval);

        (void)snprintf(content, sizeof(content), "[%.*s%.*s] %2lld/%lld",
            filled, "##########", 10 - filled, "..........",
            (long long)node->progress, (long long)interval);
    } else if (card.definition->simulation.behavior ==
            RELAY_NODE_BEHAVIOR_TIMER) {
        (void)snprintf(content, sizeof(content), "interval: %lld s",
            (long long)(node->timer_interval_steps /
                RELAY_GAME_SIMULATION_STEPS_PER_SECOND));
    } else if (node->runtime_kind ==
            RELAY_NODE_RUNTIME_BLUEPRINT_INPUT_BOUNDARY) {
        (void)snprintf(content, sizeof(content), "public interface");
    } else if (node->runtime_kind ==
            RELAY_NODE_RUNTIME_BLUEPRINT_OUTPUT_BOUNDARY) {
        (void)snprintf(content, sizeof(content), "public interface");
    } else if (node->runtime_kind ==
            RELAY_NODE_RUNTIME_BLUEPRINT_PROCESS) {
        (void)snprintf(content, sizeof(content), "Lua process");
    } else if (node->runtime_kind ==
            RELAY_NODE_RUNTIME_BLUEPRINT_WRAPPER) {
        (void)snprintf(content, sizeof(content), "module instance");
    } else {
        (void)snprintf(content, sizeof(content), "enabled");
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

/** Draw the active blueprint source editor in the Windows console backend. */
static const char *relay_terminal_editor_ansi(Relay_ScriptTokenKind kind)
{
    switch (kind) {
    case RELAY_SCRIPT_TOKEN_KEYWORD: return "\x1b[35m";
    case RELAY_SCRIPT_TOKEN_STRING: return "\x1b[32m";
    case RELAY_SCRIPT_TOKEN_NUMBER: return "\x1b[33m";
    case RELAY_SCRIPT_TOKEN_COMMENT: return "\x1b[90m";
    case RELAY_SCRIPT_TOKEN_FUNCTION: return "\x1b[36m";
    case RELAY_SCRIPT_TOKEN_TYPE: return "\x1b[96m";
    case RELAY_SCRIPT_TOKEN_NAMESPACE: return "\x1b[33m";
    case RELAY_SCRIPT_TOKEN_MEMBER: return "\x1b[34m";
    case RELAY_SCRIPT_TOKEN_OPERATOR: return "\x1b[37m";
    case RELAY_SCRIPT_TOKEN_DEFAULT: return "\x1b[37m";
    }
    return "\x1b[37m";
}

/** Draw syntax overlays and language assistance for the Windows editor. */
static bool relay_terminal_draw_editor(HANDLE output, int divider_x, int height,
    const Relay_Game *game, const Relay_Blueprint *blueprint)
{
    const size_t visible_lines = height > 4 ? (size_t)(height - 4) : 0;
    const size_t text_capacity = divider_x > 9 ?
        (size_t)(divider_x - 9) : 0;
    const Relay_TerminalEditorView view = relay_terminal_editor_view(blueprint,
        visible_lines, text_capacity);
    Relay_ScriptToken tokens[RELAY_SCRIPT_LANGUAGE_MAX_TOKENS];
    const size_t token_count = relay_script_language_tokenize(
        blueprint->source, blueprint->source_size, tokens,
        RELAY_SCRIPT_LANGUAGE_MAX_TOKENS);
    size_t row;

    for (row = 0; row < visible_lines; row++) {
        const size_t line_number = view.first_line + row;
        size_t start;
        size_t length;
        char line[192];

        if (!relay_terminal_editor_line(blueprint, line_number, &start,
                &length)) {
            break;
        }
        if (view.first_column < length) {
            start += view.first_column;
            length -= view.first_column;
        } else {
            length = 0;
        }
        if (length > text_capacity) {
            length = text_capacity;
        }
        (void)snprintf(line, sizeof(line), "\x1b[2m%4zu\x1b[0m │ %-.*s",
            line_number + 1, (int)length, &blueprint->source[start]);
        if (!relay_terminal_write_at(output, 1, 2 + (int)row, line)) {
            return false;
        }
        {
            size_t column = 0;

            while (column < length) {
                const Relay_ScriptTokenKind kind =
                    relay_terminal_editor_token_at(tokens, token_count,
                        start + column);
                size_t segment_end = column + 1;
                char segment[256];

                while (segment_end < length &&
                    relay_terminal_editor_token_at(tokens, token_count,
                        start + segment_end) == kind) {
                    segment_end++;
                }
                (void)snprintf(segment, sizeof(segment), "%s%.*s\x1b[0m",
                    relay_terminal_editor_ansi(kind),
                    (int)(segment_end - column),
                    &blueprint->source[start + column]);
                if (!relay_terminal_write_at(output, 8 + (int)column,
                        2 + (int)row, segment)) {
                    return false;
                }
                column = segment_end;
            }
        }
    }
    if (view.cursor_line >= view.first_line &&
        view.cursor_line < view.first_line + visible_lines) {
        const int cursor_x = 8 +
            (int)(view.cursor_column - view.first_column);
        const int cursor_y = 2 + (int)(view.cursor_line - view.first_line);
        char cursor[32];
        const char character = blueprint->cursor < blueprint->source_size &&
            blueprint->source[blueprint->cursor] != '\n' ?
            blueprint->source[blueprint->cursor] : ' ';

        (void)snprintf(cursor, sizeof(cursor), "\x1b[7m%c\x1b[0m", character);
        if (cursor_x < divider_x &&
            !relay_terminal_write_at(output, cursor_x, cursor_y, cursor)) {
            return false;
        }
    }
    if (blueprint->editor_mode == RELAY_BLUEPRINT_EDITOR_INSERT &&
        divider_x >= 46) {
        Relay_ScriptCompletion completions[
            RELAY_SCRIPT_LANGUAGE_MAX_COMPLETIONS];
        Relay_ScriptSignature signature;
        size_t replacement_start;
        const size_t count = blueprint->completion_suppressed ? 0 :
            relay_game_editor_completions(game, completions,
                RELAY_SCRIPT_LANGUAGE_MAX_COMPLETIONS, &replacement_start);
        const size_t selected = count == 0 ? 0 :
            blueprint->completion_selection % count;
        const size_t first = selected < 5 ? 0 : selected - 4;
        int popup_y = 3 + (int)(view.cursor_line - view.first_line);
        size_t index;

        (void)replacement_start;
        if (relay_script_language_signature(blueprint->source,
                blueprint->source_size, blueprint->cursor, &signature) &&
            popup_y < height - 2) {
            char help[96];

            (void)snprintf(help, sizeof(help),
                "\x1b[30;46m %.27s  p%zu \x1b[0m", signature.label,
                signature.active_parameter + 1);
            if (!relay_terminal_write_at(output, 8, popup_y++, help)) {
                return false;
            }
        }
        for (index = first; index < count && index < first + 5 &&
                popup_y < height - 2; index++, popup_y++) {
            char item[128];
            const bool item_selected = index == selected;

            (void)snprintf(item, sizeof(item),
                "%s %-13.13s %-21.21s \x1b[0m",
                item_selected ? "\x1b[30;47m" : "\x1b[37;40m",
                completions[index].label, completions[index].detail);
            if (!relay_terminal_write_at(output, 8, popup_y, item)) {
                return false;
            }
        }
    }
    return true;
}

/** Render Relay's tabbed graph workspace and right-side control panel. */
static bool relay_terminal_draw_split(HANDLE output, int width, int height,
    const Relay_Game *game, const Relay_Terminal *terminal)
{
    int divider_x;
    int y;
    char line[64];
    size_t index;
    const Relay_Node *focused;
    const Relay_Blueprint *editing = game->editing_blueprint_id == 0 ? NULL :
        relay_blueprint_library_find_const(&game->blueprints,
            game->editing_blueprint_id);

    if (!relay_terminal_split(width, height, &divider_x)) {
        return relay_terminal_write_at(output, 0, 0,
            "Terminal is too small for the Relay layout.");
    }
    for (y = 0; y < height; y++) {
        if (!relay_terminal_write_at(output, divider_x, y, "\x1b[90m|\x1b[0m")) {
            return false;
        }
    }
    if (!relay_terminal_draw_workspace_tabs(output, divider_x, game) ||
        !relay_terminal_write_at(output, divider_x + 2, 1,
            editing != NULL ? "\x1b[1;36m[ CODE ]\x1b[0m" :
            game->active_tab == RELAY_GAME_PANEL_TAB_SHOP ?
                "\x1b[1;36m[SHOP]\x1b[0m [Inspect] [Scripts]" :
            game->active_tab == RELAY_GAME_PANEL_TAB_INSPECTOR ?
                "[Shop] \x1b[1;36m[INSPECT]\x1b[0m [Scripts]" :
                "[Shop] [Inspect] \x1b[1;36m[SCRIPTS]\x1b[0m")) {
        return false;
    }
    if (editing != NULL) {
        if (!relay_terminal_draw_editor(output, divider_x, height, game,
                editing)) {
            return false;
        }
    } else if (!relay_terminal_draw_grid(output, divider_x, height, terminal)) {
        return false;
    }
    if (editing == NULL && game->workspace_mode == RELAY_GAME_WORKSPACE_GRAPH &&
        !relay_terminal_draw_wires(output, game, terminal, divider_x, height)) {
        return false;
    }
    (void)snprintf(line, sizeof(line), "◈ %llu", (unsigned long long)game->currency);
    if (!relay_terminal_write_at(output, divider_x + 2, 3, line)) {
        return false;
    }
    (void)snprintf(line, sizeof(line), "wires: %zu",
        relay_game_active_world_const(game)->connection_count);
    if (!relay_terminal_write_at(output, divider_x + 2, 4, line)) {
        return false;
    }
    if (editing != NULL) {
        (void)snprintf(line, sizeof(line), "revision: %llu%s",
            (unsigned long long)editing->revision,
            editing->dirty ? "  modified" : "");
        if (!relay_terminal_write_at(output, divider_x + 2, 3, line)) {
            return false;
        }
        (void)snprintf(line, sizeof(line), "mode: %s",
            relay_terminal_editor_mode_label(editing->editor_mode));
        if (!relay_terminal_write_at(output, divider_x + 2, 4, line)) {
            return false;
        }
        (void)snprintf(line, sizeof(line), "%.36s",
            editing->diagnostic.message);
        if (!relay_terminal_write_at(output, divider_x + 2, 5, line)) {
            return false;
        }
        if (editing->editor_mode == RELAY_BLUEPRINT_EDITOR_COMMAND) {
            (void)snprintf(line, sizeof(line), ":%s",
                editing->editor_command);
            if (!relay_terminal_write_at(output, divider_x + 2, 7, line)) {
                return false;
            }
        }
    } else if (game->active_tab == RELAY_GAME_PANEL_TAB_SHOP) {
        for (index = 0; index < relay_game_shop_offer_count() &&
            6 + (int)index < height - 2; index++) {
            const Relay_ShopOffer *offer = relay_game_shop_offer_at(index);
            const Relay_NodeDefinition *definition = relay_game_shop_offer_definition(offer);

            (void)snprintf(line, sizeof(line), "%c %-16s %3llu", index == game->selected_offer ?
                '>' : ' ', definition->display_name, (unsigned long long)offer->price);
            if (!relay_terminal_write_at(output, divider_x + 2, 6 + (int)index, line)) {
                return false;
            }
        }
    } else if (game->active_tab == RELAY_GAME_PANEL_TAB_INSPECTOR) {
        focused = relay_node_world_find_const(relay_game_active_world_const(game),
            game->focused_node_id);
        if (focused == NULL) {
            if (!relay_terminal_write_at(output, divider_x + 2, 6,
                    "No node selected.")) {
                return false;
            }
        } else {
            const Relay_NodeDefinition *definition =
                relay_node_definition_for(focused);

            if (definition == NULL) {
                return false;
            }
            (void)snprintf(line, sizeof(line), "%s", definition->display_name);
            if (!relay_terminal_write_at(output, divider_x + 2, 6, line)) {
                return false;
            }
            (void)snprintf(line, sizeof(line), "id: %llu", (unsigned long long)focused->id);
            if (!relay_terminal_write_at(output, divider_x + 2, 7, line)) {
                return false;
            }
            if (definition->simulation.behavior ==
                    RELAY_NODE_BEHAVIOR_TIMER) {
                if (!relay_terminal_write_at(output, divider_x + 2, 9,
                        "Timer") ||
                    !relay_terminal_write_at(output, divider_x + 2, 10,
                        "output: Trigger")) {
                    return false;
                }
                (void)snprintf(line, sizeof(line), "interval: %lld seconds",
                    (long long)(focused->timer_interval_steps /
                        RELAY_GAME_SIMULATION_STEPS_PER_SECOND));
                if (!relay_terminal_write_at(output, divider_x + 2, 11,
                        line)) {
                    return false;
                }
                relay_terminal_timer_intervals(line, sizeof(line));
                if (!relay_terminal_write_at(output, divider_x + 2, 12, line) ||
                    !relay_terminal_write_at(output, divider_x + 2, 13,
                        "[ / ] change interval")) {
                    return false;
                }
            } else if (definition->simulation.behavior ==
                    RELAY_NODE_BEHAVIOR_FIXED_RATE_SOURCE) {
                const Relay_NodeSimulationDefinition *simulation =
                    &definition->simulation;
                const Relay_NodePortDefinition *source_output =
                    &definition->outputs[simulation->output_port_index];

                (void)snprintf(line, sizeof(line), "output: %s",
                    source_output->display_name);
                if (!relay_terminal_write_at(output, divider_x + 2, 9, line)) {
                    return false;
                }
                (void)snprintf(line, sizeof(line), "rate: %lld / second",
                    (long long)simulation->output_amount);
                if (!relay_terminal_write_at(output, divider_x + 2, 10,
                        line)) {
                    return false;
                }
                (void)snprintf(line, sizeof(line), "progress: %lld / %u",
                    (long long)focused->progress,
                    simulation->interval_steps);
                if (!relay_terminal_write_at(output, divider_x + 2, 11,
                        line)) {
                    return false;
                }
                (void)snprintf(line, sizeof(line), "produced: %lld",
                    (long long)focused->produced);
                if (!relay_terminal_write_at(output, divider_x + 2, 12,
                        line)) {
                    return false;
                }
            } else if (focused->runtime_kind ==
                    RELAY_NODE_RUNTIME_BLUEPRINT_INPUT_BOUNDARY) {
                if (!relay_terminal_write_at(output, divider_x + 2, 9,
                        "Public module inputs") ||
                    !relay_terminal_write_at(output, divider_x + 2, 10,
                        "Wire outputs to components.") ||
                    !relay_terminal_write_at(output, divider_x + 2, 12,
                        "VHDL: entity input ports")) {
                    return false;
                }
            } else if (focused->runtime_kind ==
                    RELAY_NODE_RUNTIME_BLUEPRINT_OUTPUT_BOUNDARY) {
                if (!relay_terminal_write_at(output, divider_x + 2, 9,
                        "Public module outputs") ||
                    !relay_terminal_write_at(output, divider_x + 2, 10,
                        "Wire components into inputs.") ||
                    !relay_terminal_write_at(output, divider_x + 2, 12,
                        "VHDL: entity output ports")) {
                    return false;
                }
            } else if (focused->runtime_kind ==
                    RELAY_NODE_RUNTIME_BLUEPRINT_PROCESS) {
                if (!relay_terminal_write_at(output, divider_x + 2, 9,
                        "Player Lua process") ||
                    !relay_terminal_write_at(output, divider_x + 2, 10,
                        "Ports mirror this module.") ||
                    !relay_terminal_write_at(output, divider_x + 2, 12,
                        "E: edit source")) {
                    return false;
                }
            } else if (focused->blueprint_id != 0) {
                const Relay_Blueprint *blueprint =
                    relay_blueprint_library_find_const(&game->blueprints,
                        focused->blueprint_id);

                if (blueprint != NULL) {
                    (void)snprintf(line, sizeof(line), "key: %s",
                        blueprint->key);
                    if (!relay_terminal_write_at(output, divider_x + 2, 9,
                            line)) {
                        return false;
                    }
                    (void)snprintf(line, sizeof(line), "revision: %llu",
                        (unsigned long long)blueprint->compiled_revision);
                    if (!relay_terminal_write_at(output, divider_x + 2, 10,
                            line) ||
                        !relay_terminal_write_at(output, divider_x + 2, 12,
                            "Reusable module instance") ||
                        !relay_terminal_write_at(output, divider_x + 2, 13,
                            "E: edit source")) {
                        return false;
                    }
                }
            }
            {
                int queue_y = 15;
                size_t port_index;

                for (port_index = 0; port_index < definition->input_count &&
                        queue_y < height - 5; port_index++) {
                    if (!relay_node_port_type_is_item(
                            definition->inputs[port_index].type)) {
                        continue;
                    }
                    (void)snprintf(line, sizeof(line), "in  %s: %zu/%d",
                        definition->inputs[port_index].key,
                        focused->input_queues[port_index].count,
                        RELAY_ITEM_QUEUE_CAPACITY);
                    if (!relay_terminal_write_at(output, divider_x + 2,
                            queue_y++, line)) {
                        return false;
                    }
                }
                for (port_index = 0; port_index < definition->output_count &&
                        queue_y < height - 5; port_index++) {
                    if (!relay_node_port_type_is_item(
                            definition->outputs[port_index].type)) {
                        continue;
                    }
                    (void)snprintf(line, sizeof(line), "out %s: %zu/%d",
                        definition->outputs[port_index].key,
                        focused->output_queues[port_index].count,
                        RELAY_ITEM_QUEUE_CAPACITY);
                    if (!relay_terminal_write_at(output, divider_x + 2,
                            queue_y++, line)) {
                        return false;
                    }
                }
            }
        }
    } else {
        for (index = 0; index < game->blueprints.count &&
            6 + (int)index < height - 2; index++) {
            const Relay_Blueprint *blueprint =
                &game->blueprints.blueprints[index];

            (void)snprintf(line, sizeof(line), "%c %s",
                index == game->selected_blueprint ? '>' : ' ', blueprint->name);
            if (!relay_terminal_write_at(output, divider_x + 2,
                    6 + (int)index, line)) {
                return false;
            }
        }
    }
    for (index = 0; editing == NULL &&
        index < relay_game_active_world_const(game)->count; index++) {
        const Relay_Node *node =
            &relay_game_active_world_const(game)->nodes[index];
        if (!relay_terminal_node_is_visible(node)) {
            continue;
        }
        if ((game->workspace_mode == RELAY_GAME_WORKSPACE_MAP &&
                !relay_terminal_draw_node_map(output, node, terminal, divider_x,
                    height)) ||
            (game->workspace_mode == RELAY_GAME_WORKSPACE_GRAPH &&
                !relay_terminal_draw_node_graph(output, node, terminal, divider_x,
                    height))) {
            return false;
        }
    }
    (void)snprintf(line, sizeof(line), "session: %.40s",
        game->session_status);
    return relay_terminal_write_at(output, divider_x + 2, height - 4, line) &&
        relay_terminal_write_at(output, divider_x + 2, height - 3,
        editing != NULL &&
            editing->editor_mode == RELAY_BLUEPRINT_EDITOR_INSERT ?
                "INSERT  Esc: normal" :
        editing != NULL &&
            editing->editor_mode == RELAY_BLUEPRINT_EDITOR_COMMAND ?
                ":w deploy  :wq deploy/back" :
        editing != NULL ? "NORMAL  i/a/o insert  : command" :
            "N:new ,/.:tab C:close E:edit S:save") &&
        relay_terminal_write_at(output, divider_x + 2, height - 2,
            editing != NULL &&
                editing->editor_mode == RELAY_BLUEPRINT_EDITOR_INSERT ?
                    "Tab complete  Enter newline" :
            editing != NULL &&
                editing->editor_mode == RELAY_BLUEPRINT_EDITOR_COMMAND ?
                    "Enter: run  Esc: cancel" :
            editing != NULL ? "hjkl move  x delete  Esc back" :
            game->active_tab == RELAY_GAME_PANEL_TAB_SHOP ?
                "j/k: select  Enter: buy" :
            game->active_tab == RELAY_GAME_PANEL_TAB_BLUEPRINTS ?
                "j/k pick  O:open  Enter:add" :
            game->active_workspace != 0 ?
                "Inputs -> components -> Outputs" :
                "click a title to inspect");
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

/** Draw one padded line in a centered Windows overlay. */
static bool relay_terminal_draw_overlay_line(HANDLE output, int x, int y,
    const char *text)
{
    char line[64];

    (void)snprintf(line, sizeof(line), "│ %-34.34s │", text);
    return relay_terminal_write_at(output, x, y, line);
}

/** Draw one fixed-width line inside the dedicated startup panel. */
static bool relay_terminal_draw_start_line(HANDLE output, int x, int y,
    const char *text)
{
    char line[64];

    (void)snprintf(line, sizeof(line), "│ %-42.42s │", text);
    return relay_terminal_write_at(output, x, y, line);
}

/** Draw Relay's centered title treatment on an otherwise empty screen. */
static bool relay_terminal_draw_start_title(HANDLE output, int width,
    int height, int *origin_x, int *origin_y)
{
    int x;
    int y;

    if (!relay_terminal_start_layout(width, height, &x, &y)) {
        *origin_x = -1;
        *origin_y = -1;
        return relay_terminal_write_at(output, 0, 0,
            "Relay needs a terminal at least 46 x 18.");
    }
    *origin_x = x;
    *origin_y = y;
    return relay_terminal_write_at(output, x + 5, y,
            " ____  _____ _        _ __   __") &&
        relay_terminal_write_at(output, x + 5, y + 1,
            "|  _ \\| ____| |      / \\ \\ / /") &&
        relay_terminal_write_at(output, x + 5, y + 2,
            "| |_) |  _| | |     / _ \\ V /") &&
        relay_terminal_write_at(output, x + 5, y + 3,
            "|  _ <| |___| |___ / ___ \\| |") &&
        relay_terminal_write_at(output, x + 5, y + 4,
            "|_| \\_\\_____|_____/_/   \\_\\_|") &&
        relay_terminal_write_at(output, x + 10, y + 5,
            "TERMINAL AUTOMATION NETWORK");
}

/** Draw the persistent four-action startup main menu. */
static bool relay_terminal_draw_session_overlay(HANDLE output, int width,
    int height, const Relay_Game *game)
{
    int x;
    int y;
    char continue_option[48];
    char new_option[48];
    char saved_option[48];
    char exit_option[48];

    if (!relay_terminal_draw_start_title(output, width, height, &x, &y)) {
        return false;
    }
    if (x < 0) {
        return true;
    }
    (void)snprintf(continue_option, sizeof(continue_option), "%c %s",
        game->session_menu_selection == RELAY_GAME_SESSION_MENU_CONTINUE ?
            '>' : ' ',
        game->session_continue_available ?
            "Continue" : "Continue (unavailable)");
    (void)snprintf(new_option, sizeof(new_option), "%c New Session",
        game->session_menu_selection == RELAY_GAME_SESSION_MENU_NEW ?
            '>' : ' ');
    (void)snprintf(saved_option, sizeof(saved_option), "%c Saved Sessions",
        game->session_menu_selection == RELAY_GAME_SESSION_MENU_SAVED ?
            '>' : ' ');
    (void)snprintf(exit_option, sizeof(exit_option), "%c Exit",
        game->session_menu_selection == RELAY_GAME_SESSION_MENU_EXIT ?
            '>' : ' ');
    return relay_terminal_write_at(output, x, y + 7,
            "╭────────────────────────────────────────────╮") &&
        relay_terminal_draw_start_line(output, x, y + 8, "MAIN MENU") &&
        relay_terminal_write_at(output, x, y + 9,
            "├────────────────────────────────────────────┤") &&
        relay_terminal_draw_start_line(output, x, y + 10,
            continue_option) &&
        relay_terminal_draw_start_line(output, x, y + 11, new_option) &&
        relay_terminal_draw_start_line(output, x, y + 12, saved_option) &&
        relay_terminal_draw_start_line(output, x, y + 13, exit_option) &&
        relay_terminal_draw_start_line(output, x, y + 14, "") &&
        relay_terminal_draw_start_line(output, x, y + 15,
            "j/k or arrows  Enter select  Mouse click") &&
        relay_terminal_draw_start_line(output, x, y + 16,
            game->session_status) &&
        relay_terminal_write_at(output, x, y + 17,
            "╰────────────────────────────────────────────╯");
}

/** Draw a bounded browser for every discovered valid or invalid slot. */
static bool relay_terminal_draw_slot_overlay(HANDLE output, int width,
    int height, const Relay_SessionStore *sessions)
{
    int start_x;
    int start_y;
    int x;
    int y;
    const size_t first = sessions->selected_slot > 3 ?
        sessions->selected_slot - 3 : 0;
    size_t row;

    if (!relay_terminal_draw_start_title(output, width, height,
            &start_x, &start_y)) {
        return false;
    }
    if (start_x < 0) {
        return true;
    }
    x = (width - 38) / 2;
    y = start_y + 7;
    if (x < 0 || y + 9 >= height ||
        !relay_terminal_write_at(output, x, y,
            "╭────────────────────────────────────╮") ||
        !relay_terminal_draw_overlay_line(output, x, y + 1,
            sessions->slot_count == 0 ?
                "Saved Sessions - none yet" : "Saved Sessions")) {
        return false;
    }
    for (row = 0; row < 5; row++) {
        const size_t index = first + row;
        char line[48] = "";

        if (index < sessions->slot_count) {
            const Relay_SessionSlot *slot = &sessions->slots[index];

            if (slot->valid) {
                (void)snprintf(line, sizeof(line), "%c %08llx  step %llu",
                    index == sessions->selected_slot ? '>' : ' ',
                    (unsigned long long)(slot->id & UINT64_C(0xffffffff)),
                    (unsigned long long)slot->simulation_step);
            } else {
                (void)snprintf(line, sizeof(line), "%c %08llx  [invalid]",
                    index == sessions->selected_slot ? '>' : ' ',
                    (unsigned long long)(slot->id & UINT64_C(0xffffffff)));
            }
        }
        if (!relay_terminal_draw_overlay_line(output, x, y + 2 + (int)row,
                line)) {
            return false;
        }
    }
    return relay_terminal_draw_overlay_line(output, x, y + 7,
            "j/k select  Enter load") &&
        relay_terminal_draw_overlay_line(output, x, y + 8, "Esc: back") &&
        relay_terminal_write_at(output, x, y + 9,
            "╰────────────────────────────────────╯");
}

/** Draw overwrite versus new-slot choices for an explicit save. */
static bool relay_terminal_draw_save_overlay(HANDLE output, int width,
    int height, const Relay_Game *game, const Relay_SessionStore *sessions)
{
    const int x = (width - 38) / 2;
    const int y = (height - 8) / 2;
    const bool can_overwrite = sessions->active_session_id != 0;
    const bool can_create =
        sessions->slot_count < RELAY_SESSION_SLOT_CAPACITY;

    return x >= 0 && y >= 0 && y + 7 < height &&
        relay_terminal_write_at(output, x, y,
            "╭────────────────────────────────────╮") &&
        relay_terminal_draw_overlay_line(output, x, y + 1, "Save Session") &&
        relay_terminal_draw_overlay_line(output, x, y + 2, "") &&
        relay_terminal_draw_overlay_line(output, x, y + 3,
            !game->session_save_new_selected && can_overwrite ?
                "> Overwrite current slot" :
                can_overwrite ? "  Overwrite current slot" :
                    "  Overwrite (unavailable)") &&
        relay_terminal_draw_overlay_line(output, x, y + 4,
            !can_create ? "  New slot (repository full)" :
                game->session_save_new_selected ?
                    "> Save to new slot" : "  Save to new slot") &&
        relay_terminal_draw_overlay_line(output, x, y + 5,
            "j/k select  Enter save") &&
        relay_terminal_draw_overlay_line(output, x, y + 6, "Esc: cancel") &&
        relay_terminal_write_at(output, x, y + 7,
            "╰────────────────────────────────────╯");
}

/** Draw the centered save-and-exit confirmation dialog. */
static bool relay_terminal_draw_exit_overlay(HANDLE output, int width,
    int height, const Relay_Game *game)
{
    const int x = (width - 38) / 2;
    const int y = (height - 6) / 2;
    int start_x;
    int start_y;

    if (game->session_exit_from_menu &&
        !relay_terminal_draw_start_title(output, width, height,
            &start_x, &start_y)) {
        return false;
    }
    if (game->session_exit_from_menu && start_x < 0) {
        return true;
    }
    return x >= 0 && y >= 0 && y + 5 < height &&
        relay_terminal_write_at(output, x, y,
            "╭────────────────────────────────────╮") &&
        relay_terminal_draw_overlay_line(output, x, y + 1, "Exit Relay?") &&
        relay_terminal_draw_overlay_line(output, x, y + 2, "") &&
        relay_terminal_draw_overlay_line(output, x, y + 3,
            game->session_exit_from_menu ?
                "Enter: confirm exit" :
                "Enter: choose save and exit") &&
        relay_terminal_draw_overlay_line(output, x, y + 4, "Esc: cancel") &&
        relay_terminal_write_at(output, x, y + 5,
            "╰────────────────────────────────────╯");
}

/** Draw recovery choices after an exit save fails. */
static bool relay_terminal_draw_save_failed_overlay(HANDLE output, int width,
    int height, const Relay_Game *game)
{
    const int x = (width - 38) / 2;
    const int y = (height - 8) / 2;

    return x >= 0 && y >= 0 && y + 7 < height &&
        relay_terminal_write_at(output, x, y,
            "╭────────────────────────────────────╮") &&
        relay_terminal_draw_overlay_line(output, x, y + 1, "Save failed") &&
        relay_terminal_draw_overlay_line(output, x, y + 2,
            game->session_status) &&
        relay_terminal_draw_overlay_line(output, x, y + 3, "") &&
        relay_terminal_draw_overlay_line(output, x, y + 4,
            "Enter: retry save") &&
        relay_terminal_draw_overlay_line(output, x, y + 5,
            game->session_exit_after_save ?
                "X: exit without saving" : "") &&
        relay_terminal_draw_overlay_line(output, x, y + 6, "Esc: cancel") &&
        relay_terminal_write_at(output, x, y + 7,
            "╰────────────────────────────────────╯");
}

bool relay_terminal_draw(const Relay_Terminal *terminal, const Relay_Game *game,
    const Relay_SessionStore *sessions, Relay_TerminalOverlay overlay)
{
    CONSOLE_SCREEN_BUFFER_INFO information;
    int width;
    int height;

    if (terminal == NULL || game == NULL || sessions == NULL ||
        !terminal->initialized ||
        !GetConsoleScreenBufferInfo(terminal->output, &information) ||
        !relay_terminal_write(terminal->output, "\x1b[2J\x1b[H")) {
        return false;
    }
    width = information.srWindow.Right - information.srWindow.Left + 1;
    height = information.srWindow.Bottom - information.srWindow.Top + 1;
    if (overlay == RELAY_TERMINAL_OVERLAY_SESSION_MENU) {
        return relay_terminal_draw_session_overlay(terminal->output, width,
            height, game);
    }
    if (overlay == RELAY_TERMINAL_OVERLAY_SLOT_SELECT) {
        return relay_terminal_draw_slot_overlay(terminal->output, width,
            height, sessions);
    }
    if (overlay == RELAY_TERMINAL_OVERLAY_EXIT_CONFIRM &&
        game->session_exit_from_menu) {
        return relay_terminal_draw_exit_overlay(terminal->output, width,
            height, game);
    }
    if (!relay_terminal_draw_split(terminal->output, width, height, game,
            terminal)) {
        return false;
    }
    if (overlay == RELAY_TERMINAL_OVERLAY_SAVE_SELECT) {
        return relay_terminal_draw_save_overlay(terminal->output, width,
            height, game, sessions);
    }
    if (overlay == RELAY_TERMINAL_OVERLAY_EXIT_CONFIRM) {
        return relay_terminal_draw_exit_overlay(terminal->output, width,
            height, game);
    }
    if (overlay == RELAY_TERMINAL_OVERLAY_SAVE_FAILED) {
        return relay_terminal_draw_save_failed_overlay(terminal->output, width,
            height, game);
    }
    return true;
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
    node = relay_node_world_find_const(relay_game_active_world_const(game),
        node_id);
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
    for (index = relay_game_active_world_const(game)->count; index > 0;
            index--) {
        const Relay_Node *node =
            &relay_game_active_world_const(game)->nodes[index - 1];
        const Relay_NodeRenderCard card = relay_node_renderer_card(node);
        const int x = 2 + node->grid_x + terminal->grid_offset_x;
        const int y = 3 + node->grid_y + terminal->grid_offset_y;

        if (relay_terminal_node_is_visible(node) &&
            mouse_x > x && mouse_x < x + card.width - 1 &&
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
    for (index = relay_game_active_world_const(game)->count; index > 0;
            index--) {
        const Relay_Node *node =
            &relay_game_active_world_const(game)->nodes[index - 1];
        const Relay_NodeRenderCard card = relay_node_renderer_card(node);
        const int x = 2 + node->grid_x + terminal->grid_offset_x;
        const int y = 3 + node->grid_y + terminal->grid_offset_y;
        const int row = mouse_y - (y + 3);

        if (relay_terminal_node_is_visible(node) &&
            card.definition != NULL && row >= 0 &&
            row < (int)card.definition->input_count && mouse_x >= x &&
            mouse_x < x + card.width / 2) {
            return (Relay_TerminalPortHit){node->id, (size_t)row, false};
        }
        if (relay_terminal_node_is_visible(node) &&
            card.definition != NULL && row >= 0 &&
            row < (int)card.definition->output_count && mouse_x >=
            x + card.width / 2 && mouse_x < x + card.width) {
            return (Relay_TerminalPortHit){node->id, (size_t)row, true};
        }
    }
    return (Relay_TerminalPortHit){0};
}

bool relay_terminal_poll(Relay_Terminal *terminal, const Relay_Game *game,
    const Relay_SessionStore *sessions, Relay_TerminalOverlay overlay,
    Relay_TerminalEvent *event)
{
    INPUT_RECORD record;
    CONSOLE_SCREEN_BUFFER_INFO information;
    DWORD count;

    if (terminal == NULL || game == NULL || sessions == NULL ||
        event == NULL || !terminal->initialized) {
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
        } else if (record.Event.KeyEvent.wVirtualKeyCode == VK_LEFT) {
            event->key = RELAY_TERMINAL_KEY_LEFT;
        } else if (record.Event.KeyEvent.wVirtualKeyCode == VK_RIGHT) {
            event->key = RELAY_TERMINAL_KEY_RIGHT;
        } else if (record.Event.KeyEvent.wVirtualKeyCode == VK_TAB) {
            event->key = RELAY_TERMINAL_KEY_TAB;
        } else if (record.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) {
            event->key = RELAY_TERMINAL_KEY_CONFIRM;
        } else if (record.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE) {
            event->key = RELAY_TERMINAL_KEY_ESCAPE;
        } else if (record.Event.KeyEvent.wVirtualKeyCode == VK_BACK) {
            event->key = RELAY_TERMINAL_KEY_BACKSPACE;
        } else if (record.Event.KeyEvent.wVirtualKeyCode == VK_DELETE) {
            event->key = RELAY_TERMINAL_KEY_DELETE;
        } else if (record.Event.KeyEvent.wVirtualKeyCode == VK_HOME) {
            event->key = RELAY_TERMINAL_KEY_HOME;
        } else if (record.Event.KeyEvent.wVirtualKeyCode == VK_END) {
            event->key = RELAY_TERMINAL_KEY_END;
        } else if (record.Event.KeyEvent.wVirtualKeyCode == 'S' &&
            (record.Event.KeyEvent.dwControlKeyState &
                (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0) {
            event->key = RELAY_TERMINAL_KEY_SAVE;
        }
    } else if (record.EventType == MOUSE_EVENT) {
        const MOUSE_EVENT_RECORD *mouse = &record.Event.MouseEvent;
        const bool left_down = (mouse->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;

        event->type = RELAY_TERMINAL_EVENT_MOUSE;
        event->mouse_x = mouse->dwMousePosition.X;
        event->mouse_y = mouse->dwMousePosition.Y;
        if (overlay == RELAY_TERMINAL_OVERLAY_SESSION_MENU && left_down &&
            GetConsoleScreenBufferInfo(terminal->output, &information)) {
            event->session_menu_index_plus_one =
                relay_terminal_session_menu_at(
                    information.srWindow.Right -
                        information.srWindow.Left + 1,
                    information.srWindow.Bottom -
                        information.srWindow.Top + 1,
                    event->mouse_x, event->mouse_y);
        }
        if (overlay != RELAY_TERMINAL_OVERLAY_NONE) {
            terminal->dragging_grid = false;
            terminal->dragging_node = false;
            terminal->wiring = false;
            terminal->dragged_node_id = 0;
            return true;
        }
        if (left_down && !terminal->dragging_grid && !terminal->dragging_node &&
            !terminal->wiring && GetConsoleScreenBufferInfo(terminal->output,
                &information) && relay_terminal_workspace_tab_at(game,
                information.srWindow.Right - information.srWindow.Left + 1,
                information.srWindow.Bottom - information.srWindow.Top + 1,
                event->mouse_x, event->mouse_y) != 0) {
            event->workspace_tab_index_plus_one =
                relay_terminal_workspace_tab_at(game,
                    information.srWindow.Right - information.srWindow.Left + 1,
                    information.srWindow.Bottom - information.srWindow.Top + 1,
                    event->mouse_x, event->mouse_y);
            return true;
        }
        if (game->editing_blueprint_id != 0) {
            return true;
        }
        if (left_down && !terminal->dragging_grid && !terminal->dragging_node &&
            !terminal->wiring && GetConsoleScreenBufferInfo(terminal->output,
                &information) && relay_terminal_panel_tab_at(
                information.srWindow.Right - information.srWindow.Left + 1,
                information.srWindow.Bottom - information.srWindow.Top + 1,
                event->mouse_x, event->mouse_y)) {
            event->panel_tab_index_plus_one = relay_terminal_panel_tab_at(
                information.srWindow.Right - information.srWindow.Left + 1,
                information.srWindow.Bottom - information.srWindow.Top + 1,
                event->mouse_x, event->mouse_y);
            return true;
        } else if (left_down && !terminal->dragging_grid &&
            !terminal->dragging_node && !terminal->wiring) {
            const Relay_TerminalPortHit hit = relay_terminal_port_at(game, terminal,
                event->mouse_x, event->mouse_y);

            if (hit.node_id != 0) {
                terminal->wiring = true;
                terminal->wiring_source_node_id = hit.node_id;
                terminal->wiring_source_port_index = hit.port_index;
                terminal->wiring_origin_is_output = hit.is_output;
                terminal->wiring_mouse_x = event->mouse_x;
                terminal->wiring_mouse_y = event->mouse_y;
                return true;
            }
            terminal->dragged_node_id = relay_terminal_node_at(game, terminal,
                event->mouse_x, event->mouse_y);
            terminal->dragging_node = terminal->dragged_node_id != 0;
            event->selected_node_id = terminal->dragged_node_id;
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

                if (hit.node_id != 0 && hit.is_output !=
                    terminal->wiring_origin_is_output) {
                    if (terminal->wiring_origin_is_output) {
                        event->connection_source_node_id = terminal->wiring_source_node_id;
                        event->connection_source_port_index = terminal->wiring_source_port_index;
                        event->connection_destination_node_id = hit.node_id;
                        event->connection_destination_port_index = hit.port_index;
                    } else {
                        event->connection_source_node_id = hit.node_id;
                        event->connection_source_port_index = hit.port_index;
                        event->connection_destination_node_id = terminal->wiring_source_node_id;
                        event->connection_destination_port_index = terminal->wiring_source_port_index;
                    }
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

/** Convert a renderer-owned fixed port visual into a Termbox foreground color. */
static uintattr_t relay_terminal_port_color(Relay_NodePortType type)
{
    switch (relay_node_renderer_port_visual(type).color) {
    case 3: return TB_CYAN | TB_BOLD;
    case 1: return TB_YELLOW | TB_BOLD;
    case 2: return TB_WHITE | TB_BOLD;
    case 6: return TB_RED | TB_BOLD;
    case 4: return TB_WHITE | TB_DIM;
    case 5: return TB_MAGENTA | TB_BOLD;
    case 7: return TB_BLUE | TB_BOLD;
    case 8: return TB_GREEN | TB_BOLD;
    default: return TB_RED | TB_BOLD;
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

    for (index = 0; index <
            relay_game_active_world_const(game)->connection_count; index++) {
        Relay_TerminalWireRoute route;
        Relay_TerminalWirePath path;

        if (relay_terminal_wire_route(game, terminal,
                &relay_game_active_world_const(game)->connections[index],
                &route)) {
            relay_terminal_wire_path_create(route.source_x, route.source_y,
                route.source_top, route.source_bottom, route.destination_x,
                route.destination_y, route.destination_top, route.destination_bottom,
                &path);
            relay_terminal_draw_wire_path(&path, divider_x, height);
        }
    }
    if (terminal->wiring) {
        const Relay_Node *source = relay_node_world_find_const(
            relay_game_active_world_const(game),
            terminal->wiring_source_node_id);
        const Relay_NodeRenderCard card = relay_node_renderer_card(source);
        Relay_TerminalWirePath path;
        int source_x;
        int source_y;
        int source_top;

        if (source != NULL) {
            source_top = 3 + (int)source->grid_y + (int)terminal->grid_offset_y;
        }
        if (source != NULL && terminal->wiring_origin_is_output &&
            relay_terminal_output_anchor(game, terminal,
                source->id, terminal->wiring_source_port_index, &source_x,
                &source_y)) {
            relay_terminal_wire_path_create(source_x, source_y, source_top,
                source_top + card.height - 1, terminal->wiring_mouse_x,
                terminal->wiring_mouse_y, terminal->wiring_mouse_y,
                terminal->wiring_mouse_y, &path);
            relay_terminal_draw_wire_path(&path, divider_x, height);
        } else if (source != NULL && relay_terminal_input_anchor(game, terminal,
                source->id, terminal->wiring_source_port_index, &source_x,
                &source_y)) {
            relay_terminal_wire_path_create(terminal->wiring_mouse_x,
                terminal->wiring_mouse_y, terminal->wiring_mouse_y,
                terminal->wiring_mouse_y, source_x, source_y, source_top,
                source_top + card.height - 1, &path);
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

    for (index = 0; index <
            relay_game_active_world_const(game)->connection_count; index++) {
        const Relay_NodeConnection *connection =
            &relay_game_active_world_const(game)->connections[index];
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
    char input_content[20];
    char output_content[20];
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

        if (input != NULL && relay_node_port_type_is_item(input->type)) {
            (void)snprintf(input_content, sizeof(input_content), "%.7s[%zu]",
                input->display_name, node->input_queues[row].count);
        } else {
            (void)snprintf(input_content, sizeof(input_content), "%s",
                input == NULL ? "" : input->display_name);
        }
        if (output != NULL && relay_node_port_type_is_item(output->type)) {
            (void)snprintf(output_content, sizeof(output_content), "%.9s[%zu]",
                output->display_name, node->output_queues[row].count);
        } else {
            (void)snprintf(output_content, sizeof(output_content), "%s",
                output == NULL ? "" : output->display_name);
        }
        (void)snprintf(content, sizeof(content), "%-10.10s%12.12s",
            input_content, output_content);
        (void)tb_printf(x, y + 3 + (int)row,
            relay_terminal_node_color(card.visual.color), TB_DEFAULT,
            "%s %-22.22s %s", input == NULL ? "│" : "●", content,
            output == NULL ? "│" : "●");
        if (input != NULL) {
            (void)tb_print(x, y + 3 + (int)row,
                relay_terminal_port_color(input->type), TB_DEFAULT, "●");
        }
        if (output != NULL) {
            (void)tb_print(x + card.width - 1, y + 3 + (int)row,
                relay_terminal_port_color(output->type), TB_DEFAULT, "●");
        }
    }
    if (card.definition->simulation.behavior ==
            RELAY_NODE_BEHAVIOR_FIXED_RATE_SOURCE) {
        const int64_t interval = card.definition->simulation.interval_steps;
        const int filled = (int)(node->progress * 10 /
            interval);

        (void)snprintf(content, sizeof(content), "[%.*s%.*s] %2lld/%lld",
            filled, "##########", 10 - filled, "..........",
            (long long)node->progress, (long long)interval);
    } else if (card.definition->simulation.behavior ==
            RELAY_NODE_BEHAVIOR_TIMER) {
        (void)snprintf(content, sizeof(content), "interval: %lld s",
            (long long)(node->timer_interval_steps /
                RELAY_GAME_SIMULATION_STEPS_PER_SECOND));
    } else if (node->runtime_kind ==
            RELAY_NODE_RUNTIME_BLUEPRINT_INPUT_BOUNDARY) {
        (void)snprintf(content, sizeof(content), "public interface");
    } else if (node->runtime_kind ==
            RELAY_NODE_RUNTIME_BLUEPRINT_OUTPUT_BOUNDARY) {
        (void)snprintf(content, sizeof(content), "public interface");
    } else if (node->runtime_kind ==
            RELAY_NODE_RUNTIME_BLUEPRINT_PROCESS) {
        (void)snprintf(content, sizeof(content), "Lua process");
    } else if (node->runtime_kind ==
            RELAY_NODE_RUNTIME_BLUEPRINT_WRAPPER) {
        (void)snprintf(content, sizeof(content), "module instance");
    } else {
        (void)snprintf(content, sizeof(content), "enabled");
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

/** Draw the active blueprint source editor in the Termbox backend. */
static uintattr_t relay_terminal_editor_color(Relay_ScriptTokenKind kind)
{
    switch (kind) {
    case RELAY_SCRIPT_TOKEN_KEYWORD: return TB_MAGENTA | TB_BOLD;
    case RELAY_SCRIPT_TOKEN_STRING: return TB_GREEN;
    case RELAY_SCRIPT_TOKEN_NUMBER: return TB_YELLOW;
    case RELAY_SCRIPT_TOKEN_COMMENT: return TB_BLACK | TB_BOLD;
    case RELAY_SCRIPT_TOKEN_FUNCTION: return TB_CYAN | TB_BOLD;
    case RELAY_SCRIPT_TOKEN_TYPE: return TB_CYAN;
    case RELAY_SCRIPT_TOKEN_NAMESPACE: return TB_YELLOW | TB_BOLD;
    case RELAY_SCRIPT_TOKEN_MEMBER: return TB_BLUE | TB_BOLD;
    case RELAY_SCRIPT_TOKEN_OPERATOR: return TB_WHITE;
    case RELAY_SCRIPT_TOKEN_DEFAULT: return TB_WHITE;
    }
    return TB_WHITE;
}

/** Draw syntax overlays and language assistance for the Termbox editor. */
static void relay_terminal_draw_editor(int divider_x, int height,
    const Relay_Game *game, const Relay_Blueprint *blueprint)
{
    const size_t visible_lines = height > 4 ? (size_t)(height - 4) : 0;
    const size_t text_capacity = divider_x > 9 ?
        (size_t)(divider_x - 9) : 0;
    const Relay_TerminalEditorView view = relay_terminal_editor_view(blueprint,
        visible_lines, text_capacity);
    Relay_ScriptToken tokens[RELAY_SCRIPT_LANGUAGE_MAX_TOKENS];
    const size_t token_count = relay_script_language_tokenize(
        blueprint->source, blueprint->source_size, tokens,
        RELAY_SCRIPT_LANGUAGE_MAX_TOKENS);
    size_t row;

    for (row = 0; row < visible_lines; row++) {
        const size_t line_number = view.first_line + row;
        size_t start;
        size_t length;

        if (!relay_terminal_editor_line(blueprint, line_number, &start,
                &length)) {
            break;
        }
        if (view.first_column < length) {
            start += view.first_column;
            length -= view.first_column;
        } else {
            length = 0;
        }
        if (length > text_capacity) {
            length = text_capacity;
        }
        (void)tb_printf(1, 2 + (int)row, TB_WHITE, TB_DEFAULT,
            "%4zu │ %.*s", line_number + 1, (int)length,
            &blueprint->source[start]);
        {
            size_t column;

            for (column = 0; column < length; column++) {
                const Relay_ScriptTokenKind kind =
                    relay_terminal_editor_token_at(tokens, token_count,
                        start + column);

                (void)tb_set_cell(8 + (int)column, 2 + (int)row,
                    (uint32_t)(unsigned char)blueprint->source[start + column],
                    relay_terminal_editor_color(kind), TB_DEFAULT);
            }
        }
    }
    if (view.cursor_line >= view.first_line &&
        view.cursor_line < view.first_line + visible_lines) {
        const int cursor_x = 8 +
            (int)(view.cursor_column - view.first_column);
        const int cursor_y = 2 + (int)(view.cursor_line - view.first_line);
        const uint32_t character =
            blueprint->cursor < blueprint->source_size &&
            blueprint->source[blueprint->cursor] != '\n' ?
                (uint32_t)(unsigned char)blueprint->source[blueprint->cursor] :
                (uint32_t)' ';

        if (cursor_x < divider_x) {
            (void)tb_set_cell(cursor_x, cursor_y, character, TB_BLACK | TB_BOLD,
                TB_CYAN);
        }
    }
    if (blueprint->editor_mode == RELAY_BLUEPRINT_EDITOR_INSERT &&
        divider_x >= 46) {
        Relay_ScriptCompletion completions[
            RELAY_SCRIPT_LANGUAGE_MAX_COMPLETIONS];
        Relay_ScriptSignature signature;
        size_t replacement_start;
        const size_t count = blueprint->completion_suppressed ? 0 :
            relay_game_editor_completions(game, completions,
                RELAY_SCRIPT_LANGUAGE_MAX_COMPLETIONS, &replacement_start);
        const size_t selected = count == 0 ? 0 :
            blueprint->completion_selection % count;
        const size_t first = selected < 5 ? 0 : selected - 4;
        int popup_y = 3 + (int)(view.cursor_line - view.first_line);
        size_t index;

        (void)replacement_start;
        if (relay_script_language_signature(blueprint->source,
                blueprint->source_size, blueprint->cursor, &signature) &&
            popup_y < height - 2) {
            (void)tb_printf(8, popup_y++, TB_BLACK | TB_BOLD, TB_CYAN,
                " %.27s  p%zu ", signature.label,
                signature.active_parameter + 1);
        }
        for (index = first; index < count && index < first + 5 &&
                popup_y < height - 2; index++, popup_y++) {
            const bool item_selected = index == selected;

            (void)tb_printf(8, popup_y,
                item_selected ? TB_BLACK | TB_BOLD : TB_WHITE,
                item_selected ? TB_WHITE : TB_BLACK,
                " %-13.13s %-21.21s ", completions[index].label,
                completions[index].detail);
        }
    }
}

/** Draw Relay and every open Blueprint as persistent Termbox workspace tabs. */
static void relay_terminal_draw_workspace_tabs(int divider_x,
    const Relay_Game *game)
{
    int tab_x = 1;
    size_t index;

    (void)tb_printf(tab_x, 0,
        game->active_workspace == 0 ? TB_CYAN | TB_BOLD : TB_WHITE,
        TB_DEFAULT, "[Relay]");
    tab_x += (int)strlen("Relay") + 3;
    for (index = 0; index < game->blueprints.count; index++) {
        const Relay_Blueprint *blueprint = &game->blueprints.blueprints[index];
        const int tab_width = (int)strlen(blueprint->name) + 2;

        if (!blueprint->workspace_open) {
            continue;
        }
        if (tab_x + tab_width > divider_x) {
            break;
        }
        (void)tb_printf(tab_x, 0,
            game->active_workspace == index + 1 ? TB_CYAN | TB_BOLD : TB_WHITE,
            TB_DEFAULT, "[%s]", blueprint->name);
        tab_x += tab_width + 1;
    }
}

/** Render Relay's playfield and right-side control panel. */
static void relay_terminal_draw_split(int width, int height,
    const Relay_Game *game, const Relay_Terminal *terminal)
{
    int divider_x;
    int y;
    size_t index;
    const Relay_Node *focused;
    const Relay_Blueprint *editing = game->editing_blueprint_id == 0 ? NULL :
        relay_blueprint_library_find_const(&game->blueprints,
            game->editing_blueprint_id);

    if (!relay_terminal_split(width, height, &divider_x)) {
        relay_terminal_draw_text(0, 0, TB_RED | TB_BOLD,
            "Terminal is too small for the Relay layout.");
        return;
    }
    for (y = 0; y < height; y++) {
        (void)tb_set_cell(divider_x, y, '|', TB_BLACK, TB_WHITE);
    }
    relay_terminal_draw_workspace_tabs(divider_x, game);
    if (editing != NULL) {
        relay_terminal_draw_editor(divider_x, height, game, editing);
    } else {
        relay_terminal_draw_grid(divider_x, height, terminal);
    }
    if (editing == NULL &&
        game->workspace_mode == RELAY_GAME_WORKSPACE_GRAPH) {
        relay_terminal_draw_wires(game, terminal, divider_x, height);
    }
    relay_terminal_draw_text(divider_x + 2, 1, TB_CYAN | TB_BOLD,
        editing != NULL ? "[ CODE ]" :
        game->active_tab == RELAY_GAME_PANEL_TAB_SHOP ?
            "[SHOP] [Inspect] [Scripts]" :
        game->active_tab == RELAY_GAME_PANEL_TAB_INSPECTOR ?
            "[Shop] [INSPECT] [Scripts]" :
            "[Shop] [Inspect] [SCRIPTS]");
    if (editing != NULL) {
        (void)tb_printf(divider_x + 2, 3, TB_WHITE | TB_BOLD, TB_DEFAULT,
            "revision: %llu%s", (unsigned long long)editing->revision,
            editing->dirty ? "  modified" : "");
        (void)tb_printf(divider_x + 2, 4, TB_WHITE, TB_DEFAULT, "mode: %s",
            relay_terminal_editor_mode_label(editing->editor_mode));
        (void)tb_printf(divider_x + 2, 5,
            editing->dirty ? TB_YELLOW : TB_GREEN, TB_DEFAULT, "%.36s",
            editing->diagnostic.message);
        if (editing->editor_mode == RELAY_BLUEPRINT_EDITOR_COMMAND) {
            (void)tb_printf(divider_x + 2, 7, TB_CYAN | TB_BOLD, TB_DEFAULT,
                ":%s", editing->editor_command);
        }
    } else {
        (void)tb_printf(divider_x + 2, 3, TB_YELLOW | TB_BOLD, TB_DEFAULT,
            "◈ %llu", (unsigned long long)game->currency);
        (void)tb_printf(divider_x + 2, 4, TB_WHITE, TB_DEFAULT, "wires: %zu",
            relay_game_active_world_const(game)->connection_count);
    }
    if (editing == NULL &&
        game->active_tab == RELAY_GAME_PANEL_TAB_SHOP) {
        for (index = 0; index < relay_game_shop_offer_count() &&
            6 + (int)index < height - 2; index++) {
            const Relay_ShopOffer *offer = relay_game_shop_offer_at(index);
            const Relay_NodeDefinition *definition = relay_game_shop_offer_definition(offer);

            (void)tb_printf(divider_x + 2, 6 + (int)index,
                index == game->selected_offer ? TB_GREEN | TB_BOLD : TB_WHITE,
                TB_DEFAULT, "%c %s %llu", index == game->selected_offer ? '>' : ' ',
                definition->display_name, (unsigned long long)offer->price);
        }
    } else if (editing == NULL &&
        game->active_tab == RELAY_GAME_PANEL_TAB_INSPECTOR) {
        focused = relay_node_world_find_const(relay_game_active_world_const(game),
            game->focused_node_id);
        if (focused == NULL) {
            relay_terminal_draw_text(divider_x + 2, 6, TB_WHITE,
                "No node selected.");
        } else {
            const Relay_NodeDefinition *definition =
                relay_node_definition_for(focused);

            if (definition == NULL) {
                relay_terminal_draw_text(divider_x + 2, 6, TB_RED,
                    "Node definition unavailable.");
                return;
            }
            (void)tb_printf(divider_x + 2, 6, TB_WHITE | TB_BOLD, TB_DEFAULT,
                "%s", definition->display_name);
            (void)tb_printf(divider_x + 2, 7, TB_WHITE, TB_DEFAULT, "id: %llu",
                (unsigned long long)focused->id);
            if (definition->simulation.behavior ==
                    RELAY_NODE_BEHAVIOR_TIMER) {
                char intervals[64];

                relay_terminal_draw_text(divider_x + 2, 9, TB_YELLOW | TB_BOLD,
                    "Timer");
                relay_terminal_draw_text(divider_x + 2, 10, TB_WHITE,
                    "output: Trigger");
                (void)tb_printf(divider_x + 2, 11, TB_WHITE, TB_DEFAULT,
                    "interval: %lld seconds",
                    (long long)(focused->timer_interval_steps /
                        RELAY_GAME_SIMULATION_STEPS_PER_SECOND));
                relay_terminal_timer_intervals(intervals, sizeof(intervals));
                relay_terminal_draw_text(divider_x + 2, 12, TB_WHITE, intervals);
                relay_terminal_draw_text(divider_x + 2, 13, TB_YELLOW,
                    "[ / ] change interval");
            } else if (definition->simulation.behavior ==
                    RELAY_NODE_BEHAVIOR_FIXED_RATE_SOURCE) {
                const Relay_NodeSimulationDefinition *simulation =
                    &definition->simulation;
                const Relay_NodePortDefinition *source_output =
                    &definition->outputs[simulation->output_port_index];

                (void)tb_printf(divider_x + 2, 9, TB_WHITE, TB_DEFAULT,
                    "output: %s", source_output->display_name);
                (void)tb_printf(divider_x + 2, 10, TB_WHITE, TB_DEFAULT,
                    "rate: %lld / second",
                    (long long)simulation->output_amount);
                (void)tb_printf(divider_x + 2, 11, TB_WHITE, TB_DEFAULT,
                    "progress: %lld / %u", (long long)focused->progress,
                    simulation->interval_steps);
                (void)tb_printf(divider_x + 2, 12, TB_WHITE, TB_DEFAULT,
                    "produced: %lld", (long long)focused->produced);
            } else if (focused->runtime_kind ==
                    RELAY_NODE_RUNTIME_BLUEPRINT_INPUT_BOUNDARY) {
                relay_terminal_draw_text(divider_x + 2, 9, TB_CYAN | TB_BOLD,
                    "Public module inputs");
                relay_terminal_draw_text(divider_x + 2, 10, TB_WHITE,
                    "Wire outputs to components.");
                relay_terminal_draw_text(divider_x + 2, 12, TB_WHITE,
                    "VHDL: entity input ports");
            } else if (focused->runtime_kind ==
                    RELAY_NODE_RUNTIME_BLUEPRINT_OUTPUT_BOUNDARY) {
                relay_terminal_draw_text(divider_x + 2, 9, TB_CYAN | TB_BOLD,
                    "Public module outputs");
                relay_terminal_draw_text(divider_x + 2, 10, TB_WHITE,
                    "Wire components into inputs.");
                relay_terminal_draw_text(divider_x + 2, 12, TB_WHITE,
                    "VHDL: entity output ports");
            } else if (focused->runtime_kind ==
                    RELAY_NODE_RUNTIME_BLUEPRINT_PROCESS) {
                relay_terminal_draw_text(divider_x + 2, 9,
                    TB_MAGENTA | TB_BOLD, "Player Lua process");
                relay_terminal_draw_text(divider_x + 2, 10, TB_WHITE,
                    "Ports mirror this module.");
                relay_terminal_draw_text(divider_x + 2, 12, TB_YELLOW,
                    "E: edit source");
            } else if (focused->blueprint_id != 0) {
                const Relay_Blueprint *blueprint =
                    relay_blueprint_library_find_const(&game->blueprints,
                        focused->blueprint_id);

                if (blueprint != NULL) {
                    (void)tb_printf(divider_x + 2, 9, TB_WHITE, TB_DEFAULT,
                        "key: %s", blueprint->key);
                    (void)tb_printf(divider_x + 2, 10, TB_WHITE, TB_DEFAULT,
                        "revision: %llu",
                        (unsigned long long)blueprint->compiled_revision);
                    relay_terminal_draw_text(divider_x + 2, 12, TB_WHITE,
                        "Reusable module instance");
                    relay_terminal_draw_text(divider_x + 2, 13, TB_YELLOW,
                        "E: edit source");
                }
            }
            {
                int queue_y = 15;
                size_t port_index;

                for (port_index = 0; port_index < definition->input_count &&
                        queue_y < height - 5; port_index++) {
                    if (!relay_node_port_type_is_item(
                            definition->inputs[port_index].type)) {
                        continue;
                    }
                    (void)tb_printf(divider_x + 2, queue_y++, TB_WHITE,
                        TB_DEFAULT, "in  %s: %zu/%d",
                        definition->inputs[port_index].key,
                        focused->input_queues[port_index].count,
                        RELAY_ITEM_QUEUE_CAPACITY);
                }
                for (port_index = 0; port_index < definition->output_count &&
                        queue_y < height - 5; port_index++) {
                    if (!relay_node_port_type_is_item(
                            definition->outputs[port_index].type)) {
                        continue;
                    }
                    (void)tb_printf(divider_x + 2, queue_y++, TB_WHITE,
                        TB_DEFAULT, "out %s: %zu/%d",
                        definition->outputs[port_index].key,
                        focused->output_queues[port_index].count,
                        RELAY_ITEM_QUEUE_CAPACITY);
                }
            }
        }
    } else if (editing == NULL) {
        for (index = 0; index < game->blueprints.count &&
            6 + (int)index < height - 2; index++) {
            const Relay_Blueprint *blueprint =
                &game->blueprints.blueprints[index];

            (void)tb_printf(divider_x + 2, 6 + (int)index,
                index == game->selected_blueprint ? TB_GREEN | TB_BOLD :
                    TB_WHITE,
                TB_DEFAULT, "%c %s",
                index == game->selected_blueprint ? '>' : ' ',
                blueprint->name);
        }
    }
    for (index = 0; editing == NULL &&
        index < relay_game_active_world_const(game)->count; index++) {
        const Relay_Node *node =
            &relay_game_active_world_const(game)->nodes[index];
        if (!relay_terminal_node_is_visible(node)) {
            continue;
        }
        if (game->workspace_mode == RELAY_GAME_WORKSPACE_MAP) {
            relay_terminal_draw_node_map(node, terminal, divider_x, height);
        } else {
            relay_terminal_draw_node_graph(node, terminal, divider_x, height);
        }
    }
    (void)tb_printf(divider_x + 2, height - 4, TB_GREEN, TB_DEFAULT,
        "session: %.40s", game->session_status);
    relay_terminal_draw_text(divider_x + 2, height - 3, TB_YELLOW,
        editing != NULL &&
            editing->editor_mode == RELAY_BLUEPRINT_EDITOR_INSERT ?
                "INSERT  Esc: normal" :
        editing != NULL &&
            editing->editor_mode == RELAY_BLUEPRINT_EDITOR_COMMAND ?
                ":w deploy  :wq deploy/back" :
        editing != NULL ? "NORMAL  i/a/o insert  : command" :
            "N:new ,/.:tab C:close E:edit S:save");
    relay_terminal_draw_text(divider_x + 2, height - 2, TB_YELLOW,
        editing != NULL &&
            editing->editor_mode == RELAY_BLUEPRINT_EDITOR_INSERT ?
                "Tab complete  Enter newline" :
        editing != NULL &&
            editing->editor_mode == RELAY_BLUEPRINT_EDITOR_COMMAND ?
                "Enter: run  Esc: cancel" :
        editing != NULL ? "hjkl move  x delete  Esc back" :
        game->active_tab == RELAY_GAME_PANEL_TAB_SHOP ?
            "j/k: select  Enter: buy" :
        game->active_tab == RELAY_GAME_PANEL_TAB_BLUEPRINTS ?
            "j/k pick  O:open  Enter:add" :
        game->active_workspace != 0 ?
            "Inputs -> components -> Outputs" :
            "click a title to inspect");
}

/** Draw one padded line in a centered Termbox overlay. */
static void relay_terminal_draw_overlay_line(int x, int y, uintattr_t color,
    const char *text)
{
    (void)tb_printf(x, y, color, TB_DEFAULT, "│ %-34.34s │", text);
}

/** Draw one fixed-width line inside the dedicated startup panel. */
static void relay_terminal_draw_start_line(int x, int y, uintattr_t color,
    const char *text)
{
    (void)tb_printf(x, y, color, TB_DEFAULT, "│ %-42.42s │", text);
}

/** Draw Relay's centered title treatment on an otherwise empty screen. */
static bool relay_terminal_draw_start_title(int width, int height,
    int *origin_x, int *origin_y)
{
    int x;
    int y;

    if (!relay_terminal_start_layout(width, height, &x, &y)) {
        (void)tb_print(0, 0, TB_RED | TB_BOLD, TB_DEFAULT,
            "Relay needs a terminal at least 46 x 18.");
        return false;
    }
    *origin_x = x;
    *origin_y = y;
    (void)tb_print(x + 5, y, TB_CYAN | TB_BOLD, TB_DEFAULT,
        " ____  _____ _        _ __   __");
    (void)tb_print(x + 5, y + 1, TB_CYAN | TB_BOLD, TB_DEFAULT,
        "|  _ \\| ____| |      / \\ \\ / /");
    (void)tb_print(x + 5, y + 2, TB_CYAN | TB_BOLD, TB_DEFAULT,
        "| |_) |  _| | |     / _ \\ V /");
    (void)tb_print(x + 5, y + 3, TB_CYAN | TB_BOLD, TB_DEFAULT,
        "|  _ <| |___| |___ / ___ \\| |");
    (void)tb_print(x + 5, y + 4, TB_CYAN | TB_BOLD, TB_DEFAULT,
        "|_| \\_\\_____|_____/_/   \\_\\_|");
    (void)tb_print(x + 10, y + 5, TB_YELLOW | TB_BOLD, TB_DEFAULT,
        "TERMINAL AUTOMATION NETWORK");
    return true;
}

/** Draw the persistent four-action startup main menu. */
static void relay_terminal_draw_session_overlay(int width, int height,
    const Relay_Game *game)
{
    int x;
    int y;
    char continue_option[48];
    char new_option[48];
    char saved_option[48];
    char exit_option[48];

    if (!relay_terminal_draw_start_title(width, height, &x, &y)) {
        return;
    }
    (void)snprintf(continue_option, sizeof(continue_option), "%c %s",
        game->session_menu_selection == RELAY_GAME_SESSION_MENU_CONTINUE ?
            '>' : ' ',
        game->session_continue_available ?
            "Continue" : "Continue (unavailable)");
    (void)snprintf(new_option, sizeof(new_option), "%c New Session",
        game->session_menu_selection == RELAY_GAME_SESSION_MENU_NEW ?
            '>' : ' ');
    (void)snprintf(saved_option, sizeof(saved_option), "%c Saved Sessions",
        game->session_menu_selection == RELAY_GAME_SESSION_MENU_SAVED ?
            '>' : ' ');
    (void)snprintf(exit_option, sizeof(exit_option), "%c Exit",
        game->session_menu_selection == RELAY_GAME_SESSION_MENU_EXIT ?
            '>' : ' ');
    (void)tb_print(x, y + 7, TB_CYAN | TB_BOLD, TB_DEFAULT,
        "╭────────────────────────────────────────────╮");
    relay_terminal_draw_start_line(x, y + 8, TB_WHITE | TB_BOLD, "MAIN MENU");
    (void)tb_print(x, y + 9, TB_CYAN | TB_BOLD, TB_DEFAULT,
        "├────────────────────────────────────────────┤");
    relay_terminal_draw_start_line(x, y + 10,
        game->session_menu_selection == RELAY_GAME_SESSION_MENU_CONTINUE &&
            game->session_continue_available ?
            TB_GREEN | TB_BOLD :
            game->session_continue_available ? TB_WHITE : TB_BLUE,
        continue_option);
    relay_terminal_draw_start_line(x, y + 11,
        game->session_menu_selection == RELAY_GAME_SESSION_MENU_NEW ?
            TB_GREEN | TB_BOLD : TB_WHITE, new_option);
    relay_terminal_draw_start_line(x, y + 12,
        game->session_menu_selection == RELAY_GAME_SESSION_MENU_SAVED ?
            TB_GREEN | TB_BOLD : TB_WHITE, saved_option);
    relay_terminal_draw_start_line(x, y + 13,
        game->session_menu_selection == RELAY_GAME_SESSION_MENU_EXIT ?
            TB_GREEN | TB_BOLD : TB_WHITE, exit_option);
    relay_terminal_draw_start_line(x, y + 14, TB_WHITE, "");
    relay_terminal_draw_start_line(x, y + 15, TB_YELLOW,
        "j/k or arrows  Enter select  Mouse click");
    relay_terminal_draw_start_line(x, y + 16, TB_WHITE,
        game->session_status);
    (void)tb_print(x, y + 17, TB_CYAN | TB_BOLD, TB_DEFAULT,
        "╰────────────────────────────────────────────╯");
}

/** Draw a bounded browser for every discovered valid or invalid slot. */
static void relay_terminal_draw_slot_overlay(int width, int height,
    const Relay_SessionStore *sessions)
{
    int start_x;
    int start_y;
    int x;
    int y;
    const size_t first = sessions->selected_slot > 3 ?
        sessions->selected_slot - 3 : 0;
    size_t row;

    if (!relay_terminal_draw_start_title(width, height, &start_x,
            &start_y)) {
        return;
    }
    x = (width - 38) / 2;
    y = start_y + 7;
    (void)start_x;
    (void)tb_print(x, y, TB_CYAN | TB_BOLD, TB_DEFAULT,
        "╭────────────────────────────────────╮");
    relay_terminal_draw_overlay_line(x, y + 1, TB_WHITE | TB_BOLD,
        sessions->slot_count == 0 ?
            "Saved Sessions - none yet" : "Saved Sessions");
    for (row = 0; row < 5; row++) {
        const size_t index = first + row;
        char line[48] = "";

        if (index < sessions->slot_count) {
            const Relay_SessionSlot *slot = &sessions->slots[index];

            if (slot->valid) {
                (void)snprintf(line, sizeof(line), "%c %08llx  step %llu",
                    index == sessions->selected_slot ? '>' : ' ',
                    (unsigned long long)(slot->id & UINT64_C(0xffffffff)),
                    (unsigned long long)slot->simulation_step);
            } else {
                (void)snprintf(line, sizeof(line), "%c %08llx  [invalid]",
                    index == sessions->selected_slot ? '>' : ' ',
                    (unsigned long long)(slot->id & UINT64_C(0xffffffff)));
            }
        }
        relay_terminal_draw_overlay_line(x, y + 2 + (int)row,
            index == sessions->selected_slot ? TB_GREEN | TB_BOLD : TB_WHITE,
            line);
    }
    relay_terminal_draw_overlay_line(x, y + 7, TB_YELLOW,
        "j/k select  Enter load");
    relay_terminal_draw_overlay_line(x, y + 8, TB_WHITE, "Esc: back");
    (void)tb_print(x, y + 9, TB_CYAN | TB_BOLD, TB_DEFAULT,
        "╰────────────────────────────────────╯");
}

/** Draw overwrite versus new-slot choices for an explicit save. */
static void relay_terminal_draw_save_overlay(int width, int height,
    const Relay_Game *game, const Relay_SessionStore *sessions)
{
    const int x = (width - 38) / 2;
    const int y = (height - 8) / 2;
    const bool can_overwrite = sessions->active_session_id != 0;
    const bool can_create =
        sessions->slot_count < RELAY_SESSION_SLOT_CAPACITY;

    if (x < 0 || y < 0 || y + 7 >= height) {
        return;
    }
    (void)tb_print(x, y, TB_CYAN | TB_BOLD, TB_DEFAULT,
        "╭────────────────────────────────────╮");
    relay_terminal_draw_overlay_line(x, y + 1, TB_WHITE | TB_BOLD,
        "Save Session");
    relay_terminal_draw_overlay_line(x, y + 2, TB_WHITE, "");
    relay_terminal_draw_overlay_line(x, y + 3,
        !game->session_save_new_selected && can_overwrite ?
            TB_GREEN | TB_BOLD : TB_WHITE,
        !game->session_save_new_selected && can_overwrite ?
            "> Overwrite current slot" :
            can_overwrite ? "  Overwrite current slot" :
                "  Overwrite (unavailable)");
    relay_terminal_draw_overlay_line(x, y + 4,
        game->session_save_new_selected && can_create ?
            TB_GREEN | TB_BOLD : TB_WHITE,
        !can_create ? "  New slot (repository full)" :
            game->session_save_new_selected ?
                "> Save to new slot" : "  Save to new slot");
    relay_terminal_draw_overlay_line(x, y + 5, TB_YELLOW,
        "j/k select  Enter save");
    relay_terminal_draw_overlay_line(x, y + 6, TB_WHITE, "Esc: cancel");
    (void)tb_print(x, y + 7, TB_CYAN | TB_BOLD, TB_DEFAULT,
        "╰────────────────────────────────────╯");
}

/** Draw the centered save-and-exit confirmation dialog. */
static void relay_terminal_draw_exit_overlay(int width, int height,
    const Relay_Game *game)
{
    const int x = (width - 38) / 2;
    const int y = (height - 6) / 2;
    int start_x;
    int start_y;

    if (game->session_exit_from_menu &&
        !relay_terminal_draw_start_title(width, height, &start_x,
            &start_y)) {
        return;
    }
    if (x < 0 || y < 0 || y + 5 >= height) {
        return;
    }
    (void)tb_print(x, y, TB_CYAN | TB_BOLD, TB_DEFAULT,
        "╭────────────────────────────────────╮");
    relay_terminal_draw_overlay_line(x, y + 1, TB_WHITE | TB_BOLD,
        "Exit Relay?");
    relay_terminal_draw_overlay_line(x, y + 2, TB_WHITE, "");
    relay_terminal_draw_overlay_line(x, y + 3, TB_WHITE,
        game->session_exit_from_menu ?
            "Enter: confirm exit" : "Enter: choose save and exit");
    relay_terminal_draw_overlay_line(x, y + 4, TB_WHITE, "Esc: cancel");
    (void)tb_print(x, y + 5, TB_CYAN | TB_BOLD, TB_DEFAULT,
        "╰────────────────────────────────────╯");
}

/** Draw recovery choices after an exit save fails. */
static void relay_terminal_draw_save_failed_overlay(int width, int height,
    const Relay_Game *game)
{
    const int x = (width - 38) / 2;
    const int y = (height - 8) / 2;

    if (x < 0 || y < 0 || y + 7 >= height) {
        return;
    }
    (void)tb_print(x, y, TB_RED | TB_BOLD, TB_DEFAULT,
        "╭────────────────────────────────────╮");
    relay_terminal_draw_overlay_line(x, y + 1, TB_RED | TB_BOLD,
        "Save failed");
    relay_terminal_draw_overlay_line(x, y + 2, TB_WHITE,
        game->session_status);
    relay_terminal_draw_overlay_line(x, y + 3, TB_WHITE, "");
    relay_terminal_draw_overlay_line(x, y + 4, TB_WHITE,
        "Enter: retry save");
    relay_terminal_draw_overlay_line(x, y + 5, TB_YELLOW,
        game->session_exit_after_save ?
            "X: exit without saving" : "");
    relay_terminal_draw_overlay_line(x, y + 6, TB_WHITE, "Esc: cancel");
    (void)tb_print(x, y + 7, TB_RED | TB_BOLD, TB_DEFAULT,
        "╰────────────────────────────────────╯");
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
    const Relay_SessionStore *sessions, Relay_TerminalOverlay overlay)
{
    const int height = tb_height();

    if (terminal == NULL || game == NULL || sessions == NULL ||
        !terminal->initialized) {
        return false;
    }

    if (tb_clear() != TB_OK) {
        return false;
    }
    if (overlay == RELAY_TERMINAL_OVERLAY_SESSION_MENU) {
        relay_terminal_draw_session_overlay(tb_width(), height, game);
    } else if (overlay == RELAY_TERMINAL_OVERLAY_SLOT_SELECT) {
        relay_terminal_draw_slot_overlay(tb_width(), height, sessions);
    } else if (overlay == RELAY_TERMINAL_OVERLAY_EXIT_CONFIRM &&
        game->session_exit_from_menu) {
        relay_terminal_draw_exit_overlay(tb_width(), height, game);
    } else {
        relay_terminal_draw_split(tb_width(), height, game, terminal);
    }
    if (overlay == RELAY_TERMINAL_OVERLAY_SAVE_SELECT) {
        relay_terminal_draw_save_overlay(tb_width(), height, game, sessions);
    } else if (overlay == RELAY_TERMINAL_OVERLAY_EXIT_CONFIRM) {
        if (!game->session_exit_from_menu) {
            relay_terminal_draw_exit_overlay(tb_width(), height, game);
        }
    } else if (overlay == RELAY_TERMINAL_OVERLAY_SAVE_FAILED) {
        relay_terminal_draw_save_failed_overlay(tb_width(), height, game);
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
    node = relay_node_world_find_const(relay_game_active_world_const(game),
        node_id);
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
    for (index = relay_game_active_world_const(game)->count; index > 0;
            index--) {
        const Relay_Node *node =
            &relay_game_active_world_const(game)->nodes[index - 1];
        const Relay_NodeRenderCard card = relay_node_renderer_card(node);
        const int x = 2 + node->grid_x + terminal->grid_offset_x;
        const int y = 3 + node->grid_y + terminal->grid_offset_y;

        if (relay_terminal_node_is_visible(node) &&
            mouse_x > x && mouse_x < x + card.width - 1 &&
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
    for (index = relay_game_active_world_const(game)->count; index > 0;
            index--) {
        const Relay_Node *node =
            &relay_game_active_world_const(game)->nodes[index - 1];
        const Relay_NodeRenderCard card = relay_node_renderer_card(node);
        const int x = 2 + node->grid_x + terminal->grid_offset_x;
        const int y = 3 + node->grid_y + terminal->grid_offset_y;
        const int row = mouse_y - (y + 3);

        if (relay_terminal_node_is_visible(node) &&
            card.definition != NULL && row >= 0 &&
            row < (int)card.definition->input_count && mouse_x >= x &&
            mouse_x < x + card.width / 2) {
            return (Relay_TerminalPortHit){node->id, (size_t)row, false};
        }
        if (relay_terminal_node_is_visible(node) &&
            card.definition != NULL && row >= 0 &&
            row < (int)card.definition->output_count && mouse_x >=
            x + card.width / 2 && mouse_x < x + card.width) {
            return (Relay_TerminalPortHit){node->id, (size_t)row, true};
        }
    }
    return (Relay_TerminalPortHit){0};
}
#endif

bool relay_terminal_poll(Relay_Terminal *terminal, const Relay_Game *game,
    const Relay_SessionStore *sessions, Relay_TerminalOverlay overlay,
    Relay_TerminalEvent *event)
{
    struct tb_event termbox_event;
    int result;

    if (terminal == NULL || game == NULL || sessions == NULL ||
        event == NULL || !terminal->initialized) {
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
        } else if (termbox_event.key == TB_KEY_ARROW_LEFT) {
            event->key = RELAY_TERMINAL_KEY_LEFT;
        } else if (termbox_event.key == TB_KEY_ARROW_RIGHT) {
            event->key = RELAY_TERMINAL_KEY_RIGHT;
        } else if (termbox_event.key == TB_KEY_TAB) {
            event->key = RELAY_TERMINAL_KEY_TAB;
        } else if (termbox_event.key == TB_KEY_ENTER) {
            event->key = RELAY_TERMINAL_KEY_CONFIRM;
        } else if (termbox_event.key == TB_KEY_ESC) {
            event->key = RELAY_TERMINAL_KEY_ESCAPE;
        } else if (termbox_event.key == TB_KEY_BACKSPACE ||
            termbox_event.key == TB_KEY_BACKSPACE2) {
            event->key = RELAY_TERMINAL_KEY_BACKSPACE;
        } else if (termbox_event.key == TB_KEY_DELETE) {
            event->key = RELAY_TERMINAL_KEY_DELETE;
        } else if (termbox_event.key == TB_KEY_HOME) {
            event->key = RELAY_TERMINAL_KEY_HOME;
        } else if (termbox_event.key == TB_KEY_END) {
            event->key = RELAY_TERMINAL_KEY_END;
        } else if (termbox_event.key == TB_KEY_CTRL_S) {
            event->key = RELAY_TERMINAL_KEY_SAVE;
        }
    } else if (termbox_event.type == TB_EVENT_MOUSE) {
        event->type = RELAY_TERMINAL_EVENT_MOUSE;
        event->mouse_x = termbox_event.x;
        event->mouse_y = termbox_event.y;
        if (overlay == RELAY_TERMINAL_OVERLAY_SESSION_MENU &&
            termbox_event.key == TB_KEY_MOUSE_LEFT) {
            event->session_menu_index_plus_one =
                relay_terminal_session_menu_at(tb_width(), tb_height(),
                    event->mouse_x, event->mouse_y);
        }
        if (overlay != RELAY_TERMINAL_OVERLAY_NONE) {
            terminal->dragging_grid = false;
            terminal->dragging_node = false;
            terminal->wiring = false;
            terminal->dragged_node_id = 0;
            return true;
        }
        if (termbox_event.key == TB_KEY_MOUSE_LEFT && !terminal->dragging_grid &&
            !terminal->dragging_node && !terminal->wiring &&
            relay_terminal_workspace_tab_at(game, tb_width(), tb_height(),
                event->mouse_x, event->mouse_y) != 0) {
            event->workspace_tab_index_plus_one =
                relay_terminal_workspace_tab_at(game, tb_width(), tb_height(),
                    event->mouse_x, event->mouse_y);
            return true;
        }
        if (game->editing_blueprint_id != 0) {
            return true;
        }
        if (termbox_event.key == TB_KEY_MOUSE_LEFT && !terminal->dragging_grid &&
            !terminal->dragging_node && !terminal->wiring &&
            relay_terminal_panel_tab_at(tb_width(), tb_height(), event->mouse_x,
                event->mouse_y)) {
            event->panel_tab_index_plus_one = relay_terminal_panel_tab_at(
                tb_width(), tb_height(), event->mouse_x, event->mouse_y);
            return true;
        } else if (termbox_event.key == TB_KEY_MOUSE_LEFT &&
            !terminal->dragging_grid && !terminal->dragging_node &&
            !terminal->wiring) {
            const Relay_TerminalPortHit hit = relay_terminal_port_at(game, terminal,
                event->mouse_x, event->mouse_y);

            if (hit.node_id != 0) {
                terminal->wiring = true;
                terminal->wiring_source_node_id = hit.node_id;
                terminal->wiring_source_port_index = hit.port_index;
                terminal->wiring_origin_is_output = hit.is_output;
                terminal->wiring_mouse_x = event->mouse_x;
                terminal->wiring_mouse_y = event->mouse_y;
                return true;
            }
            terminal->dragged_node_id = relay_terminal_node_at(game, terminal,
                event->mouse_x, event->mouse_y);
            terminal->dragging_node = terminal->dragged_node_id != 0;
            event->selected_node_id = terminal->dragged_node_id;
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

                if (hit.node_id != 0 && hit.is_output !=
                    terminal->wiring_origin_is_output) {
                    if (terminal->wiring_origin_is_output) {
                        event->connection_source_node_id = terminal->wiring_source_node_id;
                        event->connection_source_port_index = terminal->wiring_source_port_index;
                        event->connection_destination_node_id = hit.node_id;
                        event->connection_destination_port_index = hit.port_index;
                    } else {
                        event->connection_source_node_id = hit.node_id;
                        event->connection_source_port_index = hit.port_index;
                        event->connection_destination_node_id = terminal->wiring_source_node_id;
                        event->connection_destination_port_index = terminal->wiring_source_port_index;
                    }
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
