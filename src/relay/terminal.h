#ifndef RELAY_TERMINAL_H
#define RELAY_TERMINAL_H

#include "relay/platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if RELAY_PLATFORM_WINDOWS
#include <windows.h>
#endif

/** High-level keys normalized from platform terminal input. */
typedef enum Relay_TerminalKey {
    RELAY_TERMINAL_KEY_NONE,
    RELAY_TERMINAL_KEY_UP,
    RELAY_TERMINAL_KEY_DOWN,
    RELAY_TERMINAL_KEY_LEFT,
    RELAY_TERMINAL_KEY_RIGHT,
    RELAY_TERMINAL_KEY_TAB,
    RELAY_TERMINAL_KEY_CONFIRM,
    RELAY_TERMINAL_KEY_ESCAPE,
    RELAY_TERMINAL_KEY_BACKSPACE,
    RELAY_TERMINAL_KEY_DELETE,
    RELAY_TERMINAL_KEY_HOME,
    RELAY_TERMINAL_KEY_END,
    RELAY_TERMINAL_KEY_SAVE
} Relay_TerminalKey;

/** Center-screen terminal overlay states owned by the application. */
typedef enum Relay_TerminalOverlay {
    RELAY_TERMINAL_OVERLAY_NONE,
    RELAY_TERMINAL_OVERLAY_EXIT_CONFIRM
} Relay_TerminalOverlay;

/** Event categories produced by Relay's terminal adapter. */
typedef enum Relay_TerminalEventType {
    RELAY_TERMINAL_EVENT_NONE,
    RELAY_TERMINAL_EVENT_QUIT,
    RELAY_TERMINAL_EVENT_RESIZED,
    RELAY_TERMINAL_EVENT_INPUT,
    RELAY_TERMINAL_EVENT_MOUSE
} Relay_TerminalEventType;

/** One normalized terminal event. */
typedef struct Relay_TerminalEvent {
    Relay_TerminalEventType type;
    Relay_TerminalKey key;
    uint32_t character;
    int mouse_x;
    int mouse_y;
    size_t panel_tab_index_plus_one;
    size_t workspace_tab_index_plus_one;
    uint64_t selected_node_id;
    uint64_t dragged_node_id;
    int grid_delta_x;
    int grid_delta_y;
    uint64_t connection_source_node_id;
    size_t connection_source_port_index;
    uint64_t connection_destination_node_id;
    size_t connection_destination_port_index;
} Relay_TerminalEvent;

typedef struct Relay_Game Relay_Game;

/** Platform terminal state owned by the Relay application. */
typedef struct Relay_Terminal {
    bool initialized;
    bool dragging_grid;
    bool dragging_node;
    bool wiring;
    uint64_t dragged_node_id;
    uint64_t wiring_source_node_id;
    size_t wiring_source_port_index;
    bool wiring_origin_is_output;
    int wiring_mouse_x;
    int wiring_mouse_y;
    int64_t grid_offset_x;
    int64_t grid_offset_y;
    int drag_last_x;
    int drag_last_y;
#if RELAY_PLATFORM_WINDOWS
    HANDLE input;
    HANDLE output;
    DWORD input_mode;
    DWORD output_mode;
#endif
} Relay_Terminal;

/** Initialize the terminal backend and enter its application screen. */
bool relay_terminal_init(Relay_Terminal *terminal);

/** Render Relay's current game and control-panel state. */
bool relay_terminal_draw(const Relay_Terminal *terminal, const Relay_Game *game,
    Relay_TerminalOverlay overlay);

/** Center the graph viewport on a node when a game action focuses it. */
void relay_terminal_focus_node(Relay_Terminal *terminal, const Relay_Game *game,
    uint64_t node_id);

/** Wait for and translate one terminal input event. */
bool relay_terminal_poll(Relay_Terminal *terminal, const Relay_Game *game,
    Relay_TerminalEvent *event);

/** Restore the terminal backend to its prior state. */
void relay_terminal_shutdown(Relay_Terminal *terminal);

#endif
