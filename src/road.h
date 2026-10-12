// road.h — road entity and routing geometry helpers.
// Pure simulation layer: no imgui/SDL/Metal headers (see game_types.h).
#pragma once

#include "entities.h"
#include "game_types.h"

#include <cmath>
#include <vector>

inline double length(Position p) {
    return std::sqrt(p.x * p.x + p.y * p.y);
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

// Snap a cursor position onto the nearest of the 8 cardinal/diagonal rays
// emanating from `from`. The result is a point such that the segment from
// `from` to the result is horizontal, vertical, or exactly 45° diagonal.
// Position snap_to_8dir(Position from, Position cursor);

// True if the two line segments (a1-a2) and (b1-b2) intersect at a point
// that is not a shared endpoint. Uses a standard cross-product parametric
// test with an epsilon threshold for floating-point robustness.
bool segments_intersect(Position a1, Position a2, Position b1, Position b2);

// True if any non-consecutive pair of segments in the polyline `vertices`
// crosses. Consecutive segments share a vertex and are never counted as
// self-intersecting.
bool polyline_self_intersects(const std::vector<Position>& vertices);

// True if any segment of `new_vertices` crosses any segment of `existing`.
// Segments that share only an endpoint (touching at a vertex) are NOT counted
// as crossing — roads can meet at vertices.
bool polyline_crosses_road(const Road& existing,
                           const std::vector<Position>& new_vertices);

// True if every segment in the polyline is horizontal, vertical, or exactly
// 45° diagonal, within floating-point epsilon.
bool all_segments_valid_direction(const std::vector<Position>& vertices);

// Total Euclidean length of all segments in the polyline.
double polyline_length(const std::vector<Position>& vertices);

// Convenience: total Euclidean length of a road.
inline double road_length(const Road& road) {
    return polyline_length(road.vertices);
}

// Cost in money to build a road: ceil(total length) * kRoadCostPerUnit.
inline int road_cost(const std::vector<Position>& vertices) {
    return static_cast<int>(std::ceil(polyline_length(vertices)) * kRoadCostPerUnit);
}
inline int road_cost(const Road& road) {
    return road_cost(road.vertices);
}
