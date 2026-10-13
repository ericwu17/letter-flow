// test_road_builder.cpp — headless tests for RoadBuilder (road_builder.h):
// snapping the road being drawn to legal segments, committing and
// collapsing vertices, and validity against the World. Run with `make test`.
// Pure simulation layer: no SDL/imgui/OpenGL — no window needed.
#include "road.h"
#include "road_builder.h"
#include "world.h"

#include <cassert>
#include <cstdio>
#include <optional>
#include <vector>

namespace {

using Path = std::vector<Position>;

// A straight or exactly diagonal stretch needs no corner.
void single_segments_need_no_corner() {
    for (const Position target : {Position{100, 0}, Position{0, -70}, Position{-30, 30}}) {
        RoadBuilder b = RoadBuilder::from_source({0, 0}, 0);
        b.build_to(target);
        assert(!b.middle.has_value());
        assert((b.head == target));
        assert((b.preview_vertices() == Path{{0, 0}, target}));
    }
    // The cursor on the source: nothing to preview.
    RoadBuilder b = RoadBuilder::from_source({5, 5}, 0);
    b.build_to({5, 5});
    assert(!b.middle.has_value() && !b.head.has_value());
    assert((b.preview_vertices() == Path{{5, 5}}));
}

// The first corner follows the direction the cursor first left the source
// in; returning to the source forgets it.
void first_corner_follows_the_cursor() {
    RoadBuilder b = RoadBuilder::from_source({0, 0}, 0);
    b.build_to({100, 40});  // left mostly along +x: straight part first
    assert((b.middle == Position{60, 0}));
    assert((b.head == Position{100, 40}));
    assert(all_segments_valid_direction(b.preview_vertices()));

    RoadBuilder d = RoadBuilder::from_source({0, 0}, 0);
    d.build_to({50, 50});   // left diagonally...
    d.build_to({100, 40});  // ...so the diagonal part comes first
    assert((d.middle == Position{40, 40}));
    assert(all_segments_valid_direction(d.preview_vertices()));

    d.build_to({1, 1});     // back on the source: the direction is forgotten
    d.build_to({100, 40});
    assert((d.middle == Position{60, 0}));
}

// Past the first bend, the corner order that turns least from the previous
// segment wins.
void later_corners_turn_least() {
    RoadBuilder b = RoadBuilder::from_source({0, 0}, 0);
    b.build_to({100, 0});
    b.commit_middle_and_head();
    b.build_to({200, 50});  // heading +x: keep going straight, then turn 45°
    assert((b.middle == Position{150, 0}));

    RoadBuilder d = RoadBuilder::from_source({0, 0}, 0);
    d.build_to({100, 100});
    d.commit_middle_and_head();
    d.build_to({250, 200});  // heading diagonally: diagonal first
    assert((d.middle == Position{200, 200}));
}

// Committing keeps only the vertices where the road actually bends.
void commit_collapses_straight_runs() {
    RoadBuilder b = RoadBuilder::from_source({0, 0}, 0);
    b.build_to({50, 0});
    b.commit_middle_and_head();
    b.build_to({100, 0});
    b.commit_middle_and_head();
    assert((b.vertices == Path{{0, 0}, {100, 0}}));
    assert(!b.middle.has_value() && !b.head.has_value());

    // Clicking twice on the same spot adds nothing.
    b.build_to({100, 0});
    b.commit_middle_and_head();
    assert((b.vertices == Path{{0, 0}, {100, 0}}));

    // A real bend stays.
    b.build_to({100, 60});
    b.commit_middle_and_head();
    assert((b.vertices == Path{{0, 0}, {100, 0}, {100, 60}}));
}

// The finished road is what World::add_road accepts, and check_road flags a
// preview that crosses an existing road or itself.
void finished_roads_and_validity() {
    World w;
    w.set_money(1'000'000);
    const PostOfficeId a = w.add_office("A", "A", {0, 0}, 0, 10, 0);
    const PostOfficeId b = w.add_office("B", "B", {200, 0}, 0, 10, 0);
    const PostOfficeId c = w.add_office("C", "C", {100, -100}, 0, 10, 0);
    const PostOfficeId d = w.add_office("D", "D", {100, 100}, 0, 10, 0);
    for (const PostOfficeId id : {a, b, c, d})
        assert(w.build_office(id));

    RoadBuilder ab = RoadBuilder::from_source({0, 0}, a);
    ab.build_to({100, 0});
    ab.commit_middle_and_head();
    ab.check_road(w);
    assert(!ab.is_invalid);
    ab.commit_to_destination({200, 0}, b);
    const Road road = ab.into_road();
    assert(road.src == a && road.dst == b);
    assert((road.vertices == Path{{0, 0}, {200, 0}}));
    assert(w.add_road(road.src, road.dst, road.vertices));

    // C straight down to D crosses A-B.
    RoadBuilder cd = RoadBuilder::from_source({100, -100}, c);
    cd.build_to({100, 100});
    cd.check_road(w);
    assert(cd.is_invalid);
    // Re-snapping clears the flag until the next check.
    cd.build_to({100, -50});
    assert(!cd.is_invalid);
    cd.check_road(w);
    assert(!cd.is_invalid);

    // Doubling back over itself is invalid too.
    RoadBuilder back = RoadBuilder::from_source({100, -100}, c);
    back.build_to({150, -100});
    back.commit_middle_and_head();
    back.build_to({120, -100});
    back.check_road(w);
    assert(back.is_invalid);
}

// preview_to() only redoes the work when the target moved or a click
// committed vertices, and then matches build_to() + check_road() exactly.
void preview_is_recomputed_only_when_needed() {
    World w;
    RoadBuilder b = RoadBuilder::from_source({0, 0}, 0);
    b.preview_to({100, 40}, w);
    assert((b.preview_vertices() == Path{{0, 0}, {60, 0}, {100, 40}}));

    // Same target: nothing recomputed (a tampered flag survives).
    b.is_invalid = true;
    b.preview_to({100, 40}, w);
    assert(b.is_invalid);

    // A new target recomputes.
    b.preview_to({100, 0}, w);
    assert(!b.is_invalid);
    assert((b.preview_vertices() == Path{{0, 0}, {100, 0}}));

    // A click commits; previewing the same spot again re-snaps from there
    // (to nothing: the cursor sits on the last vertex).
    b.commit_middle_and_head();
    b.preview_to({100, 0}, w);
    assert(!b.middle.has_value() && !b.head.has_value());
    b.preview_to({100, 50}, w);
    assert((b.preview_vertices() == Path{{0, 0}, {100, 0}, {100, 50}}));
}

}  // namespace

int main() {
    single_segments_need_no_corner();
    printf("PASS single_segments_need_no_corner\n");
    first_corner_follows_the_cursor();
    printf("PASS first_corner_follows_the_cursor\n");
    later_corners_turn_least();
    printf("PASS later_corners_turn_least\n");
    commit_collapses_straight_runs();
    printf("PASS commit_collapses_straight_runs\n");
    finished_roads_and_validity();
    printf("PASS finished_roads_and_validity\n");
    preview_is_recomputed_only_when_needed();
    printf("PASS preview_is_recomputed_only_when_needed\n");

    printf("all road builder tests passed\n");
    return 0;
}
