// road_builder.cpp — RoadBuilder (road_builder.h): snapping the road being
// drawn to legal directions. Pure logic, no rendering, no input.
#include "road_builder.h"

#include "world.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace {

int sign(int64_t v) { return (v > 0) - (v < 0); }

// 8-way direction index: 0=+x, 1=+x+y, 2=+y, ... 7=+x-y. Even = orthogonal.
int dir_index(int64_t dx, int64_t dy) {
    constexpr double kPi = 3.14159265358979323846;
    const double angle = std::atan2(static_cast<double>(dy), static_cast<double>(dx));
    const int idx = static_cast<int>(std::lround(angle / (kPi / 4.0)));
    return ((idx % 8) + 8) % 8;
}

// Angular difference between two 8-way directions, in 45-degree steps (0..4).
int turn_steps(int a, int b) {
    const int d = std::abs(a - b) % 8;
    return d > 4 ? 8 - d : d;
}

}  // namespace

RoadBuilder RoadBuilder::from_source(Position p, PostOfficeId src_id) {
    RoadBuilder builder;
    builder.src = src_id;
    builder.vertices = {p};
    return builder;
}

std::vector<Position> RoadBuilder::preview_vertices() const {
    std::vector<Position> path = vertices;
    if (middle.has_value())
        path.push_back(*middle);
    if (head.has_value())
        path.push_back(*head);
    return path;
}

Road RoadBuilder::into_road() const {
    return Road{src, dst, preview_vertices()};
}

void RoadBuilder::commit_to_destination(Position p, PostOfficeId dst_id) {
    build_to(p);
    commit_middle_and_head();
    dst = dst_id;
}

void RoadBuilder::build_to(Position target_position) {
    assert(!vertices.empty() && "build_to needs the source vertex");

    middle.reset();
    head.reset();
    is_invalid = false;

    const Position start = vertices.back();
    const int64_t dx = target_position.x - start.x;
    const int64_t dy = target_position.y - start.y;
    const int64_t w = std::abs(dx);
    const int64_t h = std::abs(dy);
    const int sx = sign(dx);
    const int sy = sign(dy);

    // --- mouse tracking: which way did the cursor leave the source? ---
    // Only relevant for the first segment (nothing committed yet beyond the source).
    if (vertices.size() == 1) {
        if (dx * dx + dy * dy < kTrailThreshold * kTrailThreshold) {
            // Cursor is back on top of the source: forget the old direction
            // so the user can leave again in a different way.
            trail_first_ortho.reset();
        } else if (!trail_first_ortho.has_value()) {
            // First significant movement: snap to 8 directions and lock it in.
            trail_first_ortho = (dir_index(dx, dy) % 2 == 0);
        }
    }

    // Zero-length: nothing to draw.
    if (w == 0 && h == 0) {
        return;
    }

    // Split the displacement into a straight part (along the longer axis)
    // and a diagonal part (equal movement on both axes).
    int64_t straight_x = 0, straight_y = 0;
    if (w > h) {
        straight_x = sx * (w - h);
    } else {
        straight_y = sy * (h - w);
    }
    const int64_t diag_x = sx * std::min(w, h);
    const int64_t diag_y = sy * std::min(w, h);

    const bool has_straight = (w != h);
    const bool has_diag = (w != 0 && h != 0);

    // A single pure segment: no corner needed.
    if (!(has_straight && has_diag)) {
        head = target_position;
        return;
    }

    // --- decide which segment comes first ---
    bool first_ortho;
    if (vertices.size() >= 2) {
        // Continuing a road: pick the ordering whose first segment turns
        // least away from the previous segment's direction.
        const Position prev = vertices[vertices.size() - 2];
        const int prev_dir = dir_index(start.x - prev.x, start.y - prev.y);
        const int straight_dir = dir_index(straight_x, straight_y);
        const int diag_dir = dir_index(diag_x, diag_y);
        const int straight_turn = turn_steps(prev_dir, straight_dir);
        const int diag_turn = turn_steps(prev_dir, diag_dir);

        if (straight_turn != diag_turn) {
            first_ortho = straight_turn < diag_turn;
        } else {
            first_ortho = prefer_first_direction_ortho;  // tie-break
        }
    } else {
        // First segment: follow the direction the cursor left the source in.
        first_ortho = trail_first_ortho.value_or(prefer_first_direction_ortho);
    }

    // --- build the corner ---
    if (first_ortho) {
        middle = Position{start.x + straight_x, start.y + straight_y};
    } else {
        middle = Position{start.x + diag_x, start.y + diag_y};
    }
    head = target_position;
}

void RoadBuilder::commit_middle_and_head() {
    vertices = preview_vertices();
    middle.reset();
    head.reset();
    collapse_collinear_vertices();
    previewed_target.reset();
}

void RoadBuilder::collapse_collinear_vertices() {
    std::vector<Position> out;
    out.reserve(vertices.size());

    for (const Position& p : vertices) {
        // Skip zero-length segments.
        if (!out.empty() && out.back() == p) continue;

        if (out.size() >= 2) {
            const Position& a = out[out.size() - 2];
            const Position& b = out.back();
            const int64_t abx = b.x - a.x, aby = b.y - a.y;
            const int64_t bcx = p.x - b.x, bcy = p.y - b.y;

            const bool collinear = (abx * bcy == aby * bcx);   // cross product == 0
            const bool same_way  = (abx * bcx + aby * bcy > 0); // dot > 0: not a reversal

            if (collinear && same_way) out.pop_back();  // b is redundant
        }
        out.push_back(p);
    }
    vertices = std::move(out);
}

void RoadBuilder::preview_to(Position target, const World& world) {
    if (previewed_target == target)
        return;
    build_to(target);
    check_road(world);
    previewed_target = target;
}

void RoadBuilder::check_road(const World& world) {
    const std::vector<Position> path = preview_vertices();
    // No self-intersection, and no crossing an existing road.
    is_invalid = polyline_self_intersects(path) || world.polyline_intersects_any(path);
}
