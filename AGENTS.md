# Relay implementation instructions

## Architecture

Relay is a terminal-based node automation game. Keep simulation, node graph,
world, terminal, persistence, and rendering systems separate behind explicit
ownership in `Relay_App`. Add state and services through the application
lifecycle: `relay_app_init`, `relay_app_run`, and `relay_app_shutdown`.

## Implementation standard

Do not deliver partial implementations, temporary compatibility layers, stub
paths, or placeholder hooks. Complete every requested feature across its owned
API, lifecycle, error handling, documentation, and validation boundary. Design
new systems for the game's intended long-term ownership and concurrency model,
then integrate and test them before considering the work complete.

Prefer the event-driven observer pattern for cross-system notifications. The
main-thread event bus (`Relay_EventBus`) is synchronous: observers may register
or unsubscribe during dispatch, but must not shut down the bus while an event
is being delivered. Background workers must not emit directly to it; transfer
their completed data to the main thread first, then emit an event there.

## Logging

Use `relay_logger_log` for diagnostics. Relay reserves the terminal for game
rendering, so no debug, warning, or error output may be printed to stdout or
stderr. Runtime logs belong in `logs/relay.log`; never add generated logs to
source control. Log service failures and meaningful state transitions, not
per-frame noise.

## Jobs

Use `Relay_JobSystem` for CPU-bound work that does not touch terminal state,
application state, or the event bus. Job contexts remain caller-owned and must
stay valid until their job completes. Synchronize completion with
`relay_job_system_wait_idle` only at an explicit ownership boundary; do not
block the normal game loop. Use a main-thread completion queue plus
`RELAY_EVENT_JOB_COMPLETED` when a future job needs to notify gameplay.

## Nodes and scripting

Node definitions are immutable data schemas in `relay/node.h`; use stable
numeric identifiers and dotted string keys so saves and gameplay scripts do not
depend on display labels. Add new gameplay data through a
`Relay_NodeDefinition`, including script-visible property definitions, then
create instances through `Relay_NodeWorld`. Do not expose mutable world arrays
to scripts: use node IDs and `relay_node_property_get` or
`relay_node_property_set` so access rules remain enforceable.

`Relay_NodeWorld` is one executable module graph. Preserve its typed directed
connection rule: one output may fan out, each input has one replaceable source,
and compatibility is determined by the declared port value type. Reusable and
script-authored modules must compile to this same graph contract;
do not create a parallel scripting-only wire format. Simulation runs in fixed
steps and must not be advanced by rendering or terminal input.

Self-connections are valid when port schemas are compatible. Evaluate them
through a previous-step output snapshot so a feedback loop has deterministic,
non-recursive gameplay semantics.

Lua 5.5 is a private implementation dependency behind Relay-owned scripting
APIs. Do not expose Lua types in public headers, open additional standard
libraries, add arbitrary execution entry points, or let scripts access the
filesystem, environment, process, host clock, network, terminal, logger, event
bus, or mutable world storage. Gameplay scripts read immutable activation
snapshots and enqueue capability-checked commands for deterministic commit.
Transient resource/trigger inputs activate a process when nonzero; level inputs
activate it when their value changes. Authoritative
gameplay values are integers, Booleans, strings, and bounded project-owned
containers; floating-point values and unordered table iteration are forbidden.
Source and project-owned typed persistent state are saved; Lua bytecode, stack
frames, pointers, and VM tables are never authoritative.

Blueprints are versioned reusable module definitions with typed ports,
parameters, components, connections, programs, and layout. Nested blueprints
must be cycle-checked and compiled into the same `Relay_NodeWorld` graph with
instance/local-path provenance. Do not introduce an opaque blueprint simulator,
parallel wire format, or script-only module graph. Follow the complete stage
contracts and exit criteria in `docs/SCRIPTING_BLUEPRINT_ROADMAP.md`.
The Blueprint's own program is an implicit `on_process` observer, never a
component in its visual architecture. Keep the canonical top-level `instance` and
`connect` declarations and visual graph transactionally synchronized in both
directions.

## Workspace rendering

Keep graph world coordinates in game data and viewport coordinates in the
terminal renderer. Panning, tabs, drag state, and terminal hit testing must not
modify nodes merely to change what is visible. Render node cards through the
node renderer's visual contract; do not duplicate definition-specific display
logic in unrelated systems. Node drags may carry only a captured node ID and a
grid delta from terminal code; apply that mutation through `relay_game_move_node`
at the application boundary. World grid coordinates are signed 64-bit values;
never clamp them to screen space or a non-negative origin.

Each definition must declare its input and output port schemas. Graph cards use
that schema to align inputs on the left and outputs on the right; do not add
display-only ports or infer ports from a node's label.

## Navigation

Escape is the universal Back action. A nested workspace mode must consume Back
before the application offers exit. Exit requires the terminal confirmation
overlay and explicit Enter confirmation; never bind `q` or Escape directly to
process termination. Newly created world objects must become the active focus
through their owning game state, then the renderer may center its viewport.
