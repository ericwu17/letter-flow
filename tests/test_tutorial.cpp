// test_tutorial.cpp — headless tests for the tutorial (tutorial.h) and its
// world (create_tutorial_world, scenario.h): the map is what the steps
// describe, and a player following the steps through the World API alone,
// with hand-set UI facts, reaches the end. Run with `make test`. Pure
// simulation layer: no SDL/imgui/OpenGL.
#include "entities.h"
#include "game_types.h"
#include "scenario.h"
#include "stats.h"
#include "tutorial.h"
#include "world.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using namespace tutorial_map;

// The tutorial's roads are single straight segments, office to office.
bool add_direct_road(World& w, PostOfficeId a, PostOfficeId b) {
    return w.add_road(a, b, {w.get_post_offices()[a].pos, w.get_post_offices()[b].pos});
}

void expect_step(const Tutorial& t, const char* title) {
    assert(!t.finished());
    if (std::strcmp(t.current().title, title) != 0) {
        std::printf("expected step \"%s\", at step %zu \"%s\"\n", title, t.index(), t.current().title);
        assert(false);
    }
}

// Runs the clock until the tutorial moves past its current step (at most two
// in-game days).
void tick_until_step_done(World& w, Tutorial& t, const TutorialUiFacts& facts) {
    const std::size_t step = t.index();
    for (Tick i = 0; i < 2 * kTicksPerDay && t.index() == step; ++i) {
        w.advance_tick();
        t.update(w, facts);
    }
    assert(t.index() != step);
}

void tutorial_world_matches_the_steps() {
    const World w = create_tutorial_world();
    const std::vector<PostOffice>& offices = w.get_post_offices();
    assert(offices.size() == kOfficeCount);
    assert(offices[kCentral].name == "Central");
    assert(offices[kAshford].name == "Ashford");
    assert(offices[kBrookside].name == "Brookside");
    assert(offices[kDunmore].name == "Dunmore");
    assert(offices[kElmwood].name == "Elmwood");
    for (const PostOffice& office : offices)
        assert(!office.built);
    assert(w.is_paused());
    assert(w.get_roads().empty());
    // The postal-code lesson needs Elmwood misfiled, and Dunmore already south.
    assert(!offices[kElmwood].postal_code.starts_with("SOUTH/"));
    assert(offices[kDunmore].postal_code.starts_with("SOUTH/"));
}

void every_step_has_text_and_info_cards_wait_for_next() {
    for (Tutorial t; !t.finished(); t.next()) {
        assert(std::strlen(t.current().title) > 0);
        assert(std::strlen(t.current().body) > 0);
    }
    // The first and last steps are info cards: update never moves past them.
    const World w = create_tutorial_world();
    Tutorial t;
    t.update(w, {});
    t.update(w, {});
    assert(t.index() == 0);
    while (t.index() + 1 < Tutorial::count())
        t.next();
    assert(t.current().done == nullptr);
    t.update(w, {});
    assert(!t.finished());
    t.next();
    assert(t.finished());
    t.next();  // no-op once finished
    assert(t.finished());
}

void following_the_steps_finishes_the_tutorial() {
    World w = create_tutorial_world();
    Tutorial t;
    TutorialUiFacts facts;

    expect_step(t, "Welcome to Letter Flow");
    t.next();
    expect_step(t, "Getting around");
    t.next();

    expect_step(t, "Pick a site");
    t.update(w, facts);
    expect_step(t, "Pick a site");  // the inspector defaults to office 0, Central
    facts.selected_office = kAshford;
    t.update(w, facts);

    expect_step(t, "Build it");
    assert(w.build_office(kAshford));
    t.update(w, facts);

    expect_step(t, "Build the hub");
    facts.selected_office = kCentral;
    assert(w.build_office(kCentral));
    t.update(w, facts);

    expect_step(t, "Start a road");
    facts.building_road = true;
    t.update(w, facts);

    expect_step(t, "Lay the road");
    assert(add_direct_road(w, kAshford, kCentral));
    facts.building_road = false;
    t.update(w, facts);

    expect_step(t, "Start the clock");
    w.set_paused(false);
    t.update(w, facts);

    expect_step(t, "Mail arrives");
    tick_until_step_done(w, t, facts);

    expect_step(t, "Create a schedule");
    assert(w.add_schedule(kAshford, kCentral, hours_minutes_to_ticks(1, 0), 0, RoutingRule{}));
    t.update(w, facts);

    expect_step(t, "First delivery");
    tick_until_step_done(w, t, facts);

    expect_step(t, "Inspect a truck");
    facts.truck_selected = true;
    t.update(w, facts);

    expect_step(t, "Update a schedule");
    assert(w.update_schedule(kAshford, 0, hours_minutes_to_ticks(0, 30), 0, RoutingRule{}));
    t.update(w, facts);

    expect_step(t, "Grow the network");
    assert(w.build_office(kBrookside));
    assert(w.build_office(kDunmore));
    assert(w.build_office(kElmwood));
    assert(add_direct_road(w, kCentral, kBrookside));
    assert(add_direct_road(w, kCentral, kDunmore));
    t.update(w, facts);
    expect_step(t, "Grow the network");  // Dunmore - Elmwood is still missing
    assert(add_direct_road(w, kDunmore, kElmwood));
    t.update(w, facts);
    // Every required build fits the tutorial's budget with plenty to spare.
    assert(w.get_money() > 5000);

    expect_step(t, "Hub and spoke");
    assert(w.update_schedule(kAshford, 0, hours_minutes_to_ticks(0, 30), 0,
                             RoutingRule{RoutingRuleType::AllLetters, {}}));
    t.update(w, facts);

    expect_step(t, "Postal codes");
    t.update(w, facts);
    expect_step(t, "Postal codes");
    t.next();

    expect_step(t, "Fix a postal code");
    w.set_postal_code(kElmwood, "SOUTH/Elmwood");
    t.update(w, facts);

    expect_step(t, "Prefix routing");
    // A prefix that misses Elmwood doesn't count.
    assert(w.add_schedule(kCentral, kDunmore, kTicksPerHour, 0,
                          RoutingRule{RoutingRuleType::PostalPrefix, {"SOUTH/Dunmore"}}));
    t.update(w, facts);
    expect_step(t, "Prefix routing");
    assert(w.add_schedule(kCentral, kDunmore, kTicksPerHour, 0,
                          RoutingRule{RoutingRuleType::PostalPrefix, {"SOUTH/"}}));
    t.update(w, facts);

    expect_step(t, "The last leg");
    assert(w.add_schedule(kDunmore, kElmwood, kTicksPerHour, 0, RoutingRule{}));
    t.update(w, facts);

    expect_step(t, "Who picks up first");
    t.next();

    expect_step(t, "Statistics");
    facts.stats_open = true;
    t.update(w, facts);

    expect_step(t, "You're ready");
    t.next();
    assert(t.finished());

    // The network the tutorial built actually delivers south-bound mail
    // through the hub: run a day and check Elmwood received letters.
    const std::size_t delivered = w.get_letters_delivered_on_time() + w.get_letters_delivered_late();
    for (Tick i = 0; i < kTicksPerDay; ++i)
        w.advance_tick();
    assert(w.get_letters_delivered_on_time() + w.get_letters_delivered_late() > delivered);
    assert(w.get_post_offices()[kElmwood].stats[OfficeStat::Delivered].total > 0);
}

void steps_done_early_are_skipped() {
    World w = create_tutorial_world();
    Tutorial t;
    TutorialUiFacts facts;
    t.next();
    t.next();

    // Build both offices and the road before following any step: once
    // Ashford is selected, everything up to pressing play is already done.
    assert(w.build_office(kAshford));
    assert(w.build_office(kCentral));
    assert(add_direct_road(w, kAshford, kCentral));
    t.update(w, facts);
    expect_step(t, "Pick a site");  // selecting is still to do
    facts.selected_office = kAshford;
    t.update(w, facts);
    expect_step(t, "Start the clock");

    // Skip works on action steps too.
    t.next();
    expect_step(t, "Mail arrives");
}

}  // namespace

int main() {
    tutorial_world_matches_the_steps();
    printf("PASS tutorial_world_matches_the_steps\n");
    every_step_has_text_and_info_cards_wait_for_next();
    printf("PASS every_step_has_text_and_info_cards_wait_for_next\n");
    following_the_steps_finishes_the_tutorial();
    printf("PASS following_the_steps_finishes_the_tutorial\n");
    steps_done_early_are_skipped();
    printf("PASS steps_done_early_are_skipped\n");

    printf("all tutorial tests passed\n");
    return 0;
}
