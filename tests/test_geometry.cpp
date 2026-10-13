// test_geometry.cpp — headless tests for the exact road geometry (road.h)
// and the road rules World::add_road builds on it. Run with `make test`.
// Pure simulation layer: no SDL/imgui/OpenGL — no window needed.
#include "balance.h"
#include "game_types.h"
#include "road.h"
#include "truck.h"
#include "world.h"

#include <cassert>
#include <cstdio>
#include <vector>

namespace {

constexpr int64_t M = kMaxCoordinate;

Road make_road(std::vector<Position> vertices) {
    return Road{0, 1, std::move(vertices)};
}

void segment_intersection() {
    // Proper crossing.
    assert(segments_intersect({0, 0}, {10, 10}, {0, 10}, {10, 0}));
    // Disjoint, parallel, and collinear-but-apart.
    assert(!segments_intersect({0, 0}, {10, 0}, {0, 5}, {10, 5}));
    assert(!segments_intersect({0, 0}, {10, 10}, {20, 0}, {11, 9}));
    assert(!segments_intersect({0, 0}, {10, 0}, {11, 0}, {20, 0}));
    // Closed segments: touching end to end, T-junctions and collinear
    // overlap all share a point.
    assert(segments_intersect({0, 0}, {10, 0}, {10, 0}, {20, 0}));
    assert(segments_intersect({0, 0}, {10, 0}, {5, 0}, {5, 10}));
    assert(segments_intersect({0, 0}, {10, 0}, {5, 0}, {15, 0}));
    // Zero-length segments behave as points.
    assert(segments_intersect({5, 0}, {5, 0}, {0, 0}, {10, 0}));
    assert(!segments_intersect({5, 1}, {5, 1}, {0, 0}, {10, 0}));

    // Exact at the coordinate bound: a parallel line one unit off never
    // touches, a one-unit miss is a miss, and nothing overflows (the
    // sanitizer build would trap on signed overflow).
    assert(!segments_intersect({-M, -M}, {M, M}, {-M + 1, -M}, {M, M - 1}));
    assert(!segments_intersect({-M, -M}, {M, M}, {M, M - 1}, {M, -M}));
    assert(segments_intersect({-M, -M}, {M, M}, {M, M}, {M, -M}));
    assert(segments_intersect({-M, M}, {M, -M}, {-M, -M}, {M, M}));
}

void polyline_self_intersection() {
    // An L and a staircase are fine.
    assert(!polyline_self_intersects({{0, 0}, {10, 0}, {10, 10}}));
    assert(!polyline_self_intersects({{0, 0}, {10, 0}, {20, 10}, {30, 10}}));
    // Doubling back over the previous segment.
    assert(polyline_self_intersects({{0, 0}, {100, 0}, {0, 0}, {0, 50}}));
    assert(polyline_self_intersects({{0, 0}, {100, 0}, {50, 0}}));
    // Crossing an earlier segment.
    assert(polyline_self_intersects({{0, 0}, {10, 0}, {10, 10}, {5, 10}, {5, -5}}));
    // Revisiting an earlier vertex without crossing.
    assert(polyline_self_intersects({{0, 0}, {10, 0}, {10, 10}, {0, 0}, {-10, 0}}));
}

void polyline_against_road() {
    const Road horizontal = make_road({{0, 10}, {10, 10}, {20, 10}});

    // Two roads may share an end office, at any angle.
    assert(!polyline_crosses_road(horizontal, {{0, 10}, {0, 30}}));
    assert(!polyline_crosses_road(horizontal, {{20, 10}, {30, 20}, {40, 20}}));
    // ...but not overlap along a stretch leaving it.
    assert(polyline_crosses_road(horizontal, {{0, 10}, {5, 10}, {5, 30}}));
    // A crossing exactly through a shared intermediate vertex is a crossing.
    assert(polyline_crosses_road(horizontal, {{10, 0}, {10, 10}, {10, 20}}));
    // So is merely touching an intermediate vertex.
    assert(polyline_crosses_road(horizontal, {{10, 0}, {10, 10}, {30, 30}}));
    // A road ending on the other's interior (T-junction).
    assert(polyline_crosses_road(horizontal, {{5, 0}, {5, 10}}));
    // An ordinary crossing, and a clean miss.
    assert(polyline_crosses_road(horizontal, {{5, 0}, {5, 20}}));
    assert(!polyline_crosses_road(horizontal, {{0, 0}, {20, 0}}));
}

void segment_directions() {
    assert(all_segments_valid_direction({{0, 0}, {10, 0}, {10, 10}, {20, 20}, {30, 10}}));
    assert(!all_segments_valid_direction({{0, 0}, {10, 5}}));          // not 0/45/90 degrees
    assert(!all_segments_valid_direction({{0, 0}, {0, 0}, {10, 0}}));  // zero-length
}

void coordinate_range() {
    assert(all_vertices_in_range({{-M, M}, {M, -M}}));
    assert(!all_vertices_in_range({{0, 0}, {M + 1, 0}}));
    assert(!all_vertices_in_range({{0, -M - 1}}));
}

// The geometry rules as World::add_road applies them, with offices at the
// four compass points around (10, 10) plus two far apart on the bound.
void add_road_rules() {
    World w;
    const PostOfficeId west = w.add_office("W", "W", {0, 10}, 0, 10, 0);
    const PostOfficeId east = w.add_office("E", "E", {20, 10}, 0, 10, 0);
    const PostOfficeId north = w.add_office("N", "N", {10, 0}, 0, 10, 0);
    const PostOfficeId south = w.add_office("S", "S", {10, 20}, 0, 10, 0);
    const PostOfficeId far_a = w.add_office("A", "A", {M + 1, 0}, 0, 10, 0);
    const PostOfficeId far_b = w.add_office("B", "B", {M + 1, 10}, 0, 10, 0);
    for (const PostOfficeId id : {west, east, north, south, far_a, far_b})
        assert(w.build_office(id));

    assert(w.add_road(west, east, {{0, 10}, {10, 10}, {20, 10}}));
    const int money = w.get_money();

    // X-crossing at the shared vertex (10, 10).
    assert(!w.add_road(north, south, {{10, 0}, {10, 10}, {10, 20}}));
    // Zero-length segment.
    assert(!w.add_road(north, west, {{10, 0}, {10, 0}, {0, 0}, {0, 10}}));
    // Out-and-back spike.
    assert(!w.add_road(north, west, {{10, 0}, {10, -20}, {10, -10}, {0, -10}, {0, 10}}));
    // Out of the coordinate range.
    assert(!w.add_road(far_a, far_b, {{M + 1, 0}, {M + 1, 10}}));
    assert(w.get_money() == money);  // failed builds cost nothing

    // Meeting the existing road at its end office is fine.
    assert(w.add_road(north, west, {{10, 0}, {0, 0}, {0, 10}}));
}

// A truck on a zero-length segment (possible in old saves) still has a
// well-defined position: 0/0 used to make it NaN.
void truck_zero_length_segment() {
    const Truck truck(0, 1, {{0, 0}, {0, 0}, {0, 50}}, 100, kTruckSpeed);
    assert((truck.get_position(100) == Position{0, 0}));
    assert((truck.get_position(110) == Position{0, 20}));
    assert((truck.get_position(1000) == Position{0, 50}));
    assert(truck.has_arrived(125));
}

// has_arrived() works from an arrival tick cached at construction; it must
// agree, on every tick around arrival and before departure, with the
// definition it replaced: distance covered (elapsed * speed) >= route length.
void truck_arrival_matches_distance() {
    const std::vector<std::vector<Position>> routes = {
        {{0, 0}, {0, 0}},                         // empty route: arrived at once
        {{0, 0}, {7, 0}},                         // not a multiple of the speed
        {{0, 0}, {300, 300}},                     // irrational length
        {{0, 0}, {40, 40}, {40, 90}, {-10, 140}},  // several segments
    };
    for (const float speed : {kTruckSpeed, 0.3f, 3.7f, 0.0f}) {
        for (const std::vector<Position>& route : routes) {
            const Tick departure = 1000;
            const Truck truck(0, 1, route, departure, speed);
            const TruckState& s = truck.get_state();
            const Truck reloaded(s);  // the save-file constructor
            assert(reloaded == truck);
            for (Tick now = 0; now < departure + 400; ++now) {
                const Tick elapsed = now > departure ? now - departure : 0;
                const bool expected = static_cast<double>(elapsed) * static_cast<double>(speed)
                                   >= truck.route_length();
                assert(truck.has_arrived(now) == expected);
                assert(reloaded.has_arrived(now) == expected);
            }
        }
    }
}

}  // namespace

int main() {
    segment_intersection();
    printf("PASS segment_intersection\n");
    polyline_self_intersection();
    printf("PASS polyline_self_intersection\n");
    polyline_against_road();
    printf("PASS polyline_against_road\n");
    segment_directions();
    printf("PASS segment_directions\n");
    coordinate_range();
    printf("PASS coordinate_range\n");
    add_road_rules();
    printf("PASS add_road_rules\n");
    truck_zero_length_segment();
    printf("PASS truck_zero_length_segment\n");
    truck_arrival_matches_distance();
    printf("PASS truck_arrival_matches_distance\n");

    printf("all geometry tests passed\n");
    return 0;
}
