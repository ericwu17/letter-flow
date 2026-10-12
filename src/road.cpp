// road.cpp — road geometry implementation. Pure logic, no rendering, no input.
#include "road.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace {

constexpr double kEpsilon = 1e-4;

bool approx_eq(double a, double b) {
    return std::abs(a - b) <= kEpsilon;
}

// Cross product of two 2D vectors (z-component).
int64_t cross(Position a, Position b) {
    return a.x * b.y - a.y * b.x;
}

// Vector from a to b.
Position delta(Position a, Position b) {
    return {b.x - a.x, b.y - a.y};
}

int64_t length_sq(Position p) {
    return p.x * p.x + p.y * p.y;
}


int64_t dot(Position a, Position b) {
    return a.x * b.x + a.y * b.y;
}

}  // namespace

// tests whether line segment (a1, a2) intersects line segment (b1, b2)
bool segments_intersect(Position a1, Position a2, Position b1, Position b2) {
    // Two segments intersect if and only if the endpoints of each segment lie
    // on opposite sides of the line defined by the other segment.
    //
    // Segment a: a1 + s * (a2 - a1), 0 <= s <= 1
    // Segment b: b1 + t * (b2 - b1), 0 <= t <= 1

    const Position da = delta(a1, a2);
    const Position db = delta(b1, b2);

    const int64_t denom = cross(da, db);

    // Check if segments are nearly parallel or degenerate.
    if (denom ==  0) {
        // Collinear case: check if they overlap.
        // If the cross product of (b1-a1) with da is non-zero, they are
        // parallel but not collinear.
        if (cross(delta(a1, b1), da) != 0)
            return false;

        // Collinear: check for overlap by projecting onto da.
        // Compute parametric intervals along da.
        const int64_t da_len_sq = length_sq(da);
        if (da_len_sq < kEpsilon * kEpsilon)
            return false;  // degenerate segment

        auto project = [&](Position p) {
            return static_cast<double>(dot(delta(a1, p), da)) / static_cast<double>(da_len_sq);
        };

        double t1 = project(b1), t2 = project(b2);
        if (t1 > t2) std::swap(t1, t2);

        // Overlap: the intervals [0,1] and [t1,t2] intersect.
        // But we exclude the case where they only touch at endpoints
        // (exactly one endpoint in common).
        const float overlap_min = std::max(0.0, t1);
        const float overlap_max = std::min(1.0, t2);
        if (overlap_max <= overlap_min + kEpsilon)
            return false;

        // They overlap. If the overlap is only at a single shared endpoint,
        // don't count it as a crossing.
        const float overlap_len = (overlap_max - overlap_min) * std::sqrt(da_len_sq);
        return overlap_len > kEpsilon;
    }

    // Non-parallel: compute intersection parameters.
    const double s = static_cast<double>(cross(delta(a1, b1), db)) / denom;
    const double t = static_cast<double>(cross(delta(a1, b1), da)) / denom;

    // Intersection must be strictly inside both segments.
    // We use a small epsilon margin so that floating-point slivers aren't
    // flagged, but legitimate interior crossings always are.
    if (s < -kEpsilon || s > 1.0f + kEpsilon) return false;
    if (t < -kEpsilon || t > 1.0f + kEpsilon) return false;

    // Reject only a shared endpoint: BOTH parameters at their segment ends
    // means the segments touch exactly where they end — s≈0 and t≈0
    // (a1==b1), s≈0 and t≈1 (a1==b2), s≈1 and t≈0 (a2==b1), or s≈1 and
    // t≈1 (a2==b2). Roads may meet end to end, so that is not a crossing.
    // An endpoint of one segment landing in the interior of the other (a
    // T-junction) fails this test and DOES count as a crossing: a road
    // vertex may not land on another road's interior.
    if ((approx_eq(s, 0.0f) || approx_eq(s, 1.0f)) &&
        (approx_eq(t, 0.0f) || approx_eq(t, 1.0f)))
        return false;  // shared endpoint, not a crossing

    return true;
}

bool polyline_self_intersects(const std::vector<Position>& vertices) {
    if (vertices.size() < 4)
        return false;  // need at least 2 disjoint segments to self-intersect

    for (std::size_t i = 1; i < vertices.size(); ++i) {
        for (std::size_t j = i + 2; j < vertices.size(); ++j) {
            // Skip consecutive segments (they share a vertex at j-1 == i).
            // j starts at i+2 so segments (i-1,i) and (j-1,j) are never
            // consecutive.
            if (j == i + 1) continue;  // already handled by j starting at i+2,
                                        // but belt and suspenders
            if (segments_intersect(vertices[i - 1], vertices[i],
                                   vertices[j - 1], vertices[j]))
                return true;
        }
    }
    return false;
}

bool polyline_crosses_road(const Road& existing,
                           const std::vector<Position>& new_vertices) {
    for (std::size_t i = 1; i < new_vertices.size(); ++i) {
        for (std::size_t j = 1; j < existing.vertices.size(); ++j) {
            if (segments_intersect(new_vertices[i - 1], new_vertices[i],
                                   existing.vertices[j - 1], existing.vertices[j]))
                return true;
        }
    }
    return false;
}

bool all_segments_valid_direction(const std::vector<Position>& vertices) {
    for (std::size_t i = 1; i < vertices.size(); ++i) {
        const int64_t dx = vertices[i].x - vertices[i - 1].x;
        const int64_t dy = vertices[i].y - vertices[i - 1].y;
        // Accept zero-length segments as valid (they'll be pruned later).
        if (dx != 0 && dy != 0 && std::abs(dx) != std::abs(dy)) {
            return false;
        }
    }
    return true;
}

double polyline_length(const std::vector<Position>& vertices) {
    double total = 0;
    for (std::size_t i = 1; i < vertices.size(); ++i) {
        total += length(vertices[i] - vertices[i-1]);
    }
    return total;
}
