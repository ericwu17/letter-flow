// world.h — World: owns all simulation state and drives it tick by tick.
// Pure simulation layer: no imgui/SDL/OpenGL headers (see game_types.h).
#pragma once

#include "entities.h"
#include "balance.h"
#include "game_types.h"
#include "road.h"
#include "truck.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

class World {
private:
    std::vector<PostOffice> post_offices;  // stable storage; PostOfficeId indexes into this
    // Trucks live on the heap, so a truck's address never changes while this
    // vector grows or swap-and-pops, and are shared, so the UI can keep its
    // selected truck alive (and readable) after it is retired on arrival.
    std::vector<std::shared_ptr<Truck>> trucks;
    std::vector<Road> roads;               // the road network
    Tick current_tick = 0;
    bool paused = false;
    float speed_multiplier = 1.0f;  // 1 = real time; >1 = fast-forward (transport bar)

    int money = kStartingMoney;
    std::size_t letters_delivered_on_time = 0;
    std::size_t letters_delivered_late = 0;

    // Payouts and departure charges since the UI last called
    // take_money_events(). Transient: neither serialized nor compared.
    std::vector<MoneyEvent> money_events;

    std::mt19937 rng{20260926u};  // fixed seed -> fully deterministic simulation

    // Simulation internals, driven by advance_tick() / seed_letters().
    void maybe_spawn_letter(PostOfficeId office);  // per-tick Bernoulli roll
    void spawn_letter(PostOfficeId office);        // one letter, buffer permitting
    void spawn_truck(PostOfficeId src, const TruckSchedule& schedule);
    void deliver(Truck& truck);
    // Shared validation for add_schedule() / update_schedule(): returns the
    // normalized rule, or nullopt if the route is invalid or would duplicate
    // (dst, rule) of any schedule of `src` other than the one at `ignore_index`.
    std::optional<RoutingRule> validate_schedule(PostOfficeId src, PostOfficeId dst, Tick period,
                                                 const RoutingRule& rule,
                                                 std::size_t ignore_index) const;

public:
    // Advances the simulation by one tick: probabilistic letter spawning,
    // schedule departures, truck arrivals and delivery/scoring.
    void advance_tick();

    // Scenario construction. create_default_world() (scenario.h) builds the
    // starter map entirely through this public API — no friendship required.
    // New offices start unbuilt; the player pays build_cost to build them.
    PostOfficeId add_office(std::string name, std::string postal_code, Position pos,
                            std::size_t letters_per_day,
                            std::size_t max_outbound_letters,
                            int build_cost);
    // Fills every built office's buffer with one day's worth of letters right away,
    // so a scenario starts in motion instead of waiting for letters to trickle
    // in one probabilistic spawn at a time.
    void seed_letters();

    // Player actions (called from the UI).
    // Builds an office, paying its build_cost up front. Only built offices
    // spawn and receive letters, and only they can be joined by roads (and
    // hence schedules). Returns false and changes nothing if the id is
    // invalid, the office is already built, or the player cannot afford it.
    bool build_office(PostOfficeId office);
    // Creates a truck schedule on the src -> dst route: departures fall on the
    // grid start_offset + k * period (both in ticks; start_offset is a time of
    // day, i.e. ticks since midnight of day one). The routing rule decides
    // which outbound letters each departure picks up (at most kTruckCapacity).
    // Creating a schedule is free; each departure costs kTruckDepartureCost.
    // Returns false and changes nothing if the route is invalid (bad ids,
    // zero period, a PostalPrefix rule with no, an empty or a repeated
    // prefix), an identical (dst, rule) schedule already exists, or no road
    // connects src and dst.
    // Schedules to the same dst with different rules are allowed; on a tick
    // where both depart, the earlier one in the list takes contested letters.
    bool add_schedule(PostOfficeId src, PostOfficeId dst, Tick period, Tick start_offset,
                      const RoutingRule& rule);
    // Edits the timing and rule of the schedule at `schedule_index` in place
    // (the inspector's schedule table), keeping its destination and list
    // position. Same validation as add_schedule, except the duplicate check
    // skips the schedule itself. Changing period or start_offset re-anchors
    // the next departure on the new grid (first slot strictly after now);
    // changing only the rule keeps the next departure as is. Returns false
    // and changes nothing on an invalid index or schedule.
    bool update_schedule(PostOfficeId src, std::size_t schedule_index,
                         Tick period, Tick start_offset, const RoutingRule& rule);
    void remove_schedule(PostOfficeId src, std::size_t schedule_index);
    // Moves the schedule at index `from` to index `to` (drag-to-reorder in the
    // inspector). List order is the pick-up priority: departures fire in list
    // order and spawn_truck() moves letters out of the buffer, so when several
    // schedules depart on the same tick, the first matching one wins a letter.
    void move_schedule(PostOfficeId src, std::size_t from, std::size_t to);
    // Replaces an office's postal code (edited in the inspector).
    void set_postal_code(PostOfficeId office, std::string postal_code);
    void set_paused(bool p) { paused = p; }
    void set_speed_multiplier(float m) { speed_multiplier = m; }
    // Overrides the balance (tests and debugging; normal play only earns and spends).
    void set_money(int m) { money = m; }

    // --- Road API ---


    // Build a road from `src` to `dst`. `vertices` is the complete polyline:
    // vertices[0] must equal post_offices[src].pos and vertices.back() must
    // equal post_offices[dst].pos, and every coordinate must be within
    // ±kMaxCoordinate. Both offices must be built. All segments must be
    // non-empty and horizontal, vertical, or exactly 45° diagonal; the road
    // must not touch itself, and
    // may touch existing roads only at post offices they share as endpoints.
    // The player pays road_cost(road) up front. Internally the
    // road is normalized so src < dst. Returns false and changes nothing on
    // failure.
    bool add_road(PostOfficeId src, PostOfficeId dst, std::vector<Position> vertices);

    // Destroy the road connecting a and b (undirected; argument order does not
    // matter). There is NO refund. Truck schedules only ever use the direct
    // road (add_schedule checks has_road), and spawn_truck would dereference
    // an empty vertex list once the road is gone, so every schedule between
    // the two endpoints is cascade-deleted on both offices (roads are
    // undirected, schedules are per-source-office). In-flight trucks are
    // unaffected: they snapshot the polyline at departure. Returns false and
    // changes nothing if the ids are invalid or no such road exists.
    bool remove_road(PostOfficeId a, PostOfficeId b);

    // True if any road connects a and b (undirected).
    bool has_road(PostOfficeId a, PostOfficeId b) const;

    // Returns a pointer to the Road connecting a and b, or nullptr.
    const Road* find_road(PostOfficeId a, PostOfficeId b) const;

    std::vector<Position> get_road_vertices_from_a_to_b(PostOfficeId src, PostOfficeId dst) const;

    bool polyline_intersects_any(const std::vector<Position>& vertices) const;
    const std::vector<Road>& get_roads() const { return roads; }

    // Persistence, implemented in serialization.cpp — the only translation
    // unit that knows nlohmann/json exists, keeping the simulation headers
    // dependency-free. save_to_file() writes the ENTIRE state (offices,
    // letters, schedules, in-flight trucks, clock, money, and the RNG's
    // internal state, so a loaded game continues the identical random
    // sequence) as JSON, via temp-file + rename (see save_to_file).
    // load_from_file() returns nullopt if the file is missing or unusable in
    // any way (corrupt JSON, incompatible version, dangling references), so
    // callers simply fall back to a fresh world. The one non-fatal problem is
    // a corrupt RNG dump: the game loads with a freshly seeded RNG instead.
    bool save_to_file(const std::string& path) const;
    static std::optional<World> load_from_file(const std::string& path);

    // Full simulation-state equality, RNG included (standard engines are
    // equality-comparable). Hand-written, unlike every entity's: a defaulted
    // one would compare the truck pointers rather than the trucks. When adding
    // a World field, extend this operator (world.cpp) as well as
    // serialization.cpp and the test fixture (see the note in entities.h).
    bool operator==(const World& other) const;

    // Read-only access for the presentation layer. Named with the same get_*
    // convention as Truck's accessors.
    Tick get_tick() const { return current_tick; }
    bool is_paused() const { return paused; }
    float get_speed_multiplier() const { return speed_multiplier; }
    int get_money() const { return money; }
    std::size_t get_letters_delivered_on_time() const { return letters_delivered_on_time; }
    std::size_t get_letters_delivered_late() const { return letters_delivered_late; }
    // Hands over (and clears) the payouts and charges recorded since the last call.
    std::vector<MoneyEvent> take_money_events() { return std::exchange(money_events, {}); }
    const std::vector<PostOffice>& get_post_offices() const { return post_offices; }
    // The vector is const but the handles point at mutable Trucks; the
    // presentation layer only ever holds them as pointers-to-const.
    const std::vector<std::shared_ptr<Truck>>& get_trucks() const { return trucks; }
};
