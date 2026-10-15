// scenario.cpp — starter scenario construction. Pure logic, no rendering,
// no input.
#include "sim/scenario.h"

#include "sim/balance.h"
#include "sim/entities.h"
#include "sim/game_types.h"

#include <cassert>
#include <cstddef>

World create_default_world() {
    World world;
    for (std::size_t i = 0; i < kInitialOffices; ++i)
        world.spawn_random_office();
    return world;
}

World create_tutorial_world() {
    World world;

    struct OfficeDef {
        PostOfficeId id;  // the tutorial_map id add_office must hand out
        const char* name;
        const char* postal_code;
        Position pos;
        std::size_t letters_per_day;
    };
    // Several times a real game's mail rates, so letters show up within
    // seconds of pressing play.
    const OfficeDef defs[] = {
        {tutorial_map::kCentral,   "Central",   "CITY/Central",    {1000, 600}, 120},
        {tutorial_map::kAshford,   "Ashford",   "NORTH/Ashford",   { 750, 350},  80},
        {tutorial_map::kBrookside, "Brookside", "NORTH/Brookside", {1250, 350},  80},
        {tutorial_map::kDunmore,   "Dunmore",   "SOUTH/Dunmore",   {1000, 900},  80},
        {tutorial_map::kElmwood,   "Elmwood",   "ELM-01",          {1300, 900},  80},
    };
    for (const OfficeDef& def : defs) {
        // Roomy buffers: overflow can still end the tutorial, but not while
        // a newcomer is reading the first few cards.
        [[maybe_unused]] const PostOfficeId id =
            world.add_office(def.name, def.postal_code, def.pos, def.letters_per_day,
                             /*max_outbound_letters=*/300);
        assert(id == def.id);
    }

    // The four roads the tutorial asks for cost about $6,500; the rest
    // leaves room for mistakes and truck departures.
    world.set_money(12000);
    world.set_paused(true);  // one of the first steps is pressing play
    return world;
}
