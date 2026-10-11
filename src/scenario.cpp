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
        {"Lawrenceville", "NJ/Mercer/Lawrenceville", {145, 614}, 5},
        {"Princeton",     "NJ/Mercer/Princeton",     {197, 547}, 7},
        {"Trenton",       "NJ/Mercer/Trenton",       {133, 703}, 9},
        {"Hopewell",      "NJ/Mercer/Hopewell",      {116, 512}, 3},

        // Central Jersey
        {"Monroe",        "NJ/Central/Monroe",       {398, 589}, 5},
        {"Edison",        "NJ/Central/Edison",       {413, 367}, 8},

        // North Jersey
        {"Florham Park",  "NJ/North/FlorhamPark",    {432,  70}, 4},
        {"Newark",        "NJ/North/Newark",         {616, 127}, 12},

        // Jersey Shore
        {"Asbury Park",   "NJ/Shore/AsburyPark",     {751, 699}, 5},
        {"Long Branch",   "NJ/Shore/LongBranch",     {768, 606}, 6},

        // New York City
        {"Manhattan",     "NY/NYC/Manhattan",        {766, 108}, 15},
        {"Brooklyn",      "NY/NYC/Brooklyn",         {809, 191}, 13},
        {"Queens",        "NY/NYC/Queens",           {940, 164}, 11},
    };
    for (const OfficeDef& def : defs)
        world.add_office(def.name, def.postal_code, def.pos, def.letters_per_day, 60);

    // Seed day one's letters immediately so the game is in motion from tick 0
    // (per-tick spawning would otherwise leave the map nearly empty for the
    // first minutes while letters trickle in one at a time).
    world.seed_letters();

    return world;
}
