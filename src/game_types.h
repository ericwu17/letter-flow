// game_types.h — pure simulation layer: shared primitive types and constants
// (time, world geometry).
//
// Simulation headers must NOT include imgui/SDL/Metal headers: the game rules
// are completely independent of rendering, so the simulation can be tested or
// run headless without a window.
#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

using Tick = std::uint64_t;

constexpr Tick kTicksPerSecond = 60;
constexpr Tick kTicksPerDay    = 3 * 60 * kTicksPerSecond;  // one in-game day = 3 real minutes = 10800 ticks

// ---------------------------------------------------------------------------
// World geometry
// ---------------------------------------------------------------------------

// The game plays out in a virtual coordinate space; the UI scales it to fit
// whatever window size the player has.
constexpr float kWorldWidth  = 1000.0f;
constexpr float kWorldHeight = 650.0f;
constexpr float kTruckSpeed  = 2.0f;  // world units per tick

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};
