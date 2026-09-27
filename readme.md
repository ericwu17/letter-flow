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

- **Click an office**: inspect its mail, add/remove truck routes
- **Mouse wheel / WASD**: zoom and pan the map
- **Transport bar**: play/pause, x4 fast-forward, day/time readout

## Source layout (will probably be out of date soon)

```
src/game_types.h   shared primitives: Tick, Position, world constants
src/entities.h     data entities: Letter, TruckSchedule, PostOffice
src/truck.*        Truck: deterministic point-to-point motion
src/world.*        World: owns all state, advances the sim in fixed 60 Hz ticks
src/scenario.*     create_default_world() starter map
src/ui.*           Dear ImGui presentation layer (the only place ImGui exists)
src/main.mm        SDL2 + Metal bootstrap and frame loop
```

Everything except `ui.*` and `main.mm` handles simulation,
and fully deterministic (fixed RNG seed, fixed timestep).
