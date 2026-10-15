// balance.h — pure simulation layer: gameplay tuning numbers (economy, trucks,
// letters). Everything a playtest might want to adjust lives here; engine
// constraints (tick rate, world size, coordinate limits) stay in game_types.h,
// and per-office stats for the starter map live in scenario.cpp.
#pragma once

#include "sim/game_types.h"

#include <cstddef>

// ---------------------------------------------------------------------------
// Economy
// ---------------------------------------------------------------------------

// Enough to join the opening offices by road, with some left over for truck
// departures; the rest has to be earned by delivering mail.
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

// ---------------------------------------------------------------------------
// Post offices
// ---------------------------------------------------------------------------

// Offices appear on their own (World::spawn_random_office): a new game opens
// with kInitialOffices of them, and another arrives every spawn interval.
constexpr std::size_t kInitialOffices = 3;

// The first spawn comes this long into the game; every office beyond the
// opening ones shortens the wait for the next by kOfficeSpawnIntervalStep,
// down to kOfficeSpawnIntervalMin.
constexpr Tick kOfficeSpawnIntervalStart = kTicksPerDay;
constexpr Tick kOfficeSpawnIntervalStep  = kTicksPerHour;
constexpr Tick kOfficeSpawnIntervalMin   = 8 * kTicksPerHour;

// The kinds of office that can appear, drawn with odds proportional to
// spawn_weight.
struct OfficeClass {
    std::size_t letters_per_day;
    std::size_t max_outbound_letters;
    double spawn_weight;
};
constexpr OfficeClass kOfficeClasses[] = {
    {20, 40, 6.0},   // small town
    {30, 60, 3.0},   // city
    {50, 100, 1.0},  // major city
};

// Where a new office may appear: at least kOfficeSpawnMargin inside the
// world's edges, kOfficeMinSpacing from every other office and
// kOfficeRoadClearance from every road (all in world units). A spawn tries
// this many random spots before giving up until the next interval.
constexpr double kOfficeSpawnMargin    = 80.0;
constexpr double kOfficeMinSpacing     = 150.0;
constexpr double kOfficeRoadClearance  = 40.0;
constexpr int kOfficeSpawnAttempts     = 200;

// ---------------------------------------------------------------------------
// Overflow
// ---------------------------------------------------------------------------

// An office whose outbound buffer is full runs down its overflow timer by a
// tick every tick, and winds it back up the same way once it has room again.
// The game is over when any office's timer reaches this (6 in-game hours,
// about 45 seconds at normal speed).
constexpr Tick kOverflowLimit = hours_minutes_to_ticks(6, 0);
