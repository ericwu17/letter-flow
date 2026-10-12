// game_types.h — pure simulation layer: shared primitive types and constants
// (time, world geometry, economy).
//
// Simulation headers must NOT include imgui/SDL/OpenGL headers: the game rules
// are completely independent of rendering, so the simulation can be tested or
// run headless without a window.
#pragma once

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

using Tick = std::uint64_t;

constexpr Tick kTicksPerSecond = 60;
constexpr Tick kTicksPerDay    = 3 * 60 * kTicksPerSecond;  // one in-game day = 3 real minutes = 10800 ticks
constexpr Tick kTicksPerHour   = kTicksPerDay / 24;         // 450 ticks; one in-game minute = 7.5 ticks

// HH:MM on a 24-hour clock. The simulation stores and computes exclusively in
// ticks; these helpers convert at the edges (UI display, player input).
struct ClockTime {
    int hour = 0;
    int minute = 0;
};

// Time of day (00:00-23:59) of an absolute tick.
constexpr ClockTime tick_to_time_of_day(Tick tick) {
    const Tick time_of_day = tick % kTicksPerDay;
    return {static_cast<int>(time_of_day / kTicksPerHour),
            static_cast<int>(time_of_day % kTicksPerHour * 60 / kTicksPerHour)};
}

// Length of a tick duration in whole hours and minutes. Hours are NOT wrapped
// at 24, so a 30-hour duration formats as {30, 0}.
constexpr ClockTime duration_to_hours_minutes(Tick duration) {
    return {static_cast<int>(duration / kTicksPerHour),
            static_cast<int>(duration % kTicksPerHour * 60 / kTicksPerHour)};
}

// HH:MM (a time of day, or a duration expressed in hours and minutes) to
// ticks. One in-game minute is kTicksPerHour / 60 = 7.5 ticks, so the minute
// part is rounded to the nearest tick.
constexpr Tick hours_minutes_to_ticks(int hour, int minute) {
    return static_cast<Tick>(hour) * kTicksPerHour
         + (static_cast<Tick>(minute) * kTicksPerHour + 30) / 60;
}

// ---------------------------------------------------------------------------
// Economy
// ---------------------------------------------------------------------------

constexpr int kStartingMoney = 25000;

// Creating a schedule is free; instead every departing truck costs this much,
// charged at the source office when it leaves. A departure the player cannot
// afford is skipped (no truck leaves, no letters move).
constexpr int kTruckDepartureCost = 15;

// Most letters one truck carries. Matching letters beyond this stay in the
// buffer for a later departure.
constexpr std::size_t kTruckCapacity = 5;

// Shortest allowed schedule period: one departure every 5 in-game minutes.
constexpr Tick kMinSchedulePeriod = hours_minutes_to_ticks(0, 5);

// Road construction: cost per world unit of road length (rounded up).
constexpr float kRoadCostPerUnit = 5.0f;

// ---------------------------------------------------------------------------
// World geometry
// ---------------------------------------------------------------------------

// The game plays out in a virtual coordinate space; the UI scales it to fit
// whatever window size the player has.
constexpr float kWorldWidth  = 2000.0f;
constexpr float kWorldHeight = 1300.0f;
constexpr float kTruckSpeed  = 2.0f;  // world units per tick

// Largest |x| or |y| a road vertex may have. Keeps the exact integer geometry
// in road.cpp overflow-free: coordinate differences stay within 2e9, so every
// cross/dot product (a sum of two products of differences) is within 8e18,
// below INT64_MAX (~9.22e18).
constexpr int64_t kMaxCoordinate = 1'000'000'000;

struct Position {
    int64_t x = 0;
    int64_t y = 0;

    // Member-wise equality; see the note in entities.h (save/load round-trip test).
    bool operator==(const Position&) const = default;

    Position operator-(const Position& other) const {
        return Position {
            x - other.x, 
            y - other.y
        };
    }

    Position operator+(const Position& other) const {
        return Position {
            x + other.x, 
            y + other.y
        };
    }
};
