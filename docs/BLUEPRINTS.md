# Script Blueprints

Relay Blueprints are hierarchical modules with a typed public interface, a
visual architecture, and a player-authored Lua process. They follow
the same model as an HDL entity plus architecture: the source declares public
ports and module behavior, while the architecture connects those ports to
built-in nodes or other Blueprints. The module itself is never displayed as a
component in its own scene. A compiled Blueprint can be placed in Relay or used
as a component inside another Blueprint.

A placed Blueprint is an ordinary `Relay_NodeWorld` node. Port typing,
one-source-per-input, output fan-out, previous-tick wire latency, and fixed-tick
simulation are identical for built-in nodes, module processes, and nested modules.
Compilation recursively flattens the hierarchy into this one graph contract;
there is no scripting-only wire format or nested simulator.

## Creating and using a Blueprint

1. Press `N` to create a Blueprint. Relay opens its design scene.
2. Press `E` to edit its source.
3. Declare the module's public inputs and outputs, then implement `tick` for
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
Script 1 Module Inputs.clock
    -> Script 2.clock
Script 2.clock_out
    -> Script 1 Module Outputs.clock_out
```

Drag may begin from either the input or output port row. The terminal normalizes
the gesture into an output-to-input connection, and the graph accepts it only
when both declared port types match. An output may feed several destinations;
an input has one source, and a new valid wire replaces its previous source.

`Module Inputs` and `Module Outputs` are architecture boundary nodes, not
runtime machines. The Lua `tick` function is the Blueprint's implicit process;
it compiles into the flattened implementation but is deliberately not rendered
as a self-component. A visual output mapping replaces the implicit process as
that public output's source.

Nested plans are elaborated transactionally. Unavailable dependencies, invalid
port maps, type errors, and dependency cycles preserve the last valid plan and
do not leave partial runtime nodes or wires.

## Source and graph synchronization

The source owns a canonical architecture region:

```lua
-- relay architecture begin
-- component n3 : blueprint.script_2 at (96, 0)
-- port map input.clock => n3.clock
-- port map n3.clock_out => output.clock_out
-- relay architecture end
```

This is Relay's compact VHDL-style component/port-map representation. Component
keys, definition keys, coordinates, and port keys are stable data rather than
display labels.

Adding a component, connecting a wire, replacing an input source, or moving a
component updates this region and transactionally recompiles the Blueprint.
Editing the canonical component or port-map statements and saving with `:w`
rebuilds the visual architecture through the same parser and typed graph
validation. Component declarations must precede their port maps. Invalid keys,
directions, types, duplicate input sources, self-imports, or dependency cycles
leave both the previous graph and compiled plan installed.

Relay is a permanent pinned tab. Creating a Blueprint opens its tab beside
Relay. `C` closes the active Blueprint tab and returns to Relay; closing a tab
does not delete its source, graph scene, compiled artifact, or placed instances.
The Blueprint remains in the Scripts panel and can be reopened with `O`.
Escape from a Blueprint architecture returns to Relay without closing its tab;
only Escape from Relay offers the exit confirmation.

## Module source

The public interface is declared before `tick`:

```lua
input("clock", "clock")
input("enabled", "boolean")
output("pulse", "clock")
output("count", "integer")

function tick(inputs, state)
  state.count = (state.count or 0) + (inputs.clock or 0)
  return {
    pulse = inputs.enabled and inputs.clock or 0,
    count = state.count
  }
end
```

Supported port types are `clock`, `coal`, `iron_ore`, `copper_ore`, `stone`,
`boolean`, and `integer`. Port keys must be unique Lua identifiers. A module
supports at most eight inputs and eight outputs.

`inputs` is an immutable snapshot for the current gameplay tick. `state` is a
private persistent table for that placed instance. State keys must be short
strings and values must be integers, Booleans, or bounded strings; at most 64
entries are retained. `tick` returns a table whose keys match declared outputs.
Missing output keys emit zero or false. A wrong output type faults that
invocation without committing outputs or state.

Self-connections retain Relay's previous-tick feedback semantics, so a module
cannot recurse through itself inside one simulation tick.

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
