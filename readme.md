# Letter Flow

A small logistics toy: letters pile up at post offices and your truck
schedules have to move them before their deadlines.

This is a project to help me learn C++, which I started after I got rejected for a
full-time new grad role at Hudson River Trading due to lack of C++ experience.

C++20, SDL2,  Metal,  Dear ImGui, macOS only (for now).

## Build & run

```sh
brew install sdl2
git submodule update --init   # Dear ImGui
make && ./letter-flow
```

## Controls

- **Click an office**: inspect its mail, add/remove truck schedules
- **Truck schedules**: each schedule has a routing rule — only letters for the
  destination, letters addressed to a postal-code prefix, or all letters.
  Letters that arrive at an office they are not addressed to are forwarded
  from there (hub and spoke). Drag the schedule list to reorder it: when
  several schedules depart at the same time, the topmost matching one picks
  up a letter first.
- **Mouse wheel / WASD**: zoom and pan the map
- **Transport bar**: play/pause, x4 fast-forward, day/time readout
- **Saving**: the game autosaves to a JSON file when you quit and reloads it
  on the next launch (`~/Library/Application Support/LetterFlow/LetterFlow/save.json`).
  The save includes the RNG's internal state, so a loaded game continues the
  exact same deterministic simulation.

## Source layout (will probably be out of date soon)

```
src/game_types.h      shared primitives: Tick, Position, world constants
src/entities.h        data entities: Letter, TruckSchedule, PostOffice
src/truck.*           Truck: deterministic point-to-point motion
src/world.*           World: owns all state, advances the sim in fixed 60 Hz ticks
src/scenario.*        create_default_world() starter map
src/serialization.cpp JSON save/load: implements World::save_to_file / load_from_file
src/ui.*              Dear ImGui presentation layer (the only place ImGui exists)
src/main.mm           SDL2 + Metal bootstrap and frame loop

tests/test_serialization.cpp  headless save/load round-trip test (make test)
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
fails loudly. Entity `operator==` are `= default` (new fields are covered
automatically); `Truck`'s and `World`'s are hand-maintained — extend those and
the fixture when you add fields there.

Everything except `ui.*` and `main.mm` handles simulation,
and is fully deterministic (fixed RNG seed, fixed timestep — the RNG state is
part of the save file, so determinism survives save/load).
