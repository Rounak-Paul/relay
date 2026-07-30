# Physical Items, Queue Scripting, and Sessions

**Status:** frozen implementation contract

This document defines Relay's physical-item ownership model, the Blueprint Lua
queue API, deterministic transport, and durable game-session lifecycle. These
systems share one rule: game state has exactly one authoritative owner at every
simulation boundary.

## Physical items

Coal, Iron Ore, Copper Ore, and Stone are physical gameplay items. Every item
has a nonzero, monotonically allocated `Relay_ItemId` and one immutable
`Relay_NodePortType`. IDs are unique within a game session and are persisted.
Only built-in sources and future data-defined recipes may create items.

Material input and output ports own bounded FIFO queues. Queue storage is
engine-owned and scripts never receive a C pointer. Boolean and Integer ports
remain retained scalar signals. Trigger ports remain transient scalar events.

The following invariants are mandatory:

1. An item exists in exactly one queue or one active script reservation.
2. Queue order and all transfers are deterministic.
3. Material inputs have one source and material outputs have one destination.
   Control outputs may fan out.
4. A full destination applies backpressure; it never destroys an item.
5. Disabling a node preserves its queues, script state, and progress.
6. Invalid scripts cannot create, copy, retain, transform, or destroy items.

## Simulation transport

Each fixed simulation step runs in stable node and connection order:

1. Move at most one item across each connected material edge when the
   destination queue has capacity.
2. Move items across flattened Blueprint input and output bindings with the
   same ownership rule.
3. Advance enabled built-in behaviors. A miner produces only when its output
   queue has capacity.
4. Invoke eligible Blueprint processes.
5. Commit scalar snapshots for the next simulation step.

Moving an item removes it from the source queue and appends it to the
destination queue atomically. Blueprint wrappers are real queue boundaries;
nested modules never copy item values.

## Blueprint process API

The only supported observer signature is:

```lua
function on_process(state, inputs, outputs)
end
```

`state` is the instance-owned bounded persistent scalar table. `inputs` and
`outputs` are sealed namespaces keyed by declared port names.

- A scalar input is read-only through `inputs.name`.
- A scalar output is assigned through `outputs.name`.
- A material port is an opaque queue proxy.
- `#queue` returns its item count.
- `queue.capacity` returns its fixed capacity.
- `queue:pop()` reserves the oldest input item.
- `queue:push(item)` stages one item into an output queue.

Example deterministic ping-pong splitter:

```lua
input("coal_in", Type.COAL)
output("coal_a", Type.COAL)
output("coal_b", Type.COAL)

function on_process(state, inputs, outputs)
  if #inputs.coal_in == 0 then
    return
  end

  local target =
    state.send_a ~= false and outputs.coal_a or outputs.coal_b

  if #target >= target.capacity then
    return
  end

  target:push(inputs.coal_in:pop())
  state.send_a = not state.send_a
end
```

## Activation transactions

`pop()` creates an activation-local reservation identified by an engine token.
Lua variables may alias the token, but aliases never copy the physical item.
The first valid `push()` consumes the reservation. The engine rejects:

- pushing one reservation more than once;
- pushing to an input or reading from an output;
- pushing to a full or incompatible queue;
- returning with an unresolved popped reservation;
- retaining a queue or item handle in `state`; and
- using a handle after its activation ends.

Queue operations, scalar outputs, and persistent-state changes are staged.
They commit together only after Lua returns successfully and every reservation
is resolved. Any Lua error, instruction-limit fault, memory failure, or
ownership violation discards the transaction and leaves all authoritative
queues and state unchanged.

No item handle contains a Lua-visible address. Tokens carry an activation
generation and reservation index validated against the active transaction.

## Activation scheduling

A Blueprint process runs:

- once for initialization;
- when a material input queue is nonempty;
- when a retained Boolean or Integer input changes; or
- when a nonzero Trigger arrives.

At most one successful process activation occurs per node per simulation step.
Scripts may move several queued items during that activation, bounded by the
instruction limit and queue capacities. Failed activations do not spin again
within the same step.

## Session storage

Relay uses versioned project-owned files rather than SQLite. Each save is one
bounded object graph plus player-authored Blueprint sources, so a database
would add unnecessary vendor surface and migration complexity. The executable
remains fully self-contained.

The data root is:

- Windows: `%USERPROFILE%\.relay\`
- macOS and Linux: `$HOME/.relay/`

The data root is organized as:

```text
.relay/
  state.rly
  sessions/
    <16-digit-session-id>/
      session.rly
      scripts/
        <blueprint-name>.lua
        <blueprint-name>.deployed.lua
```

`state.rly` contains only its format version and the last-played session ID. It
is checksummed and atomically replaced. Slot discovery scans `sessions/`
instead of trusting the root state file, so a missing or corrupt state file
cannot hide valid slot directories.

Each slot directory is a complete snapshot. `<blueprint-name>.lua` is the
current editor source, including an undeployed invalid draft.
`<blueprint-name>.deployed.lua` is the last successfully compiled source used
by simulation. `session.rly` stores the expected size and checksum of both
files, never a second embedded copy. Loading therefore fails transactionally
when a script is absent, truncated, renamed, or does not match the snapshot.

Saving builds a complete sibling staging directory, flushes every script and
the binary session file, and only then installs the directory. Overwrite keeps
the prior directory as a recoverable backup until the new slot and root state
file are committed. Startup resolves interrupted staging/backup states without
partially combining generations. A failed save leaves the previous valid slot
untouched.

The binary session file contains:

- magic, format version, payload length, and payload checksum;
- session ID, save timestamp, simulation step, currency, and next stable IDs;
- every Blueprint's name, source checksums, revision, and open/editor state;
- root and Blueprint design-world nodes, connections, and positions;
- flattened runtime node state needed to resume placed modules;
- all physical item IDs, types, queues, progress, timers, and counters; and
- bounded persistent Lua state encoded as project-owned scalar entries.

Compiled Lua registry references, definition pointers, terminal handles,
viewport drag state, jobs, and event subscribers are never serialized. Loading
validates all counts, IDs, enum values, keys, graph references, item uniqueness,
queue types, queue capacities, Blueprint dependencies, and checksum before
replacing the current game. Blueprints are recompiled and runtime references
are reconstructed transactionally.

## Session lifecycle

When saved slots exist, startup offers:

- **Continue**, which validates and opens the last-played slot;
- **Saved Slots**, which opens a bounded slot browser ordered by most recent
  save; and
- **New Session**, which creates the deterministic starting game.

`S` opens a save dialog. A game already loaded from or saved into a slot may
either overwrite that slot or fork into a new slot with a new session ID. An
unsaved game can only create a new slot. Confirmed exit uses the same dialog
and exits only after the chosen save completes. Save success or failure is
shown in the TUI and written through `relay_logger_log`; generated diagnostics
never print to stdout or stderr.

Loading updates `state.rly` only after the entire candidate has validated.
Corrupt or unsupported slots remain visible as invalid diagnostic entries,
are rejected without partial loading, and never overwrite the running game.

## Required validation

Completion requires automated coverage for:

- FIFO order, capacity, backpressure, material fan-out rejection, and stable IDs;
- duplicate aliases, stale handles, unresolved pops, wrong types, full outputs,
  Lua faults, and atomic rollback;
- ping-pong routing and nested Blueprint queue transport;
- miner production without loss while disconnected or blocked;
- save/load round trips containing scripts, nested modules, queues, and state;
- multiple slots, last-played continuation, overwrite, and save-as-new forks;
- missing, renamed, modified, and truncated external Blueprint sources;
- interrupted staging/backup directory recovery and corrupt root state;
- truncated, corrupt, oversized, duplicate-item, and unsupported-version saves;
- deterministic continuation producing the same later state as uninterrupted
  simulation;
- ASan and UBSan runtime tests; and
- interactive new, save, exit, continue, and corrupt-save recovery flows.
