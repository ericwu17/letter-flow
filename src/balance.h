// balance.h — pure simulation layer: gameplay tuning numbers (economy, trucks,
// letters). Everything a playtest might want to adjust lives here; engine
// constraints (tick rate, world size, coordinate limits) stay in game_types.h,
// and per-office stats for the starter map live in scenario.cpp.
#pragma once

#include "game_types.h"

#include <cstddef>

// ---------------------------------------------------------------------------
// Economy
// ---------------------------------------------------------------------------

// Enough to open with a small town and a city joined by a road (or three
// small towns in a chain), with some left over for truck departures, but not
// two cities.
constexpr int kStartingMoney = 5000;

// Road construction: cost per world unit of road length (rounded up).
constexpr float kRoadCostPerUnit = 5.0f;

// ---------------------------------------------------------------------------
// Trucks
// ---------------------------------------------------------------------------

// Creating a schedule is free; instead every departing truck costs this much,
// charged at the source office when it leaves. A departure the player cannot
// afford is skipped (no truck leaves, no letters move).
constexpr int kTruckDepartureCost = 15;

// Most letters one truck carries. Matching letters beyond this stay in the
// buffer for a later departure.
constexpr std::size_t kTruckCapacity = 5;

constexpr float kTruckSpeed = 2.0f;  // world units per tick

// Shortest allowed schedule period: one departure every 5 in-game minutes.
constexpr Tick kMinSchedulePeriod = hours_minutes_to_ticks(0, 5);

// ---------------------------------------------------------------------------
// Letters
// ---------------------------------------------------------------------------

// Each new letter draws these uniformly from [min, max] (inclusive).
constexpr Tick kLetterDeadlineMin = kTicksPerDay / 2;  // time from spawn to deadline
constexpr Tick kLetterDeadlineMax = 2 * kTicksPerDay;
constexpr int kLetterValueMin = 5;                     // paid on delivery, late or not
constexpr int kLetterValueMax = 15;
constexpr int kLetterBonusMin = 3;                     // max early bonus (see Letter::created)
constexpr int kLetterBonusMax = 10;

// Destination odds: bigger offices (by letters_per_day) and closer ones
// attract more mail. weight = (size + 1) / (1 + distance / kDestinationDistanceScale).
constexpr double kDestinationDistanceScale = 150.0;
