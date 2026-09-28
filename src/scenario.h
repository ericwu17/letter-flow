// scenario.h — starter scenarios built entirely on the World public API.
// Pure simulation layer: no imgui/SDL/Metal headers (see game_types.h).
#pragma once

#include "world.h"

// A small starter scenario: four offices and no truck routes — the player
// buys the schedules they want, and each one costs money (see add_schedule).
World create_default_world();
