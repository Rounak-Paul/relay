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

## Scripting and blueprint boundary

`vendors/lua/` contains the checksum-pinned Lua 5.5.0 upstream distribution.
`CMakeLists.txt` builds the required core and safe libraries as the private
static `relay_lua` target; it does not build the Lua interpreter or compiler
executables. `src/script_runtime.c` owns the application Lua state, fixed string
hash seed, hard allocator limit, selected-library initialization, and fail-closed
sandbox validation. Only base, string, table, and UTF-8 libraries are compiled
and opened. Unsafe base globals and unordered table iteration are removed.
`Relay_App` initializes and shuts down the runtime through its normal lifecycle.

Lua is syntax and execution infrastructure, not a second simulation.
`src/script_runtime.c` compiles each module in an isolated environment, enforces
integer-only source rules and deterministic instruction/memory limits, then
invokes it with a read-only activation snapshot. Each placed module owns a
bounded scalar state table; state and outputs commit only after a successful
invocation. Lua headers and state pointers remain private and gameplay-facing
values stay project-owned. Script interfaces use the immutable `Type` enum
namespace (`Type.TRIGGER`, material types, `Type.BOOLEAN`, and `Type.INTEGER`);
the enum is built from one runtime registry and lowercase string type
declarations are not part of the language.

`src/script_language.c` is the terminal-independent Blueprint language service.
Its bounded full-source lexer classifies multiline Lua strings/comments,
keywords, numbers, Relay callables, type constants, definition namespaces,
members, and operators. The immutable built-in API catalog plus a bounded
caller-owned catalog of live `script.*` names drives context-filtered completion
and nested, string-aware signature help. `Relay_Game` assembles that dynamic
catalog so insertion and both terminal renderers consume identical results.
`Relay_Blueprint` owns only transient selection and dismissal state; game editor
commands own insertion. Insert-mode Up/Down selects an active completion, Tab
accepts it, and Escape dismisses assistance before leaving Insert mode.

`src/blueprint.c` owns stable player Blueprint IDs, canonical script names,
dotted definition symbols, source revisions, typed schemas, compiled artifacts,
boundary/process definitions,
top-level architecture scenes, and immutable flattened plans. Every scene owns
`Module Inputs` and `Module Outputs`; only actual built-in or nested components
appear between them. The Blueprint's Lua function is an implicit internal
process and never renders as a self-component. Compilation validates dependency
acyclicity, recursively flattens child plans, and records external input fan-out
and output-source bindings. Instantiation transactionally creates one visible
wrapper plus private implementation nodes with Blueprint/node provenance.
Runtime execution remains one graph and one previous-step wire model, never a
nested or scripting-only simulator.

The source buffer contains ordinary top-level Lua-shaped architecture
declarations: `local component = instance(namespace.member, ...)` plus typed
`connect(...)` port maps through module `inputs`/`outputs` and component
`.inputs`/`.outputs` namespaces. Definition references are unquoted symbols,
never string keys: immutable built-ins use `source.*` and `control.*`, while
reusable modules use `script.<blueprint_name>`. Blueprint names and both symbol
segments use strict lowercase snake_case; the parser rejects quoted references,
spaces, uppercase characters, repeated/trailing underscores, and additional
namespace separators transactionally. The Blueprint compiler blanks these
declarative lines from the Lua runtime chunk while preserving diagnostic line
numbers; the architecture parser owns them. `on_process(inputs, state)` is the
deterministic activation observer.
Successful graph component creation, connection replacement, and movement
regenerate these declarations and recompile the artifact and plan
transactionally. Regeneration canonicalizes the architecture boundary to one
blank line before the generated declarations and one before `on_process`, so
repeated graph edits cannot accumulate empty lines; function-body whitespace is
preserved. `:w` parses the same declarations back into a candidate scene,
resolves stable definition and port keys, validates it through `Relay_NodeWorld`,
and swaps source, scene, artifact, and plan only as one successful deployment.

`Relay_Game` owns workspace selection, editor commands, design-graph mutation,
instantiation, and fixed-step execution. Failed architecture mutations restore
the prior connection or node boundary and preserve the last valid plan. Direct
and indirect recursive dependencies are rejected. Interface-changing
redeployment is rejected after instances or architecture references exist,
while compatible code changes replace the Lua artifact transactionally. The
implemented workflow is documented in `docs/BLUEPRINTS.md`; persistence,
debugger, and broader language capability gates remain in
`docs/SCRIPTING_BLUEPRINT_ROADMAP.md`.

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
input port; replacing an input's wire is deliberate, and exact compatibility is
validated centrally through fixed `Relay_NodePortType` values. Port types are
separate from property scalar types: Trigger, Coal, Iron Ore, Copper Ore, Stone,
Boolean, Integer, and Text are semantic graph channels. This graph contract is
the implemented boundary for built-in reusable modules and script-authored
VHDL-like modules. Scripting uses stable node IDs, port indexes, and property
APIs rather than terminal coordinates or mutable storage.

The initial graph starts with one Coal Miner. Coal, Iron, Copper, and Stone
miners use the same `RELAY_NODE_BEHAVIOR_FIXED_RATE_SOURCE` executor and differ
only through immutable `Relay_NodeDefinition` data: stable identity, output
schema, interval, output port, and quantity. Each has no inputs and emits one
typed resource every 60 simulation steps while enabled. Runtime progress and
lifetime production are Inspector state rather than script properties.
`node.enabled` is the universal script-visible Boolean property on every
built-in and Blueprint-owned node; it is the only script-visible property on
miners. Disabling a Blueprint wrapper pauses every flattened internal node and
suppresses the wrapper outputs; re-enabling it resumes retained progress and
script state. New source definitions therefore require no simulation or
terminal branch.

The Shop sells optional Timer modules. A Timer emits a transient typed Trigger
at a configurable 1, 2, 4, 8, or 16 second interval for script and control
activation. Trigger and material values are one-step deliveries; Boolean,
integer, and text values are retained levels. Blueprint `on_process` observers
initialize once, then run only for nonzero transient inputs or changed level
inputs. `Relay_App` uses a monotonic accumulator and executes simulation at a
fixed 60 Hz independently of rendering. Each outer-loop iteration performs at
most eight catch-up steps and retains remaining simulation debt, so slow
presentation never silently discards authoritative steps. While debt remains,
normal simulation redraws are skipped so processing catches up before presenting
the next graph state.

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
`relay_node_renderer_port_visual` owns the terminal-agnostic type color token,
which both terminal backends apply to the port glyphs. Trigger, Coal, Iron Ore,
Copper Ore, and Stone have distinct colors, so the visual language mirrors the
graph's exact type rule.

`Relay_Game.workspace_mode` owns the graph/map workspace state and
`focused_node_id` records the active node for inspection as well as the last node
created by a successful purchase. The terminal centers newly created nodes after
creation; clicking a card title changes focus through `relay_game_focus_node`
and activates the Inspector.
`Relay_Game.active_tab` owns the Shop/Inspector/Scripts choice. Tab cycles the
wider right-side control panel, while mouse hit testing selects the exact tab
clicked. The Inspector reads the immutable
definition and script-visible node properties; a focused Timer presents its
interval configuration and valid duration set without a parallel UI-only
configuration model. `m` toggles the compact map renderer;
Escape is a universal Back request, returning map view to graph view before the
application opens its centered exit-confirmation overlay. Only Enter confirms
that overlay; `q` is never an exit shortcut.

Relay is the permanently open top-level workspace zero and each Blueprint owns
one additional architecture scene plus independent tab-open state. Creating or
opening a Blueprint makes its tab visible beside Relay; closing removes only the
tab and preserves its definition, source, scene, artifact, plan, and instances.
Mouse clicks activate exact visible tabs, while `,` and `.` cycle only open
tabs. The Scripts panel opens the selected Blueprint with `O` or places its
compiled module into Relay or another Blueprint with Enter. Architecture scenes
show their public input/output boundaries and real components as normal graph
cards; the inspector explains the `Module Inputs -> components -> Module
Outputs` port-map direction. `E` opens either the active Blueprint or a focused
Blueprint node in the bounded code editor. Editor mutations stay in game-owned
source buffers;
the modal normal/insert/command state is Blueprint-owned, and `:w`/`:wq`
perform transactional deployment without depending on terminal control-key
forwarding. Escape first returns insert/command mode to normal, then consumes
Back through the Blueprint architecture and Relay workspace before any exit
prompt. Both terminal backends share the editor viewport and line parsing logic
and expose source revision, dirty state, command text, and diagnostics in the
panel.

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
