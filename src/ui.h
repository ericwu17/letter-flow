// ui.h — the only place that knows Dear ImGui exists.
//
// These functions read the World (and let the player mutate it through its
// public API), once per frame. The simulation layer knows nothing about them.
#pragma once

class World;

// Fullscreen dockspace over the main viewport, hosting the dockable windows.
// The central node is passthrough (transparent and click-through) so the map
// drawn into the background draw list stays visible and interactive. On first
// run builds the default layout: "Post office" docked into a left split;
// afterwards the user's own arrangement is loaded from imgui.ini untouched.
void DrawDockspace();

// Draws offices, routes and trucks into the background draw list (underneath
// all ImGui windows) and handles click-to-select on the map.
void DrawWorld(const World& world);

// Top-center transport bar: play/pause, fast-forward and the day/time
// readout, drawn into the background draw list on top of the map. Laid out
// from the display size each frame, so it survives window resizes.
void DrawTransportBar(World& world);

// Status window: money and delivery stats.
void DrawHUD(const World& world);

// Inspector for the selected post office: outbound letter table and
// schedule editing.
void DrawInspector(World& world);
