// world.cpp — simulation rules. Pure logic, no rendering, no input.
#include "world.h"

#include <utility>

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

    // Basic routing rule: the truck takes every outbound letter addressed to
    // its destination. This is the extension point for fancier RoutingRules
    // (capacity limits, priority by deadline, multi-hop forwarding, ...).
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
        if (outbound[i].dst == schedule.dst)
            truck.push_back_letter(std::move(outbound[i]));
        else
            outbound[keep++] = std::move(outbound[i]);
    }
    outbound.resize(keep);

    if (truck.get_num_letters() > 0)  // no cargo, no truck
        trucks.push_back(std::move(truck));
}

void World::deliver(Truck& truck) {
    for (const Letter& letter : truck.get_letters()) {
        if (current_tick <= letter.deadline) {
            money += letter.value;
            letters_delivered_on_time += 1;
        } else {
            money -= letter.fine;
            letters_delivered_late += 1;
        }
    }
    truck.clear_letters();
}

void World::add_schedule(PostOfficeId src, PostOfficeId dst, Tick period) {
    if (src >= post_offices.size() || dst >= post_offices.size() || src == dst || period == 0)
        return;
    for (const TruckSchedule& existing : post_offices[src].outbound_schedules) {
        if (existing.dst == dst)
            return;  // route already exists
    }
    TruckSchedule schedule;
    schedule.dst = dst;
    schedule.period = period;
    schedule.next_departure = current_tick + period;
    post_offices[src].outbound_schedules.push_back(schedule);
}

void World::remove_schedule(PostOfficeId src, std::size_t schedule_index) {
    if (src >= post_offices.size())
        return;
    std::vector<TruckSchedule>& schedules = post_offices[src].outbound_schedules;
    if (schedule_index < schedules.size())
        schedules.erase(schedules.begin() + static_cast<std::ptrdiff_t>(schedule_index));
}

PostOfficeId World::add_office(std::string name, Position pos,
                               std::size_t letters_per_day,
                               std::size_t max_outbound_letters) {
    PostOffice office;
    office.name = std::move(name);
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
