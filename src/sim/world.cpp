// world.cpp — simulation rules. Pure logic, no rendering, no input.
#include "sim/world.h"
#include "sim/balance.h"
#include "sim/entities.h"
#include "sim/events.h"
#include "sim/office_names.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

// First departure tick strictly after `now` for a schedule that departs at
// time of day `start_offset` and then every `period` ticks until midnight.
// The grid restarts every day: departures fall on
// day_start + start_offset + k * period for k = 0, 1, 2, ... while that stays
// within the day. e.g. "every 5 hours starting at 00:30" fires at 00:30,
// 05:30, 10:30, 15:30, 20:30, then 00:30 again the next day.
Tick first_departure_after(Tick start_offset, Tick period, Tick now) {
    const Tick time_of_day = now % kTicksPerDay;
    const Tick day_start = now - time_of_day;
    if (time_of_day < start_offset)
        return day_start + start_offset;
    const Tick next = start_offset + ((time_of_day - start_offset) / period + 1) * period;
    if (next < kTicksPerDay)
        return day_start + next;
    return day_start + kTicksPerDay + start_offset;  // first departure after is the first departure of the next day.
}

// Only PostalPrefix rules carry prefixes, so equality (e.g. update_schedule's
// "did the rule change" check) compares like with like.
RoutingRule normalized(RoutingRule rule) {
    if (rule.type != RoutingRuleType::PostalPrefix)
        rule.prefixes.clear();
    return rule;
}

// Appends to a bounded log, dropping the oldest entry once it is full.
template <typename T>
void push_bounded(std::vector<T>& log, T entry, std::size_t max_size) {
    log.push_back(std::move(entry));
    if (log.size() > max_size)
        log.erase(log.begin(), log.begin() + static_cast<std::ptrdiff_t>(log.size() - max_size));
}

// Tick durations and counts as stat amounts (well within int64 range).
template <typename T>
int64_t as_stat(T amount) { return static_cast<int64_t>(amount); }

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
        && stats == other.stats
        && recent_deliveries == other.recent_deliveries
        && next_schedule_id == other.next_schedule_id
        && next_office_spawn == other.next_office_spawn
        && game_over_office == other.game_over_office
        && event == other.event
        && next_event_announce == other.next_event_announce
        && events_so_far == other.events_so_far
        && rng == other.rng;
}

void World::advance_tick() {
    if (is_game_over())
        return;
    current_tick += 1;

    // 0. Now and then a new office opens somewhere on the map.
    if (current_tick >= next_office_spawn) {
        spawn_random_office();
        next_office_spawn = current_tick + office_spawn_interval();
    }

    // 1. Network events: announced ahead, then a day of extra mail.
    update_event();

    // 2. Letters trickle in: on every tick each office spawns a single letter
    //    with probability letters_per_day / kTicksPerDay, so the expected
    //    number of letters per office per day is still letters_per_day.
    for (PostOfficeId id = 0; id < post_offices.size(); ++id)
        maybe_spawn_letter(id);

    // 3. Departures: fire every schedule that has come due.
    //    Schedules fire in list order (the order shown in the inspector) and
    //    spawn_truck() MOVES picked-up letters out of the buffer, so when
    //    several schedules of one office depart on the same tick and their
    //    rules both match a letter, the first schedule in the list wins it.
    for (PostOfficeId id = 0; id < post_offices.size(); ++id) {
        for (TruckSchedule& schedule : post_offices[id].outbound_schedules) {
            if (schedule.period != 0 && current_tick >= schedule.next_departure) {
                spawn_truck(id, schedule);
                schedule.next_departure = first_departure_after(schedule.start_offset, schedule.period, current_tick);
            }
        }
    }

    // 4. Arrivals: deliver cargo and retire the truck (swap-and-pop removal).
    for (std::size_t i = 0; i < trucks.size();) {
        if (trucks[i]->has_arrived(current_tick)) {
            deliver(*trucks[i]);
            trucks[i] = std::move(trucks.back());
            trucks.pop_back();
        } else {
            ++i;
        }
    }

    // 5. Overflow: a full office runs down its timer; one that runs out ends
    //    the game.
    update_overflow();

    // 6. Statistics: flows were recorded as things happened above; gauges
    //    are sampled once per tick, after everything has moved.
    sample_gauges();
}

void World::update_overflow() {
    for (PostOfficeId id = 0; id < post_offices.size(); ++id) {
        PostOffice& office = post_offices[id];
        // At or over the cap counts as full: forwarded letters can push a
        // buffer past it (see deliver()).
        if (office.outbound_letters.size() >= office.max_outbound_letters)
            office.overflow_ticks += 1;
        else if (office.overflow_ticks > 0)
            office.overflow_ticks -= 1;
        if (office.overflow_ticks >= kOverflowLimit && !is_game_over())
            game_over_office = id;  // the first (lowest id) to run out on this tick
    }
}

void World::update_event() {
    // An event retires at its end, and the next is due a period after its
    // start (announced kEventWarning ahead).
    if (event && current_tick >= event->end) {
        next_event_announce = event->start + kEventPeriod - kEventWarning;
        events_so_far += 1;
        event.reset();
    }
    // Events switch on the first time the map is big enough. Offices are
    // never removed, so this happens once per game.
    if (next_event_announce == kNever && post_offices.size() >= kEventMinOffices)
        next_event_announce = current_tick + kFirstEventDelay;
    if (!event && current_tick >= next_event_announce)
        announce_event();
    if (event && current_tick >= event->start)
        post_event_letters();
}

void World::announce_event() {
    if (post_offices.size() < 2) {
        // Nobody to write to (only possible in hand-built worlds): try again
        // a period later.
        next_event_announce = current_tick + kEventPeriod;
        return;
    }

    NetworkEvent e;
    e.type = std::bernoulli_distribution(0.5)(rng) ? EventType::Surge : EventType::Corridor;

    // Bigger offices are likelier to host a surge (the +1 keeps an office
    // that posts nothing in the running).
    std::vector<double> sizes;
    for (const PostOffice& office : post_offices)
        sizes.push_back(static_cast<double>(office.letters_per_day) + 1.0);
    if (e.type == EventType::Surge) {
        e.dst = std::discrete_distribution<PostOfficeId>(sizes.begin(), sizes.end())(rng);
    } else {
        e.src = std::uniform_int_distribution<PostOfficeId>(0, post_offices.size() - 1)(rng);
        std::vector<PostOfficeId> candidates;
        std::vector<double> weights;
        destination_weights(e.src, candidates, weights);
        e.dst = candidates[std::discrete_distribution<std::size_t>(weights.begin(), weights.end())(rng)];
    }

    // Size: a share of what the whole network posts in a day, growing a
    // little with every event.
    double network_mail = 0.0;
    for (const PostOffice& office : post_offices)
        network_mail += static_cast<double>(office.letters_per_day);
    const double share = e.type == EventType::Surge ? kSurgeShare : kCorridorShare;
    e.letters = static_cast<std::size_t>(
        std::llround(share * network_mail * (1.0 + kEventGrowth * static_cast<double>(events_so_far))));

    e.name = std::uniform_int_distribution<std::size_t>(0, event_name_count(e.type) - 1)(rng);
    e.start = current_tick + kEventWarning;
    e.end = e.start + kEventDuration;
    event = e;
}

void World::post_event_letters() {
    NetworkEvent& e = *event;
    // Evenly over the event: by the end of tick t, letters * (t - start + 1)
    // / duration of them are out, all of them on the last tick.
    const Tick duration = e.end - e.start;
    const Tick elapsed = std::min(current_tick - e.start + 1, duration);
    const auto due = static_cast<std::size_t>(static_cast<Tick>(e.letters) * elapsed / duration);
    if (e.posted >= due)
        return;

    // A surge's letters come from every other office, in proportion to size.
    std::vector<double> sizes;
    if (e.type == EventType::Surge) {
        for (PostOfficeId id = 0; id < post_offices.size(); ++id)
            sizes.push_back(id == e.dst ? 0.0 : static_cast<double>(post_offices[id].letters_per_day) + 1.0);
    }
    for (; e.posted < due; ++e.posted) {
        const PostOfficeId src = e.type == EventType::Corridor
            ? e.src
            : std::discrete_distribution<PostOfficeId>(sizes.begin(), sizes.end())(rng);
        spawn_letter(src, e.dst, /*event=*/true);
    }
}

Tick World::office_spawn_interval() const {
    const std::size_t extra = post_offices.size() > kInitialOffices ? post_offices.size() - kInitialOffices : 0;
    const Tick shortened = static_cast<Tick>(extra) * kOfficeSpawnIntervalStep;
    if (shortened >= kOfficeSpawnIntervalStart - kOfficeSpawnIntervalMin)
        return kOfficeSpawnIntervalMin;
    return kOfficeSpawnIntervalStart - shortened;
}

std::optional<PostOfficeId> World::spawn_random_office() {
    // A name no office has yet.
    std::vector<const char*> free_names;
    for (const char* name : office_names()) {
        const bool taken = std::any_of(post_offices.begin(), post_offices.end(),
                                       [&](const PostOffice& o) { return o.name == std::string_view(name); });
        if (!taken)
            free_names.push_back(name);
    }
    if (free_names.empty())
        return std::nullopt;

    // A spot inside the margins, clear of every office and every road.
    // Coordinates are whole world units, like every other position.
    std::uniform_int_distribution<int64_t> pick_x(static_cast<int64_t>(kOfficeSpawnMargin),
                                                  static_cast<int64_t>(kWorldWidth - kOfficeSpawnMargin));
    std::uniform_int_distribution<int64_t> pick_y(static_cast<int64_t>(kOfficeSpawnMargin),
                                                  static_cast<int64_t>(kWorldHeight - kOfficeSpawnMargin));
    std::optional<Position> spot;
    for (int attempt = 0; attempt < kOfficeSpawnAttempts && !spot; ++attempt) {
        const Position candidate{pick_x(rng), pick_y(rng)};
        const bool crowded = std::any_of(post_offices.begin(), post_offices.end(), [&](const PostOffice& o) {
            return euclidean_length(o.pos - candidate) < kOfficeMinSpacing;
        });
        const bool on_road = std::any_of(roads.begin(), roads.end(), [&](const Road& r) {
            return distance_to_polyline(candidate, r.vertices) < kOfficeRoadClearance;
        });
        if (!crowded && !on_road)
            spot = candidate;
    }
    if (!spot)
        return std::nullopt;

    std::vector<double> weights;
    for (const OfficeClass& office_class : kOfficeClasses)
        weights.push_back(office_class.spawn_weight);
    const OfficeClass& office_class =
        kOfficeClasses[std::discrete_distribution<std::size_t>(weights.begin(), weights.end())(rng)];
    const char* name = free_names[std::uniform_int_distribution<std::size_t>(0, free_names.size() - 1)(rng)];

    return add_office(name, "", *spot, office_class.letters_per_day, office_class.max_outbound_letters);
}

void World::sample_gauges() {
    std::size_t waiting = 0;
    for (PostOffice& office : post_offices) {
        office.stats[OfficeStat::Backlog].sample(current_tick, as_stat(office.outbound_letters.size()));
        waiting += office.outbound_letters.size();
    }
    std::size_t in_transit = 0;
    for (const std::shared_ptr<Truck>& truck : trucks)
        in_transit += truck->get_num_letters();
    stats[GlobalStat::Backlog].sample(current_tick, as_stat(waiting));
    stats[GlobalStat::InTransit].sample(current_tick, as_stat(in_transit));
    stats[GlobalStat::Money].sample(current_tick, std::max(money, 0));
}

TruckSchedule* World::find_schedule(PostOfficeId office, ScheduleId id) {
    if (office >= post_offices.size() || id == kNoSchedule)
        return nullptr;
    for (TruckSchedule& schedule : post_offices[office].outbound_schedules)
        if (schedule.id == id)
            return &schedule;
    return nullptr;
}

void World::maybe_spawn_letter(PostOfficeId office_id) {
    const PostOffice& office = post_offices[office_id];

    // Exact integer roll: a uniform value in [0, kTicksPerDay) lands below
    // letters_per_day with probability letters_per_day / kTicksPerDay.
    // Over a full day that is kTicksPerDay rolls, so the expected number of
    // letters is kTicksPerDay * (letters_per_day / kTicksPerDay) = letters_per_day.
    std::uniform_int_distribution<Tick> roll(0, kTicksPerDay - 1);
    if (roll(rng) < office.letters_per_day)
        spawn_letter(office_id);
}

void World::destination_weights(PostOfficeId src, std::vector<PostOfficeId>& candidates,
                                std::vector<double>& weights) const {
    // Destination odds: see kDestinationDistanceScale in balance.h.
    const PostOffice& office = post_offices[src];
    candidates.clear();
    weights.clear();
    for (PostOfficeId id = 0; id < post_offices.size(); ++id) {
        const PostOffice& other = post_offices[id];
        if (id == src)
            continue;
        const double dx = static_cast<double>(other.pos.x - office.pos.x);
        const double dy = static_cast<double>(other.pos.y - office.pos.y);
        candidates.push_back(id);
        weights.push_back((static_cast<double>(other.letters_per_day) + 1.0) / (1.0 + std::hypot(dx, dy) / kDestinationDistanceScale));
    }
}

void World::spawn_letter(PostOfficeId office_id, std::optional<PostOfficeId> forced_dst, bool event_letter) {
    PostOffice& office = post_offices[office_id];

    // Letters are only addressed to other offices; with none, there is
    // nowhere to send mail (this also keeps the dst loop below non-empty).
    if (post_offices.size() < 2)
        return;

    if (office.outbound_letters.size() >= office.max_outbound_letters) {
        // Buffer full: this letter is lost.
        stats[GlobalStat::Lost].add(current_tick, 1);
        office.stats[OfficeStat::Lost].add(current_tick, 1);
        return;
    }

    std::uniform_int_distribution<Tick> pick_deadline(kLetterDeadlineMin, kLetterDeadlineMax);
    std::uniform_int_distribution<int> pick_value(kLetterValueMin, kLetterValueMax);
    std::uniform_int_distribution<int> pick_bonus(kLetterBonusMin, kLetterBonusMax);

    PostOfficeId dst = kNoPostOffice;
    if (forced_dst) {
        dst = *forced_dst;
    } else {
        std::vector<PostOfficeId> candidates;
        std::vector<double> weights;
        destination_weights(office_id, candidates, weights);
        dst = candidates[std::discrete_distribution<std::size_t>(weights.begin(), weights.end())(rng)];
    }

    Letter letter;
    letter.src = office_id;
    letter.dst = dst;
    letter.created = current_tick;
    letter.deadline = current_tick + pick_deadline(rng);
    letter.value = pick_value(rng);
    letter.max_bonus = pick_bonus(rng);
    letter.event = event_letter;
    letter.trail.push_back({current_tick, TrailEvent::Spawned, office_id, kNoSchedule});
    office.outbound_letters.push_back(std::move(letter));
    stats[GlobalStat::Spawned].add(current_tick, 1);
    office.stats[OfficeStat::Spawned].add(current_tick, 1);
    if (event_letter) {
        stats[GlobalStat::EventSpawned].add(current_tick, 1);
        office.stats[OfficeStat::EventSpawned].add(current_tick, 1);
    }
}

void World::spawn_truck(PostOfficeId src_id, TruckSchedule& schedule) {
    PostOffice& src = post_offices[src_id];

    // Every departure is paid for when the truck leaves; one the player
    // cannot afford simply does not happen.
    if (money < kTruckDepartureCost)
        return;
    money -= kTruckDepartureCost;
    money_events.push_back({src_id, -kTruckDepartureCost});
    stats[GlobalStat::SpentTrucks].add(current_tick, kTruckDepartureCost);
    src.stats[OfficeStat::Departures].add(current_tick, 1);
    schedule.stats[ScheduleStat::Departures].add(current_tick, 1);

    // The schedule's routing rule decides which of the office's letters board
    // the truck: the exact destination, every letter addressed to a postal
    // code prefix, or the whole buffer. Letters board oldest first, up to
    // kTruckCapacity; matching letters past that wait for a later departure.
    std::shared_ptr<Truck> truck = std::make_shared<Truck>(
        src_id,
        schedule.dst,
        road_path(src_id, schedule.dst),
        current_tick,
        kTruckSpeed,
        schedule.id
    );

    std::vector<Letter>& outbound = src.outbound_letters;
    std::size_t keep = 0;
    std::uint32_t left_behind = 0;  // matching letters the truck had no room for
    for (std::size_t i = 0; i < outbound.size(); ++i) {
        Letter& letter = outbound[i];
        const bool matches = schedule.rule.matches(letter.dst, post_offices[letter.dst].postal_code, schedule.dst);
        if (matches && truck->get_num_letters() < kTruckCapacity) {
            src.stats[OfficeStat::Loaded].add(current_tick, 1);
            letter.trail.push_back({current_tick, TrailEvent::Departed, src_id, schedule.id});
            truck->push_back_letter(std::move(letter));
            continue;
        }
        if (matches)
            ++left_behind;
        // Letters ahead of the first one to board are already in place
        // (keep == i). Moving one onto itself would empty its trail: vector
        // self-move-assignment leaves the vector empty (libc++ does).
        if (keep != i)
            outbound[keep] = std::move(letter);
        ++keep;
    }
    outbound.resize(keep);

    const auto loaded = static_cast<std::uint32_t>(truck->get_num_letters());
    schedule.stats[ScheduleStat::Loaded].add(current_tick, loaded);
    schedule.stats[ScheduleStat::LeftBehind].add(current_tick, left_behind);
    push_bounded(schedule.departure_log, DepartureRecord{current_tick, loaded, left_behind},
                 kDepartureLogSize);

    trucks.push_back(std::move(truck));
}

std::vector<Position> World::road_path(PostOfficeId src, PostOfficeId dst) const {
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
        letter.trail.push_back({current_tick, TrailEvent::Arrived, arrived_at, kNoSchedule});
        if (letter.dst == arrived_at) {
            delivered_any = true;
            // Final delivery: the base value is always paid; arriving before
            // the deadline adds a bonus proportional to the time left.
            int bonus = 0;
            if (current_tick <= letter.deadline) {
                const Tick window = letter.deadline - letter.created;
                if (window > 0)
                    bonus = static_cast<int>(static_cast<Tick>(letter.max_bonus) * (letter.deadline - current_tick) / window);
                letters_delivered_on_time += 1;
                stats[GlobalStat::DeliveredOnTime].add(current_tick, 1);
            } else {
                letters_delivered_late += 1;
                stats[GlobalStat::DeliveredLate].add(current_tick, 1);
            }
            const int letter_earned = letter.value + bonus;
            money += letter_earned;
            earned += letter_earned;

            stats[GlobalStat::IncomeValue].add(current_tick, letter.value);
            stats[GlobalStat::IncomeBonus].add(current_tick, bonus);
            office.stats[OfficeStat::Delivered].add(current_tick, 1);
            office.stats[OfficeStat::Income].add(current_tick, letter_earned);
            push_bounded(recent_deliveries, DeliveredLetter{std::move(letter), current_tick, letter_earned},
                         kRecentDeliveriesSize);
        } else {
            office.stats[OfficeStat::Forwarded].add(current_tick, 1);
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

ScheduleError World::validate_schedule(PostOfficeId src, PostOfficeId dst, Tick period,
                                       const RoutingRule& normalized_rule) const {
    if (src >= post_offices.size() || dst >= post_offices.size())
        return ScheduleError::UnknownOffice;
    if (src == dst)
        return ScheduleError::SameOffice;
    if (period == 0)
        return ScheduleError::ZeroPeriod;
    if (!normalized_rule.is_well_formed())
        return ScheduleError::MalformedRule;  // e.g. an empty prefix must be an explicit AllLetters rule

    // Road constraint: a schedule requires a road connecting the two offices.
    if (!has_road(src, dst))
        return ScheduleError::NoRoad;
    return ScheduleError::None;
}

ScheduleError World::check_add_schedule(PostOfficeId src, PostOfficeId dst, Tick period,
                                        const RoutingRule& rule) const {
    return validate_schedule(src, dst, period, normalized(rule));
}

ScheduleError World::check_update_schedule(PostOfficeId src, std::size_t schedule_index, Tick period,
                                           const RoutingRule& rule) const {
    if (src >= post_offices.size() || schedule_index >= post_offices[src].outbound_schedules.size())
        return ScheduleError::UnknownOffice;
    const PostOfficeId dst = post_offices[src].outbound_schedules[schedule_index].dst;
    return validate_schedule(src, dst, period, normalized(rule));
}

bool World::add_schedule(PostOfficeId src, PostOfficeId dst, Tick period, Tick start_offset,
                         const RoutingRule& rule) {
    RoutingRule normalized_rule = normalized(rule);
    if (validate_schedule(src, dst, period, normalized_rule) != ScheduleError::None)
        return false;

    TruckSchedule schedule;
    schedule.id = next_schedule_id++;
    schedule.dst = dst;
    schedule.rule = std::move(normalized_rule);
    schedule.period = period;
    schedule.start_offset = start_offset;
    schedule.next_departure = first_departure_after(start_offset, period, current_tick);
    post_offices[src].outbound_schedules.push_back(std::move(schedule));
    return true;
}

bool World::update_schedule(PostOfficeId src, std::size_t schedule_index,
                            Tick period, Tick start_offset, const RoutingRule& rule) {
    if (check_update_schedule(src, schedule_index, period, rule) != ScheduleError::None)
        return false;
    TruckSchedule& schedule = post_offices[src].outbound_schedules[schedule_index];
    RoutingRule normalized_rule = normalized(rule);

    // Only a new departure grid moves the next departure; a rule edit leaves
    // the timing exactly as it was. A real change is logged for the
    // schedule's utilization chart; re-submitting the same values is not one.
    const bool timing_changed = period != schedule.period || start_offset != schedule.start_offset;
    if (timing_changed)
        schedule.next_departure = first_departure_after(start_offset, period, current_tick);
    if (timing_changed || normalized_rule != schedule.rule)
        push_bounded(schedule.edits, current_tick, kScheduleEditLogSize);
    schedule.rule = std::move(normalized_rule);
    schedule.period = period;
    schedule.start_offset = start_offset;
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
    if (polyline_intersects_any(vertices))
        return false;

    // Build the road object (roads are free).
    Road road;
    road.src = src;
    road.dst = dst;
    road.vertices = std::move(vertices);
    roads.push_back(std::move(road));
    return true;
}

bool World::remove_road(PostOfficeId a, PostOfficeId b) {
    // find_road validates the ids and handles either argument order.
    const Road* road = find_road(a, b);
    if (road == nullptr)
        return false;
    roads.erase(roads.begin() + (road - roads.data()));

    // Cascade: drop every schedule that depends on this road, on both endpoint
    // offices — spawn_truck would otherwise build a truck from an empty vertex
    // list on the next departure.
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
