// road_builder.h — RoadBuilder: the road the player is drawing, one click at
// a time, before it is handed to World::add_road. Pure simulation layer: no
// imgui/SDL/OpenGL headers (see game_types.h); the UI feeds it world-space
// cursor positions and draws preview_vertices().
//
// A road is a polyline of horizontal, vertical and 45° segments. The builder
// keeps the vertices the player has committed (clicked) so far, and snaps the
// stretch from the last of them to the cursor into at most two legal
// segments: an optional corner (`middle`) and the cursor point (`head`).
#pragma once

#include "sim/entities.h"
#include "sim/game_types.h"
#include "sim/road.h"

#include <cstdint>
#include <optional>
#include <vector>

class World;

struct RoadBuilder {
    PostOfficeId src = kNoPostOffice;
    PostOfficeId dst = kNoPostOffice;
    std::vector<Position> vertices = {};  // the part that the user has committed to building
    std::optional<Position> middle = {};  // the snapped corner towards the cursor, if any
    std::optional<Position> head = {};    // the cursor end of the uncommitted stretch
    bool is_invalid = false;              // set by check_road()
    bool prefer_first_direction_ortho = true;
    std::optional<bool> trail_first_ortho = {};
    // How far (in position units) the cursor must be from the source before
    // we trust the direction it left in.
    static constexpr int64_t kTrailThreshold = 20;

    // A builder whose road starts at the office `src_id`, at `p`.
    static RoadBuilder from_source(Position p, PostOfficeId src_id);

    // The whole road as currently previewed: the committed vertices, then
    // the snapped corner and cursor point if present.
    std::vector<Position> preview_vertices() const;

    // Re-snaps the uncommitted stretch so it ends at `target_position`.
    // Requires at least one committed vertex (the source).
    void build_to(Position target_position);

    // Commits the uncommitted stretch, dropping vertices that a straight run
    // makes redundant.
    void commit_middle_and_head();

    // Snaps to the office at `p` and commits the road up to it.
    void commit_to_destination(Position p, PostOfficeId dst_id);

    // The finished road (call after commit_to_destination).
    Road into_road() const;

    // Sets is_invalid if the previewed road crosses itself or any road
    // already in `world`, and clears it otherwise.
    void check_road(const World& world);

    // build_to(target) then check_road(world), skipped when nothing changed
    // since the last call: the UI calls this every frame, and the crossing
    // checks grow with the road network. The world's roads must not change
    // while a road is being drawn.
    void preview_to(Position target, const World& world);

private:
    void collapse_collinear_vertices();

    // The target of the last preview_to, cleared whenever the committed
    // vertices change.
    std::optional<Position> previewed_target;
};
