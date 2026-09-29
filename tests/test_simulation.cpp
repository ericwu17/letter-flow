// test_simulation.cpp — headless tests for the simulation rules (World's
// public API): routing-rule matching, schedule validation and pricing, the
// departure grid, pick-up priority, and delivery/forwarding. Run with
// `make test`. Pure simulation layer: no SDL/imgui/Metal — no window needed.
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
#include <string>

namespace {

// A world of `count` offices in a row along the x axis, 100 world units
// apart — one hop takes exactly 50 ticks at kTruckSpeed. Only office 0
// produces mail (`letters_at_source` seeded letters, all addressed to one of
// the other offices at random, plus that same rate trickling in per tick);
// every other office has letters_per_day = 0 and stays inert.
World make_line_world(std::size_t count, std::size_t letters_at_source) {
    World w;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string name = "O" + std::to_string(i);
        const std::string code = "T/" + std::to_string(i);
        w.add_office(name, code, {100.0f * static_cast<float>(i), 0.0f},
                     i == 0 ? letters_at_source : 0, /*max_outbound_letters=*/1000);
    }
    w.seed_letters();
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
    const int money0 = w.get_money();
    assert(money0 == kStartingMoney);

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
    const int unit_cost = schedule_cost(kMinSchedulePeriod);
    int bought = 0;
    while (broke.add_schedule(0, 1, kMinSchedulePeriod, 0,
                              {RoutingRuleType::PostalPrefix, "p" + std::to_string(bought)}))
        ++bought;
    assert(bought == kStartingMoney / unit_cost);  // bought until one no longer fit
    assert(broke.get_money() == kStartingMoney - bought * unit_cost);
    assert(broke.get_money() < unit_cost);         // hence the rejection
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
    assert(w.get_trucks()[0].get_dst() == 1);

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
// move_schedule() (drag-to-reorder) flips the winner.
void first_matching_schedule_wins_contested_letters() {
    const RoutingRule all{RoutingRuleType::AllLetters, ""};
    const Tick huge = 100 * kTicksPerDay;

    // The first AllLetters departure takes the whole buffer; the second finds
    // nothing and spawns no truck ("no cargo, no truck").
    World w = make_line_world(3, 40);
    assert(w.add_schedule(0, 1, huge, 1, all));
    assert(w.add_schedule(0, 2, huge, 1, all));
    w.advance_tick();  // tick 1: both come due
    assert(w.get_trucks().size() == 1);
    assert(w.get_trucks()[0].get_dst() == 1);
    assert(w.get_post_offices()[0].outbound_letters.empty());

    World w2 = make_line_world(3, 40);
    assert(w2.add_schedule(0, 1, huge, 1, all));
    assert(w2.add_schedule(0, 2, huge, 1, all));
    w2.move_schedule(0, 0, 1);  // drag the route-to-1 below the route-to-2
    w2.advance_tick();
    assert(w2.get_trucks().size() == 1);
    assert(w2.get_trucks()[0].get_dst() == 2);
}

void empty_buffer_spawns_no_truck() {
    World w = make_line_world(2, 0);  // nobody ever produces mail
    assert(w.add_schedule(0, 1, kTicksPerHour, 1, {RoutingRuleType::AllLetters, ""}));
    for (Tick t = 0; t < 2000; ++t)  // several departures come and go
        w.advance_tick();
    assert(w.get_trucks().empty());
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
    assert(w.get_trucks().empty());  // no code starts with "ZZZ": nothing boarded

    w.set_postal_code(1, "ZZZ/rebranded");
    // Same prefix idea, different string (an identical (dst, rule) pair would
    // be rejected as a duplicate).
    assert(w.add_schedule(0, 1, huge, 50, {RoutingRuleType::PostalPrefix, "ZZ"}));
    while (w.get_tick() < 50)
        w.advance_tick();
    assert(w.get_trucks().size() == 1);
    // Only office-1 mail (now "ZZZ/...") boarded; office-2 mail waited.
    for (const Letter& letter : w.get_trucks()[0].get_letters())
        assert(letter.dst == 1);
    for (const Letter& letter : w.get_post_offices()[0].outbound_letters)
        assert(letter.dst == 2);
}

// Hub-and-spoke end to end: mail rides a leg it is not addressed to, is
// forwarded into the hub's buffer (bypassing the spawn cap), and a second
// schedule delivers it onward — every seeded letter ends up scored, on time.
void multi_hop_forwarding_delivers_every_letter() {
    World w;
    const PostOfficeId a = w.add_office("A", "P/A", {0.0f, 0.0f}, 12, 100);
    const PostOfficeId b = w.add_office("B", "P/B", {100.0f, 0.0f}, 0, 100);
    const PostOfficeId c = w.add_office("C", "P/C", {200.0f, 0.0f}, 0, 100);
    w.seed_letters();  // 12 letters at A, addressed to B or C at random

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
    empty_buffer_spawns_no_truck();
    printf("PASS empty_buffer_spawns_no_truck\n");
    postal_code_edits_take_effect_at_departure();
    printf("PASS postal_code_edits_take_effect_at_departure\n");
    multi_hop_forwarding_delivers_every_letter();
    printf("PASS multi_hop_forwarding_delivers_every_letter\n");

    printf("all simulation tests passed\n");
    return 0;
}
