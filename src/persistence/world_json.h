// world_json.h — World <-> JSON conversion, for the persistence layer only
// (serialization.cpp implements it; save_library.cpp and the tests use it).
// Pure simulation layer: no imgui/SDL/OpenGL headers (see game_types.h).
//
// Kept out of world.h so that only the code that actually reads or writes
// saves pays for (and knows about) nlohmann/json. World befriends WorldJson
// so these functions can see its private state.
#pragma once

#include "sim/world.h"

#include <nlohmann/json.hpp>

#include <optional>

struct WorldJson {
    // The ENTIRE simulation state, RNG included, tagged with its own
    // format_version.
    static nlohmann::json to_json(const World& world);

    // nullopt if `j` is unusable in any way (incompatible version, missing or
    // mis-typed keys, dangling references), so callers simply fall back. The
    // one non-fatal problem is a corrupt RNG dump: the world loads with a
    // freshly seeded RNG instead.
    static std::optional<World> from_json(const nlohmann::json& j);
};
