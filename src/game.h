// game.h — pure simulation layer.
//
// This file must NOT include imgui/SDL/Metal headers: the game rules are
// completely independent of rendering, so the simulation can be tested or
// run headless without a window.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

using Tick = std::uint64_t;

constexpr Tick kTicksPerSecond = 60;
constexpr Tick kTicksPerDay    = 3 * 60 * kTicksPerSecond;  // one in-game day = 3 real minutes = 10800 ticks

// ---------------------------------------------------------------------------
// World geometry
// ---------------------------------------------------------------------------

// The game plays out in a virtual coordinate space; the UI scales it to fit
// whatever window size the player has.
constexpr float kWorldWidth  = 1000.0f;
constexpr float kWorldHeight = 650.0f;
constexpr float kTruckSpeed  = 2.0f;  // world units per tick

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};

// ---------------------------------------------------------------------------
// Entities
// ---------------------------------------------------------------------------

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
    void push_back_letter(Letter);
    std::size_t get_num_letters() const;
    std::vector<Letter>const& get_letters() const;
    void clear_letters();

};

// ---------------------------------------------------------------------------
// World — owns all state and drives the simulation
// ---------------------------------------------------------------------------

class World {
private:
    std::vector<PostOffice> post_offices;  // stable storage; PostOfficeId indexes into this
    std::vector<Truck> trucks;
    Tick current_tick = 0;
    bool paused = false;
    float speed_multiplier = 1.0f;  // 1 = real time; >1 = fast-forward (transport bar)

    int money = 0;
    std::size_t letters_delivered_on_time = 0;
    std::size_t letters_delivered_late = 0;

    std::mt19937 rng{20260926u};  // fixed seed -> fully deterministic simulation

    // Simulation internals, driven by advance_tick() / seed_letters().
    void generate_letters(PostOfficeId office);
    void spawn_truck(PostOfficeId src, const TruckSchedule& schedule);
    void deliver(Truck& truck);

public:
    // Advances the simulation by one tick: daily letter generation, schedule
    // departures, truck arrivals and delivery/scoring.
    void advance_tick();

    // Scenario construction. create_default_world() builds the starter map
    // entirely through this public API — no friendship required.
    PostOfficeId add_office(std::string name, Position pos,
                            std::size_t letters_per_day,
                            std::size_t max_outbound_letters);
    // Fills every office's buffer with one day's worth of letters right away,
    // so a scenario starts in motion instead of waiting for the first day
    // boundary.
    void seed_letters();

    // Player actions (called from the UI).
    void add_schedule(PostOfficeId src, PostOfficeId dst, Tick period);
    void remove_schedule(PostOfficeId src, std::size_t schedule_index);
    void set_paused(bool p) { paused = p; }
    void set_speed_multiplier(float m) { speed_multiplier = m; }

    // Read-only access for the presentation layer. Named with the same get_*
    // convention as Truck's accessors above.
    Tick get_tick() const { return current_tick; }
    bool is_paused() const { return paused; }
    float get_speed_multiplier() const { return speed_multiplier; }
    int get_money() const { return money; }
    std::size_t get_letters_delivered_on_time() const { return letters_delivered_on_time; }
    std::size_t get_letters_delivered_late() const { return letters_delivered_late; }
    const std::vector<PostOffice>& get_post_offices() const { return post_offices; }
    const std::vector<Truck>& get_trucks() const { return trucks; }
};

// A small starter scenario: four offices, fully connected by default routes.
World create_default_world();
