// scenario.cpp — starter scenario construction. Pure logic, no rendering,
// no input.
#include "scenario.h"

#include "entities.h"
#include "game_types.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

// Spreads the office coordinates below apart; kWorldWidth/kWorldHeight in
// game_types.h are sized to match.
constexpr std::int64_t kMapScale = 2;

World create_default_world() {
    World world;

    struct OfficeDef {
        const char* name;
        const char* postal_code;
        Position pos;
        std::size_t office_class;  // 1 = small town, 2 = city, 3 = major city
    };
    // Defaults per class, indexed by office_class.
    const std::size_t class_letters_per_day[] = {0, 20, 30, 50};
    const std::size_t class_max_outbound[]    = {0, 40, 60, 100};
    const int class_build_cost[]              = {0, 1000, 2500, 5000};
    const OfficeDef defs[] = {
        // Local cluster (Mercer County)
        {"Lawrenceville", "NJ/Mercer/Lawrenceville",      {459, 422}, 1},
        {"Princeton",     "NJ/Mercer/Princeton",          {484, 391}, 2},
        {"Trenton",       "NJ/Mercer/Trenton",            {454, 463}, 2},
        {"Hopewell",      "NJ/Mercer/Hopewell",           {447, 374}, 1},

        // Central Jersey
        {"Monroe",        "NJ/Central/Monroe",            {577, 410}, 1},
        {"Edison",        "NJ/Central/Edison",            {585, 307}, 2},
        {"Somerville",    "NJ/Central/Somerville",        {507, 278}, 1},

        // North Jersey
        {"Florham Park",  "NJ/North/FlorhamPark",         {594, 167}, 1},
        {"Newark",        "NJ/North/Newark",              {679, 194}, 3},
        {"Paterson",      "NJ/North/Paterson",            {679, 101}, 2},
        {"Elizabeth",     "NJ/North/Elizabeth",           {664, 232}, 2},

        // Jersey Shore
        {"Asbury Park",   "NJ/Shore/AsburyPark",          {742, 462}, 1},
        {"Long Branch",   "NJ/Shore/LongBranch",          {750, 418}, 1},
        {"Toms River",    "NJ/Shore/TomsRiver",           {669, 600}, 2},

        // South Jersey
        {"Cherry Hill",   "NJ/South/CherryHill",          {341, 610}, 1},

        // New York City
        {"Manhattan",     "NY/NYC/Manhattan",             {758, 170}, 3},
        {"Brooklyn",      "NY/NYC/Brooklyn",              {767, 239}, 3},
        {"Queens",        "NY/NYC/Queens",                {828, 199}, 3},
        {"Bronx",         "NY/NYC/Bronx",                 {800, 138}, 3},
        {"Staten Island", "NY/NYC/StatenIsland",          {687, 276}, 2},

        // Westchester
        {"Yonkers",       "NY/Westchester/Yonkers",       {787,  93}, 2},
        {"White Plains",  "NY/Westchester/WhitePlains",   {840,  40}, 1},

        // Philadelphia area
        {"Philadelphia",  "PA/Philadelphia/Philadelphia", {288, 601}, 3},
        {"Norristown",    "PA/Montgomery/Norristown",     {219, 513}, 1},

        // Bucks County
        {"Levittown",     "PA/Bucks/Levittown",           {420, 495}, 1},
        {"Doylestown",    "PA/Bucks/Doylestown",          {302, 415}, 1},
        {"New Hope",      "PA/Bucks/NewHope",             {372, 387}, 1},

        // Lehigh Valley
        {"Allentown",     "PA/LehighValley/Allentown",    {160, 261}, 2},
        {"Bethlehem",     "PA/LehighValley/Bethlehem",    {207, 251}, 2},
        {"Easton",        "PA/LehighValley/Easton",       {266, 219}, 1},
    };
    for (const OfficeDef& def : defs)
        world.add_office(def.name, def.postal_code,
                         {def.pos.x * kMapScale, def.pos.y * kMapScale},
                         class_letters_per_day[def.office_class],
                         class_max_outbound[def.office_class],
                         class_build_cost[def.office_class]);

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
        int build_cost;
    };
    // Several times the starter map's mail rates, so letters show up within
    // seconds of building an office.
    const OfficeDef defs[] = {
        {tutorial_map::kCentral,   "Central",   "CITY/Central",    {1000, 600}, 120, 2000},
        {tutorial_map::kAshford,   "Ashford",   "NORTH/Ashford",   { 750, 350},  80, 1000},
        {tutorial_map::kBrookside, "Brookside", "NORTH/Brookside", {1250, 350},  80, 1000},
        {tutorial_map::kDunmore,   "Dunmore",   "SOUTH/Dunmore",   {1000, 900},  80, 1000},
        {tutorial_map::kElmwood,   "Elmwood",   "ELM-01",          {1300, 900},  80, 1000},
    };
    for (const OfficeDef& def : defs) {
        [[maybe_unused]] const PostOfficeId id =
            world.add_office(def.name, def.postal_code, def.pos, def.letters_per_day,
                             /*max_outbound_letters=*/40, def.build_cost);
        assert(id == def.id);
    }

    // Every office and road the tutorial asks for costs about $12,500; the
    // rest leaves room for mistakes and truck departures.
    world.set_money(20000);
    world.set_paused(true);  // one of the first steps is pressing play
    return world;
}
