#ifndef RELAY_GAME_H
#define RELAY_GAME_H

#include "relay/blueprint.h"
#include "relay/node.h"
#include "relay/script_language.h"
#include "relay/script_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Generic in-world currency amount with no real-world denomination. */
typedef uint64_t Relay_Currency;

/** Tabs currently available in Relay's right-side control panel. */
typedef enum Relay_GamePanelTab {
    RELAY_GAME_PANEL_TAB_SHOP,
    RELAY_GAME_PANEL_TAB_INSPECTOR,
    RELAY_GAME_PANEL_TAB_BLUEPRINTS
} Relay_GamePanelTab;

/** Zoom level used by Relay's graph workspace. */
typedef enum Relay_GameWorkspaceMode {
    RELAY_GAME_WORKSPACE_GRAPH,
    RELAY_GAME_WORKSPACE_MAP
} Relay_GameWorkspaceMode;

/** Actions exposed by the persistent startup main menu. */
typedef enum Relay_GameSessionMenuItem {
    RELAY_GAME_SESSION_MENU_CONTINUE,
    RELAY_GAME_SESSION_MENU_NEW,
    RELAY_GAME_SESSION_MENU_SAVED,
    RELAY_GAME_SESSION_MENU_EXIT,
    RELAY_GAME_SESSION_MENU_COUNT
} Relay_GameSessionMenuItem;

/** Inputs the game consumes from its terminal adapter. */
typedef enum Relay_GameInput {
    RELAY_GAME_INPUT_NONE,
    RELAY_GAME_INPUT_PREVIOUS,
    RELAY_GAME_INPUT_NEXT,
    RELAY_GAME_INPUT_CONFIRM,
    RELAY_GAME_INPUT_TOGGLE_MAP,
    RELAY_GAME_INPUT_TOGGLE_PANEL_TAB,
    RELAY_GAME_INPUT_PREVIOUS_TIMER_INTERVAL,
    RELAY_GAME_INPUT_NEXT_TIMER_INTERVAL
} Relay_GameInput;

enum {
    RELAY_GAME_SIMULATION_STEPS_PER_SECOND = 60,
    RELAY_GAME_MAX_CATCH_UP_STEPS = 8
};

/** Results from shop and menu actions exposed for UI and scripting feedback. */
typedef enum Relay_GameActionResult {
    RELAY_GAME_ACTION_NONE,
    RELAY_GAME_ACTION_PURCHASED,
    RELAY_GAME_ACTION_INSUFFICIENT_CURRENCY,
    RELAY_GAME_ACTION_CREATION_FAILED,
    RELAY_GAME_ACTION_CONNECTION_REJECTED
} Relay_GameActionResult;

/** Results produced by one completed editor command. */
typedef enum Relay_GameEditorCommandResult {
    RELAY_GAME_EDITOR_COMMAND_NONE,
    RELAY_GAME_EDITOR_COMMAND_SAVED,
    RELAY_GAME_EDITOR_COMMAND_CLOSED,
    RELAY_GAME_EDITOR_COMMAND_FAILED
} Relay_GameEditorCommandResult;

/** One catalog entry that can instantiate a node definition. */
typedef struct Relay_ShopOffer {
    Relay_NodeDefinitionId definition_id;
    Relay_Currency price;
} Relay_ShopOffer;

/** Game-owned state for source nodes, shop state, and currency. */
typedef struct Relay_Game {
    Relay_NodeWorld nodes;
    Relay_BlueprintLibrary blueprints;
    Relay_ScriptRuntime *script_runtime;
    Relay_Currency currency;
    Relay_GamePanelTab active_tab;
    Relay_GameWorkspaceMode workspace_mode;
    size_t selected_offer;
    size_t selected_blueprint;
    Relay_GameActionResult last_action;
    Relay_NodeId focused_node_id;
    Relay_NodeId root_focused_node_id;
    size_t active_workspace;
    Relay_BlueprintId editing_blueprint_id;
    uint64_t simulation_step;
    uint64_t session_id;
    uint64_t save_revision;
    char session_status[96];
    bool session_continue_available;
    Relay_GameSessionMenuItem session_menu_selection;
    bool session_save_new_selected;
    bool session_exit_after_save;
    bool session_exit_from_menu;
} Relay_Game;

/** Initialize the node world and starting currency. */
bool relay_game_init(Relay_Game *game, Relay_ScriptRuntime *script_runtime);

/** Return the mutable graph shown in the active top-level workspace. */
Relay_NodeWorld *relay_game_active_world(Relay_Game *game);

/** Return the immutable graph shown in the active top-level workspace. */
const Relay_NodeWorld *relay_game_active_world_const(const Relay_Game *game);

/** Return the active blueprint, or NULL while the Relay root is active. */
Relay_Blueprint *relay_game_active_blueprint(Relay_Game *game);

/** Create and open a new top-level script blueprint. */
bool relay_game_create_blueprint(Relay_Game *game);

/** Move to the adjacent Relay/blueprint top-level workspace. */
bool relay_game_switch_workspace(Relay_Game *game, int direction);

/** Activate Relay or one currently open Blueprint workspace by registry index. */
bool relay_game_activate_workspace(Relay_Game *game, size_t workspace_index);

/** Open the selected Blueprint as a visible top-level workspace tab. */
bool relay_game_open_selected_blueprint(Relay_Game *game);

/** Close the active Blueprint tab without deleting its definition or scene. */
bool relay_game_close_active_blueprint(Relay_Game *game);

/** Add a compiled blueprint as a normal typed node in the active scene. */
bool relay_game_add_blueprint(Relay_Game *game, Relay_BlueprintId blueprint_id);

/** Open the active or focused blueprint in the code editor. */
bool relay_game_open_editor(Relay_Game *game);

/** Return the blueprint currently owned by the code editor. */
Relay_Blueprint *relay_game_editing_blueprint(Relay_Game *game);

/** Query completion using built-in and live reusable-script symbols. */
size_t relay_game_editor_completions(const Relay_Game *game,
    Relay_ScriptCompletion *items, size_t item_capacity,
    size_t *replacement_start);

/** Insert one ASCII code point at the editor cursor. */
bool relay_game_editor_insert(Relay_Game *game, uint32_t character);

/** Remove the character immediately before the editor cursor. */
bool relay_game_editor_backspace(Relay_Game *game);

/** Remove the character at the editor cursor. */
bool relay_game_editor_delete(Relay_Game *game);

/** Move the editor cursor horizontally by one character. */
bool relay_game_editor_move_horizontal(Relay_Game *game, int direction);

/** Move the editor cursor vertically while preserving its visual column. */
bool relay_game_editor_move_vertical(Relay_Game *game, int direction);

/** Move the editor cursor to the start or end of its current line. */
bool relay_game_editor_move_line_boundary(Relay_Game *game, bool to_end);

/** Enter insert mode at the current cursor. */
bool relay_game_editor_enter_insert(Relay_Game *game);

/** Enter Vim-style command mode with an empty command buffer. */
bool relay_game_editor_enter_command(Relay_Game *game);

/** Return insert or command mode to normal mode. */
bool relay_game_editor_leave_mode(Relay_Game *game);

/** Move the active code-completion selection by one item. */
bool relay_game_editor_completion_move(Relay_Game *game, int direction);

/** Replace the current identifier prefix with the selected completion. */
bool relay_game_editor_completion_accept(Relay_Game *game);

/** Suppress automatic completion until the source or cursor changes. */
bool relay_game_editor_completion_dismiss(Relay_Game *game);

/** Append one printable character to the active editor command. */
bool relay_game_editor_command_insert(Relay_Game *game, uint32_t character);

/** Remove the final character from the active editor command. */
bool relay_game_editor_command_backspace(Relay_Game *game);

/** Execute the active `:w`, `:q`, or `:wq` editor command. */
Relay_GameEditorCommandResult relay_game_editor_command_execute(
    Relay_Game *game);

/** Compile and transactionally deploy the edited blueprint source. */
bool relay_game_editor_save(Relay_Game *game);

/** Select one right-panel tab by its stable enum value. */
bool relay_game_select_panel_tab(Relay_Game *game, size_t tab_index);

/** Handle one navigation or confirmation input. */
Relay_GameActionResult relay_game_handle_input(Relay_Game *game,
    Relay_GameInput input);

/** Move one node by a grid delta after validated workspace interaction. */
bool relay_game_move_node(Relay_Game *game, Relay_NodeId id, int delta_x,
    int delta_y);

/** Make an existing node the active target for workspace controls and inspection. */
bool relay_game_focus_node(Relay_Game *game, Relay_NodeId id);

/** Connect a typed source port to an input port in the root gameplay module. */
bool relay_game_connect_nodes(Relay_Game *game, Relay_NodeId source_node_id,
    size_t source_port_index, Relay_NodeId destination_node_id,
    size_t destination_port_index);

/** Advance the root module by one deterministic fixed simulation step. */
bool relay_game_step(Relay_Game *game);

/** Leave a nested workspace mode, returning true when one was left. */
bool relay_game_back(Relay_Game *game);

/** Return the number of source-node offers in the shop. */
size_t relay_game_shop_offer_count(void);

/** Return a shop offer by index, or NULL when out of range. */
const Relay_ShopOffer *relay_game_shop_offer_at(size_t index);

/** Return the definition referenced by a shop offer. */
const Relay_NodeDefinition *relay_game_shop_offer_definition(
    const Relay_ShopOffer *offer);

/** Return an explanatory label for the most recent game action. */
const char *relay_game_action_result_label(Relay_GameActionResult result);

/** Release all game-owned state. */
void relay_game_shutdown(Relay_Game *game);

#endif
