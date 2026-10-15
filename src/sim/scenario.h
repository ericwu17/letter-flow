// scenario.h — starter scenarios built entirely on the World public API.
// Pure simulation layer: no imgui/SDL/OpenGL headers (see game_types.h).
#pragma once

#include "sim/entities.h"
#include "sim/world.h"

// A new game: kInitialOffices randomly placed and named offices (see
// World::spawn_random_office), with no roads or truck routes. More offices
// keep appearing as the game goes on; the player builds the roads and
// schedules that keep their mail moving (see add_road and add_schedule).
// Deterministic: the World's RNG starts from a fixed seed.
World create_default_world();

// The tutorial's offices, by id (create_tutorial_world adds them in this
// order). Central is the hub; Ashford and Brookside lie on 45° diagonals
// from it, Dunmore straight below it and Elmwood straight right of Dunmore,
// so every road the tutorial asks for is a single straight segment.
namespace tutorial_map {
inline constexpr PostOfficeId kCentral = 0;
inline constexpr PostOfficeId kAshford = 1;
inline constexpr PostOfficeId kBrookside = 2;
inline constexpr PostOfficeId kDunmore = 3;
inline constexpr PostOfficeId kElmwood = 4;
inline constexpr PostOfficeId kOfficeCount = 5;
}  // namespace tutorial_map

// The tutorial world (see tutorial.h): the five offices above with fast mail,
// a generous balance, and the clock paused. Elmwood starts with a
// misfiled postal code ("ELM-01") for the postal-code lesson to fix.
World create_tutorial_world();
