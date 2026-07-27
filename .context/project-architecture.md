# Relay project architecture

## Build boundary

`CMakeLists.txt` builds one C17 executable named `relay` and writes it directly
to `bin/`. It explicitly accepts Windows, macOS, and Linux only. The project
deliberately has no package-manager or runtime asset dependency.

## Source boundary

Application code lives in `src/`. `src/relay/app.h` owns the root `Relay_App`
state and its explicit lifecycle: uninitialized, initializing, running,
shutting down, stopped, or failed. `relay_app_init`, `relay_app_run`, and
`relay_app_shutdown` are the only application lifecycle entry points. The app
owns a terminal backend, logger, synchronous main-thread event bus, and worker
pool; future simulation, node graph, world, and UI systems should be added as
independently owned app services.

`src/relay/platform.h` is the only platform classification surface; it supplies
Windows, macOS, and Linux macros that can be used by all application sources.

## Diagnostics boundary

`src/logger.c` owns the file-only logger. It creates `logs/relay.log`, truncates
it for each run, timestamps and flushes every entry, and never writes to the
terminal. The log directory is ignored by Git.

## Event and job boundary

`src/event.c` provides a synchronous, main-thread observer bus. Listeners are
safe to add or remove while an event dispatch is in progress; bus shutdown is
only valid outside a listener. Application terminal input is translated into
events, so resize and quit behavior stays observer-driven.

`src/job_system.c` provides a cross-platform worker pool. It uses POSIX threads
on macOS/Linux and Windows threads on Windows, drains accepted jobs before
shutdown, and never calls terminal, app, or event code from workers. Future
background work must pass its results to the main thread before emitting an
event.

`tests/runtime_test.c` proves that an observer can safely remove itself during
dispatch and that submitted work completes across multiple worker threads.

## Game node boundary

`src/node.c` owns the immutable source-node registry and dynamic `Relay_NodeWorld`.
Definitions use stable numeric IDs and dotted keys (`source.iron_ore`,
`resource.remaining`) so future scripting and saves do not depend on display
text. `relay_node_property_get` and `relay_node_property_set` are the only
script-ready property access boundary; writability comes from each definition's
schema.

`src/game.c` owns generic currency, the source shop catalog, selection, and
deterministic initial placement. The game begins with 100 neutral currency.
`src/node_renderer.c` converts definition data into a fixed header-and-port
card contract. Definitions declare ports, so terminal code lays out left-side
inputs and right-side outputs without source-specific branches.

`Relay_NodeWorld` is the runtime graph for the root gameplay module: it owns
instances and typed directed connections. A connection is an output port to one
input port; replacing an input's wire is deliberate, and type compatibility is
validated centrally. This graph contract is also the future boundary for
built-in reusable modules and script-authored VHDL-like modules, so scripting
must use stable node IDs, port indexes, and property APIs rather than terminal
coordinates or mutable storage.

The initial graph starts with one Coal Miner. The Shop sells Clock modules. A
Clock emits its `Clock` signal at one of the valid periods (2 through 128 fixed ticks),
and the Coal Miner consumes one Coal fuel plus 16 connected pulses to produce
one Coal. The initial miner has one bootstrap fuel item; players can feed its
Coal output back to its Fuel input through a permitted self-connection. Each
tick snapshots prior outputs before evaluating the graph, so a self-fed coal
item becomes fuel on the following tick rather than recursively duplicating in
one simulation step. `Relay_App` uses a monotonic accumulator and executes
gameplay at a fixed 60 Hz; rendering and terminal input never determine
simulation progress. Progress and output state stay on the node instance and
are exposed through script-stable property keys.

## Workspace renderer boundary

The left terminal pane is a tabbed workspace with `Relay` as its first tab.
It renders a grid in world coordinates and source nodes as graph cards. The
viewport offset and mouse-drag state belong to `Relay_Terminal`, not game or
node state, so viewport motion never mutates the world. Termbox mouse mode and
Windows console mouse input both normalize to terminal mouse events; app code
only requests a redraw after those events. Hit testing captures a node ID on
press; only the app applies emitted grid deltas through `relay_game_move_node`.
Empty-space dragging changes only the viewport, while node-card dragging
changes only the captured node's signed 64-bit world position. Only a card's
title row may capture a node drag; ports and all other card rows remain free
for graph interactions.

Graph connections render as clipped orthogonal terminal wires behind node cards,
with rounded turns and short horizontal stubs outside edge-mounted port anchors.
A live preview follows the pointer while either port direction is being wired.
Their port anchors are resolved from immutable port schemas plus the current
viewport, so panning and dragging never mutate a connection simply to redraw it.
Forward links use the minimal two-turn centered route; only reverse-direction
links use an outside-card detour to preserve output-right and input-left flow.
Mouse hit testing uses the full left half of an input row and the full right
half of an output row, rather than a single connector glyph. A wire may begin
from either side; terminal input normalizes that interaction to one output-to-
input graph connection before the game validates it.

`Relay_Game.workspace_mode` owns the graph/map workspace state and
`focused_node_id` records the active node for inspection as well as the last node
created by a successful purchase. The terminal centers newly created nodes after
creation; clicking a card title changes focus through `relay_game_focus_node`
and activates the Inspector.
`Relay_Game.active_tab` owns the Shop/Inspector choice. Tab switches the wider
right-side control panel between those views; clicking its tab strip toggles the
same state. The Inspector reads the immutable
definition and script-visible node properties; a focused Clock presents its
Clocking Wizard configuration and valid period set without a parallel UI-only
configuration model. `m` toggles the compact map renderer;
Escape is a universal Back request, returning map view to graph view before the
application opens its centered exit-confirmation overlay. Only Enter confirms
that overlay; `q` is never an exit shortcut.

## Embedded font boundary

`assets/fonts/DepartureMonoNerdFontMono-Regular.otf` and its OFL license are
vendored source assets. CMake builds `tools/embed_asset.c` first and generates
a byte-array header, which `src/font_asset.c` compiles into the executable. The
asset is available to future native renderers, but a terminal continues to use
the user-selected terminal font.

## Terminal boundary

Termbox2 is vendored unchanged in `vendors/termbox2`. On macOS and Linux,
`src/termbox2_impl.c` defines `TB_IMPL` exactly once, compiling Termbox2
directly into the executable. Its isolated warning exception prevents an
upstream unused static helper from weakening application diagnostics. Upstream
Termbox2 is POSIX-only, so Windows uses its native Console API and
virtual-terminal output. The renderer has one responsive split: a large left
playfield and a wider right control panel. It keeps a divider and panel headers
visible whenever the terminal meets the minimum 43-column by 10-row layout;
smaller terminals show a concise resize message instead.
