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

// What the player chose in the pause menu.
enum class PauseAction { None, MainMenu, Quit };

// The in-game pause menu (ui_menu.cpp): a modal over the game, opened with
// Esc (unless Esc has something else to cancel, such as road building, an
// open popup or a text field) or the HUD's Menu button, and closed with Esc
// or Resume. Save stores `world` into `save` right away and says how that
// went; it is disabled in the tutorial (no save). Main menu and Quit are
// left to the caller, which autosaves before doing either.
PauseAction DrawPauseMenu(const World& world, SaveLibrary& library, std::optional<SaveId> save);

// Whether the pause menu is open; the game is frozen while it is.
bool PauseMenuOpen();

// The game-over card (ui_menu.cpp): a modal over the frozen final map once
// the World says the game is over, naming the office that overflowed and
// summing up the game, with Main menu and Quit. It cannot be dismissed
// otherwise: an ended game can be looked at but not played on. Takes the
// pause menu's place (DrawPauseMenu does nothing while the game is over);
// the caller handles the action as it does the pause menu's.
PauseAction DrawGameOver(const World& world);

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
// the statistics and pause-menu buttons. Also toggles the ImGui / ImPlot demo
// windows (F1 / F2).
void DrawHUD(const World& world);

// Road tool pinned to the bottom-left of the map: the "Build road" button
// (also R, which cancels too), growing into a banner that walks through the
// steps while a road is being drawn. Hidden once the game is over.
void DrawRoadTool(const World& world);

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
