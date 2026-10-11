#pragma once

#include "entities.h"
#include "game_types.h"
#include "world.h"
#include "road.h"

#include <cmath>
#include <stdexcept>
#include <vector>


struct Road;

struct RoadBuilder {
    PostOfficeId src = kNoPostOffice;
    PostOfficeId dst = kNoPostOffice;
    std::vector<Position> vertices = {};  // the part that the user has committed to building
    std::optional<Position> middle = {};
    std::optional<Position> head = {};
    bool is_invalid = false;
    bool prefer_first_direction_ortho = true;
    std::optional<bool> trail_first_ortho = {};
    static constexpr double kTrailThreshold = 20.0;  // How far (in position units) the cursor must be from the source before we trust its direction

    // Call this when the user commits a vertex or cancels, so the next
    // segment starts with a fresh trail.
    void reset_trail() { trail_first_ortho.reset(); }

    // ---- helpers ----
    static int sign(double v) { return (v > 0) - (v < 0); }

    // 8-way direction index: 0=+x, 1=+x+y, 2=+y, ... 7=+x-y. Even = orthogonal.
    static int dir_index(double dx, double dy) {
        constexpr double kPi = 3.14159265358979323846;
        int idx = static_cast<int>(std::lround(std::atan2(dy, dx) / (kPi / 4.0)));
        return ((idx % 8) + 8) % 8;
    }

    // Angular difference between two 8-way directions, in 45-degree steps (0..4).
    static int turn_steps(int a, int b) {
        int d = std::abs(a - b) % 8;
        return d > 4 ? 8 - d : d;
    }

    Road into_road() {
        if (middle.has_value()) {
            vertices.push_back(middle.value());
        }
        if (head.has_value()) {
            vertices.push_back(head.value());
        }
        return Road {src, dst, std::move(vertices) };
    }

    static RoadBuilder RoadBuilder_from_source(Position p, PostOfficeId src_id) {
        return RoadBuilder {
            src_id, kNoPostOffice, { p }, {}, {}
        };
    }

    void commit_to_destination(Position p, PostOfficeId dst_id) {
        build_to(p);
        commit_middle_and_head();
        dst = dst_id;
    }

    void build_to(Position target_position) {
        if (vertices.size() < 1) {
            throw std::runtime_error("called build_to with empty set of vertices!");
        }

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
            if (std::hypot(dx, dy) < kTrailThreshold) {
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

        // Reject hairpins: first segment must not turn more than 90 degrees
        // from the previous one (2 steps of 45 degrees).
        if (vertices.size() >= 2) {
            const Position prev = vertices[vertices.size() - 2];
            const int prev_dir = dir_index(start.x - prev.x, start.y - prev.y);
            const int first_dir = dir_index(middle->x - start.x, middle->y - start.y);
            if (turn_steps(prev_dir, first_dir) > 2) {
                is_invalid = true;
            }
        }
    }

    void commit_middle_and_head() {
        if (middle.has_value()) {
            vertices.push_back(middle.value());
            middle = std::nullopt;
        }
        if (head.has_value()) {
            vertices.push_back(head.value());
            head = std::nullopt;
        }
        collapse_collinear_vertices();
    }

    void collapse_collinear_vertices() {
        std::vector<Position> out;
        out.reserve(vertices.size());

        for (const Position& p : vertices) {
            // Skip zero-length segments.
            if (!out.empty() && out.back().x == p.x && out.back().y == p.y) continue;

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

    // sets the is_invalid flag if this road_builder is found to be invalid.
    void check_road(const World& world) {
        std::vector<Position> verts = vertices;
        if (middle.has_value()) {
            verts.push_back(middle.value());
        }
        if (head.has_value()) {
            verts.push_back(head.value());
        }

        // No self-intersection (non-consecutive segment crossings).
        if (polyline_self_intersects(verts)) {
            is_invalid = true;
            return;
        }

        // Must not cross any existing road.
        if (world.polyline_intersects_any(verts)) {
            is_invalid = true;
            return;
        }
        
        is_invalid = false;
    }
};
