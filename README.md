# Relay

Relay is a small, self-contained C terminal UI starter built with CMake and
[Termbox2](https://github.com/termbox/termbox2).

## Requirements

- CMake 3.24 or newer
- A C17 compiler
- A terminal on macOS or Linux; Windows 10 version 1607 or newer for its native
  virtual-terminal console path

## Build and run

```sh
cmake -S . -B build
cmake --build build
./bin/relay
```

On Windows, run `bin\\relay.exe` from a console.

The executable is emitted directly into `bin/` and has no third-party runtime
dependencies. Termbox2 is vendored in `vendors/termbox2`; its header-only
implementation is compiled into the executable on macOS and Linux. Windows
uses the native Console API because upstream Termbox2 is POSIX-only.

Lua 5.5.0 is checksum-pinned under `vendors/lua/` and compiled into the
executable as a private static library. Relay opens only its sandboxed safe
subset; there is no external Lua installation, interpreter executable, dynamic
module loader, filesystem, operating-system, debug, or math library available
to player programs. See [the Blueprint guide](docs/BLUEPRINTS.md) for the
editor, module API, and sandbox rules, and
[the scripting roadmap](docs/SCRIPTING_BLUEPRINT_ROADMAP.md) for the staged
long-term capability model.

Runtime diagnostics are written only to `logs/relay.log`, keeping the terminal
surface reserved for the game UI.

The application owns a main-thread observer bus and a cross-platform worker
pool. Their thread-safety and ownership rules are documented for contributors
in [AGENTS.md](AGENTS.md).

## Controls

The game starts with a Coal Miner and 100 neutral currency. The Shop sells Coal,
Iron, Copper, and Stone miners plus the optional Timer. Use `j`/`k` or the arrow
keys to choose an offer, then Enter to purchase and place it in the Relay
workspace. Every miner is an autonomous source that emits one correctly typed
resource per second while enabled; it has no material or control input. A Timer
emits a typed `Trigger` event at a configurable interval for scripts and future
control nodes. Press Tab or click a panel tab to switch between Shop, Inspector,
and Scripts. Click a node title to focus it and open its Inspector. Miner
Inspectors show output type, fixed rate, progress, and lifetime production. The
Timer Inspector uses `[` and `]` to choose an interval of 1, 2, 4, 8, or 16
seconds.

The main workspace begins with a `Relay` tab. It is an unbounded graph canvas:
drag with the left mouse button to pan its grid, and purchased sources appear
as node cards with input and output ports. Drag a node card to move its world
grid position from its title bar; drag empty canvas space to pan the viewport.
Graph links are
typed and replace the destination input's prior link, which is the same graph
contract future script-authored modules will use. Connected ports are joined by
cyan orthogonal wires with rounded turns and short port stubs, rendered behind
the cards. A matching live wire previews its route while you drag from either
port direction; forward links use the minimal centered path, while reverse
links take a safe outside-card detour. All wires follow panning and node movement.
Port colors identify fixed semantic types for Trigger, Coal, Iron Ore, Copper
Ore, and Stone. Types are enforced by the graph, so resource kinds cannot be
interchanged or fed into a Trigger input.

Every purchased node becomes the viewport focus. Press `m` to toggle the Relay
map view, which zooms out to compact node names; drag its empty space to pan at
the map scale. Escape is always Back: it first returns from map view, then
opens a centered exit confirmation from the graph view. Press Enter there to
exit, or Escape to cancel. `q` never exits the game.

## Script Blueprints

Press `N` to create a compiled Blueprint and open its top-level design scene.
Relay remains permanently visible as the first tab. Open Blueprint tabs appear
beside it; click a visible tab or use `,` and `.` to move between open tabs.
Press `C` to close the active Blueprint tab without deleting its source, scene,
or placed instances. Each Blueprint architecture contains `Module Inputs`,
`Module Outputs`, and only the real components added by the player—the
Blueprint never renders itself as a middle node. Add another Blueprint from the
Scripts panel, then wire `Module Inputs → child component → Module Outputs` to
create a typed HDL-style port map. Graph additions, connections, replacements,
and component layout are synchronized into the source's canonical architecture
section; editing that section and saving rebuilds the graph.
Architectures recursively compile into one deterministic flattened
`Relay_NodeWorld`; direct and indirect dependency cycles are rejected. Press
`O` to open the selected Blueprint or Enter to add it as a normal typed node in
the active scene.

Press `E` in a Blueprint scene, or while one of its module nodes is focused, to
open the modal code editor. Normal mode supports `h`/`j`/`k`/`l`, `0`, `$`,
`x`, `i`, `a`, `I`, `A`, and `o`. Escape returns insert or command mode to
normal mode. Insert mode has built-in semantic highlighting, completion with
Up/Down and Tab, and live signature help for Relay and safe Lua APIs. Use `:w`
to validate, compile, and transactionally deploy, `:wq`
to deploy and return, or `:q` to return while retaining the draft. A failed
compile leaves the prior installed revision running and shows the diagnostic in
the right panel. Escape from normal mode returns to the graph without closing
the application. Escape from a Blueprint graph returns to Relay; a subsequent
Escape from Relay opens the exit confirmation.

## Embedded font

Departure Mono Nerd Font Mono is kept in `assets/fonts/` with its SIL Open Font
License and compiled into the executable during the CMake build. A terminal
still chooses its own rendering font, so select Departure Mono in the terminal
profile to see its Nerd Font glyphs.

## Platform macros

Include `relay/platform.h` from application code. It exposes exactly one of
`RELAY_PLATFORM_WINDOWS`, `RELAY_PLATFORM_MACOS`, or `RELAY_PLATFORM_LINUX` as
`1`, and the other platform macros as `0`. `RELAY_PLATFORM_NAME` supplies a
display-ready platform name.
