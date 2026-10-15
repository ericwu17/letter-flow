// world.h — World: owns all simulation state and drives it tick by tick.
// Pure simulation layer: no imgui/SDL/OpenGL headers (see game_types.h).
#pragma once

#include "sim/entities.h"
#include "sim/balance.h"
#include "sim/game_types.h"
#include "sim/road.h"
#include "sim/stats.h"
#include "sim/truck.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

// A tick that never comes: "not scheduled".
inline constexpr Tick kNever = std::numeric_limits<Tick>::max();

// The seed a World's RNG starts from unless reseeded (see World::reseed).
inline constexpr std::uint32_t kDefaultSeed = 20260926u;

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

    // Statistics (see stats.h): network-wide series here, per-office and
    // per-schedule ones on those entities. Simulation state like any other:
    // saved, loaded and compared.
    GlobalStats stats;
    std::vector<DeliveredLetter> recent_deliveries;  // oldest first, at most kRecentDeliveriesSize
    ScheduleId next_schedule_id = 0;                 // next id add_schedule hands out

    // When advance_tick() next calls spawn_random_office().
    Tick next_office_spawn = kOfficeSpawnIntervalStart;
    // The office whose overflow timer ran out, ending the game; kNoPostOffice
    // while the game is still on.
    PostOfficeId game_over_office = kNoPostOffice;

    // Network events (see the Events section of balance.h): the one
    // announced or running, if any; when the next is announced (kNever until
    // the map first has kEventMinOffices offices); and how many have ended.
    std::optional<NetworkEvent> event;
    Tick next_event_announce = kNever;
    std::size_t events_so_far = 0;

    // Payouts and departure charges since the UI last called
    // take_money_events(). Transient: neither serialized nor compared.
    std::vector<MoneyEvent> money_events;

    std::mt19937 rng{kDefaultSeed};  // fixed seed -> fully deterministic simulation

    // Simulation internals, driven by advance_tick() / seed_letters().
    void maybe_spawn_letter(PostOfficeId office);  // per-tick Bernoulli roll
    // One letter posted at `office`, buffer permitting (else it is counted as
    // lost). Addressed to `forced_dst` if given, else to a random other
    // office (see kDestinationDistanceScale); `event` marks event mail.
    void spawn_letter(PostOfficeId office, std::optional<PostOfficeId> forced_dst = std::nullopt,
                      bool event = false);
    // The odds of a letter posted at `src` going to each other office:
    // fills `candidates` with the other offices and `weights` with their odds.
    void destination_weights(PostOfficeId src, std::vector<PostOfficeId>& candidates,
                             std::vector<double>& weights) const;
    // Network events: schedules, announces, runs and retires them (one step
    // of advance_tick()).
    void update_event();
    void announce_event();
    void post_event_letters();
    void spawn_truck(PostOfficeId src, TruckSchedule& schedule);
    void deliver(Truck& truck);
    // Runs every office's overflow timer for this tick and ends the game when
    // one runs out.
    void update_overflow();
    // How long after a spawn the next one comes, given the offices so far.
    Tick office_spawn_interval() const;
    // Records this tick's sample of every gauge statistic.
    void sample_gauges();
    // The schedule with this id among `office`'s schedules, or nullptr.
    TruckSchedule* find_schedule(PostOfficeId office, ScheduleId id);
    // Shared validation for add_schedule() / update_schedule() and their
    // check_* queries: why a schedule src -> dst with this period and
    // (normalized) rule is refused.
    ScheduleError validate_schedule(PostOfficeId src, PostOfficeId dst, Tick period,
                                    const RoutingRule& normalized_rule) const;

public:
    // Advances the simulation by one tick: new offices appearing, network
    // events, probabilistic letter spawning, schedule departures, truck
    // arrivals and delivery/scoring, and the overflow timers. Does nothing at all once the
    // game is over (the clock stops too).
    void advance_tick();

    // Scenario construction. create_default_world() (scenario.h) builds the
    // starter map entirely through this public API — no friendship required.
    // Restarts the RNG from `seed`. Same seed, same game: everything random
    // (office placement, mail, events) follows from it.
    void reseed(std::uint32_t seed) { rng.seed(seed); }
    // An office is open for mail the moment it is added.
    PostOfficeId add_office(std::string name, std::string postal_code, Position pos,
                            std::size_t letters_per_day,
                            std::size_t max_outbound_letters);
    // Adds an office of a random class (kOfficeClasses) at a random free spot
    // (see the placement rules in balance.h), named after a random unused
    // entry of office_names(), with no postal code. advance_tick() calls it
    // whenever next_office_spawn comes due; scenarios call it to open a game.
    // Returns nullopt, adding nothing, if every name is taken or no free spot
    // turned up.
    std::optional<PostOfficeId> spawn_random_office();
    // Fills every office's buffer with one day's worth of letters right away,
    // so a scenario starts in motion instead of waiting for letters to trickle
    // in one probabilistic spawn at a time.
    void seed_letters();

    // Player actions (called from the UI).
    // Creates a truck schedule on the src -> dst route: each day, departures
    // fall on the grid start_offset + k * period up to midnight, restarting at
    // start_offset the next day (both in ticks; start_offset is a time of day,
    // i.e. ticks since midnight). The routing rule decides
    // which outbound letters each departure picks up (at most kTruckCapacity).
    // Creating a schedule is free; each departure costs kTruckDepartureCost.
    // Returns false and changes nothing if the route is invalid (bad ids,
    // zero period, a PostalPrefix rule with no, an empty or a repeated
    // prefix) or no road connects src and dst.
    // Any number of schedules may share a dst, identical ones included; on a
    // tick where several depart, the earlier one in the list takes contested
    // letters.
    bool add_schedule(PostOfficeId src, PostOfficeId dst, Tick period, Tick start_offset,
                      const RoutingRule& rule);
    // Edits the timing and rule of the schedule at `schedule_index` in place
    // (the inspector's schedule table), keeping its destination and list
    // position. Same validation as add_schedule. Changing period or
    // start_offset re-anchors the next departure on the new grid (first slot
    // strictly after now); changing only the rule keeps the next departure as
    // is. Returns false and changes nothing on an invalid index or schedule.
    bool update_schedule(PostOfficeId src, std::size_t schedule_index,
                         Tick period, Tick start_offset, const RoutingRule& rule);
    // Why add_schedule() / update_schedule() with these arguments would
    // return false right now, or ScheduleError::None if it would succeed
    // (start_offset is never a reason, so it is left out). The UI uses these
    // to explain a refused schedule-table edit.
    ScheduleError check_add_schedule(PostOfficeId src, PostOfficeId dst, Tick period,
                                     const RoutingRule& rule) const;
    ScheduleError check_update_schedule(PostOfficeId src, std::size_t schedule_index, Tick period,
                                        const RoutingRule& rule) const;
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
    // Overrides the balance (scenarios, tests and debugging; normal play only
    // earns and spends).
    void set_money(int m) { money = m; }
    // Moves the next network event's announcement to `tick` (tests and
    // debugging; normal play schedules events itself). Meant for between
    // events: one that ends schedules the next itself, overriding this.
    void schedule_next_event(Tick tick) { next_event_announce = tick; }

    // --- Road API ---


    // Build a road from `src` to `dst`. `vertices` is the complete polyline:
    // vertices[0] must equal post_offices[src].pos and vertices.back() must
    // equal post_offices[dst].pos, and every coordinate must be within
    // ±kMaxCoordinate. All segments must be
    // non-empty and horizontal, vertical, or exactly 45° diagonal; the road
    // must not touch itself, and
    // may touch existing roads only at post offices they share as endpoints.
    // Roads are free to build. Internally the
    // road is normalized so src < dst. Returns false and changes nothing on
    // failure.
    bool add_road(PostOfficeId src, PostOfficeId dst, std::vector<Position> vertices);

    // Destroy the road connecting a and b (undirected; argument order does not
    // matter). Truck schedules only ever use the direct
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

    // The polyline of the road between src and dst, oriented from src to dst
    // (roads are stored normalized), or empty if there is no such road.
    std::vector<Position> road_path(PostOfficeId src, PostOfficeId dst) const;

    bool polyline_intersects_any(const std::vector<Position>& vertices) const;
    const std::vector<Road>& get_roads() const { return roads; }

    // Persistence: WorldJson (world_json.h, implemented in serialization.cpp)
    // converts the ENTIRE state (offices, letters, schedules, in-flight
    // trucks, clock, money, and the RNG's internal state, so a loaded game
    // continues the identical random sequence) to and from JSON. It is a
    // friend so that nlohmann/json stays out of this header; SaveLibrary
    // (save_library.h) is what reads and writes the files.
    friend struct WorldJson;

    // Full simulation-state equality, RNG included (standard engines are
    // equality-comparable). Hand-written, unlike every entity's: a defaulted
    // one would compare the truck pointers rather than the trucks. When adding
    // a World field, extend this operator (world.cpp) as well as
    // serialization.cpp and the test fixture (see the note in entities.h).
    bool operator==(const World& other) const;

    // Read-only access for the presentation layer. Named with the same get_*
    // convention as Truck's accessors.
    Tick get_tick() const { return current_tick; }
    Tick get_next_office_spawn() const { return next_office_spawn; }
    bool is_game_over() const { return game_over_office != kNoPostOffice; }
    // The office that overflowed and ended the game, or kNoPostOffice.
    PostOfficeId get_game_over_office() const { return game_over_office; }
    // The network event announced or under way, if any. It is under way once
    // the clock reaches its start.
    const std::optional<NetworkEvent>& get_event() const { return event; }
    // When the next event will be announced, or kNever while events are off.
    Tick get_next_event_announce() const { return next_event_announce; }
    // How many events have run their course.
    std::size_t get_events_so_far() const { return events_so_far; }
    bool is_paused() const { return paused; }
    float get_speed_multiplier() const { return speed_multiplier; }
    int get_money() const { return money; }
    std::size_t get_letters_delivered_on_time() const { return letters_delivered_on_time; }
    std::size_t get_letters_delivered_late() const { return letters_delivered_late; }
    const GlobalStats& get_stats() const { return stats; }
    const std::vector<DeliveredLetter>& get_recent_deliveries() const { return recent_deliveries; }
    // Hands over (and clears) the payouts and charges recorded since the last call.
    std::vector<MoneyEvent> take_money_events() { return std::exchange(money_events, {}); }
    const std::vector<PostOffice>& get_post_offices() const { return post_offices; }
    // The vector is const but the handles point at mutable Trucks; the
    // presentation layer only ever holds them as pointers-to-const.
    const std::vector<std::shared_ptr<Truck>>& get_trucks() const { return trucks; }
};
