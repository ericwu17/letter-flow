// scenario.h — starter scenarios built entirely on the World public API.
// Pure simulation layer: no imgui/SDL/Metal headers (see game_types.h).
#pragma once

#include "world.h"

// The starter scenario: thirteen offices across New Jersey and New York City
// and no truck routes — the player buys the schedules they want, and each one
// costs money (see add_schedule).
World create_default_world();
