// stats.cpp — StatSeries: multi-resolution ring buffers (see stats.h).
#include "sim/stats.h"

#include <algorithm>

int64_t& StatSeries::bucket_at(std::size_t level, Tick now) {
    StatLevel& l = levels[level];
    const Tick bucket = now / kStatBucketTicks[level];
    if (bucket > l.newest) {
        // Clear the buckets between the newest one written and this one:
        // they belong to stretches of time in which nothing was recorded.
        const Tick gap = bucket - l.newest;
        if (gap >= kStatBuckets)
            l.buckets.fill(0);
        else
            for (Tick b = l.newest + 1; b <= bucket; ++b)
                l.buckets[b % kStatBuckets] = 0;
        l.newest = bucket;
    }
    return l.buckets[bucket % kStatBuckets];
}

void StatSeries::add(Tick now, int64_t amount) {
    if (amount == 0)
        return;
    for (std::size_t level = 0; level < kStatLevels; ++level)
        bucket_at(level, now) += amount;
    total += amount;
}

void StatSeries::sample(Tick now, int64_t value) {
    for (std::size_t level = 0; level < kStatLevels; ++level) {
        int64_t& bucket = bucket_at(level, now);
        bucket = std::max(bucket, value);
    }
    total = value;
}

void StatSeries::read(std::size_t level, Tick now, std::size_t count, int64_t* out) const {
    const StatLevel& l = levels[level];
    const Tick current = now / kStatBucketTicks[level];
    count = std::min(count, kStatBuckets);
    for (std::size_t i = 0; i < count; ++i) {
        const Tick back = count - 1 - i;  // how many buckets before `current`
        // Before tick 0, after the newest write, or already overwritten: empty.
        if (back > current) {
            out[i] = 0;
            continue;
        }
        const Tick bucket = current - back;
        const bool written = bucket <= l.newest && l.newest - bucket < kStatBuckets;
        out[i] = written ? l.buckets[bucket % kStatBuckets] : 0;
    }
}

int64_t StatSeries::sum(std::size_t level, Tick now, std::size_t count) const {
    std::array<int64_t, kStatBuckets> values{};
    count = std::min(count, kStatBuckets);
    read(level, now, count, values.data());
    int64_t total_in_range = 0;
    for (std::size_t i = 0; i < count; ++i)
        total_in_range += values[i];
    return total_in_range;
}
