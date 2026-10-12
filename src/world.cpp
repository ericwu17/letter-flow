// world.cpp — simulation rules. Pure logic, no rendering, no input.
#include "world.h"
#include "balance.h"
#include "entities.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

namespace {

// First departure tick strictly after `now` for a schedule anchored at
// `start_offset` with departures every `period` ticks. The departure grid is
// start_offset + k * period for k = 0, 1, 2, ... — e.g. "every 3 hours
// starting at 00:30" fires at 00:30, 03:30, 06:30, ... and keeps that phase
// across day boundaries.
Tick first_departure_after(Tick start_offset, Tick period, Tick now) {
    if (now < start_offset)
        return start_offset;
    return start_offset + ((now - start_offset) / period + 1) * period;
}

}  // namespace

bool World::operator==(const World& other) const {
    return post_offices == other.post_offices
        && std::equal(trucks.begin(), trucks.end(), other.trucks.begin(), other.trucks.end(),
                      [](const std::shared_ptr<Truck>& a, const std::shared_ptr<Truck>& b) {
                          return *a == *b;  // the trucks themselves, not their addresses
                      })
        && roads == other.roads
        && current_tick == other.current_tick
        && paused == other.paused
        && speed_multiplier == other.speed_multiplier
        && money == other.money
        && letters_delivered_on_time == other.letters_delivered_on_time
        && letters_delivered_late == other.letters_delivered_late
        && rng == other.rng;
}

void World::advance_tick() {
    current_tick += 1;

    // 1. Letters trickle in: on every tick each office spawns a single letter
    //    with probability letters_per_day / kTicksPerDay, so the expected
    //    number of letters per office per day is still letters_per_day.
    for (PostOfficeId id = 0; id < post_offices.size(); ++id)
        maybe_spawn_letter(id);

    // 2. Departures: fire every schedule that has come due. A single pass is
    //    safe: spawn_truck() only touches the office's letters buffer and the
    //    trucks vector — never the schedules vector being iterated here — and
    //    post_offices itself does not grow during a tick.
    //    Schedules fire in list order (the order shown in the inspector) and
    //    spawn_truck() MOVES picked-up letters out of the buffer, so when
    //    several schedules of one office depart on the same tick and their
    //    rules both match a letter, the first schedule in the list wins it.
    for (PostOfficeId id = 0; id < post_offices.size(); ++id) {
        for (TruckSchedule& schedule : post_offices[id].outbound_schedules) {
            if (schedule.period == 0 || current_tick < schedule.next_departure)
                continue;
            spawn_truck(id, schedule);  // at most one truck per schedule per tick...
            while (current_tick >= schedule.next_departure)
                schedule.next_departure += schedule.period;  // ...missed departures are skipped
        }
    }

    // 3. Arrivals: deliver cargo and retire the truck (swap-and-pop removal).
    for (std::size_t i = 0; i < trucks.size();) {
        if (trucks[i]->has_arrived(current_tick)) {
            deliver(*trucks[i]);
            trucks[i] = std::move(trucks.back());
            trucks.pop_back();
        } else {
            ++i;
        }
    }
}

void World::maybe_spawn_letter(PostOfficeId office_id) {
    const PostOffice& office = post_offices[office_id];
    if (!office.built)
        return;

    // Exact integer roll: a uniform value in [0, kTicksPerDay) lands below
    // letters_per_day with probability letters_per_day / kTicksPerDay.
    // Over a full day that is kTicksPerDay rolls, so the expected number of
    // letters is kTicksPerDay * (letters_per_day / kTicksPerDay) = letters_per_day.
    std::uniform_int_distribution<Tick> roll(0, kTicksPerDay - 1);
    if (roll(rng) < office.letters_per_day)
        spawn_letter(office_id);
}

void World::spawn_letter(PostOfficeId office_id) {
    PostOffice& office = post_offices[office_id];
    if (!office.built)
        return;

    // Letters are only addressed to other built offices; with none, there is
    // nowhere to send mail (this also keeps the dst loop below terminating).
    const bool has_built_dst = std::any_of(
        post_offices.begin(), post_offices.end(), [&](const PostOffice& other) {
            return other.built && &other != &office;
        });
    if (!has_built_dst)
        return;

    if (office.outbound_letters.size() >= office.max_outbound_letters)
        return;  // buffer full: this letter is lost

    std::uniform_int_distribution<Tick> pick_deadline(kLetterDeadlineMin, kLetterDeadlineMax);
    std::uniform_int_distribution<int> pick_value(kLetterValueMin, kLetterValueMax);
    std::uniform_int_distribution<int> pick_bonus(kLetterBonusMin, kLetterBonusMax);

    // Destination odds: see kDestinationDistanceScale in balance.h.
    std::vector<PostOfficeId> candidates;
    std::vector<double> weights;
    for (PostOfficeId id = 0; id < post_offices.size(); ++id) {
        const PostOffice& other = post_offices[id];
        if (id == office_id || !other.built)
            continue;
        const double dx = static_cast<double>(other.pos.x - office.pos.x);
        const double dy = static_cast<double>(other.pos.y - office.pos.y);
        candidates.push_back(id);
        weights.push_back((static_cast<double>(other.letters_per_day) + 1.0) / (1.0 + std::hypot(dx, dy) / kDestinationDistanceScale));
    }
    const PostOfficeId dst = candidates[std::discrete_distribution<std::size_t>(
        weights.begin(), weights.end())(rng)];

    Letter letter;
    letter.src = office_id;
    letter.dst = dst;
    letter.created = current_tick;
    letter.deadline = current_tick + pick_deadline(rng);
    letter.value = pick_value(rng);
    letter.max_bonus = pick_bonus(rng);
    office.outbound_letters.push_back(letter);
}

void World::spawn_truck(PostOfficeId src_id, const TruckSchedule& schedule) {
    PostOffice& src = post_offices[src_id];

    // Every departure is paid for when the truck leaves; one the player
    // cannot afford simply does not happen.
    if (money < kTruckDepartureCost)
        return;
    money -= kTruckDepartureCost;
    money_events.push_back({src_id, -kTruckDepartureCost});

    // The schedule's routing rule decides which of the office's letters board
    // the truck: the exact destination, every letter addressed to a postal
    // code prefix, or the whole buffer. Letters board oldest first, up to
    // kTruckCapacity; matching letters past that wait for a later departure.
    std::shared_ptr<Truck> truck = std::make_shared<Truck>(
        src_id,
        schedule.dst,
        get_road_vertices_from_a_to_b(src_id, schedule.dst),
        current_tick,
        kTruckSpeed
    );

    std::vector<Letter>& outbound = src.outbound_letters;
    std::size_t keep = 0;
    for (std::size_t i = 0; i < outbound.size(); ++i) {
        const PostOffice& letter_dst = post_offices[outbound[i].dst];
        if (truck->get_num_letters() < kTruckCapacity
            && schedule.rule.matches(outbound[i].dst, letter_dst.postal_code, schedule.dst))
            truck->push_back_letter(std::move(outbound[i]));
        else
            outbound[keep++] = std::move(outbound[i]);
    }
    outbound.resize(keep);

    trucks.push_back(std::move(truck));
}

std::vector<Position> World::get_road_vertices_from_a_to_b(PostOfficeId src, PostOfficeId dst) const {
    const Road* r = find_road(src, dst);
    if (r == nullptr) {
        return std::vector<Position>{};
    }
    std::vector<Position> result = r->vertices;
    if (src > dst) {
        std::reverse(result.begin(), result.end());
    }
    return result;
}

void World::deliver(Truck& truck) {
    const PostOfficeId arrived_at = truck.get_dst();
    PostOffice& office = post_offices[arrived_at];

    int earned = 0;
    bool delivered_any = false;
    for (Letter& letter : truck.take_letters()) {
        if (letter.dst == arrived_at) {
            delivered_any = true;
            const int before = money;
            // Final delivery: the base value is always paid; arriving before
            // the deadline adds a bonus proportional to the time left.
            money += letter.value;
            if (current_tick <= letter.deadline) {
                const Tick window = letter.deadline - letter.created;
                if (window > 0)
                    money += static_cast<int>(static_cast<Tick>(letter.max_bonus) * (letter.deadline - current_tick) / window);
                letters_delivered_on_time += 1;
            } else {
                letters_delivered_late += 1;
            }
            earned += money - before;
        } else {
            // Multi-hop forwarding: the letter is addressed somewhere else, so
            // it joins this office's outbound buffer, where later schedules
            // can pick it up (hub-and-spoke routing). Forwarding bypasses
            // max_outbound_letters — the cap only gates spawning — so no
            // letter is ever lost in transit; an over-cap buffer simply stops
            // new spawns until it drains below the cap again.
            office.outbound_letters.push_back(std::move(letter));
        }
    }
    if (delivered_any)
        money_events.push_back({arrived_at, earned});
}

bool World::add_schedule(PostOfficeId src, PostOfficeId dst, Tick period, Tick start_offset,
                         const RoutingRule& rule) {
    if (src >= post_offices.size() || dst >= post_offices.size() || src == dst || period == 0)
        return false;
    if (rule.type == RoutingRuleType::PostalPrefix && rule.prefix.empty())
        return false;  // an empty prefix must be an explicit AllLetters rule

    // Road constraint: a schedule requires a road connecting the two offices.
    if (!has_road(src, dst))
        return false;

    // Normalize: only PostalPrefix rules carry a prefix, so equality (and the
    // duplicate check below) compares like with like.
    RoutingRule normalized_rule = rule;
    if (normalized_rule.type != RoutingRuleType::PostalPrefix)
        normalized_rule.prefix.clear();

    for (const TruckSchedule& existing : post_offices[src].outbound_schedules) {
        if (existing.dst == dst && existing.rule == normalized_rule)
            return false;  // identical schedule already exists
    }

    TruckSchedule schedule;
    schedule.dst = dst;
    schedule.rule = std::move(normalized_rule);
    schedule.period = period;
    schedule.start_offset = start_offset;
    schedule.next_departure = first_departure_after(start_offset, period, current_tick);
    post_offices[src].outbound_schedules.push_back(std::move(schedule));
    return true;
}

void World::remove_schedule(PostOfficeId src, std::size_t schedule_index) {
    if (src >= post_offices.size())
        return;
    std::vector<TruckSchedule>& schedules = post_offices[src].outbound_schedules;
    if (schedule_index < schedules.size())
        schedules.erase(schedules.begin() + static_cast<std::ptrdiff_t>(schedule_index));
}

void World::move_schedule(PostOfficeId src, std::size_t from, std::size_t to) {
    if (src >= post_offices.size())
        return;
    std::vector<TruckSchedule>& schedules = post_offices[src].outbound_schedules;
    if (from >= schedules.size() || to >= schedules.size() || from == to)
        return;
    // "Drag row `from` onto row `to`": the dragged schedule ends up at index
    // `to` and the rows in between shift by one, the usual list-reorder feel.
    TruckSchedule moved = std::move(schedules[from]);
    schedules.erase(schedules.begin() + static_cast<std::ptrdiff_t>(from));
    schedules.insert(schedules.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
}

void World::set_postal_code(PostOfficeId office, std::string postal_code) {
    if (office < post_offices.size())
        post_offices[office].postal_code = std::move(postal_code);
}

PostOfficeId World::add_office(std::string name, std::string postal_code, Position pos,
                               std::size_t letters_per_day,
                               std::size_t max_outbound_letters,
                               int build_cost) {
    PostOffice office;
    office.name = std::move(name);
    office.postal_code = std::move(postal_code);
    office.pos = pos;
    office.letters_per_day = letters_per_day;
    office.max_outbound_letters = max_outbound_letters;
    office.build_cost = build_cost;
    const PostOfficeId id = post_offices.size();
    post_offices.push_back(std::move(office));
    return id;
}

bool World::build_office(PostOfficeId office) {
    if (office >= post_offices.size() || post_offices[office].built)
        return false;
    const int cost = post_offices[office].build_cost;
    if (money < cost)
        return false;
    money -= cost;
    post_offices[office].built = true;
    return true;
}

void World::seed_letters() {
    for (PostOfficeId id = 0; id < post_offices.size(); ++id)
        for (std::size_t i = 0; i < post_offices[id].letters_per_day; ++i)
            spawn_letter(id);
}

// ---------------------------------------------------------------------------
// Road API
// ---------------------------------------------------------------------------

bool World::has_road(PostOfficeId a, PostOfficeId b) const {
    return find_road(a, b) != nullptr;
}

const Road* World::find_road(PostOfficeId a, PostOfficeId b) const {
    if (a == b || a >= post_offices.size() || b >= post_offices.size())
        return nullptr;
    if (a > b) {
        std::swap(a, b);
    }
    for (const Road& road : roads) {
        if (road.src == a && road.dst == b) {
            return &road;
        }
    }
    return nullptr;
}

bool World::add_road(PostOfficeId src, PostOfficeId dst, std::vector<Position> vertices) {
    // Validate office indices.
    if (src >= post_offices.size() || dst >= post_offices.size() || src == dst)
        return false;
    if (!post_offices[src].built || !post_offices[dst].built)
        return false;
    if (vertices.size() < 2)
        return false;

    // The geometry checks below are exact only within the coordinate bound.
    if (!all_vertices_in_range(vertices))
        return false;

    // Normalize so src < dst (roads are undirected).
    if (src > dst) {
        std::swap(src, dst);
        std::reverse(vertices.begin(), vertices.end());
    }

    // Reject duplicates roads between same src, dst
    if (has_road(src, dst))
        return false;

    // Vertices must start and end at the office positions.
    const Position src_pos = post_offices[src].pos;
    const Position dst_pos = post_offices[dst].pos;
    
    if (vertices.front() != src_pos || vertices.back() != dst_pos) {
        return false;
    }

    // Every segment must be non-empty and horizontal, vertical, or 45° diagonal.
    if (!all_segments_valid_direction(vertices))
        return false;

    // No self-intersection (non-consecutive segment crossings).
    if (polyline_self_intersects(vertices))
        return false;

    // Must not cross any existing road.
    for (const Road& existing : roads) {
        if (polyline_crosses_road(existing, vertices))
            return false;
    }

    // Build the road object and deduct the cost.
    Road road;
    road.src = src;
    road.dst = dst;
    road.vertices = std::move(vertices);
    const int cost = road_cost(road);
    if (money < cost)
        return false;
    money -= cost;
    roads.push_back(std::move(road));
    return true;
}

bool World::remove_road(PostOfficeId a, PostOfficeId b) {
    // Validate office indices.
    if (a == b || a >= post_offices.size() || b >= post_offices.size())
        return false;

    // Normalize so a < b (roads are undirected).
    if (a > b) {
        std::swap(a, b);
    }

    const auto it = std::find_if(roads.begin(), roads.end(), [&](const Road& road) {
        return road.src == a && road.dst == b;
    });
    if (it == roads.end())
        return false;

    roads.erase(it);

    // Cascade: drop every schedule that depends on this road, on both endpoint
    // offices — spawn_truck would otherwise build a truck from an empty vertex
    // list on the next departure. No refund, for the road or the schedules.
    std::erase_if(post_offices[a].outbound_schedules,
                  [b](const TruckSchedule& s) { return s.dst == b; });
    std::erase_if(post_offices[b].outbound_schedules,
                  [a](const TruckSchedule& s) { return s.dst == a; });
    return true;
}

bool World::polyline_intersects_any(const std::vector<Position>& vertices) const {
    for (const Road& existing : roads) {
        if (polyline_crosses_road(existing, vertices))
            return true;
    }
    return false;
}
