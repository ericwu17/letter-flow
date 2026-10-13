// truck.h — truck entity: deterministic point-to-point motion between two
// post offices, plus its carried cargo.
// Pure simulation layer: no imgui/SDL/OpenGL headers (see game_types.h).
#pragma once

#include "entities.h"
#include "balance.h"
#include "game_types.h"
#include "road.h"

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
    ScheduleId schedule = kNoSchedule;  // the schedule it departed on (credited on arrival)
    std::vector<Position> vertices;
    std::vector<double> distances;

    Tick departure_tick = 0;
    float speed = kTruckSpeed;   // world units per tick
    std::vector<Letter> carried_letters;

    bool operator==(const TruckState&) const = default;
};

class Truck {
private:
    TruckState state;

    // Distance along the route covered by tick `now` (0 before departure).
    double distance_travelled(Tick now) const;

public:
    Truck(
        PostOfficeId src,
        PostOfficeId dst,
        std::vector<Position> vertices,
        Tick departure_tick,
        float speed,
        ScheduleId schedule = kNoSchedule
    ): state{src, dst, schedule, vertices, {}, departure_tick, speed, {}} {
        for (size_t i = 1; i < vertices.size(); i++) {
            state.distances.push_back((length(vertices[i] - vertices[i-1])));
        }
    }
    explicit Truck(TruckState initial_state): state(std::move(initial_state)) {}

    // Truck position is computed from the distance travelled since departure
    // (elapsed ticks * speed) along the route, rather than accumulated frame by
    // frame, so it never drifts and is exactly reproducible (deterministic replay).
    double route_length() const;
    Position get_position(Tick now) const;
    bool has_arrived(Tick now) const;
    PostOfficeId get_src() const { return state.src; }
    PostOfficeId get_dst() const { return state.dst; }
    ScheduleId get_schedule() const { return state.schedule; }
    Tick get_departure_tick() const { return state.departure_tick; }
    // First tick at which has_arrived() is true.
    Tick get_arrival_tick() const;
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
