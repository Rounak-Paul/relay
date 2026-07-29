# Script Blueprints

Relay Blueprints are hierarchical modules with a typed public interface, a
visual architecture, and a player-authored Lua process. They follow
the same model as an HDL entity plus architecture: the source declares public
ports and module behavior, while the architecture connects those ports to
built-in nodes or other Blueprints. The module itself is never displayed as a
component in its own scene. A compiled Blueprint can be placed in Relay or used
as a component inside another Blueprint.

A placed Blueprint is an ordinary `Relay_NodeWorld` node. Port typing,
one-source-per-input, output fan-out, previous-step wire latency, and fixed-step
simulation are identical for built-in nodes, module processes, and nested modules.
Compilation recursively flattens the hierarchy into this one graph contract;
there is no scripting-only wire format or nested simulator.

## Creating and using a Blueprint

1. Press `N` to create a Blueprint. Relay opens its design scene.
2. Press `E` to edit its source.
3. Declare the module's public inputs and outputs, then implement `on_process` for
   any behavior that belongs in its Lua process.
4. Return to normal mode and enter `:w` to compile and deploy it.
5. Press Escape to return to the visual architecture. It contains
   `Module Inputs`, `Module Outputs`, and only the real components you add.
6. To use another Blueprint as a component, open the Scripts panel with Tab or
   a mouse click, select it with `j`/`k`, and press Enter.
7. Wire from the right-side ports of `Module Inputs` into component inputs,
   then wire component outputs into the left-side ports of `Module Outputs`.
   Unmapped outputs are produced by the module's implicit Lua process; wiring a
   component into a `Module Outputs` port overrides that output mapping.
8. Use `,` or `.` to move through open Relay and Blueprint tabs.
9. In Relay, open the Scripts panel and select a Blueprint with
   `j`/`k`.
10. Press `O` to open its workspace tab, or Enter to place it in the active
   scene.
11. Connect its color-coded ports exactly like built-in nodes.

The Scripts panel may place a Blueprint in Relay or in a different Blueprint
scene. Direct or indirect recursive instantiation is rejected. Every root
placement creates a visible module wrapper plus private flattened implementation
nodes. Each module process in each placement owns independent persistent state.

## Visual port mapping

For a parent `Script 1` containing child `Script 2`, the complete visual
equivalent of an HDL component port map is:

```text
Script 1 Module Inputs.trigger
    -> Script 2.trigger
Script 2.trigger_out
    -> Script 1 Module Outputs.trigger_out
```

Drag may begin from either the input or output port row. The terminal normalizes
the gesture into an output-to-input connection, and the graph accepts it only
when both declared port types match. An output may feed several destinations;
an input has one source, and a new valid wire replaces its previous source.

`Module Inputs` and `Module Outputs` are architecture boundary nodes, not
runtime machines. The Lua `on_process` function is the Blueprint's implicit
observer; it compiles into the flattened implementation but is deliberately not
rendered as a self-component. A visual output mapping replaces the implicit
process as that public output's source.

Nested plans are elaborated transactionally. Unavailable dependencies, invalid
port maps, type errors, and dependency cycles preserve the last valid plan and
do not leave partial runtime nodes or wires.

## Source and graph synchronization

The source owns canonical top-level architecture declarations:

```lua
local n3 = instance("blueprint.script_2", { x = 96, y = 0 })
connect(inputs.trigger, n3.inputs.trigger)
connect(n3.outputs.trigger_out, outputs.trigger_out)
```

This is Relay's Lua-shaped equivalent of VHDL component declarations and port
maps. `instance` returns a typed component handle. `inputs` and `outputs` are
the module boundary namespaces; each component exposes explicit `.inputs` and
`.outputs` namespaces. The local variable is the component's stable
architecture key, while definition and port keys are stable data rather than
display labels.

These declarations are compile-time Blueprint DSL: Relay removes their text
from the runtime Lua chunk while retaining line positions for diagnostics.
Adding a component, connecting a wire, replacing an input source, or moving a
component updates the declarations and transactionally recompiles the
Blueprint. Generated source always keeps one blank line on each side of this
architecture block, regardless of how many visual edits occur. Editing
`instance` or `connect` and saving with `:w` rebuilds the visual architecture
through the same parser and typed graph validation. Component declarations must
precede their port maps. Invalid keys, directions, types, duplicate input
sources, self-imports, or dependency cycles leave both
the previous graph and compiled plan installed.

Relay is a permanent pinned tab. Creating a Blueprint opens its tab beside
Relay. `C` closes the active Blueprint tab and returns to Relay; closing a tab
does not delete its source, graph scene, compiled artifact, or placed instances.
The Blueprint remains in the Scripts panel and can be reopened with `O`.
Escape from a Blueprint architecture returns to Relay without closing its tab;
only Escape from Relay offers the exit confirmation.

## Module source

The public interface and architecture are declared before `on_process`:

```lua
input("trigger", Type.TRIGGER)
input("enabled", Type.BOOLEAN)
output("trigger_out", Type.TRIGGER)
output("count", Type.INTEGER)

function on_process(inputs, state)
  state.count = (state.count or 0) + 1
  return {
    trigger_out = inputs.enabled and inputs.trigger or 0,
    count = state.count
  }
end
```

`Type` is Relay's immutable port-type enum. Every executable script port type is
available through a capitalized member:

```lua
Type.TRIGGER
Type.COAL
Type.IRON_ORE
Type.COPPER_ORE
Type.STONE
Type.BOOLEAN
Type.INTEGER
```

`input` and `output` accept these enum values, not lowercase type strings. Port
keys must be unique Lua identifiers. A module supports at most eight inputs and
eight outputs.

`inputs` is an immutable activation snapshot. `on_process` runs once when the
module is initialized, when a nonzero transient Trigger or material delivery
arrives, or when a level-valued Boolean or Integer input changes. This
keeps player programs event-driven; periodic work requires an explicitly wired
Timer rather than access to the engine's fixed simulation cadence.

`state` is a private persistent table for that placed instance. State keys must
be short strings and values must be integers, Booleans, or bounded strings; at
most 64 entries are retained. `on_process` returns a table whose keys match
declared outputs. Trigger and material outputs last for one simulation step;
Boolean, integer, and text outputs retain their value between activations.
Missing output keys emit zero or false when a process runs. A wrong output type
faults that activation without committing outputs or state.

Self-connections retain Relay's previous-step feedback semantics, so a module
cannot recurse through itself inside one simulation step.

## Editor

The built-in editor is a compact modal editor:

- `i`, `a`, `I`, `A`, and `o` enter insert mode at familiar Vim positions;
- `h`/`j`/`k`/`l` or the arrow keys move in normal mode;
- `0`/`$` or Home/End move to line boundaries;
- `x` or Delete removes the character under the cursor;
- insert mode accepts text, Enter, Backspace, Delete, arrows, Home, and End;
- Escape returns insert or command mode to normal mode;
- `:w` validates, compiles, and deploys;
- `:wq` deploys and returns to the graph;
- `:q` returns while retaining the current source as an undeployed draft; and
- Escape from normal mode returns to the design scene.

The right panel shows the source revision, modified state, and the latest
compiler or runtime diagnostic. Deployment is transactional: invalid source
does not replace the running artifact. Once instances exist, changing a
Blueprint's public port interface is rejected so existing wires cannot silently
change meaning; code-only revisions remain deployable.

## Deterministic sandbox

Blueprint Lua runs from the statically linked, pinned Lua runtime. It has no
filesystem, process, environment, network, dynamic loader, host clock, debug
library, terminal output, or module loading. Standard-library proxies and input
snapshots are read-only.

Authoritative values are integers and Booleans. Floating-point literals,
floating division (`/`), and exponentiation are rejected before compilation;
use integer division (`//`) when needed. Unordered table iteration, metatable
access, raw table operations, and dynamic code loading are unavailable.

Each compilation and invocation has a deterministic instruction budget, and
the shared runtime has a hard memory ceiling. Budget exhaustion and runtime
errors produce diagnostics instead of stalling the game. Source is
authoritative; compiled Lua functions are runtime artifacts and are never a
portable save or interchange format.
