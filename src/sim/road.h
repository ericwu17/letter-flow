// road.h — road entity and routing geometry helpers.
// Pure simulation layer: no imgui/SDL/OpenGL headers (see game_types.h).
//
// All geometry is exact integer arithmetic on Position (no floating point, no
// epsilons), valid as long as every coordinate is within ±kMaxCoordinate.
#pragma once

#include "sim/entities.h"
#include "sim/game_types.h"

#include <cmath>
#include <vector>

// Length of the vector p (e.g. a segment b - a).
inline double euclidean_length(Position p) {
    const auto x = static_cast<double>(p.x);
    const auto y = static_cast<double>(p.y);
    return std::sqrt(x * x + y * y);
}

// A road connecting two post offices.
// vertices[0] equals the position of offices[src], vertices.back() equals the
// position of offices[dst]. Intermediate vertices define the road path (only 90 and 45 degree connections allowed).
struct Road {
    PostOfficeId src = kNoPostOffice;  // normalized: src < dst
    PostOfficeId dst = kNoPostOffice;
    std::vector<Position> vertices;    // complete polyline, at least 2 vertices

    bool operator==(const Road&) const = default;
};


// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

// True if |x| and |y| of every vertex are at most kMaxCoordinate — the
// precondition of every other helper below.
bool all_vertices_in_range(const std::vector<Position>& vertices);

// True if the closed segments a1-a2 and b1-b2 share at least one point,
// including touching at an endpoint and collinear overlap.
bool segments_intersect(Position a1, Position a2, Position b1, Position b2);

// True if the polyline touches itself anywhere other than the joint vertex of
// two consecutive segments: crossings, revisited vertices, and consecutive
// segments that double back over each other all count.
bool polyline_self_intersects(const std::vector<Position>& vertices);

// True if `new_vertices` shares any point with `existing` other than a post
// office both roads end at. Roads may only meet at their end offices: a
// crossing, an overlap, a T-junction or a shared intermediate vertex all count.
bool polyline_crosses_road(const Road& existing,
                           const std::vector<Position>& new_vertices);

// True if every segment in the polyline has non-zero length and is
// horizontal, vertical, or exactly 45° diagonal.
bool all_segments_valid_direction(const std::vector<Position>& vertices);

// Total Euclidean length of all segments in the polyline.
double polyline_length(const std::vector<Position>& vertices);

// Shortest Euclidean distance from p to any point of the polyline
// (infinity for an empty one). Floating point: used to keep new offices clear
// of roads, never to judge a road's legality.
double distance_to_polyline(Position p, const std::vector<Position>& vertices);

// Convenience: total Euclidean length of a road.
inline double road_length(const Road& road) {
    return polyline_length(road.vertices);
}
