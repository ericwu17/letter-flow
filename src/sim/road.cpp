// road.cpp — road geometry implementation. Pure logic, no rendering, no input.
//
// Every predicate here is exact: positions are integers and the only
// operations are subtraction, multiplication and comparison. (The length and
// distance measures at the end are floating point; nothing decides road
// legality with them.) The coordinate
// bound (kMaxCoordinate, see game_types.h) guarantees no int64_t overflow.
#include "sim/road.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace {

// Cross product of two 2D vectors (z-component).
int64_t cross(Position a, Position b) {
    return a.x * b.y - a.y * b.x;
}

int64_t dot(Position a, Position b) {
    return a.x * b.x + a.y * b.y;
}

// Turn direction of p -> q -> r: +1 counter-clockwise, -1 clockwise,
// 0 collinear.
int orientation(Position p, Position q, Position r) {
    const int64_t c = cross(q - p, r - p);
    return (c > 0) - (c < 0);
}

// For r collinear with p and q: true if r lies on the closed segment p-q.
bool on_segment(Position p, Position q, Position r) {
    return std::min(p.x, q.x) <= r.x && r.x <= std::max(p.x, q.x)
        && std::min(p.y, q.y) <= r.y && r.y <= std::max(p.y, q.y);
}

// If intersecting segments a and b meet in exactly one point and that point
// is an endpoint of both (end-to-end contact), returns it. Returns nullopt if
// they share no endpoint, or overlap along a stretch beyond the shared one.
std::optional<Position> sole_common_endpoint(Position a1, Position a2,
                                             Position b1, Position b2) {
    for (const auto& [a_end, a_far] : {std::pair{a1, a2}, std::pair{a2, a1}}) {
        for (const auto& [b_end, b_far] : {std::pair{b1, b2}, std::pair{b2, b1}}) {
            if (a_end != b_end)
                continue;
            // Collinear and heading the same way from the shared point: the
            // segments overlap beyond it.
            const Position u = a_far - a_end;
            const Position v = b_far - b_end;
            if (cross(u, v) == 0 && dot(u, v) > 0)
                return std::nullopt;
            return a_end;
        }
    }
    return std::nullopt;
}

}  // namespace

bool all_vertices_in_range(const std::vector<Position>& vertices) {
    return std::all_of(vertices.begin(), vertices.end(), [](Position p) {
        return std::abs(p.x) <= kMaxCoordinate && std::abs(p.y) <= kMaxCoordinate;
    });
}

bool segments_intersect(Position a1, Position a2, Position b1, Position b2) {
    const int o1 = orientation(a1, a2, b1);
    const int o2 = orientation(a1, a2, b2);
    const int o3 = orientation(b1, b2, a1);
    const int o4 = orientation(b1, b2, a2);

    // Each segment's endpoints are on different sides of (or one is on) the
    // other segment's line.
    if (o1 != o2 && o3 != o4)
        return true;

    // Collinear cases: an endpoint of one segment lies on the other.
    return (o1 == 0 && on_segment(a1, a2, b1))
        || (o2 == 0 && on_segment(a1, a2, b2))
        || (o3 == 0 && on_segment(b1, b2, a1))
        || (o4 == 0 && on_segment(b1, b2, a2));
}

bool polyline_self_intersects(const std::vector<Position>& vertices) {
    for (std::size_t i = 1; i < vertices.size(); ++i) {
        for (std::size_t j = i + 1; j < vertices.size(); ++j) {
            const Position a1 = vertices[i - 1], a2 = vertices[i];
            const Position b1 = vertices[j - 1], b2 = vertices[j];
            if (!segments_intersect(a1, a2, b1, b2))
                continue;
            // Consecutive segments always share their joint vertex; that
            // alone is fine, doubling back over each other is not.
            if (j == i + 1 && sole_common_endpoint(a1, a2, b1, b2))
                continue;
            return true;
        }
    }
    return false;
}

bool polyline_crosses_road(const Road& existing,
                           const std::vector<Position>& new_vertices) {
    const std::vector<Position>& old_vertices = existing.vertices;
    if (new_vertices.empty() || old_vertices.empty())
        return false;

    // A post office both roads start or end at.
    auto is_shared_end = [&](Position p) {
        return (p == new_vertices.front() || p == new_vertices.back())
            && (p == old_vertices.front() || p == old_vertices.back());
    };

    for (std::size_t i = 1; i < new_vertices.size(); ++i) {
        for (std::size_t j = 1; j < old_vertices.size(); ++j) {
            const Position a1 = new_vertices[i - 1], a2 = new_vertices[i];
            const Position b1 = old_vertices[j - 1], b2 = old_vertices[j];
            if (!segments_intersect(a1, a2, b1, b2))
                continue;
            const std::optional<Position> touch = sole_common_endpoint(a1, a2, b1, b2);
            if (touch && is_shared_end(*touch))
                continue;
            return true;
        }
    }
    return false;
}

bool all_segments_valid_direction(const std::vector<Position>& vertices) {
    for (std::size_t i = 1; i < vertices.size(); ++i) {
        const int64_t dx = vertices[i].x - vertices[i - 1].x;
        const int64_t dy = vertices[i].y - vertices[i - 1].y;
        if (dx == 0 && dy == 0)
            return false;  // zero-length segment
        if (dx != 0 && dy != 0 && std::abs(dx) != std::abs(dy))
            return false;  // neither axis-aligned nor 45° diagonal
    }
    return true;
}

double polyline_length(const std::vector<Position>& vertices) {
    double total = 0;
    for (std::size_t i = 1; i < vertices.size(); ++i) {
        total += euclidean_length(vertices[i] - vertices[i - 1]);
    }
    return total;
}

double distance_to_polyline(Position p, const std::vector<Position>& vertices) {
    if (vertices.empty())
        return std::numeric_limits<double>::infinity();
    double best = euclidean_length(p - vertices[0]);
    for (std::size_t i = 1; i < vertices.size(); ++i) {
        const Position a = vertices[i - 1];
        const Position b = vertices[i];
        const auto ab_x = static_cast<double>(b.x - a.x);
        const auto ab_y = static_cast<double>(b.y - a.y);
        const auto ap_x = static_cast<double>(p.x - a.x);
        const auto ap_y = static_cast<double>(p.y - a.y);
        const double len_sq = ab_x * ab_x + ab_y * ab_y;
        // The closest point of the segment: p projected onto it, clamped to its ends.
        const double t = len_sq == 0.0 ? 0.0 : std::clamp((ap_x * ab_x + ap_y * ab_y) / len_sq, 0.0, 1.0);
        best = std::min(best, std::hypot(ap_x - t * ab_x, ap_y - t * ab_y));
    }
    return best;
}
