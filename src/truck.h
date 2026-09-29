// truck.h — truck entity: deterministic point-to-point motion between two
// post offices, plus its carried cargo.
// Pure simulation layer: no imgui/SDL/Metal headers (see game_types.h).
#pragma once

#include "entities.h"
#include "game_types.h"

#include <cstddef>
#include <vector>

class Truck {
private:
    PostOfficeId src = kNoPostOffice;
    PostOfficeId dst = kNoPostOffice;
    Position from;               // world position at departure
    Position to;                 // world position of destination
    Tick departure_tick = 0;
    float speed = kTruckSpeed;   // world units per tick
    std::vector<Letter> carried_letters;

public:
    Truck(
        PostOfficeId src,
        PostOfficeId dst,
        Position from,
        Position to,
        Tick departure_tick,
        float speed
    ): src(src), dst(dst), from(from), to(to), departure_tick(departure_tick), speed(speed) {};

    // Truck position is computed from (departure point + direction * elapsed time)
    // rather than accumulated frame by frame, so it never drifts and is exactly
    // reproducible (deterministic replay).
    float route_length() const;
    Position get_position(Tick now) const;
    bool has_arrived(Tick now) const;
    PostOfficeId get_src() const { return src; }
    PostOfficeId get_dst() const { return dst; }
    void push_back_letter(Letter);
    std::size_t get_num_letters() const;
    std::vector<Letter>const& get_letters() const;
    // Moves the whole cargo out and leaves the truck empty; used on arrival so
    // the World can score final deliveries and forward the rest onward.
    std::vector<Letter> take_letters();

};
