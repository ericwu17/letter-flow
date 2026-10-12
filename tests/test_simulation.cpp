// test_simulation.cpp — headless tests for the simulation rules (World's
// public API): routing-rule matching, schedule validation and pricing, the
// departure grid, pick-up priority, and delivery/forwarding. Run with
// `make test`. Pure simulation layer: no SDL/imgui/OpenGL — no window needed.
//
// Everything here is deterministic (fixed RNG seed, fixed tick counts) and
// worlds are built to order through add_office()/seed_letters(), so the
// asserts hold on every run.
#include "entities.h"
#include "game_types.h"
#include "truck.h"
#include "world.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

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
// office has letters_per_day = 0 and stays inert.
World make_line_world(std::size_t count, std::size_t letters_at_source) {
    World w;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string name = "O" + std::to_string(i);
        const std::string code = "T/" + std::to_string(i);
        w.add_office(name, code, {100 * static_cast<int64_t>(i), 0},
                     i == 0 ? letters_at_source : 0, /*max_outbound_letters=*/1000);
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
    const RoutingRule exact{RoutingRuleType::ExactDestination, ""};
    assert(exact.matches(3, "T/1", 3));
    assert(!exact.matches(3, "T/1", 4));

    const RoutingRule all{RoutingRuleType::AllLetters, ""};
    assert(all.matches(0, "", 9));

    const RoutingRule prefix{RoutingRuleType::PostalPrefix, "NJ/Mercer"};
    assert(prefix.matches(5, "NJ/Mercer/Princeton", 9));  // the truck's own dst is irrelevant
    assert(prefix.matches(5, "NJ/Mercer", 9));
    assert(!prefix.matches(5, "NJ/Central/Monroe", 9));
    assert(!prefix.matches(5, "NJ", 9));  // shorter than the prefix

    // A misconfigured empty prefix must match nothing, never everything.
    const RoutingRule empty_prefix{RoutingRuleType::PostalPrefix, ""};
    assert(!empty_prefix.matches(5, "NJ/Mercer", 9));
}

// add_schedule's rejection matrix, the frequency-based pricing, duplicate
// detection (including prefix normalization), and the affordability gate.
void schedule_validation_and_pricing() {
    World w = make_line_world(3, 0);
    add_underpass_road(w, 0, 2);  // for the distinct-dst schedule below
    const int money0 = w.get_money();
    assert(money0 < kStartingMoney);  // the fixture roads were paid for

    // Invalid routes are rejected and cost nothing.
    const RoutingRule exact{RoutingRuleType::ExactDestination, ""};
    assert(!w.add_schedule(99, 1, kTicksPerHour, 0, exact));        // bad src
    assert(!w.add_schedule(0, 99, kTicksPerHour, 0, exact));        // bad dst
    assert(!w.add_schedule(0, 0, kTicksPerHour, 0, exact));         // self-route
    assert(!w.add_schedule(0, 1, 0, 0, exact));                     // zero period
    const RoutingRule empty_prefix{RoutingRuleType::PostalPrefix, ""};
    assert(!w.add_schedule(0, 1, kTicksPerHour, 0, empty_prefix));  // empty prefix
    assert(w.get_money() == money0);

    // Pricing: charged per departure per in-game day.
    assert(schedule_cost(3 * kTicksPerHour) == 8 * kScheduleCostPerDailyDeparture);
    assert(schedule_cost(kTicksPerDay) == kScheduleCostPerDailyDeparture);
    assert(schedule_cost(0) == 0);  // invalid period; callers reject it

    const int three_hourly = schedule_cost(3 * kTicksPerHour);
    assert(w.add_schedule(0, 1, 3 * kTicksPerHour, 0, exact));
    assert(w.get_money() == money0 - three_hourly);

    // Duplicates compare (dst, rule) only — period and offset may differ —
    // and a stale prefix on a non-prefix rule is normalized away first.
    assert(!w.add_schedule(0, 1, kTicksPerHour, 7, exact));
    const RoutingRule dirty_exact{RoutingRuleType::ExactDestination, "junk"};
    assert(!w.add_schedule(0, 1, kTicksPerHour, 0, dirty_exact));
    // A different rule or a different dst is a distinct product, though.
    const RoutingRule all{RoutingRuleType::AllLetters, ""};
    assert(w.add_schedule(0, 1, kTicksPerHour, 0, all));
    assert(w.add_schedule(0, 2, kTicksPerHour, 0, exact));

    // Affordability: buy the fastest (priciest) schedules until rejected.
    World broke = make_line_world(2, 0);
    const int broke_money0 = broke.get_money();  // after the chain road's cost
    const int unit_cost = schedule_cost(kMinSchedulePeriod);
    int bought = 0;
    while (broke.add_schedule(0, 1, kMinSchedulePeriod, 0,
                              {RoutingRuleType::PostalPrefix, "p" + std::to_string(bought)}))
        ++bought;
    assert(bought == broke_money0 / unit_cost);  // bought until one no longer fit
    assert(broke.get_money() == broke_money0 - bought * unit_cost);
    assert(broke.get_money() < unit_cost);       // hence the rejection
}

// Departures land exactly on the start_offset + k * period grid, and a
// schedule created at tick == start_offset first departs one period later
// (the grid point strictly after creation).
void departures_follow_the_offset_grid() {
    const RoutingRule all{RoutingRuleType::AllLetters, ""};
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
    const RoutingRule all{RoutingRuleType::AllLetters, ""};
    const Tick huge = 100 * kTicksPerDay;

    // The first AllLetters departure takes the whole buffer; the second finds
    // nothing and departs empty. Trucks spawn in schedule (list) order.
    World w = make_line_world(3, 40);
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

    World w2 = make_line_world(3, 40);
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
}

// Trucks depart on schedule even with an empty buffer — they just carry
// nothing — and retire on arrival like any other truck.
void empty_buffer_still_spawns_a_truck() {
    World w = make_line_world(2, 0);  // nobody ever produces mail
    assert(w.add_schedule(0, 1, kTicksPerHour, 1, {RoutingRuleType::AllLetters, ""}));
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
    assert(w.add_schedule(0, 1, huge, 5, {RoutingRuleType::PostalPrefix, "ZZZ"}));
    while (w.get_tick() < 5)
        w.advance_tick();
    // No code starts with "ZZZ": the truck still departs, but empty.
    assert(w.get_trucks().size() == 1);
    assert(w.get_trucks()[0]->get_num_letters() == 0);

    w.set_postal_code(1, "ZZZ/rebranded");
    // Same prefix idea, different string (an identical (dst, rule) pair would
    // be rejected as a duplicate).
    assert(w.add_schedule(0, 1, huge, 50, {RoutingRuleType::PostalPrefix, "ZZ"}));
    // Run to tick 55: the loaded truck departs at 50, and the empty one
    // (departed at 5, one 50-tick hop) arrives and retires at exactly 55.
    while (w.get_tick() < 55)
        w.advance_tick();
    assert(w.get_trucks().size() == 1);
    assert(w.get_trucks()[0]->get_num_letters() > 0);
    // Only office-1 mail (now "ZZZ/...") boarded; office-2 mail waited.
    for (const Letter& letter : w.get_trucks()[0]->get_letters())
        assert(letter.dst == 1);
    for (const Letter& letter : w.get_post_offices()[0].outbound_letters)
        assert(letter.dst == 2);
}

// The UI observes its selected truck through a weak_ptr, with the World as
// the sole owner: the observer stays valid while the truck is in flight and
// expires exactly when the World retires the truck on arrival.
// get_arrival_tick() is exactly the tick has_arrived() first turns true.
void observed_truck_expires_on_arrival() {
    World w = make_line_world(2, 10);  // 10 seeded letters, all for office 1
    assert(w.add_schedule(0, 1, kTicksPerHour, 1, {RoutingRuleType::ExactDestination, ""}));
    w.advance_tick();  // tick 1: the truck departs with every seeded letter
    assert(w.get_trucks().size() == 1);
    const std::weak_ptr<const Truck> observed = w.get_trucks()[0];

    std::size_t cargo = 0;
    Tick arrival = 0;
    {
        const std::shared_ptr<const Truck> truck = observed.lock();
        assert(truck != nullptr);
        cargo = truck->get_num_letters();
        assert(cargo >= 10);
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
    const RoutingRule exact{RoutingRuleType::ExactDestination, ""};
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
    assert(w3.add_schedule(0, 1, huge, 1, {RoutingRuleType::AllLetters, ""}));
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
    const PostOfficeId a = w.add_office("A", "P/A", {0, 0}, 12, 100);
    const PostOfficeId b = w.add_office("B", "P/B", {100, 0}, 0, 100);
    const PostOfficeId c = w.add_office("C", "P/C", {200, 0}, 0, 100);
    w.seed_letters();  // 12 letters at A, addressed to B or C at random
    add_straight_road(w, a, b);
    add_straight_road(w, b, c);

    // Leg 1 (A -> B) takes everything: both downstream codes start with "P".
    // Leg 2 (B -> C) is exact-destination and departs after leg 1 arrives
    // (tick 1 + 50 = 51), so it finds the forwarded C-mail waiting at B.
    assert(w.add_schedule(a, b, 100 * kTicksPerDay, 1, {RoutingRuleType::PostalPrefix, "P"}));
    assert(w.add_schedule(b, c, 100 * kTicksPerDay, 60, {RoutingRuleType::ExactDestination, ""}));

    for (Tick t = 0; t < 110; ++t)  // leg 2 departs at 60, arrives at 110
        w.advance_tick();

    assert(w.get_trucks().empty());
    assert(w.get_letters_delivered_late() == 0);      // deadlines are >= half a day out
    assert(w.get_letters_delivered_on_time() == 12);  // every seeded letter, nothing lost
    for (const PostOffice& office : w.get_post_offices())
        assert(office.outbound_letters.empty());
}

}  // namespace

int main() {
    routing_rule_matches();
    printf("PASS routing_rule_matches\n");
    schedule_validation_and_pricing();
    printf("PASS schedule_validation_and_pricing\n");
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
    road_removal();
    printf("PASS road_removal\n");
    observed_truck_expires_on_arrival();
    printf("PASS observed_truck_expires_on_arrival\n");

    printf("all simulation tests passed\n");
    return 0;
}
