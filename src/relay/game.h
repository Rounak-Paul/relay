#ifndef RELAY_GAME_H
#define RELAY_GAME_H

#include "relay/node.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Generic in-world currency amount with no real-world denomination. */
typedef uint64_t Relay_Currency;

/** Tabs currently available in Relay's right-side control panel. */
typedef enum Relay_GamePanelTab {
    RELAY_GAME_PANEL_TAB_SHOP,
    RELAY_GAME_PANEL_TAB_INSPECTOR
} Relay_GamePanelTab;

/** Zoom level used by Relay's graph workspace. */
typedef enum Relay_GameWorkspaceMode {
    RELAY_GAME_WORKSPACE_GRAPH,
    RELAY_GAME_WORKSPACE_MAP
} Relay_GameWorkspaceMode;

/** Inputs the game consumes from its terminal adapter. */
typedef enum Relay_GameInput {
    RELAY_GAME_INPUT_NONE,
    RELAY_GAME_INPUT_PREVIOUS,
    RELAY_GAME_INPUT_NEXT,
    RELAY_GAME_INPUT_CONFIRM,
    RELAY_GAME_INPUT_TOGGLE_MAP,
    RELAY_GAME_INPUT_TOGGLE_PANEL_TAB,
    RELAY_GAME_INPUT_PREVIOUS_CLOCK_RATE,
    RELAY_GAME_INPUT_NEXT_CLOCK_RATE
} Relay_GameInput;

/** Results from shop and menu actions exposed for UI and scripting feedback. */
typedef enum Relay_GameActionResult {
    RELAY_GAME_ACTION_NONE,
    RELAY_GAME_ACTION_PURCHASED,
    RELAY_GAME_ACTION_INSUFFICIENT_CURRENCY,
    RELAY_GAME_ACTION_CREATION_FAILED,
    RELAY_GAME_ACTION_CONNECTION_REJECTED
} Relay_GameActionResult;

/** One catalog entry that can instantiate a node definition. */
typedef struct Relay_ShopOffer {
    Relay_NodeDefinitionId definition_id;
    Relay_Currency price;
} Relay_ShopOffer;

/** Game-owned state for source nodes, shop state, and currency. */
typedef struct Relay_Game {
    Relay_NodeWorld nodes;
    Relay_Currency currency;
    Relay_GamePanelTab active_tab;
    Relay_GameWorkspaceMode workspace_mode;
    size_t selected_offer;
    Relay_GameActionResult last_action;
    Relay_NodeId focused_node_id;
    uint64_t simulation_tick;
} Relay_Game;

/** Initialize the node world and starting currency. */
bool relay_game_init(Relay_Game *game);

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

/** Advance the root module by one fixed 60 Hz gameplay tick. */
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
