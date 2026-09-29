// truck.h — truck entity: deterministic point-to-point motion between two
// post offices, plus its carried cargo.
// Pure simulation layer: no imgui/SDL/Metal headers (see game_types.h).
#pragma once

#include "entities.h"
#include "game_types.h"

#include <cstddef>
#include <utility>
#include <vector>

// A truck's full state as plain data. Truck wraps one of these; serialization
// (serialization.cpp) round-trips it through the public get_state() accessor
// and the state constructor — no friend access into Truck needed. Its
// member-wise equality is defaulted, so Truck's is too: when adding a field,
// add it here (comparison updates automatically) and extend serialization.cpp
// and the test fixture (see the note in entities.h).
struct TruckState {
    PostOfficeId src = kNoPostOffice;
    PostOfficeId dst = kNoPostOffice;
    Position from;               // world position at departure
    Position to;                 // world position of destination
    Tick departure_tick = 0;
    float speed = kTruckSpeed;   // world units per tick
    std::vector<Letter> carried_letters;

    bool operator==(const TruckState&) const = default;
};

class Truck {
private:
    TruckState state;

public:
    Truck(
        PostOfficeId src,
        PostOfficeId dst,
        Position from,
        Position to,
        Tick departure_tick,
        float speed
    ): state{src, dst, from, to, departure_tick, speed, {}} {}
    explicit Truck(TruckState initial_state): state(std::move(initial_state)) {}

    // Truck position is computed from (departure point + direction * elapsed time)
    // rather than accumulated frame by frame, so it never drifts and is exactly
    // reproducible (deterministic replay).
    float route_length() const;
    Position get_position(Tick now) const;
    bool has_arrived(Tick now) const;
    PostOfficeId get_src() const { return state.src; }
    PostOfficeId get_dst() const { return state.dst; }
    void push_back_letter(Letter);
    std::size_t get_num_letters() const;
    const std::vector<Letter>& get_letters() const;
    // Moves the whole cargo out and leaves the truck empty; used on arrival so
    // the World can score final deliveries and forward the rest onward.
    std::vector<Letter> take_letters();
    // Full state (motion + cargo); save files store exactly this.
    const TruckState& get_state() const { return state; }

    // Full-state equality — defaulted, comparing the TruckState member whose
    // own equality is defaulted in turn (see the note in entities.h).
    bool operator==(const Truck&) const = default;
};
