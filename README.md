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

Runtime diagnostics are written only to `logs/relay.log`, keeping the terminal
surface reserved for the game UI.

The application owns a main-thread observer bus and a cross-platform worker
pool. Their thread-safety and ownership rules are documented for contributors
in [AGENTS.md](AGENTS.md).

## Controls

The game starts with a Coal Miner and 100 neutral currency. The Shop sells
Clock modules. Use `j`/`k` or the arrow keys to choose an offer, then Enter to
purchase and place it in the Relay workspace. Drag a Clock's right-side pulse
port onto the Coal Miner's left-side clock port to wire them together. A Clock
emits one pulse every configured gameplay ticks; use `[` and `]` while the
newly created Clock is focused to choose 2, 4, 8, 16, 32, 64, or 128 ticks.
Drag the Coal Miner's Coal output back to its Fuel input to make the initial
bootstrap loop self-sustaining. The starter miner carries one coal of fuel,
then consumes one coal for each 16-pulse mining cycle. Its progress row shows
remaining fuel (`F`) and processing progress.

The main workspace begins with a `Relay` tab. It is an unbounded graph canvas:
drag with the left mouse button to pan its grid, and purchased sources appear
as node cards with input and output ports. Drag a node card to move its world
grid position from its title bar; drag empty canvas space to pan the viewport.
Graph links are
typed and replace the destination input's prior link, which is the same graph
contract future script-authored modules will use. Connected ports are joined by
cyan orthogonal wires with rounded turns and short port stubs, rendered behind
the cards. A matching live wire previews its route while you drag from an
output; forward links use the minimal centered path, while reverse links take a
safe outside-card detour. All wires follow panning and node movement.

Every purchased node becomes the viewport focus. Press `m` to toggle the Relay
map view, which zooms out to compact node names; drag its empty space to pan at
the map scale. Escape is always Back: it first returns from map view, then
opens a centered exit confirmation from the graph view. Press Enter there to
exit, or Escape to cancel. `q` never exits the game.

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
