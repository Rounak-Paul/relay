# Relay scripting and blueprint implementation roadmap

## Design decision

Relay uses pinned Lua 5.5 as the syntax and execution engine for player
programs. Lua is an implementation detail behind project-owned C APIs. It does
not define simulation ownership, port types, save data, capabilities, or
blueprint structure.

The supplied Foundry Logic specification remains authoritative except for its
custom-language VM choice, which is replaced by the explicitly approved Lua
runtime. The replacement preserves the important constraints:

- no filesystem, process, environment, networking, dynamic loading, host clock,
  debug library, or terminal output;
- no floating-point value may cross the gameplay API;
- scripts read immutable tick snapshots and emit commands for a later phase;
- stable IDs and deterministic ordering define all observable behavior;
- source plus version metadata is authoritative; Lua bytecode is never a save
  or blueprint interchange format;
- every invocation has deterministic instruction and memory budgets; and
- script access is granted through typed control/message connections, never
  global mutable world access.

## Blueprint model

A blueprint is a versioned, reusable module definition. It owns:

- a stable dotted key and content version;
- typed input/output ports and typed parameters;
- component node instances with local stable keys;
- typed internal connections;
- Lua source artifacts and event bindings;
- external-port bindings to internal component ports; and
- editor layout metadata that never affects simulation.

Blueprints may instantiate other blueprints. Compilation detects recursive
dependency cycles, validates all bindings, then flattens the hierarchy into the
same `Relay_NodeWorld` graph contract used by hand-built nodes. Runtime nodes
retain blueprint instance and local-path provenance so the UI can collapse,
enter, inspect, debug, decommission, and save a module as one unit. There is no
parallel scripting graph or special nested simulation path.

## Stage 0 — pinned Lua foundation

**Status:** implemented.

Scope:

- vendor Lua 5.5.0 from the official release archive with recorded SHA-256;
- compile only the Lua core and selected safe libraries into `relay_lua`;
- exclude standalone `lua`/`luac` tools and unsafe standard libraries;
- use a fixed Lua string-hash seed and a hard allocator limit;
- expose an application-owned, idempotent runtime lifecycle;
- verify the Lua version, memory limit, double-init rejection, and shutdown; and
- retain upstream licensing and a project third-party notice.

Exit criteria:

- `relay` and tests link the pinned static target;
- the shipped executable has no dynamic Lua dependency;
- sandbox startup fails closed if forbidden globals become visible; and
- lifecycle tests pass under the project warning policy.

## Stage 1 — deterministic Lua artifacts

**Status:** core artifact execution implemented; stable source hashes and
save/load recompilation remain part of this stage.

Scope:

- introduce stable script source/artifact IDs, ownership, revisions, and hashes;
- implement a project-owned Lua-subset validator before Lua compilation;
- reject floating literals, floating division, exponentiation, unordered table
  iteration, dynamic code loading, metatable manipulation, and unsupported
  syntax with source-ranged diagnostics;
- compile source in memory into isolated per-artifact environments;
- bind only deterministic integer, Boolean, string, fixed-array, and
  project-owned APIs;
- install a new artifact transactionally only after validation and compilation;
- keep the prior installed artifact active after failure; and
- discard compiled Lua functions on save/load and recompile authoritative source.

Exit criteria:

- accepted/rejected source fixtures have exact diagnostics;
- identical source and API versions produce identical artifact hashes;
- forbidden APIs and numeric behavior cannot be reached indirectly; and
- malformed source or allocator exhaustion leaves the prior artifact intact.

## Stage 2 — capability-scoped program execution

Scope:

- add controller/program-slot ownership to node definitions and instances;
- expose immutable previous-tick node/property/port snapshots to Lua;
- resolve authored names to stable IDs at install time;
- derive read/write/message capabilities from physical typed graph connections;
- queue typed property writes and messages without direct world mutation;
- order queued effects by target ID, property ID, controller ID, and sequence;
- enforce deterministic instruction hooks, invocation depth, event queue bounds,
  string limits, and transient-memory limits;
- store persistent script state in project-owned typed values; and
- emit observable compile, deploy, budget, denial, and runtime-fault events.

Exit criteria:

- two controllers produce the same committed state regardless of storage order;
- access without a valid control/message path returns a typed denial;
- budget exhaustion cannot stall a simulation tick;
- program state survives save/reload independently of Lua VM internals; and
- scripts cannot mutate terminal, application, event bus, or node arrays.

## Stage 3 — blueprint definition and compiler

**Status:** implemented. Stable schemas, typed boundary nodes, visual
component port maps, recursive deterministic flattening, dependency-cycle
validation, transactional instantiation, and instance/local-node provenance
all use the normal `Relay_NodeWorld` contract.

Scope:

- implement immutable blueprint schemas, typed parameters, external ports,
  components, internal wires, programs, and layout;
- validate unique local keys, definition references, port directions/types,
  parameter values, external bindings, and dependency acyclicity;
- recursively compile nested blueprints into a deterministic flattened plan;
- instantiate plans transactionally with new monotonic node/edge IDs;
- retain instance/local-path provenance and external connection bindings;
- preserve module abstraction in the renderer without creating a second
  simulation graph; and
- reject an invalid entire instance without leaking partial graph state.

Exit criteria:

- a blueprint containing a Clock, Coal Miner, feedback fuel edge, and control
  program can be nested twice and instantiated atomically;
- invalid nested dependencies or bindings create no nodes or edges;
- equivalent compilations yield identical plans and provenance paths; and
- instantiated modules use normal graph compatibility and tick semantics.

## Stage 4 — in-game Code and Blueprint workspaces

**Status:** core workspace implemented. Blueprint creation, typed architecture
boundaries, nested component placement, top-level scene switching, Script-panel
instantiation, the bounded Vim-style modal editor, transactional `:w`/`:wq`
deployment, diagnostics, and universal Back are complete. Completion/API
assistance, rename/parameter actions, and persistence land in their owning
later stages.

Scope:

- add keyboard-first Code and Blueprint tabs with mouse enhancement;
- provide a bounded text editor, source diagnostics, API completion, and examples;
- expose compile, validate, deploy, revert, rename, parameterize, and test actions;
- render blueprint components, external ports, nested instances, and validation
  errors using the graph renderer contract;
- make Escape unwind editor dialogs, nested blueprints, and workspaces before
  exit confirmation; and
- log diagnostics through Relay’s file logger while showing player-facing
  compile/runtime results inside the TUI.

Exit criteria:

- a player can create, edit, compile, deploy, and instantiate a blueprint
  entirely with the keyboard at 80×24;
- failed deployment visibly preserves the running version;
- mouse and keyboard actions share the same commands; and
- TUI snapshot tests cover compact and wide layouts.

## Stage 5 — debugger and deterministic event programs

Scope:

- implement subscriptions for tick, clock edge, node state, message, and fault;
- add bounded deterministic event queues and latched critical events;
- provide pause, step, bounded run, watches, breakpoints, invocation traces,
  property-write history, capability failures, and “why blocked” explanations;
- expose instruction use, queue depth, persistent-state diffs, and source ranges;
- record script-caused commands in the simulation event/command history; and
- verify replay hashes across pause/run/debug interaction sequences.

Exit criteria:

- the exact program and source line responsible for a state change is traceable;
- stepping and continuous running reach the same state at the same tick;
- queue overflow and budget failures are deterministic and visible; and
- replay reproduces script commands and state hashes.

## Stage 6 — blueprint persistence and exchange

Scope:

- add versioned, validated blueprint import/export containing source, graph,
  parameters, ports, metadata, and layout but no runtime resources or Lua state;
- add binary save chunks for installed source revisions, typed persistent state,
  blueprint definitions, instances, provenance, and pending deterministic events;
- preserve unknown optional chunks and implement explicit migrations;
- validate checksums, sizes, recursion depth, source limits, and all references;
- recompile source after load when Lua/API/compiler versions change; and
- make import and deployment transactional with stable error reports.

Exit criteria:

- nested blueprint export/import round-trips structurally;
- corrupt and hostile inputs fail without modifying the active world/save;
- historical fixtures migrate through explicit tested paths; and
- save/replay is independent of Lua bytecode and pointer identity.

## Stage 7 — scripted automation vertical slice

Scope:

- add the first controller and typed control connections;
- provide Clock, Coal Miner, storage/target state, and the minimum material chain;
- ship a working starter program and API hints;
- require the player to adapt policy rather than manually trigger each cycle;
- package the working line as a reusable blueprint and instantiate it again; and
- provide scenario tests covering run, pause, edit, failed deploy, debug, reuse,
  save/load, and replay.

Exit criteria:

- a fresh player scripts a self-running production line;
- the line continues without per-cycle input;
- the module can be reused inside another blueprint;
- the event trace explains every enable, block, and produced resource; and
- the complete scenario is deterministic across supported platforms.

## Stage 8 — hardening and scale gate

Scope:

- fuzz the subset validator, blueprint importer, artifact loader, and API decoder;
- soak nested scripted factories under instruction/memory/event limits;
- benchmark compile, instantiate, tick, debugger, save, and load paths;
- validate deterministic hashes across macOS, Linux, and Windows;
- complete keyboard-only and terminal compatibility acceptance; and
- audit all Lua and blueprint APIs for capability, bounds, and lifecycle safety.

Exit criteria:

- all scripting/blueprint invariants and release gates are automated;
- a script cannot escape its capability, memory, or instruction limits;
- a 10,000-node scripted benchmark meets the project simulation target; and
- no stage relies on compatibility shims, placeholder hooks, or a second graph.
