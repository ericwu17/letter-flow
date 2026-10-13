// test_stats.cpp — headless tests for StatSeries, the multi-resolution ring
// buffer behind every statistic (stats.h). Run with `make test`.
// Pure simulation layer: no SDL/imgui/OpenGL — no window is needed.
#include "sim/stats.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

namespace {

constexpr Tick kB0 = kStatBucketTicks[0];  // level-0 bucket length
constexpr Tick kB1 = kStatBucketTicks[1];

std::vector<int64_t> read(const StatSeries& s, std::size_t level, Tick now, std::size_t count) {
    std::vector<int64_t> out(count);
    s.read(level, now, count, out.data());
    return out;
}

// Levels must nest: each bucket length a multiple of the previous one, so a
// coarse bucket is exactly a run of fine ones.
void levels_nest() {
    for (std::size_t level = 1; level < kStatLevels; ++level)
        assert(kStatBucketTicks[level] % kStatBucketTicks[level - 1] == 0);
}

// A flow sums what is added within a bucket, starts a new bucket on the
// boundary, and feeds every level at once.
void flows_sum_per_bucket() {
    StatSeries s;
    s.add(0, 1);
    s.add(kB0 - 1, 2);     // still bucket 0
    s.add(kB0, 4);         // bucket 1
    s.add(kB0 + 5, 0);     // zero amounts are no-ops
    assert(s.total == 7);
    assert(read(s, 0, kB0, 3) == (std::vector<int64_t>{0, 3, 4}));  // bucket -1 is before time
    assert(read(s, 1, kB0, 1) == (std::vector<int64_t>{7}));        // one coarse bucket
    assert(s.sum(0, kB0, kStatBuckets) == 7);
    assert(s.sum(1, kB1, 2) == 7);  // a level-1 bucket later: last two coarse buckets
}

// Reading later than the last write shows the silent stretch as zeros.
void stale_buckets_read_as_zero() {
    StatSeries s;
    s.add(0, 1);
    s.add(kB0, 2);
    assert(read(s, 0, 3 * kB0, 4) == (std::vector<int64_t>{1, 2, 0, 0}));
    // So far ahead that everything written has scrolled out of the window.
    assert(s.sum(0, (kStatBuckets + 5) * kB0, kStatBuckets) == 0);
    assert(s.total == 3);  // the lifetime total keeps it
}

// Writing after a gap clears the skipped buckets, so values from a whole
// ring ago never reappear.
void gaps_are_cleared_on_write() {
    StatSeries s;
    for (Tick b = 0; b < kStatBuckets; ++b)
        s.add(b * kB0, 1);  // fill the whole ring
    const Tick later = (kStatBuckets + 3) * kB0;
    s.add(later, 5);
    const std::vector<int64_t> window = read(s, 0, later, kStatBuckets);
    // Buckets 4 .. kStatBuckets-1 survive; kStatBuckets .. +2 were silent.
    assert(window[kStatBuckets - 1] == 5);
    assert(window[kStatBuckets - 2] == 0 && window[kStatBuckets - 3] == 0 && window[kStatBuckets - 4] == 0);
    assert(s.sum(0, later, kStatBuckets) == 5 + static_cast<int64_t>(kStatBuckets - 4));

    // A gap longer than the ring clears everything.
    StatSeries t;
    t.add(0, 9);
    t.add(10 * kStatBuckets * kB0, 1);
    assert(t.sum(0, 10 * kStatBuckets * kB0, kStatBuckets) == 1);
}

// The ring wraps: after more buckets than it holds, the window is exactly
// the newest kStatBuckets of them, oldest first.
void ring_wraps() {
    StatSeries s;
    const Tick written = kStatBuckets + 10;
    for (Tick b = 0; b < written; ++b)
        s.add(b * kB0, static_cast<int64_t>(b));
    const std::vector<int64_t> window = read(s, 0, (written - 1) * kB0, kStatBuckets);
    for (std::size_t i = 0; i < kStatBuckets; ++i)
        assert(window[i] == static_cast<int64_t>(10 + i));
}

// A gauge keeps the largest sample in each bucket; total is the latest.
void gauges_keep_the_bucket_max() {
    StatSeries s;
    s.sample(0, 5);
    s.sample(1, 3);
    s.sample(kB0, 2);
    assert(s.total == 2);
    assert(read(s, 0, kB0, 2) == (std::vector<int64_t>{5, 2}));
    assert(read(s, 1, kB0, 1) == (std::vector<int64_t>{5}));
}

}  // namespace

int main() {
    levels_nest();
    printf("PASS levels_nest\n");
    flows_sum_per_bucket();
    printf("PASS flows_sum_per_bucket\n");
    stale_buckets_read_as_zero();
    printf("PASS stale_buckets_read_as_zero\n");
    gaps_are_cleared_on_write();
    printf("PASS gaps_are_cleared_on_write\n");
    ring_wraps();
    printf("PASS ring_wraps\n");
    gauges_keep_the_bucket_max();
    printf("PASS gauges_keep_the_bucket_max\n");

    printf("all stats tests passed\n");
    return 0;
}
