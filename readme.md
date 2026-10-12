# Letter Flow

A small logistics toy: letters pile up at post offices and your truck
schedules have to move them before their deadlines.

This is a project to help me learn C++, which I started after I got rejected for a
full-time new grad role at Hudson River Trading due to lack of C++ experience.

C++20, SDL2, Dear ImGui (OpenGL3). Runs on macOS, Linux and Windows.

## Build & run

The top-level `Makefile` detects the host OS and includes the matching
per-platform makefile (`Makefile.macos` / `Makefile.linux` /
`Makefile.windows`), so a bare `make` works everywhere. Each platform keeps
its objects in its own build dir, so they never collide.

macOS:

```sh
brew install sdl2
git submodule update --init   # Dear ImGui
make && ./letter-flow
```

Linux:

```sh
sudo apt install build-essential libsdl2-dev libgl-dev   # (or your distro's equivalent)
git submodule update --init   # Dear ImGui
nix-shell
make && ./letter-flow
```

Windows (MSYS2, from the "MSYS2 MinGW 64-bit" shell — not plain cmd):

```sh
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-SDL2 mingw-w64-x86_64-make
git submodule update --init   # Dear ImGui
mingw32-make && ./letter-flow.exe
```

## Controls

- **Click an office**: inspect its mail, add/remove truck schedules
- **Truck schedules**: each schedule has a routing rule — only letters for the
  destination, letters addressed to a postal-code prefix, or all letters.
  Letters that arrive at an office they are not addressed to are forwarded
  from there (hub and spoke). Drag the schedule list to reorder it: when
  several schedules depart at the same time, the topmost matching one picks
  up a letter first. Schedules are free to create; each truck costs $15 when
  it departs (skipped if you can't afford it) and carries at most 5 letters,
  oldest first; the rest wait for the next truck.
- **Mouse wheel / WASD**: zoom and pan the map
- **Transport bar**: play/pause, x4 fast-forward, day/time readout
- **Saving**: the game autosaves to a JSON file when you quit and reloads it
  on the next launch, in SDL's per-user app-data directory (`SDL_GetPrefPath`):
  `~/Library/Application Support/LetterFlow/LetterFlow/save.json` on macOS,
  `~/.local/share/LetterFlow/LetterFlow/save.json` on Linux,
  `%APPDATA%\LetterFlow\LetterFlow\save.json` on Windows.
  The save includes the RNG's internal state, so a loaded game continues the
  exact same deterministic simulation (if that state is corrupt, the game
  still loads, with a freshly seeded RNG). The window layout (`imgui.ini`)
  is kept in the same directory, so it's the same however the game is launched.

## Source layout (will probably be out of date soon)

```
src/game_types.h      shared primitives: Tick, Position, world constants
src/entities.h        data entities: Letter, RoutingRule, TruckSchedule, PostOffice
src/truck.*           Truck: deterministic point-to-point motion
src/world.*           World: owns all state, advances the sim in fixed 60 Hz ticks
src/scenario.*        create_default_world() starter map
src/serialization.cpp JSON save/load: implements World::save_to_file / load_from_file
src/ui.*              Dear ImGui presentation layer (the only place ImGui exists)
src/main.cpp          SDL2 + OpenGL3 bootstrap and frame loop (all platforms)
src/platform_macos.*  macOS-only startup tweaks (the sole Objective-C++ file)

tests/test_geometry.cpp       headless exact road-geometry tests (make test)
tests/test_serialization.cpp  headless save/load round-trip tests (make test)
tests/test_simulation.cpp     headless simulation-rule tests (make test)
third_party/nlohmann/json.hpp vendored single-header JSON library (only
                              serialization.cpp includes it)
```

## Tests

```sh
make test   # headless: no window, no SDL — links only the simulation layer
```

The save/load round-trip test builds a fixture world in which **every
serialized field holds a non-default value**, saves it, loads it back and
asserts full-state equality. If you add a field to the simulation and forget
`serialization.cpp`, the loaded copy comes back with the default and the test
fails loudly. Every `operator==` is `= default` (`World`'s and `Truck`'s
included), so new fields are compared automatically — just give them a
distinct value in the fixture.

Everything except `ui.*`, `main.cpp` and `platform_macos.*` handles simulation,
and is fully deterministic (fixed RNG seed, fixed timestep — the RNG state is
part of the save file, so determinism survives save/load).
