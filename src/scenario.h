// scenario.h — starter scenarios built entirely on the World public API.
// Pure simulation layer: no imgui/SDL/Metal headers (see game_types.h).
#pragma once

#include "world.h"

// A small starter scenario: four offices, fully connected by default routes.
World create_default_world();
