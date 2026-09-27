// scenario.cpp — starter scenario construction. Pure logic, no rendering,
// no input.
#include "scenario.h"

#include "entities.h"
#include "game_types.h"

#include <cstddef>

World create_default_world() {
    World world;

    struct OfficeDef {
        const char* name;
        Position pos;
        std::size_t letters_per_day;
    };
    const OfficeDef defs[] = {
        {"Northgate", {220.0f, 140.0f}, 8},
        {"Eastport",  {780.0f, 160.0f}, 6},
        {"Southvale", {740.0f, 520.0f}, 8},
        {"Westbrook", {240.0f, 500.0f}, 6},
    };
    for (const OfficeDef& def : defs)
        world.add_office(def.name, def.pos, def.letters_per_day, /*max_outbound_letters=*/60);

    // The basic routing rule only loads letters addressed directly to the
    // truck's destination, so every office needs a route to every other one.
    // (Removing routes in the UI and watching letters pile up / go late is
    // the interesting part of this toy.)
    const Tick default_period = 900;  // one truck every 15 seconds
    const std::size_t num_offices = world.get_post_offices().size();
    for (PostOfficeId src = 0; src < num_offices; ++src)
        for (PostOfficeId dst = 0; dst < num_offices; ++dst)
            world.add_schedule(src, dst, default_period);

    // Seed day one's letters immediately so the game is in motion from tick 0
    // (per-tick spawning would otherwise leave the map nearly empty for the
    // first minutes while letters trickle in one at a time).
    world.seed_letters();

    return world;
}
