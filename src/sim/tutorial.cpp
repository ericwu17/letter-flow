// tutorial.cpp — the tutorial's step table and progress. Pure logic, no
// rendering, no input: each step's goal is a check against the World and the
// UI facts the presentation layer reports.
#include "sim/tutorial.h"

#include "sim/entities.h"
#include "sim/scenario.h"
#include "sim/world.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>

namespace {

using namespace tutorial_map;

// Whether `src` has a schedule to `dst` whose rule satisfies `pred`.
template <typename Pred>
bool has_schedule(const World& w, PostOfficeId src, PostOfficeId dst, Pred pred) {
    const std::vector<TruckSchedule>& schedules = w.get_post_offices()[src].outbound_schedules;
    return std::any_of(schedules.begin(), schedules.end(),
                       [&](const TruckSchedule& s) { return s.dst == dst && pred(s); });
}

bool has_schedule(const World& w, PostOfficeId src, PostOfficeId dst) {
    return has_schedule(w, src, dst, [](const TruckSchedule&) { return true; });
}

const TutorialStep kSteps[] = {
    {
        .title = "Welcome to Letter Flow",
        .body = "Letters are posted at post offices every day, each addressed to another office "
                "and due by a deadline. Your job is to build roads and run trucks that deliver "
                "them before the offices fill up. Every delivery pays, and early ones pay a "
                "bonus.\n\n"
                "This tutorial walks you through it on a small map of five towns. Nothing here "
                "is saved.",
    },
    {
        .title = "Getting around",
        .body = "Scroll the mouse wheel to zoom and use W A S D to pan the map.\n\n"
                "The panel on the left is the inspector: it shows whatever you select on the "
                "map. The card at the top right shows your money and deliveries.",
    },
    {
        .title = "Start a road",
        .body = "Trucks only drive on roads, and there are none yet. Press \"Build a road...\" "
                "at the bottom of the inspector to start drawing one.",
        .anchor = TutorialAnchor::BuildRoadButton,
        .done = [](const World& w, const TutorialUiFacts& f) {
            return f.building_road || w.has_road(kAshford, kCentral);
        },
    },
    {
        .title = "Lay the road",
        .body = "Click Ashford, then click Central to finish the road.\n\n"
                "Roads run straight or at 45 degrees; on longer trips, click empty map to place "
                "bends along the way. Roads are free, but can't cross each other. Esc or "
                "a right-click cancels. Clicking a road later lets you remove it.",
        .rings = {kAshford, kCentral, kNoPostOffice},
        .done = [](const World& w, const TutorialUiFacts&) { return w.has_road(kAshford, kCentral); },
    },
    {
        .title = "Start the clock",
        .body = "The game has been paused all along. Press the red play button in the bar at the "
                "top of the map (or Shift) to let time run.",
        .anchor = TutorialAnchor::TransportBar,
        .done = [](const World& w, const TutorialUiFacts&) { return !w.is_paused(); },
    },
    {
        .title = "Mail arrives",
        .body = "Select Ashford and watch its outbound letters pile up. Each one has a "
                "destination, a deadline and a value. Hover a letter to see where it has been.\n\n"
                "The bar above the list fills as the office runs out of room: a full office "
                "turns new letters away.",
        .office = kAshford,
        .anchor = TutorialAnchor::OutboundLetters,
        .done = [](const World& w, const TutorialUiFacts&) {
            return w.get_post_offices()[kAshford].outbound_letters.size() >= 3 ||
                   has_schedule(w, kAshford, kCentral);
        },
    },
    {
        .title = "Don't let it overflow",
        .body = "A full office flashes red, and a red ring around it starts filling up. If an "
                "office stays full until its ring closes, the game is over. Once the office has "
                "room again, the ring slowly drains.\n\n"
                "New offices also keep appearing on the map as time goes by, each one more mail "
                "to move.",
    },
    {
        .title = "Create a schedule",
        .body = "A truck schedule sends trucks on a regular timetable. Under Ashford's schedule "
                "table, click \"+ New schedule\" and choose Central. New schedules pick up mail "
                "for their destination and leave every 6 hours, starting at midnight.\n\n"
                "Schedules are free, but every truck costs $15 when it leaves and carries at "
                "most 5 letters.",
        .office = kAshford,
        .anchor = TutorialAnchor::AddScheduleButton,
        .done = [](const World& w, const TutorialUiFacts&) { return has_schedule(w, kAshford, kCentral); },
    },
    {
        .title = "First delivery",
        .body = "The next truck leaves at the schedule's next departure time. Use the >> button "
                "in the top bar (or the period key) to fast-forward, and watch the red truck drive to Central. When "
                "it arrives, the letters are delivered and you get paid.",
        .anchor = TutorialAnchor::TransportBar,
        .done = [](const World& w, const TutorialUiFacts&) {
            return w.get_letters_delivered_on_time() + w.get_letters_delivered_late() >= 1;
        },
    },
    {
        .title = "Inspect a truck",
        .body = "Click a truck on the road while it is driving to see its route, its arrival "
                "time and the letters it carries. Trucks leave Ashford every 6 hours.",
        .done = [](const World&, const TutorialUiFacts& f) { return f.truck_selected; },
    },
    {
        .title = "Update a schedule",
        .body = "Six hours is a long wait for a busy office, and schedules can be changed at any "
                "time. Select Ashford and, in its schedule table, set \"Every\" to 01:00 by "
                "typing into its hour and minute fields.\n\n"
                "Busy routes need more trucks; quiet ones waste $15 per empty run.",
        .office = kAshford,
        .anchor = TutorialAnchor::ScheduleTable,
        .done = [](const World& w, const TutorialUiFacts&) {
            for (const PostOffice& office : w.get_post_offices())
                for (const TruckSchedule& s : office.outbound_schedules)
                    if (!s.edits.empty())
                        return true;
            return false;
        },
    },
    {
        .title = "Grow the network",
        .body = "Brookside, Dunmore and Elmwood are piling up mail too. Build the roads Central "
                "to Brookside, Central to Dunmore, and Dunmore to Elmwood.\n\n"
                "Note that Elmwood only connects through Dunmore.",
        .rings = {kBrookside, kDunmore, kElmwood},
        .done = [](const World& w, const TutorialUiFacts&) {
            return w.has_road(kCentral, kBrookside) && w.has_road(kCentral, kDunmore) &&
                   w.has_road(kDunmore, kElmwood);
        },
    },
    {
        .title = "Hub and spoke",
        .body = "Ashford's mail is now addressed to every town, but its trucks only take mail "
                "for Central. In Ashford's schedule table, change \"Picks up\" to \"All mail\".\n\n"
                "Letters that arrive at an office they aren't addressed to wait there to be "
                "sent on, so Central becomes a hub for the whole map.",
        .office = kAshford,
        .anchor = TutorialAnchor::ScheduleTable,
        .done = [](const World& w, const TutorialUiFacts&) {
            return has_schedule(w, kAshford, kCentral, [](const TruckSchedule& s) {
                return s.rule.type == RoutingRuleType::AllLetters;
            });
        },
    },
    {
        .title = "Postal codes",
        .body = "Every office has a postal code, shown under its name on the map. Codes are "
                "hierarchical: Dunmore is SOUTH/Dunmore, Ashford is NORTH/Ashford.\n\n"
                "A schedule can pick up letters by postal-code prefix: \"SOUTH/\" collects mail "
                "for every office in the south at once.",
    },
    {
        .title = "Fix a postal code",
        .body = "Elmwood is in the south, but it was filed as ELM-01, so a \"SOUTH/\" schedule "
                "would miss its mail. Select Elmwood, open its Settings section and change its "
                "postal code to SOUTH/Elmwood.",
        .office = kElmwood,
        .anchor = TutorialAnchor::PostalCodeInput,
        .done = [](const World& w, const TutorialUiFacts&) {
            return w.get_post_offices()[kElmwood].postal_code.starts_with("SOUTH/");
        },
    },
    {
        .title = "Prefix routing",
        .body = "Now send the south's mail from Central in one truck. Select Central, add a "
                "schedule to Dunmore with \"+ New schedule\", then change its \"Picks up\" to "
                "\"Postal prefix\" and enter SOUTH/.\n\n"
                "It collects letters for both Dunmore and Elmwood.",
        .office = kCentral,
        .anchor = TutorialAnchor::AddScheduleButton,
        .done = [](const World& w, const TutorialUiFacts&) {
            const std::vector<PostOffice>& offices = w.get_post_offices();
            return has_schedule(w, kCentral, kDunmore, [&](const TruckSchedule& s) {
                return s.rule.type == RoutingRuleType::PostalPrefix &&
                       s.rule.matches(kElmwood, offices[kElmwood].postal_code, kDunmore);
            });
        },
    },
    {
        .title = "The last leg",
        .body = "Elmwood's letters now wait at Dunmore. Select Dunmore and add a schedule to "
                "Elmwood to carry them the rest of the way.",
        .office = kDunmore,
        .anchor = TutorialAnchor::AddScheduleButton,
        .done = [](const World& w, const TutorialUiFacts&) { return has_schedule(w, kDunmore, kElmwood); },
    },
    {
        .title = "Who picks up first",
        .body = "An office can run several schedules. When two leave at the same moment and both "
                "want a letter, the one higher in the schedule table takes it. Drag a row by its "
                "# grip to reorder.\n\n"
                "Remove a schedule with its x button.",
    },
    {
        .title = "Statistics",
        .body = "Press L, or the Statistics button top right, to see charts of money, mail and "
                "how full each schedule's trucks run.",
        .anchor = TutorialAnchor::StatsButton,
        .done = [](const World&, const TutorialUiFacts& f) { return f.stats_open; },
    },
    {
        .title = "You're ready",
        .body = "That's everything: build roads, run schedules, sort mail with postal codes, "
                "and keep an eye on deadlines, costs and full offices.\n\n"
                "Start a real game from Play on the main menu. It opens with three towns and a "
                "tighter budget, and new towns keep appearing, with no postal codes until you "
                "give them some.",
    },
};

}  // namespace

void Tutorial::update(const World& world, const TutorialUiFacts& facts) {
    while (!finished() && current().done != nullptr && current().done(world, facts))
        ++step;
}

void Tutorial::next() {
    if (!finished())
        ++step;
}

const TutorialStep& Tutorial::current() const {
    assert(!finished());
    return kSteps[step];
}

std::size_t Tutorial::count() { return std::size(kSteps); }
