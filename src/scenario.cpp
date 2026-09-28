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
        const char* postal_code;
        Position pos;
        std::size_t letters_per_day;
    };
    const OfficeDef defs[] = {
        // Local cluster (Mercer County)
        {"Lawrenceville", "NJ/Mercer/Lawrenceville", {145.0f, 614.0f}, 5},
        {"Princeton",     "NJ/Mercer/Princeton",     {197.0f, 547.0f}, 7},
        {"Trenton",       "NJ/Mercer/Trenton",       {133.0f, 703.0f}, 9},
        {"Hopewell",      "NJ/Mercer/Hopewell",      {116.0f, 512.0f}, 3},

        // Central Jersey
        {"Monroe",        "NJ/Central/Monroe",       {398.0f, 589.0f}, 5},
        {"Edison",        "NJ/Central/Edison",       {413.0f, 367.0f}, 8},

        // North Jersey
        {"Florham Park",  "NJ/North/FlorhamPark",    {432.0f,  70.0f}, 4},
        {"Newark",        "NJ/North/Newark",         {616.0f, 127.0f}, 12},

        // Jersey Shore
        {"Asbury Park",   "NJ/Shore/AsburyPark",     {751.0f, 699.0f}, 5},
        {"Long Branch",   "NJ/Shore/LongBranch",     {768.0f, 606.0f}, 6},

        // New York City
        {"Manhattan",     "NY/NYC/Manhattan",        {766.0f, 108.0f}, 15},
        {"Brooklyn",      "NY/NYC/Brooklyn",         {809.0f, 191.0f}, 13},
        {"Queens",        "NY/NYC/Queens",           {940.0f, 164.0f}, 11},
    };
    for (const OfficeDef& def : defs)
        world.add_office(def.name, def.postal_code, def.pos, def.letters_per_day, 60);

    // Seed day one's letters immediately so the game is in motion from tick 0
    // (per-tick spawning would otherwise leave the map nearly empty for the
    // first minutes while letters trickle in one at a time).
    world.seed_letters();

    return world;
}
