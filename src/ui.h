// ui.h — the only place that knows Dear ImGui exists.
//
// These functions read the World (and let the player mutate it through its
// public API), once per frame. The simulation layer knows nothing about them.
#pragma once

class World;

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

// Status window: money and delivery stats.
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
