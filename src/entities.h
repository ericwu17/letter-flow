// entities.h — pure simulation layer: passive data entities (letters, truck
// schedules, post offices). No imgui/SDL/Metal headers (see game_types.h).
#pragma once

#include "game_types.h"

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

// Post offices live in World::post_offices and are referenced everywhere by
// index (a "handle") instead of raw pointer. Indices stay valid no matter how
// the vector grows — pointers would dangle on every reallocation.
using PostOfficeId = std::size_t;
inline constexpr PostOfficeId kNoPostOffice = std::numeric_limits<PostOfficeId>::max();

struct Letter {
    PostOfficeId src = kNoPostOffice;
    PostOfficeId dst = kNoPostOffice;
    Tick deadline = 0;  // delivered by this tick -> earn value; later -> pay fine
    int value = 0;      // earned on on-time delivery
    int fine = 0;       // paid on late delivery
};

struct TruckSchedule {
    PostOfficeId dst = kNoPostOffice;  // src is implicitly the office that owns this schedule
    Tick period = 0;                   // one departure every `period` ticks (must be > 0)
    Tick next_departure = 0;           // absolute tick of the next departure
};

struct PostOffice {
    Position pos;
    std::string name;
    std::size_t max_outbound_letters = 0;  // game-rule cap on the buffer (not vector::capacity!)
    std::size_t letters_per_day = 0;       // letters generated at each day boundary
    std::vector<Letter> outbound_letters;
    std::vector<TruckSchedule> outbound_schedules;
};
