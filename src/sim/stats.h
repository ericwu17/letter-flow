// stats.h — pure simulation layer: Factorio-style time-series statistics.
// No imgui/SDL/OpenGL headers (see game_types.h).
//
// A StatSeries records one metric at several time resolutions at once. Each
// resolution ("level") is a fixed ring of buckets keyed by absolute bucket
// number (tick / bucket length), so memory stays bounded however long the
// game runs, and a level never needs to be told that time has passed: a write
// or read at tick `now` simply treats every bucket after the newest one
// written as empty. Everything is integer ticks and integer amounts, so stats
// are as deterministic as the rest of the simulation and survive save/load.
#pragma once

#include "sim/game_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// ---------------------------------------------------------------------------
// Resolutions
// ---------------------------------------------------------------------------

// Every level holds the same number of buckets; the bucket length sets how
// much time the level spans. With one in-game day = 10800 ticks:
//   level 0: 90-tick buckets   (12 game-min) -> 1 day
//   level 1: 900-tick buckets  (2 h)         -> 10 days
//   level 2: 2700-tick buckets (6 h)         -> 30 days
// Each length is a multiple of the previous one, so a coarse bucket covers
// exactly a run of finer ones. The UI's "6 h" range is simply the newest 30
// buckets of level 0.
inline constexpr std::size_t kStatBuckets = 120;
inline constexpr std::size_t kStatLevels = 3;
inline constexpr std::array<Tick, kStatLevels> kStatBucketTicks = {90, 900, 2700};

// How a metric aggregates within a bucket.
enum class StatKind {
    Flow,   // a counter: amounts added during the bucket are summed
            // ("letters spawned", "money earned")
    Gauge,  // a level, sampled every tick: the bucket keeps the largest
            // sample, so short spikes still show ("letters waiting")
};

// One time resolution of a series: a ring of kStatBuckets buckets, where the
// bucket for absolute bucket number b lives at index b % kStatBuckets.
struct StatLevel {
    std::array<int64_t, kStatBuckets> buckets{};
    Tick newest = 0;  // absolute number of the newest bucket written to

    bool operator==(const StatLevel&) const = default;
};

class StatSeries {
public:
    std::array<StatLevel, kStatLevels> levels{};
    // Flows: the lifetime sum. Gauges: the most recent sample.
    int64_t total = 0;

    // Flow: adds `amount` to the bucket holding `now` at every level.
    void add(Tick now, int64_t amount);
    // Gauge: records `value` (non-negative) for the bucket holding `now` at
    // every level, keeping the bucket's maximum.
    void sample(Tick now, int64_t value);

    // The newest `count` buckets of `level` (count <= kStatBuckets), oldest
    // first, ending with the bucket that holds `now`; buckets nobody wrote
    // to read as 0. `out` must hold at least `count` values.
    void read(std::size_t level, Tick now, std::size_t count, int64_t* out) const;
    // Sum of those same buckets.
    int64_t sum(std::size_t level, Tick now, std::size_t count) const;

    bool operator==(const StatSeries&) const = default;

private:
    // Makes the bucket holding `now` the newest one at `level`, clearing the
    // buckets skipped on the way, and returns it.
    int64_t& bucket_at(std::size_t level, Tick now);
};

// ---------------------------------------------------------------------------
// Metric catalogues
// ---------------------------------------------------------------------------

// Static description of one metric: the stable key it is saved under, and
// how it aggregates. Changing a key orphans that history in existing saves,
// so every rename needs a matching entry in tools/migrate_stat_keys.py.
struct StatInfo {
    const char* key;
    StatKind kind;
};

// Network-wide metrics, owned by the World.
enum class GlobalStat : std::size_t {
    IncomeValue,        // base letter values paid on delivery
    IncomeBonus,        // early-delivery bonuses
    SpentTrucks,        // truck departure charges
    SpentRoads,         // road construction
    Spawned,
    Lost,               // spawn refused because the office's buffer was full
    DeliveredOnTime,
    DeliveredLate,
    Backlog,            // gauge: letters in all outbound buffers
    InTransit,          // gauge: letters on trucks
    Money,              // gauge: the balance
    Count
};

// Per post office, owned by the PostOffice.
enum class OfficeStat : std::size_t {
    Spawned,          // letters created here
    Lost,             // letters refused here because the buffer was full
    Delivered,        // letters that reached this office as their destination
    Forwarded,        // letters that arrived here addressed elsewhere (hub traffic)
    Departures,       // trucks that left here
    Income,           // money earned by deliveries here
    Loaded,           // letters that left here on a truck
    Backlog,          // gauge: letters in the outbound buffer
    Count
};

// Per truck schedule, owned by the TruckSchedule.
enum class ScheduleStat : std::size_t {
    Departures,       // trucks that left on this schedule
    Loaded,           // letters that left on this schedule's trucks
    LeftBehind,       // letters matching the rule that did not fit on the truck
    Count
};

inline constexpr std::array<StatInfo, static_cast<std::size_t>(GlobalStat::Count)> kGlobalStatInfo = {{
    {"income_value", StatKind::Flow},
    {"income_bonus", StatKind::Flow},
    {"spent_trucks", StatKind::Flow},
    {"spent_roads", StatKind::Flow},
    {"spawned", StatKind::Flow},
    {"lost", StatKind::Flow},
    {"delivered_on_time", StatKind::Flow},
    {"delivered_late", StatKind::Flow},
    {"backlog", StatKind::Gauge},
    {"in_transit", StatKind::Gauge},
    {"money", StatKind::Gauge},
}};

inline constexpr std::array<StatInfo, static_cast<std::size_t>(OfficeStat::Count)> kOfficeStatInfo = {{
    {"spawned", StatKind::Flow},
    {"lost", StatKind::Flow},
    {"delivered", StatKind::Flow},
    {"forwarded", StatKind::Flow},
    {"departures", StatKind::Flow},
    {"income", StatKind::Flow},
    {"loaded", StatKind::Flow},
    {"backlog", StatKind::Gauge},
}};

inline constexpr std::array<StatInfo, static_cast<std::size_t>(ScheduleStat::Count)> kScheduleStatInfo = {{
    {"departures", StatKind::Flow},
    {"loaded", StatKind::Flow},
    {"left_behind", StatKind::Flow},
}};

// The catalogue of each metric enum, found by overload on a tag value
// (stat_infos(GlobalStat{})), so StatTable and serialization stay generic.
constexpr std::span<const StatInfo> stat_infos(GlobalStat) { return kGlobalStatInfo; }
constexpr std::span<const StatInfo> stat_infos(OfficeStat) { return kOfficeStatInfo; }
constexpr std::span<const StatInfo> stat_infos(ScheduleStat) { return kScheduleStatInfo; }

// One StatSeries per value of the metric enum `Metric`, indexed by it.
template <typename Metric>
struct StatTable {
    std::array<StatSeries, static_cast<std::size_t>(Metric::Count)> series{};

    StatSeries& operator[](Metric m) { return series[static_cast<std::size_t>(m)]; }
    const StatSeries& operator[](Metric m) const { return series[static_cast<std::size_t>(m)]; }

    bool operator==(const StatTable&) const = default;
};

using GlobalStats = StatTable<GlobalStat>;
using OfficeStats = StatTable<OfficeStat>;
using ScheduleStats = StatTable<ScheduleStat>;
