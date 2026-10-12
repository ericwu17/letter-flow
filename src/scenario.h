// scenario.h — starter scenarios built entirely on the World public API.
// Pure simulation layer: no imgui/SDL/OpenGL headers (see game_types.h).
#pragma once

#include "world.h"

// The starter scenario: thirty offices across New Jersey, New York, and
// Pennsylvania and no truck routes — the player buys the schedules they want, and each one
// costs money (see add_schedule).
World create_default_world();
