// ui.h — the only place that knows Dear ImGui exists.
//
// These functions read the World (and let the player mutate it through its
// public API), once per frame. The simulation layer knows nothing about them.
#pragma once

class World;

// Draws offices, routes and trucks into the background draw list (underneath
// all ImGui windows) and handles click-to-select on the map.
void DrawWorld(const World& world);

// Top-left status window: day/tick, money, delivery stats, pause.
void DrawHUD(World& world);

// Inspector for the selected post office: outbound letter table and
// schedule editing.
void DrawInspector(World& world);
