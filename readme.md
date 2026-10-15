# Letter Flow

A small logistics toy: letters pile up at post offices and your truck
schedules have to move them before their deadlines.

In the spirit of Mini Metro, the map grows on its own: a game opens with
three post offices, and new ones keep appearing at random spots, each named
from `assets/office_names.txt` and with no postal code. A post office whose
outbound buffer is full flashes red, and a red ring around it starts
closing; once the office has room again the ring slowly opens back up. If
any office's ring closes (6 in-game hours of being full, about 45 seconds
at normal speed), the game is over.

This is a project to help me learn C++, which I started after I got rejected for a
full-time new grad role at Hudson River Trading due to lack of C++ experience.

C++20, SDL2, Dear ImGui (OpenGL3), ImPlot. Runs on macOS, Linux and Windows.

## Build & run

The top-level `Makefile` detects the host OS and includes the matching
per-platform makefile (`Makefile.macos` / `Makefile.linux` /
`Makefile.windows`), so a bare `make` works everywhere. Those only set the
platform's compiler, libraries and extra sources; the source lists and every
rule live in `Makefile.common`. Each platform keeps its objects in its own
build dir, so they never collide.

macOS:

```sh
brew install sdl2
git submodule update --init   # Dear ImGui, ImPlot
make && ./letter-flow
```

Linux:

```sh
sudo apt install build-essential libsdl2-dev libgl-dev   # (or your distro's equivalent)
git submodule update --init   # Dear ImGui, ImPlot
nix-shell
make && ./letter-flow
```

Windows (MSYS2, from the "MSYS2 MinGW 64-bit" shell — not plain cmd):

```sh
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-SDL2 mingw-w64-x86_64-make
git submodule update --init   # Dear ImGui, ImPlot
mingw32-make && ./letter-flow.exe
```

## Controls

- **Click an office**: inspect its mail, add/remove truck schedules, and
  give it a postal code (Settings). While its overflow timer runs, the
  inspector says how long is left.
- **Truck schedules**: each schedule has a routing rule — only letters for the
  destination, letters addressed to a postal-code prefix, or all letters.
  Letters that arrive at an office they are not addressed to are forwarded
  from there (hub and spoke). Schedules are listed in an editable table:
  change a schedule's rule, prefix, frequency or start time in place. Drag a row's # to reorder the table: when
  several schedules depart at the same time, the topmost matching one picks
  up a letter first. Schedules are free to create; each truck costs $15 when
  it departs (skipped if you can't afford it) and carries at most 5 letters,
  oldest first; the rest wait for the next truck.
- **Statistics (P, or the HUD button)**: Factorio-style charts over the last
  6 h / 1 day / 10 days / 30 days of game time. *Network*: money in and out,
  letters posted / delivered / lost (refused by a full buffer), letters waiting
  vs. on trucks, delivery time. *Offices*: a sortable table plus charts for
  mail flow, backlog against capacity and waiting time. *Schedules*: how full
  each schedule's trucks leave ("load"), matching letters left behind for lack
  of room, empty runs, income and net profit, with a per-departure bar chart
  and edits marked on the load chart. *Recent deliveries*: the last 100
  delivered letters. The inspector shows the same office stats, and a Load
  column in the schedule table.
- **Letter trails**: every letter records where it was posted, when it left
  which office on which schedule, and where it arrived. Hover a letter in any
  list for its journey (with time spent waiting vs. on the road); the map
  highlights its route, its truck and its destination.
- **Mouse wheel / WASD**: zoom and pan the map
- **Transport bar**: play/pause, x4 fast-forward, day/time readout
- **Pause menu (Esc, or the HUD's Menu button)**: Resume / Save / Main menu /
  Quit, over the frozen game; Esc again resumes. When Esc has something else
  to cancel (road building, an open popup, a text field), it does that first.
  In the tutorial, Save is disabled.
- **F1 / F2**: Dear ImGui / ImPlot demo windows (widget reference while developing)
- **Game over**: when an office overflows, the clock stops and a card
  names the office and sums up the game (how long it lasted, letters
  delivered and lost), with Main menu and Quit. The ended game is saved
  right away and stays in the save list marked GAME OVER; playing it
  again shows the final map and the card, but it can't be continued.
- **Main menu and saves**: the game opens on a main menu. *Play* lists
  your saves (most recently played first): start a new game, or rename,
  copy, delete or play an existing save (double-clicking a row plays it
  too). *Tutorial* opens a small five-town practice map, where pop-up
  cards walk you through building roads, creating and editing schedules,
  overflowing offices, hub-and-spoke forwarding and postal-code routing.
  Most cards continue once you've done what they ask; any can be skipped.
  New offices appear and overflow ends the game there too. The tutorial
  is never saved: leaving it, or quitting during it, discards it. *Credits*
  is a placeholder page for now.
- **Saving**: each save is a JSON file of its own in `saves/`,
  inside SDL's per-user app-data directory (`SDL_GetPrefPath`):
  `~/Library/Application Support/LetterFlow/LetterFlow/saves/` on macOS,
  `~/.local/share/LetterFlow/LetterFlow/saves/` on Linux,
  `%APPDATA%\LetterFlow\LetterFlow\saves\` on Windows.
  A save's filename is its name, lowercased with anything but letters and
  digits turned into `_` (`Bob's Game #2` → `bob_s_game_2.json`); the real
  name is kept inside the file. Saves whose names give the same filename are
  numbered (`game_1.json`, `game_1-2.json`, ...), and renaming a save moves
  it to a file named after its new name. A game is saved back into the save
  you launched at the start of every game day, when you return to the main
  menu, when you quit, and whenever you pick Save in the pause menu. A file in the folder that can't be read
  is skipped (and left as it is), and the menu says so.
  The single `saves.json` (and the older `save.json`) of earlier versions is
  no longer read. Saves from before offices appeared on their own (world
  format 3, with offices you paid to build) still list but no longer load;
  they can be deleted from the list.
  Each save also holds the statistics history and letter trails (saves from
  before statistics existed still load, with empty history).
  The save includes the RNG's internal state, so a loaded game continues the
  exact same deterministic simulation (if that state is corrupt, the game
  still loads, with a freshly seeded RNG). The window layout (`imgui.ini`)
  is kept in the same directory, so it's the same however the game is launched.

## Source layout (will probably be out of date soon)

Sources are grouped by layer, one directory each, and includes name the layer
(`#include "sim/world.h"`). Dependencies only point down the list: `sim/`
includes nothing else of ours, `persistence/` only `sim/`, `ui/` both of those,
and `app/` everything.

```
src/sim/                 the game rules: pure logic, deterministic, no SDL/ImGui/OpenGL
  game_types.h           shared primitives: Tick, Position, world constants
  balance.h              gameplay tuning: costs, starting money, truck, letter, office-spawning and overflow numbers
  entities.h             data entities: Letter (+ trail), RoutingRule, TruckSchedule, PostOffice
  office_names.*         the names new offices get (assets/office_names.txt, embedded at build time)
  stats.*                StatSeries: multi-resolution ring buffers behind every statistic
  truck.*                Truck: deterministic point-to-point motion
  road.*                 Road and the exact integer geometry behind road rules
  road_builder.*         RoadBuilder: snaps the road being drawn to legal segments
  world.*                World: owns all state, advances the sim in fixed 60 Hz ticks, spawns offices, ends the game
  scenario.*             create_default_world() new game, create_tutorial_world() practice map
  tutorial.*             Tutorial: the tutorial's steps, their completion checks and progress
src/persistence/         saving and loading (the only code that knows nlohmann/json)
  world_json.h           WorldJson: World <-> JSON (keeps nlohmann out of world.h)
  serialization.cpp      JSON conversion of the whole simulation state: implements WorldJson
  save_library.*         SaveLibrary: one JSON file per save (create, copy, rename, delete, load, store)
src/ui/                  Dear ImGui presentation layer (the only place ImGui exists)
  ui.*                   public entry points; shared UI state and its reset, formatting, the dockspace
  ui_internal.h          the per-game UI state (GameUiState) and helpers shared between the ui*.cpp files
  ui_map.cpp             the map: camera, offices, roads, trucks, selection, road building
  ui_hud.cpp             transport bar (play/pause, speed, clock) and the HUD card
  ui_inspector.cpp       inspector: office panel (letters, schedules, new-schedule form), truck panel
  ui_menu.cpp            main menu, credits page and save list (the screens before a game starts), pause menu, game-over card
  ui_tutorial.cpp        tutorial cards and spotlights
  ui_stats.cpp           statistics window, stat widgets and letter-trail tooltips (ImPlot)
  ui_theme.*             the "postal" look: palette, ImGui/ImPlot style, embedded font, styled widgets
src/app/                 the application shell
  main.cpp               SDL2 + OpenGL3 bootstrap and frame loop (all platforms)
  app_icon.*             window/Dock icon from assets/logo.png (embedded at build time)
  platform_macos.*       macOS-only startup tweaks (the sole Objective-C++ file)

tests/test_geometry.cpp       headless exact road-geometry and truck-motion tests (make test)
tests/test_road_builder.cpp   headless road-drawing tests: snapping, committing, validity (make test)
tests/test_serialization.cpp  headless World <-> JSON round-trip tests (make test)
tests/test_save_library.cpp   headless save-file tests: filenames, duplicates, corruption (make test)
tests/test_simulation.cpp     headless simulation-rule tests (make test)
tests/test_stats.cpp          headless StatSeries ring-buffer tests (make test)
tests/test_tutorial.cpp       headless tutorial play-through on the practice map (make test)
implot/                       ImPlot v1.0 (git submodule), charts
third_party/nlohmann/json.hpp vendored single-header JSON library (only
                              the persistence code includes it, via world_json.h)
third_party/stb/stb_image.h   vendored PNG decoder, v2.30 (only app_icon.cpp
                              includes it)
assets/logo.png               app icon, compiled into the executable
assets/office_names.txt       post office names, one per line, compiled into the executable
imgui/misc/fonts/Roboto-Medium.ttf  UI font, compiled into the executable
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

Everything in `src/sim/` and `src/persistence/` handles simulation (the
headless tests link exactly those), and is fully deterministic (fixed RNG seed, fixed timestep — the RNG state is
part of the save file, so determinism survives save/load).
