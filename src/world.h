// world.h — World: owns all simulation state and drives it tick by tick.
// Pure simulation layer: no imgui/SDL/Metal headers (see game_types.h).
#pragma once

#include "entities.h"
#include "game_types.h"
#include "truck.h"

#include <cstddef>
#include <random>
#include <string>
#include <vector>

class World {
private:
    std::vector<PostOffice> post_offices;  // stable storage; PostOfficeId indexes into this
    std::vector<Truck> trucks;
    Tick current_tick = 0;
    bool paused = false;
    float speed_multiplier = 1.0f;  // 1 = real time; >1 = fast-forward (transport bar)

    int money = kStartingMoney;
    std::size_t letters_delivered_on_time = 0;
    std::size_t letters_delivered_late = 0;

    std::mt19937 rng{20260926u};  // fixed seed -> fully deterministic simulation

    // Simulation internals, driven by advance_tick() / seed_letters().
    void maybe_spawn_letter(PostOfficeId office);  // per-tick Bernoulli roll
    void spawn_letter(PostOfficeId office);        // one letter, buffer permitting
    void spawn_truck(PostOfficeId src, const TruckSchedule& schedule);
    void deliver(Truck& truck);

public:
    // Advances the simulation by one tick: probabilistic letter spawning,
    // schedule departures, truck arrivals and delivery/scoring.
    void advance_tick();

    // Scenario construction. create_default_world() (scenario.h) builds the
    // starter map entirely through this public API — no friendship required.
    PostOfficeId add_office(std::string name, std::string postal_code, Position pos,
                            std::size_t letters_per_day,
                            std::size_t max_outbound_letters);
    // Fills every office's buffer with one day's worth of letters right away,
    // so a scenario starts in motion instead of waiting for letters to trickle
    // in one probabilistic spawn at a time.
    void seed_letters();

    // Player actions (called from the UI).
    // Creates a truck schedule on the src -> dst route: departures fall on the
    // grid start_offset + k * period (both in ticks; start_offset is a time of
    // day, i.e. ticks since midnight of day one). Creating a schedule costs
    // schedule_cost(period) money up front — the higher the frequency, the
    // higher the price. Returns false and changes nothing if the route is
    // invalid, already exists, or the player cannot afford it.
    bool add_schedule(PostOfficeId src, PostOfficeId dst, Tick period, Tick start_offset);
    void remove_schedule(PostOfficeId src, std::size_t schedule_index);
    // Replaces an office's postal code (edited in the inspector).
    void set_postal_code(PostOfficeId office, std::string postal_code);
    void set_paused(bool p) { paused = p; }
    void set_speed_multiplier(float m) { speed_multiplier = m; }

    // Read-only access for the presentation layer. Named with the same get_*
    // convention as Truck's accessors.
    Tick get_tick() const { return current_tick; }
    bool is_paused() const { return paused; }
    float get_speed_multiplier() const { return speed_multiplier; }
    int get_money() const { return money; }
    std::size_t get_letters_delivered_on_time() const { return letters_delivered_on_time; }
    std::size_t get_letters_delivered_late() const { return letters_delivered_late; }
    const std::vector<PostOffice>& get_post_offices() const { return post_offices; }
    const std::vector<Truck>& get_trucks() const { return trucks; }
};
