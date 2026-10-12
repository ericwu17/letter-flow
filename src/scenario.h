// scenario.h — starter scenarios built entirely on the World public API.
// Pure simulation layer: no imgui/SDL/OpenGL headers (see game_types.h).
#pragma once

#include "world.h"

// The starter scenario: thirty unbuilt offices across New Jersey, New York,
// and Pennsylvania, and no roads or truck routes. The player pays to build the
// offices, roads and schedules they want (see build_office, add_road and
// add_schedule); bigger offices cost more to build.
World create_default_world();
World create_default_world();
