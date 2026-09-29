// world.cpp — simulation rules. Pure logic, no rendering, no input.
#include "world.h"

#include <sstream>
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

// std::mt19937 has no operator==; its standard-library text dump (the same
// one save files store) fully captures the engine state, so comparing the
// dumps compares the generators.
std::string rng_state_dump(const std::mt19937& rng) {
    std::ostringstream out;
    out << rng;
    return out.str();
}

}  // namespace

bool World::operator==(const World& other) const {
    return current_tick == other.current_tick
        && paused == other.paused
        && speed_multiplier == other.speed_multiplier
        && money == other.money
        && letters_delivered_on_time == other.letters_delivered_on_time
        && letters_delivered_late == other.letters_delivered_late
        && post_offices == other.post_offices
        && trucks == other.trucks
        && rng_state_dump(rng) == rng_state_dump(other.rng);
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
        if (trucks[i].has_arrived(current_tick)) {
            deliver(trucks[i]);
            trucks[i] = std::move(trucks.back());
            trucks.pop_back();
        } else {
            ++i;
        }
    }
}

void World::maybe_spawn_letter(PostOfficeId office_id) {
    const PostOffice& office = post_offices[office_id];

    // Exact integer roll: a uniform value in [0, kTicksPerDay) lands below
    // letters_per_day with probability letters_per_day / kTicksPerDay.
    // Over a full day that is kTicksPerDay rolls, so the expected number of
    // letters is kTicksPerDay * (letters_per_day / kTicksPerDay) = letters_per_day,
    // matching the old day-boundary batch exactly.
    std::uniform_int_distribution<Tick> roll(0, kTicksPerDay - 1);
    if (roll(rng) < office.letters_per_day)
        spawn_letter(office_id);
}

void World::spawn_letter(PostOfficeId office_id) {
    if (post_offices.size() < 2)
        return;
    PostOffice& office = post_offices[office_id];

    if (office.outbound_letters.size() >= office.max_outbound_letters)
        return;  // buffer full: this letter is lost

    std::uniform_int_distribution<PostOfficeId> pick_office(0, post_offices.size() - 1);
    std::uniform_int_distribution<Tick> pick_deadline(kTicksPerDay / 2, 2 * kTicksPerDay);
    std::uniform_int_distribution<int> pick_value(5, 15);
    std::uniform_int_distribution<int> pick_fine(3, 10);

    PostOfficeId dst = office_id;
    while (dst == office_id)
        dst = pick_office(rng);
    Letter letter;
    letter.src = office_id;
    letter.dst = dst;
    letter.deadline = current_tick + pick_deadline(rng);
    letter.value = pick_value(rng);
    letter.fine = pick_fine(rng);
    office.outbound_letters.push_back(letter);
}

void World::spawn_truck(PostOfficeId src_id, const TruckSchedule& schedule) {
    PostOffice& src = post_offices[src_id];

    // The schedule's routing rule decides which of the office's letters board
    // the truck: the exact destination, every letter addressed to a postal
    // code prefix, or the whole buffer. This is the extension point for even
    // fancier RoutingRules (capacity limits, priority by deadline, ...).
    Truck truck(
        src_id,
        schedule.dst,
        src.pos,
        post_offices[schedule.dst].pos,
        current_tick,
        kTruckSpeed
    );

    std::vector<Letter>& outbound = src.outbound_letters;
    std::size_t keep = 0;
    for (std::size_t i = 0; i < outbound.size(); ++i) {
        const PostOffice& letter_dst = post_offices[outbound[i].dst];
        if (schedule.rule.matches(outbound[i].dst, letter_dst.postal_code, schedule.dst))
            truck.push_back_letter(std::move(outbound[i]));
        else
            outbound[keep++] = std::move(outbound[i]);
    }
    outbound.resize(keep);

    if (truck.get_num_letters() > 0)  // no cargo, no truck
        trucks.push_back(std::move(truck));
}

void World::deliver(Truck& truck) {
    const PostOfficeId arrived_at = truck.get_dst();
    PostOffice& office = post_offices[arrived_at];

    for (Letter& letter : truck.take_letters()) {
        if (letter.dst == arrived_at) {
            // Final delivery: score against the deadline.
            if (current_tick <= letter.deadline) {
                money += letter.value;
                letters_delivered_on_time += 1;
            } else {
                money -= letter.fine;
                letters_delivered_late += 1;
            }
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
}

bool World::add_schedule(PostOfficeId src, PostOfficeId dst, Tick period, Tick start_offset,
                         const RoutingRule& rule) {
    if (src >= post_offices.size() || dst >= post_offices.size() || src == dst || period == 0)
        return false;
    if (rule.type == RoutingRuleType::PostalPrefix && rule.prefix.empty())
        return false;  // an empty prefix must be an explicit AllLetters rule
    // Normalize: only PostalPrefix rules carry a prefix, so equality (and the
    // duplicate check below) compares like with like.
    RoutingRule normalized_rule = rule;
    if (normalized_rule.type != RoutingRuleType::PostalPrefix)
        normalized_rule.prefix.clear();

    for (const TruckSchedule& existing : post_offices[src].outbound_schedules) {
        if (existing.dst == dst && existing.rule == normalized_rule)
            return false;  // identical schedule already exists
    }

    // Higher-frequency schedules cost more; creating one is only allowed if
    // the player can pay the up-front price.
    const int cost = schedule_cost(period);
    if (money < cost)
        return false;
    money -= cost;

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
                               std::size_t max_outbound_letters) {
    PostOffice office;
    office.name = std::move(name);
    office.postal_code = std::move(postal_code);
    office.pos = pos;
    office.letters_per_day = letters_per_day;
    office.max_outbound_letters = max_outbound_letters;
    const PostOfficeId id = post_offices.size();
    post_offices.push_back(std::move(office));
    return id;
}

void World::seed_letters() {
    for (PostOfficeId id = 0; id < post_offices.size(); ++id)
        for (std::size_t i = 0; i < post_offices[id].letters_per_day; ++i)
            spawn_letter(id);
}
