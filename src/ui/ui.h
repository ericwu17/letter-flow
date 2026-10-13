// ui.h — the only place that knows Dear ImGui exists.
//
// These functions read the World (and let the player mutate it through its
// public API), once per frame. The simulation layer knows nothing about them.
#pragma once

#include "persistence/save_library.h"  // SaveId, SaveSummary
#include "sim/tutorial.h"
#include "sim/world.h"

#include <optional>

// What the main menu launched: a save, or (no save) the tutorial, which is
// never stored anywhere.
struct MenuLaunch {
    std::optional<SaveId> save;
};

// The screens before a game starts (ui_menu.cpp): the main menu, the credits
// page and the save list, drawn fullscreen. The save list creates, renames,
// copies and deletes saves in `library`. When the player launches one, its
// world is loaded into `game` and its id returned; a save that fails to load
// stays on the list with an error instead. The Tutorial button loads the
// tutorial world (create_tutorial_world) into `game` instead.
std::optional<MenuLaunch> DrawMainMenu(SaveLibrary& library, std::optional<World>& game);

// Forgets the UI state that belongs to one game (selection, camera, road
// building, form inputs, the statistics window), so the next game starts
// clean. DrawMainMenu calls it whenever it launches a game.
void ResetGameUi();

// Fullscreen dockspace over the main viewport, hosting the dockable windows.
// The central node is passthrough (transparent and click-through) so the map
// drawn into the background draw list stays visible and interactive. On first
// run builds the default layout: the inspector docked into a left split;
// afterwards the user's own arrangement is loaded from imgui.ini untouched.
void DrawDockspace();

// Draws offices, routes and trucks into the background draw list (underneath
// all ImGui windows) and handles hover and click-to-select (offices, trucks)
// on the map.
void DrawWorld(World& world);

// Top-center transport bar: play/pause, slow-down/speed-up and the day/time
// readout, drawn into the background draw list on top of the map. Laid out
// from the display size each frame, so it survives window resizes.
void DrawTransportBar(World& world);

// Status card pinned to the top-right of the map: money, delivery counts and
// the statistics button. Also toggles the ImGui / ImPlot demo windows (F1 / F2).
void DrawHUD(const World& world);

// Inspector for the current selection, docked on the left: for a post
// office, its outbound letter table and schedule editing; for a truck, its
// route, timing and cargo. A selected truck that arrives hands the
// selection over to its destination office.
void DrawInspector(World& world);

// The statistics window (toggled with P or the HUD button): network-wide,
// per-office and per-schedule charts over a chosen time range, and the
// journeys of recently delivered letters.
void DrawStatsWindow(const World& world);

// The tutorial's cards (ui_tutorial.cpp), drawn over everything else: the
// current step's text with Next / Skip / Exit, and a pulsing spotlight on
// what it asks the player to use. Advances `tutorial` from the World and the
// UI's state. Returns true when the player leaves the tutorial.
bool DrawTutorial(Tutorial& tutorial, const World& world);
