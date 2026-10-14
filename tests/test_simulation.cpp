// test_simulation.cpp — headless tests for the simulation rules (World's
// public API): routing-rule matching, schedule validation, departure pricing
// and truck capacity, the departure grid, pick-up priority, and
// delivery/forwarding. Run with
// `make test`. Pure simulation layer: no SDL/imgui/OpenGL — no window needed.
//
// Everything here is deterministic (fixed RNG seed, fixed tick counts) and
// worlds are built to order through add_office()/seed_letters(), so the
// asserts hold on every run.
#include "sim/entities.h"
#include "sim/balance.h"
#include "sim/game_types.h"
#include "sim/truck.h"
#include "sim/world.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

// --- road helpers -----------------------------------------------------------
// Schedules require a road between their endpoints (add_schedule checks
// has_road), so every fixture world builds the roads its tests rely on.

// Straight road between two axis-aligned offices.
void add_straight_road(World& w, PostOfficeId a, PostOfficeId b) {
    assert(w.add_road(a, b, {w.get_post_offices()[a].pos,
                             w.get_post_offices()[b].pos}));
}

// A direct road between offices two hops apart would overlap the straight
// chain road between them (collinear overlap is rejected), so route it below
// the x axis with two 45-degree diagonals meeting at a single corner.
void add_underpass_road(World& w, PostOfficeId a, PostOfficeId b) {
    const Position pa = w.get_post_offices()[a].pos;
    const Position pb = w.get_post_offices()[b].pos;
    const Position mid{(pa.x + pb.x) / 2, pa.y - std::abs(pb.x - pa.x) / 2};
    assert(w.add_road(a, b, {pa, mid, pb}));
}

// A world of `count` offices in a row along the x axis, 100 world units
// apart — one hop takes exactly 50 ticks at kTruckSpeed. Consecutive offices
// are joined by straight roads. Only office 0 produces mail
// (`letters_at_source` seeded letters, all addressed to one of the other
// offices at random, plus that same rate trickling in per tick); every other
// office has letters_per_day = 0 and stays inert. Every office is built, for
// free (build_cost 0), so money only moves for roads and truck departures.
World make_line_world(std::size_t count, std::size_t letters_at_source) {
    World w;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string name = "O" + std::to_string(i);
        const std::string code = "T/" + std::to_string(i);
        const PostOfficeId id = w.add_office(name, code, {100 * static_cast<int64_t>(i), 0},
                                             i == 0 ? letters_at_source : 0,
                                             /*max_outbound_letters=*/1000, /*build_cost=*/0);
        assert(w.build_office(id));
    }
    w.seed_letters();
    for (std::size_t i = 1; i < count; ++i)
        add_straight_road(w, static_cast<PostOfficeId>(i - 1),
                          static_cast<PostOfficeId>(i));
    return w;
}

// --- the tests -------------------------------------------------------------

// RoutingRule::matches is pure logic: the rule types, prefix semantics, and
// the "empty prefix matches nothing" safety net.
void routing_rule_matches() {
    const RoutingRule exact{RoutingRuleType::ExactDestination, {}};
    assert(exact.matches(3, "T/1", 3));
    assert(!exact.matches(3, "T/1", 4));

    const RoutingRule all{RoutingRuleType::AllLetters, {}};
    assert(all.matches(0, "", 9));

    const RoutingRule prefix{RoutingRuleType::PostalPrefix, {"NJ/Mercer"}};
    assert(prefix.matches(5, "NJ/Mercer/Princeton", 9));  // the truck's own dst is irrelevant
    assert(prefix.matches(5, "NJ/Mercer", 9));
    assert(!prefix.matches(5, "NJ/Central/Monroe", 9));
    assert(!prefix.matches(5, "NJ", 9));  // shorter than the prefix

    // Several prefixes are OR-ed together.
    const RoutingRule multi{RoutingRuleType::PostalPrefix, {"NJ/Mercer", "PA/Bucks"}};
    assert(multi.matches(5, "NJ/Mercer/Princeton", 9));
    assert(multi.matches(5, "PA/Bucks/Doylestown", 9));
    assert(!multi.matches(5, "NY/Kings", 9));

    // A misconfigured empty prefix must match nothing, never everything —
    // whether the list is empty or one of its prefixes is.
    const RoutingRule empty_prefix{RoutingRuleType::PostalPrefix, {}};
    assert(!empty_prefix.matches(5, "NJ/Mercer", 9));
    const RoutingRule empty_element{RoutingRuleType::PostalPrefix, {""}};
    assert(!empty_element.matches(5, "NJ/Mercer", 9));
}

// add_schedule's rejection matrix, free schedule creation, and identical
// schedules being allowed (each its own schedule, with its own id).
void schedule_validation_and_pricing() {
    World w = make_line_world(3, 0);
    add_underpass_road(w, 0, 2);  // for the distinct-dst schedule below
    const int money0 = w.get_money();
    assert(money0 < kStartingMoney);  // the fixture roads were paid for

    // Invalid routes are rejected and cost nothing.
    const RoutingRule exact{RoutingRuleType::ExactDestination, {}};
    assert(!w.add_schedule(99, 1, kTicksPerHour, 0, exact));        // bad src
    assert(!w.add_schedule(0, 99, kTicksPerHour, 0, exact));        // bad dst
    assert(!w.add_schedule(0, 0, kTicksPerHour, 0, exact));         // self-route
    assert(!w.add_schedule(0, 1, 0, 0, exact));                     // zero period
    const RoutingRule empty_prefix{RoutingRuleType::PostalPrefix, {}};
    assert(!w.add_schedule(0, 1, kTicksPerHour, 0, empty_prefix));  // empty prefix
    const RoutingRule empty_element{RoutingRuleType::PostalPrefix, {"T/1", ""}};
    assert(!w.add_schedule(0, 1, kTicksPerHour, 0, empty_element));  // one empty prefix
    assert(w.get_money() == money0);

    // Creating a schedule is free: money only moves when trucks depart.
    assert(w.add_schedule(0, 1, 3 * kTicksPerHour, 0, exact));
    assert(w.get_money() == money0);

    // The same (dst, rule) again is just another schedule, with its own id;
    // a stale prefix on a non-prefix rule is normalized away.
    assert(w.add_schedule(0, 1, kTicksPerHour, 7, exact));
    const RoutingRule dirty_exact{RoutingRuleType::ExactDestination, {"junk"}};
    assert(w.add_schedule(0, 1, kTicksPerHour, 0, dirty_exact));
    {
        const std::vector<TruckSchedule>& schedules = w.get_post_offices()[0].outbound_schedules;
        assert(schedules.size() == 3);
        assert(schedules[2].rule == exact);
        assert(schedules[0].id != schedules[1].id && schedules[1].id != schedules[2].id &&
               schedules[0].id != schedules[2].id);
    }
    const RoutingRule all{RoutingRuleType::AllLetters, {}};
    assert(w.add_schedule(0, 1, kTicksPerHour, 0, all));
    assert(w.add_schedule(0, 2, kTicksPerHour, 0, exact));
    assert(w.get_money() == money0);


    // A prefix listed twice is refused rather than quietly dropped, so an
    // inspector edit never makes a field vanish while it is being typed in.
    const std::size_t count = w.get_post_offices()[0].outbound_schedules.size();
    assert(!w.add_schedule(0, 1, kTicksPerHour, 0, {RoutingRuleType::PostalPrefix, {"T/2", "T/1", "T/2"}}));
    assert(w.get_post_offices()[0].outbound_schedules.size() == count);
    assert(w.add_schedule(0, 1, kTicksPerHour, 0, {RoutingRuleType::PostalPrefix, {"T/2", "T/1"}}));
}

// update_schedule() edits a schedule's rule and timing in place: same
// rejection rules as add_schedule, destination and list position kept, and
// the next departure re-anchored only when the timing changes.
void schedule_editing() {
    World w = make_line_world(2, 0);
    const RoutingRule exact{RoutingRuleType::ExactDestination, {}};
    const RoutingRule all{RoutingRuleType::AllLetters, {}};
    const RoutingRule prefix{RoutingRuleType::PostalPrefix, {"T/"}};
    assert(w.add_schedule(0, 1, kTicksPerHour, 100, exact));
    assert(w.add_schedule(0, 1, kTicksPerHour, 200, all));
    const auto schedules = [&] { return w.get_post_offices()[0].outbound_schedules; };
    const std::vector<TruckSchedule> before = schedules();

    // Rejected edits change nothing.
    assert(!w.update_schedule(0, 9, kTicksPerHour, 100, exact));  // bad index
    assert(!w.update_schedule(9, 0, kTicksPerHour, 100, exact));  // bad src
    assert(!w.update_schedule(0, 0, 0, 100, exact));              // zero period
    assert(!w.update_schedule(0, 0, kTicksPerHour, 100,
                              {RoutingRuleType::PostalPrefix, {}}));  // empty prefix
    assert(schedules() == before);

    // Re-submitting a schedule's own values changes nothing (no edit logged).
    assert(w.update_schedule(0, 0, kTicksPerHour, 100, exact));
    assert(schedules() == before);

    // A rule edit keeps the destination, the timing and the list position.
    assert(w.update_schedule(0, 0, kTicksPerHour, 100, prefix));
    assert(schedules()[0].rule == prefix);
    assert(schedules()[0].dst == before[0].dst);
    assert(schedules()[0].next_departure == before[0].next_departure);
    assert(schedules()[1] == before[1]);
    assert(schedules()[0].edits == std::vector<Tick>{0});  // logged once, at tick 0
    // A stale prefix on a non-prefix rule is normalized away, as in
    // add_schedule, even when that makes the rule identical to #2's.
    assert(w.update_schedule(0, 0, kTicksPerHour, 100, {RoutingRuleType::AllLetters, {"junk"}}));
    assert(schedules()[0].rule == all);
    assert(w.update_schedule(0, 0, kTicksPerHour, 100, {RoutingRuleType::ExactDestination, {"junk"}}));
    assert(schedules()[0].rule == exact);

    // A new period or offset re-anchors the next departure on the new grid.
    while (w.get_tick() < 1000)
        w.advance_tick();
    assert(w.update_schedule(0, 0, 300, 50, exact));
    assert(schedules()[0].period == 300);
    assert(schedules()[0].start_offset == 50);
    assert(schedules()[0].next_departure == 1250);  // 50 + 4 * 300, first slot after 1000
}

// check_add_schedule() / check_update_schedule() name the reason the
// matching mutator refuses, and ScheduleError::None exactly when it accepts.
void schedule_errors() {
    World w = make_line_world(3, 0);  // roads 0-1 and 1-2, none 0-2
    const RoutingRule exact{RoutingRuleType::ExactDestination, {}};
    const RoutingRule all{RoutingRuleType::AllLetters, {}};
    const RoutingRule bad_prefix{RoutingRuleType::PostalPrefix, {"T/", ""}};
    const Tick h = kTicksPerHour;

    assert(w.check_add_schedule(99, 1, h, exact) == ScheduleError::UnknownOffice);
    assert(w.check_add_schedule(0, 99, h, exact) == ScheduleError::UnknownOffice);
    assert(w.check_add_schedule(0, 0, h, exact) == ScheduleError::SameOffice);
    assert(w.check_add_schedule(0, 1, 0, exact) == ScheduleError::ZeroPeriod);
    assert(w.check_add_schedule(0, 1, h, bad_prefix) == ScheduleError::MalformedRule);
    assert(w.check_add_schedule(0, 2, h, exact) == ScheduleError::NoRoad);
    assert(w.check_add_schedule(0, 1, h, exact) == ScheduleError::None);
    assert(w.add_schedule(0, 1, h, 0, exact));
    // An identical schedule is no reason to refuse.
    assert(w.check_add_schedule(0, 1, 2 * h, exact) == ScheduleError::None);
    assert(w.check_add_schedule(0, 1, h, {RoutingRuleType::ExactDestination, {"junk"}}) == ScheduleError::None);
    assert(w.add_schedule(0, 1, h, 0, all));

    assert(w.check_update_schedule(0, 9, h, exact) == ScheduleError::UnknownOffice);  // no such schedule
    assert(w.check_update_schedule(9, 0, h, exact) == ScheduleError::UnknownOffice);
    assert(w.check_update_schedule(0, 0, 0, exact) == ScheduleError::ZeroPeriod);
    assert(w.check_update_schedule(0, 0, h, bad_prefix) == ScheduleError::MalformedRule);
    assert(w.check_update_schedule(0, 0, h, all) == ScheduleError::None);  // == #2, allowed
    assert(w.check_update_schedule(0, 0, 3 * h, exact) == ScheduleError::None);
    assert(w.update_schedule(0, 0, h, 0, all));
    assert(w.update_schedule(0, 0, 3 * h, 0, exact));

    // Removing the road takes the schedules with it; a new one is refused.
    assert(w.remove_road(1, 0));
    assert(w.get_post_offices()[0].outbound_schedules.empty());
    assert(w.check_add_schedule(0, 1, h, exact) == ScheduleError::NoRoad);
    assert(!w.remove_road(0, 1));  // already gone
    assert(!w.remove_road(0, 99));
}

// Each departing truck costs kTruckDepartureCost, charged as it leaves, and
// reported as a negative MoneyEvent at the source office. A departure the
// player cannot afford is skipped: no truck, no letters moved, no charge.
void departures_are_charged() {
    const RoutingRule all{RoutingRuleType::AllLetters, {}};
    World w = make_line_world(2, 3);
    const int money0 = w.get_money();
    assert(w.add_schedule(0, 1, kTicksPerHour, 1, all));
    w.take_money_events();
    w.advance_tick();  // tick 1: first departure
    assert(w.get_trucks().size() == 1);
    assert(w.get_money() == money0 - kTruckDepartureCost);
    const std::vector<MoneyEvent> events = w.take_money_events();
    assert(events.size() == 1);
    assert(events[0].office == 0 && events[0].amount == -kTruckDepartureCost);

    // Spend down to just under one departure's price by building a pricey
    // (inert) office: the departure is skipped and nothing changes.
    World broke = make_line_world(2, 3);
    const PostOfficeId sink = broke.add_office("Sink", "S", {0, 1000}, 0, 0,
                                               broke.get_money() - (kTruckDepartureCost - 1));
    assert(broke.build_office(sink));
    assert(broke.get_money() == kTruckDepartureCost - 1);
    assert(broke.add_schedule(0, 1, kTicksPerHour, 1, all));
    const std::size_t waiting = broke.get_post_offices()[0].outbound_letters.size();
    broke.take_money_events();
    broke.advance_tick();  // tick 1: due, but unaffordable
    assert(broke.get_trucks().empty());
    assert(broke.get_money() == kTruckDepartureCost - 1);
    assert(broke.take_money_events().empty());
    assert(broke.get_post_offices()[0].outbound_letters.size() >= waiting);  // nothing boarded
}

// A truck carries at most kTruckCapacity letters, oldest first; the rest of
// the matching letters stay in the buffer for the next departure.
void trucks_carry_at_most_capacity() {
    World w = make_line_world(2, 12);  // 12 seeded letters, all for office 1
    const std::vector<Letter> seeded = w.get_post_offices()[0].outbound_letters;
    assert(seeded.size() == 12);
    assert(w.add_schedule(0, 1, kTicksPerHour, 1, {RoutingRuleType::AllLetters, {}}));
    w.advance_tick();  // tick 1
    assert(w.get_trucks().size() == 1);
    const std::vector<Letter>& cargo = w.get_trucks()[0]->get_letters();
    assert(cargo.size() == kTruckCapacity);
    const ScheduleId schedule = w.get_post_offices()[0].outbound_schedules[0].id;
    for (std::size_t i = 0; i < kTruckCapacity; ++i) {
        // The oldest letters boarded, their trails noting the departure.
        Letter expected = seeded[i];
        expected.trail.push_back({1, TrailEvent::Departed, 0, schedule});
        assert(cargo[i] == expected);
    }
    assert(w.get_post_offices()[0].outbound_letters.size() >= seeded.size() - kTruckCapacity);
}

// Departures land exactly on the start_offset + k * period grid, and a
// schedule created at tick == start_offset first departs one period later
// (the grid point strictly after creation).
void departures_follow_the_offset_grid() {
    const RoutingRule all{RoutingRuleType::AllLetters, {}};
    const Tick huge = 100 * kTicksPerDay;  // one departure, ever

    World w = make_line_world(2, 5);  // cargo waiting at office 0, all addressed to 1
    assert(w.add_schedule(0, 1, huge, 1000, all));
    while (w.get_tick() < 999)
        w.advance_tick();
    assert(w.get_trucks().empty());
    w.advance_tick();  // tick 1000
    assert(w.get_trucks().size() == 1);
    assert(w.get_trucks()[0]->get_dst() == 1);

    World w2 = make_line_world(2, 5);
    while (w2.get_tick() < 500)
        w2.advance_tick();
    assert(w2.add_schedule(0, 1, 200, 500, all));  // created AT its start offset...
    w2.advance_tick();                             // ...so tick 500 is already past:
    assert(w2.get_trucks().empty());               // the first departure is 500 + 200
    while (w2.get_tick() < 699)
        w2.advance_tick();
    assert(w2.get_trucks().empty());
    w2.advance_tick();  // tick 700
    assert(w2.get_trucks().size() == 1);
}

// List order is pick-up priority: when two schedules depart on the same tick
// and both match, the first in the list wins the contested letters — and
// move_schedule() (drag-to-reorder) flips the winner. Both trucks still
// depart: a truck leaves even when the winner took everything.
void first_matching_schedule_wins_contested_letters() {
    const RoutingRule all{RoutingRuleType::AllLetters, {}};
    const Tick huge = 100 * kTicksPerDay;

    // The buffer holds exactly one truckload: the first AllLetters departure
    // takes all of it; the second finds nothing and departs empty. Trucks
    // spawn in schedule (list) order.
    World w = make_line_world(3, kTruckCapacity);
    add_underpass_road(w, 0, 2);
    assert(w.add_schedule(0, 1, huge, 1, all));
    assert(w.add_schedule(0, 2, huge, 1, all));
    w.advance_tick();  // tick 1: both come due
    assert(w.get_trucks().size() == 2);
    assert(w.get_trucks()[0]->get_dst() == 1);
    assert(w.get_trucks()[0]->get_num_letters() > 0);   // the winner is loaded
    assert(w.get_trucks()[1]->get_dst() == 2);
    assert(w.get_trucks()[1]->get_num_letters() == 0);  // the loser departs empty
    assert(w.get_post_offices()[0].outbound_letters.empty());

    World w2 = make_line_world(3, kTruckCapacity);
    add_underpass_road(w2, 0, 2);
    assert(w2.add_schedule(0, 1, huge, 1, all));
    assert(w2.add_schedule(0, 2, huge, 1, all));
    w2.move_schedule(0, 0, 1);  // drag the route-to-1 below the route-to-2
    w2.advance_tick();
    assert(w2.get_trucks().size() == 2);
    assert(w2.get_trucks()[0]->get_dst() == 2);
    assert(w2.get_trucks()[0]->get_num_letters() > 0);
    assert(w2.get_trucks()[1]->get_dst() == 1);
    assert(w2.get_trucks()[1]->get_num_letters() == 0);

    // Identical schedules follow the same order: the first fills up, the
    // second takes what is left.
    World w3 = make_line_world(2, kTruckCapacity + 2);
    assert(w3.add_schedule(0, 1, huge, 1, all));
    assert(w3.add_schedule(0, 1, huge, 1, all));
    w3.advance_tick();
    assert(w3.get_trucks().size() == 2);
    assert(w3.get_trucks()[0]->get_num_letters() == kTruckCapacity);
    assert(w3.get_trucks()[1]->get_num_letters() > 0);
}

// Trucks depart on schedule even with an empty buffer — they just carry
// nothing — and retire on arrival like any other truck.
void empty_buffer_still_spawns_a_truck() {
    World w = make_line_world(2, 0);  // nobody ever produces mail
    assert(w.add_schedule(0, 1, kTicksPerHour, 1, {RoutingRuleType::AllLetters, {}}));
    w.advance_tick();  // tick 1: first departure
    assert(w.get_trucks().size() == 1);
    assert(w.get_trucks()[0]->get_num_letters() == 0);  // departs empty
    for (Tick t = 1; t < 2000; ++t)  // several departures come and go
        w.advance_tick();
    assert(w.get_trucks().empty());  // every empty truck arrived and retired
}

// A schedule's rule is evaluated at departure against the destination
// offices' *current* postal codes, so inspector edits take effect on the
// next departure without touching the schedule.
void postal_code_edits_take_effect_at_departure() {
    World w = make_line_world(3, 30);  // offices coded "T/0", "T/1", "T/2"
    const Tick huge = 100 * kTicksPerDay;
    assert(w.add_schedule(0, 1, huge, 5, {RoutingRuleType::PostalPrefix, {"ZZZ"}}));
    while (w.get_tick() < 5)
        w.advance_tick();
    // No code starts with "ZZZ": the truck still departs, but empty.
    assert(w.get_trucks().size() == 1);
    assert(w.get_trucks()[0]->get_num_letters() == 0);

    w.set_postal_code(1, "ZZZ/rebranded");
    // Same prefix idea, different string (an identical (dst, rule) pair would
    // be rejected as a duplicate).
    assert(w.add_schedule(0, 1, huge, 50, {RoutingRuleType::PostalPrefix, {"ZZ"}}));
    // Run to tick 55: the loaded truck departs at 50, and the empty one
    // (departed at 5, one 50-tick hop) arrives and retires at exactly 55.
    while (w.get_tick() < 55)
        w.advance_tick();
    assert(w.get_trucks().size() == 1);
    // Only office-1 mail (now "ZZZ/...") boarded — a full truckload, since
    // 30 seeded letters leave more office-1 mail than fits — while every
    // office-2 letter waited.
    assert(w.get_trucks()[0]->get_num_letters() == kTruckCapacity);
    for (const Letter& letter : w.get_trucks()[0]->get_letters())
        assert(letter.dst == 1);
    std::size_t waiting_for_2 = 0;
    for (const Letter& letter : w.get_post_offices()[0].outbound_letters)
        waiting_for_2 += letter.dst == 2 ? 1 : 0;
    assert(waiting_for_2 > 0);
}

// The UI observes its selected truck through a weak_ptr, with the World as
// the sole owner: the observer stays valid while the truck is in flight and
// expires exactly when the World retires the truck on arrival.
// get_arrival_tick() is exactly the tick has_arrived() first turns true.
void observed_truck_expires_on_arrival() {
    World w = make_line_world(2, 10);  // 10 seeded letters, all for office 1
    assert(w.add_schedule(0, 1, kTicksPerHour, 1, {RoutingRuleType::ExactDestination, {}}));
    w.advance_tick();  // tick 1: the truck departs with every seeded letter
    assert(w.get_trucks().size() == 1);
    const std::weak_ptr<const Truck> observed = w.get_trucks()[0];

    std::size_t cargo = 0;
    Tick arrival = 0;
    {
        const std::shared_ptr<const Truck> truck = observed.lock();
        assert(truck != nullptr);
        cargo = truck->get_num_letters();
        assert(cargo == kTruckCapacity);  // 10 waiting, but a truck holds only 5
        arrival = truck->get_arrival_tick();
        assert(arrival == 1 + 50);  // one 100-unit hop at kTruckSpeed
        assert(!truck->has_arrived(arrival - 1));
        assert(truck->has_arrived(arrival));
    }  // drop the temporary owner: the World is the sole owner again

    while (w.get_tick() < arrival - 1)
        w.advance_tick();
    assert(!observed.expired());  // still in flight one tick before arrival
    w.advance_tick();  // the arrival tick: delivered and retired
    assert(w.get_trucks().empty());
    assert(observed.expired());
    assert(w.get_letters_delivered_on_time() + w.get_letters_delivered_late() == cargo);
}

// remove_road: no refund, lookups fail in both argument orders, invalid calls
// change nothing, dependent schedules cascade-delete on both endpoint
// offices, the world stays stable afterwards (no phantom departures from the
// removed road), in-flight trucks still deliver, and the road can be rebuilt.
void road_removal() {
    const RoutingRule exact{RoutingRuleType::ExactDestination, {}};
    const Tick huge = 100 * kTicksPerDay;  // one departure, ever

    // --- removal, validation and no refund --------------------------------
    World w = make_line_world(3, 5);  // roads: 0-1, 1-2
    add_underpass_road(w, 0, 2);
    const int money0 = w.get_money();
    assert(w.get_roads().size() == 3);

    assert(!w.remove_road(0, 0));    // self
    assert(!w.remove_road(0, 99));   // out-of-range dst
    assert(!w.remove_road(99, 0));   // out-of-range src
    assert(w.get_roads().size() == 3);

    assert(w.remove_road(1, 0));  // argument order does not matter
    assert(w.get_money() == money0);  // NO refund
    assert(w.get_roads().size() == 2);
    assert(!w.has_road(0, 1));
    assert(!w.has_road(1, 0));
    assert(w.find_road(0, 1) == nullptr);
    assert(w.find_road(1, 0) == nullptr);
    assert(!w.remove_road(0, 1));  // already gone
    assert(w.get_money() == money0);

    // --- schedule cascade on both endpoint offices -------------------------
    assert(w.add_schedule(0, 2, kTicksPerHour, 0, exact));
    assert(w.add_schedule(2, 0, kTicksPerHour, 0, exact));
    assert(w.add_schedule(1, 2, kTicksPerHour, 0, exact));  // unrelated: survives
    assert(w.remove_road(0, 2));
    assert(w.get_post_offices()[0].outbound_schedules.empty());
    assert(w.get_post_offices()[2].outbound_schedules.empty());
    assert(w.get_post_offices()[1].outbound_schedules.size() == 1);

    // A schedule cannot exist without its road...
    assert(!w.add_schedule(0, 2, kTicksPerHour, 0, exact));

    // --- stability: no phantom departures from removed roads ---------------
    // (Guards the crash where spawn_truck built a truck from an empty vertex
    // list after its road was destroyed.) The surviving 1->2 schedule keeps
    // running normally while the removed 0-1 road never spawns anything.
    World w2 = make_line_world(2, 5);  // road 0-1 only
    assert(w2.add_schedule(0, 1, kTicksPerHour, 0, exact));
    assert(w2.remove_road(0, 1));
    assert(w2.get_post_offices()[0].outbound_schedules.empty());  // cascaded
    for (int i = 0; i < 200; ++i)
        w2.advance_tick();
    assert(w2.get_trucks().empty());  // nothing departs, nothing crashes

    // --- in-flight trucks are safe: they snapshot the polyline -------------
    World w3 = make_line_world(2, 5);
    assert(w3.add_schedule(0, 1, huge, 1, {RoutingRuleType::AllLetters, {}}));
    w3.advance_tick();  // tick 1: truck departs with the 5 seeded letters
    assert(w3.get_trucks().size() == 1);
    assert(w3.remove_road(0, 1));  // destroy the road mid-transit
    while (w3.get_tick() < 51)     // one 100-unit hop takes exactly 50 ticks
        w3.advance_tick();
    assert(w3.get_trucks().empty());  // arrived and retired
    assert(w3.get_letters_delivered_on_time() + w3.get_letters_delivered_late() == 5);

    // --- rebuild: a removed road can be built again, at full price ---------
    World w4 = make_line_world(2, 0);
    const int money4 = w4.get_money();
    assert(w4.remove_road(0, 1));
    assert(w4.get_money() == money4);
    add_straight_road(w4, 0, 1);
    assert(w4.has_road(0, 1));
    assert(w4.get_money() < money4);  // paid again — the removal refunded nothing
}

// Hub-and-spoke end to end: mail rides a leg it is not addressed to, is
// forwarded into the hub's buffer (bypassing the spawn cap), and a second
// schedule delivers it onward — every seeded letter ends up scored, on time.
void multi_hop_forwarding_delivers_every_letter() {
    World w;
    const PostOfficeId a = w.add_office("A", "P/A", {0, 0}, kTruckCapacity, 100, 0);
    const PostOfficeId b = w.add_office("B", "P/B", {100, 0}, 0, 100, 0);
    const PostOfficeId c = w.add_office("C", "P/C", {200, 0}, 0, 100, 0);
    for (const PostOfficeId id : {a, b, c})
        assert(w.build_office(id));
    w.seed_letters();  // one truckload of letters at A, addressed to B or C at random
    add_straight_road(w, a, b);
    add_straight_road(w, b, c);

    // Leg 1 (A -> B) takes everything: both downstream codes start with "P".
    // Leg 2 (B -> C) is exact-destination and departs after leg 1 arrives
    // (tick 1 + 50 = 51), so it finds the forwarded C-mail waiting at B.
    assert(w.add_schedule(a, b, 100 * kTicksPerDay, 1, {RoutingRuleType::PostalPrefix, {"P"}}));
    assert(w.add_schedule(b, c, 100 * kTicksPerDay, 60, {RoutingRuleType::ExactDestination, {}}));

    for (Tick t = 0; t < 110; ++t)  // leg 2 departs at 60, arrives at 110
        w.advance_tick();

    assert(w.get_trucks().empty());
    assert(w.get_letters_delivered_late() == 0);      // deadlines are >= half a day out
    assert(w.get_letters_delivered_on_time() == kTruckCapacity);  // every seeded letter, nothing lost
    for (const PostOffice& office : w.get_post_offices())
        assert(office.outbound_letters.empty());
}

// The hub route above, checked through its statistics and letter trails:
// every flow lands on the right office and schedule, and each letter's trail
// records exactly when it left and reached which office on which schedule.
void stats_and_trails_follow_a_hub_route() {
    World w;
    const PostOfficeId a = w.add_office("A", "P/A", {0, 0}, kTruckCapacity, 100, 0);
    const PostOfficeId b = w.add_office("B", "P/B", {100, 0}, 0, 100, 0);
    const PostOfficeId c = w.add_office("C", "P/C", {200, 0}, 0, 100, 0);
    for (const PostOfficeId id : {a, b, c})
        assert(w.build_office(id));
    w.seed_letters();
    const int before_roads = w.get_money();
    add_straight_road(w, a, b);
    add_straight_road(w, b, c);
    const int road_spend = before_roads - w.get_money();
    assert(w.add_schedule(a, b, 100 * kTicksPerDay, 1, {RoutingRuleType::PostalPrefix, {"P"}}));
    assert(w.add_schedule(b, c, 100 * kTicksPerDay, 60, {RoutingRuleType::ExactDestination, {}}));

    int64_t to_b = 0, to_c = 0;
    for (const Letter& letter : w.get_post_offices()[a].outbound_letters)
        (letter.dst == b ? to_b : to_c) += 1;
    assert(to_b + to_c == static_cast<int64_t>(kTruckCapacity));

    for (Tick t = 0; t < 110; ++t)
        w.advance_tick();

    const GlobalStats& g = w.get_stats();
    const auto& offices = w.get_post_offices();
    const ScheduleId leg1 = offices[a].outbound_schedules[0].id;
    const ScheduleId leg2 = offices[b].outbound_schedules[0].id;
    assert(leg1 != leg2);

    // Global flows. Spawns are counted per office too, trickle included.
    assert(g[GlobalStat::Spawned].total == offices[a].stats[OfficeStat::Spawned].total);
    assert(g[GlobalStat::Spawned].total >= static_cast<int64_t>(kTruckCapacity));
    assert(g[GlobalStat::Lost].total == 0);
    assert(g[GlobalStat::DeliveredOnTime].total == static_cast<int64_t>(kTruckCapacity));
    assert(g[GlobalStat::DeliveredLate].total == 0);
    assert(g[GlobalStat::SpentRoads].total == road_spend);
    assert(g[GlobalStat::SpentTrucks].total == 2 * kTruckDepartureCost);
    assert(g[GlobalStat::SpentOffices].total == 0);  // they were free

    // Income adds up three ways: globally, per delivering office, per log entry.
    int64_t logged = 0;
    for (const DeliveredLetter& d : w.get_recent_deliveries())
        logged += d.earned;
    const int64_t income = g[GlobalStat::IncomeValue].total + g[GlobalStat::IncomeBonus].total;
    assert(income == logged);
    assert(income == offices[b].stats[OfficeStat::Income].total + offices[c].stats[OfficeStat::Income].total);
    assert(w.get_money() == kStartingMoney - road_spend - 2 * kTruckDepartureCost + income);

    // Offices: B delivers its own mail and forwards C's.
    assert(offices[b].stats[OfficeStat::Delivered].total == to_b);
    assert(offices[b].stats[OfficeStat::Forwarded].total == to_c);
    assert(offices[c].stats[OfficeStat::Delivered].total == to_c);
    assert(offices[a].stats[OfficeStat::Loaded].total == static_cast<int64_t>(kTruckCapacity));
    assert(offices[a].stats[OfficeStat::Departures].total == 1);

    // Schedules: leg 1 took everything.
    const TruckSchedule& s1 = offices[a].outbound_schedules[0];
    const TruckSchedule& s2 = offices[b].outbound_schedules[0];
    assert(s1.stats[ScheduleStat::Departures].total == 1);
    assert(s1.stats[ScheduleStat::Loaded].total == static_cast<int64_t>(kTruckCapacity));
    assert(s1.stats[ScheduleStat::LeftBehind].total == 0);
    assert(s2.stats[ScheduleStat::Loaded].total == to_c);
    assert(s1.departure_log == (std::vector<DepartureRecord>{{1, static_cast<std::uint32_t>(kTruckCapacity), 0}}));

    // Trails, from the recent-deliveries log.
    assert(w.get_recent_deliveries().size() == kTruckCapacity);
    for (const DeliveredLetter& d : w.get_recent_deliveries()) {
        std::vector<TrailStop> expected = {
            {0, TrailEvent::Spawned, a, kNoSchedule},
            {1, TrailEvent::Departed, a, leg1},
            {51, TrailEvent::Arrived, b, kNoSchedule},
        };
        if (d.letter.dst == c) {
            expected.push_back({60, TrailEvent::Departed, b, leg2});
            expected.push_back({110, TrailEvent::Arrived, c, kNoSchedule});
        }
        assert(d.letter.trail == expected);
        assert(d.delivered == expected.back().tick);
    }
}

// A full buffer refuses new letters: they are counted as lost, globally and
// at the office, and the backlog gauge shows the buffer at its cap.
void lost_letters_are_counted() {
    World w;
    const PostOfficeId a = w.add_office("A", "P/A", {0, 0}, 100, /*max_outbound_letters=*/2, 0);
    const PostOfficeId b = w.add_office("B", "P/B", {100, 0}, 0, 100, 0);
    assert(w.build_office(a) && w.build_office(b));
    w.seed_letters();  // 100 attempts into a buffer of 2
    w.advance_tick();
    const OfficeStats& stats = w.get_post_offices()[a].stats;
    const int64_t spawned = stats[OfficeStat::Spawned].total;
    const int64_t lost = stats[OfficeStat::Lost].total;
    assert(spawned == 2);
    assert(lost >= 98);  // plus any refused trickle spawn on the tick
    assert(w.get_stats()[GlobalStat::Lost].total == lost);
    assert(stats[OfficeStat::Backlog].total == 2);
    assert(w.get_stats()[GlobalStat::Backlog].total == 2);
}

// An unaffordable departure does not happen, and a truck whose schedule is
// removed mid-trip still scores its letters globally.
void skipped_departures_and_orphaned_trucks() {
    World w = make_line_world(2, 5);
    assert(w.add_schedule(0, 1, kTicksPerHour, 1, {RoutingRuleType::AllLetters, {}}));
    const int money = w.get_money();
    w.set_money(0);
    w.advance_tick();  // tick 1: due, unaffordable
    assert(w.get_trucks().empty());

    w.set_money(money);
    while (w.get_trucks().empty())
        w.advance_tick();
    assert(w.get_trucks()[0]->get_schedule() == w.get_post_offices()[0].outbound_schedules[0].id);
    w.remove_schedule(0, 0);
    while (!w.get_trucks().empty())
        w.advance_tick();
    assert(w.get_stats()[GlobalStat::DeliveredOnTime].total > 0);
    assert(w.get_stats()[GlobalStat::IncomeValue].total > 0);
}

// Offices start unbuilt and inert: they spawn no letters, receive none, and
// cannot be joined by roads until the player pays to build them.
void building_offices() {
    World w;
    const PostOfficeId a = w.add_office("A", "P/A", {0, 0}, 50, 100, 1000);
    const PostOfficeId b = w.add_office("B", "P/B", {100, 0}, 50, 100, 2000);
    const PostOfficeId c = w.add_office("C", "P/C", {200, 0}, 50, 100, 10 * kStartingMoney);
    for (const PostOffice& office : w.get_post_offices())
        assert(!office.built);

    // Unbuilt: no letters, no roads.
    w.seed_letters();
    for (Tick t = 0; t < kTicksPerDay; ++t)
        w.advance_tick();
    for (const PostOffice& office : w.get_post_offices())
        assert(office.outbound_letters.empty());
    assert(!w.add_road(a, b, {{0, 0}, {100, 0}}));
    assert(w.get_money() == kStartingMoney);

    // Building charges build_cost once; rebuilding and bad ids are rejected.
    assert(w.build_office(a));
    assert(w.get_money() == kStartingMoney - 1000);
    assert(!w.build_office(a));
    assert(!w.build_office(99));
    assert(w.get_money() == kStartingMoney - 1000);

    // A lone built office has nobody to write to.
    w.seed_letters();
    assert(w.get_post_offices()[a].outbound_letters.empty());

    // Unaffordable offices stay unbuilt and cost nothing.
    assert(!w.build_office(c));
    assert(!w.get_post_offices()[c].built);
    assert(w.get_money() == kStartingMoney - 1000);

    // With two built offices, mail flows only between them, never to C.
    assert(w.build_office(b));
    assert(w.get_money() == kStartingMoney - 3000);
    assert(w.add_road(a, b, {{0, 0}, {100, 0}}));
    assert(!w.add_road(b, c, {{100, 0}, {200, 0}}));
    w.seed_letters();
    assert(!w.get_post_offices()[a].outbound_letters.empty());
    assert(!w.get_post_offices()[b].outbound_letters.empty());
    assert(w.get_post_offices()[c].outbound_letters.empty());
    for (const PostOffice& office : w.get_post_offices())
        for (const Letter& letter : office.outbound_letters)
            assert(letter.dst != c);
}

}  // namespace

int main() {
    routing_rule_matches();
    printf("PASS routing_rule_matches\n");
    schedule_validation_and_pricing();
    printf("PASS schedule_validation_and_pricing\n");
    schedule_editing();
    printf("PASS schedule_editing\n");
    schedule_errors();
    printf("PASS schedule_errors\n");
    departures_are_charged();
    printf("PASS departures_are_charged\n");
    trucks_carry_at_most_capacity();
    printf("PASS trucks_carry_at_most_capacity\n");
    departures_follow_the_offset_grid();
    printf("PASS departures_follow_the_offset_grid\n");
    first_matching_schedule_wins_contested_letters();
    printf("PASS first_matching_schedule_wins_contested_letters\n");
    empty_buffer_still_spawns_a_truck();
    printf("PASS empty_buffer_still_spawns_a_truck\n");
    postal_code_edits_take_effect_at_departure();
    printf("PASS postal_code_edits_take_effect_at_departure\n");
    multi_hop_forwarding_delivers_every_letter();
    printf("PASS multi_hop_forwarding_delivers_every_letter\n");
    stats_and_trails_follow_a_hub_route();
    printf("PASS stats_and_trails_follow_a_hub_route\n");
    lost_letters_are_counted();
    printf("PASS lost_letters_are_counted\n");
    skipped_departures_and_orphaned_trucks();
    printf("PASS skipped_departures_and_orphaned_trucks\n");
    road_removal();
    printf("PASS road_removal\n");
    observed_truck_expires_on_arrival();
    printf("PASS observed_truck_expires_on_arrival\n");
    building_offices();
    printf("PASS building_offices\n");

    printf("all simulation tests passed\n");
    return 0;
}
