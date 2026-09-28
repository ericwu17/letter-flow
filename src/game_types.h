// game_types.h — pure simulation layer: shared primitive types and constants
// (time, world geometry, economy).
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

// Up-front price of a truck schedule, charged per departure per in-game day:
// a 3-hourly route departs 8 times a day and costs 8 * 5 = 40. Higher
// frequency -> higher price.
constexpr int kScheduleCostPerDailyDeparture = 5;

// Shortest allowed schedule period: one departure every 5 in-game minutes.
constexpr Tick kMinSchedulePeriod = hours_minutes_to_ticks(0, 5);

// Price of creating a schedule that departs every `period` ticks (rounded up).
constexpr int schedule_cost(Tick period) {
    if (period == 0)
        return 0;  // invalid period; callers must reject it
    const Tick cost_per_day = static_cast<Tick>(kScheduleCostPerDailyDeparture) * kTicksPerDay;
    return static_cast<int>((cost_per_day + period - 1) / period);  // ceil division
}

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
