// test_serialization.cpp — headless round-trip tests for the World <-> JSON
// conversion (WorldJson::to_json / WorldJson::from_json). Run with `make test`.
// Pure simulation layer: no SDL/imgui/OpenGL — no window is needed.
//
// The core test is round_trip_preserves_every_field(): its fixture world
// deliberately holds a NON-DEFAULT value in EVERY serialized field, so a
// field forgotten in serialization.cpp comes back from a save as its default
// and the equality assert fails loudly instead of the data being quietly
// lost. Every entity's operator== (Truck's included) is defaulted and picks
// up new fields automatically; World's is hand-written (world.cpp) and must
// be extended by hand. When adding any new state, make the fixture give the
// new field a distinct value (see the note in entities.h).
//
// Everything happens in memory: worlds are dumped to JSON text exactly as
// the saves file stores them (dump(2)), edited as text where a test needs a
// corrupt or old-format save, and parsed back. The saves file itself is
// covered by test_save_library.cpp.
#include "sim/balance.h"
#include "sim/office_names.h"
#include "sim/world.h"
#include "persistence/world_json.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// Adds a road between offices a and b as an L-shaped path of two
// axis-aligned segments: vertical-then-horizontal, or horizontal-then-
// vertical per `vertical_first`. The explicit per-road choice (instead of an
// automatic pick) is what keeps the fixture's seven roads from crossing —
// see the calls in build_fixture_world().
void add_l_road(World& w, PostOfficeId a, PostOfficeId b, bool vertical_first) {
    const Position pa = w.get_post_offices()[a].pos;
    const Position pb = w.get_post_offices()[b].pos;
    const Position corner = vertical_first ? Position{pa.x, pb.y}
                                           : Position{pb.x, pa.y};
    assert(w.add_road(a, b, {pa, corner, pb}));
}

// Twelve offices of the old starter map (New Jersey), at its spacing, with
// their hierarchical postal codes. Every buffer is roomy enough that none of
// them overflows during the fixture's run (see build_fixture_world()).
World create_nj_world() {
    struct OfficeDef {
        const char* name;
        const char* postal_code;
        Position pos;
        std::size_t letters_per_day;
    };
    const OfficeDef defs[] = {
        {"Lawrenceville", "NJ/Mercer/Lawrenceville", { 918, 844}, 20},  // 0
        {"Princeton",     "NJ/Mercer/Princeton",     { 968, 782}, 30},  // 1
        {"Trenton",       "NJ/Mercer/Trenton",       { 908, 926}, 30},  // 2
        {"Hopewell",      "NJ/Mercer/Hopewell",      { 894, 748}, 20},  // 3
        {"Monroe",        "NJ/Central/Monroe",       {1154, 820}, 20},  // 4
        {"Edison",        "NJ/Central/Edison",       {1170, 614}, 30},  // 5
        {"Somerville",    "NJ/Central/Somerville",   {1014, 556}, 20},  // 6
        {"Florham Park",  "NJ/North/FlorhamPark",    {1188, 334}, 20},  // 7
        {"Newark",        "NJ/North/Newark",         {1358, 388}, 50},  // 8
        {"Paterson",      "NJ/North/Paterson",       {1358, 202}, 30},  // 9
        {"Elizabeth",     "NJ/North/Elizabeth",      {1328, 464}, 30},  // 10
        {"Asbury Park",   "NJ/Shore/AsburyPark",     {1484, 924}, 20},  // 11
    };
    World w;
    for (const OfficeDef& def : defs)
        w.add_office(def.name, def.postal_code, def.pos, def.letters_per_day, /*max_outbound_letters=*/1000);

    // Every name new offices could get, taken by an inert office far off the
    // map: no office appears on its own here. Unserved, a new office would
    // overflow and end the game a few days in (sooner if an event mails it).
    int64_t x = -100'000;
    for (const char* name : office_names()) {
        w.add_office(name, "", {x, -100'000}, 0, /*max_outbound_letters=*/100'000);
        x -= 1'000;
    }
    return w;
}

// A world as the saves file stores it.
std::string dump(const World& w) {
    return WorldJson::to_json(w).dump(2);
}

// The inverse of dump(): nullopt for text that isn't JSON, or isn't a usable world.
std::optional<World> parse(const std::string& text) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded())
        return std::nullopt;
    return WorldJson::from_json(j);
}

// A world in which every serialized field holds a distinct, non-default
// value. Fully deterministic (fixed RNG seed + fixed tick counts), so the
// fixture asserts at the end hold on every run. The game is still on: an
// ended game (game_over_office) round-trips in game_over_round_trips().
World build_fixture_world() {
    World w = create_nj_world();  // offices: names, postal codes, positions and
                                  // spawn rates all non-default

    // The fixture runs more trucks than a real opening could afford, so
    // fund it directly instead of depending on kStartingMoney.
    w.set_money(100'000);
    w.seed_letters();

    // Non-default transport state. (advance_tick() ignores these — the frame
    // loop honors them — but they are part of the saved state.)
    w.set_paused(true);                 // default: false
    w.set_speed_multiplier(4.0f);       // default: 1.0

    // Roads: every schedule below requires a road between its endpoints
    // (add_schedule checks has_road). The seven L-shaped roads form a
    // non-crossing chain Hopewell - Princeton - Lawrenceville - Trenton plus
    // Princeton - Monroe - Edison - Florham Park - Asbury Park; the per-road
    // vertical_first choices below are the ones that avoid crossings
    // (e.g. Lawrenceville -> Princeton must arrive from the west, since its
    // northern and southern approaches are taken).
    add_l_road(w, 3, 1, /*vertical_first=*/false);  // Hopewell -> Princeton
    add_l_road(w, 0, 1, /*vertical_first=*/true);   // Lawrenceville -> Princeton
    add_l_road(w, 0, 2, /*vertical_first=*/false);  // Lawrenceville -> Trenton
    add_l_road(w, 1, 4, /*vertical_first=*/true);   // Princeton -> Monroe
    add_l_road(w, 4, 5, /*vertical_first=*/false);  // Monroe -> Edison
    add_l_road(w, 5, 7, /*vertical_first=*/false);  // Edison -> Florham Park
    add_l_road(w, 7, 11, /*vertical_first=*/false); // Florham Park -> Asbury Park

    // All three routing-rule types, distinct periods and start offsets;
    // office 0 ends up with three schedules, so schedule list order (the
    // pick-up priority) is exercised too. Periods stay modest: every
    // departure costs kTruckDepartureCost, and the late routes below must
    // still be affordable three days in.
    const RoutingRule prefix_rule{RoutingRuleType::PostalPrefix, {"NJ/Mercer", "PA"}};
    const RoutingRule all_rule{RoutingRuleType::AllLetters, {}};
    assert(w.add_schedule(0, 1, 6 * kTicksPerHour, 0, {}));            // exact
    assert(w.add_schedule(0, 2, 4 * kTicksPerHour, 13, prefix_rule));  // prefix
    assert(w.add_schedule(3, 1, 8 * kTicksPerHour, 7, all_rule));      // all

    // Force some late deliveries so letters_delivered_late is non-zero:
    // AllLetters schedules whose first departure is later than any waiting
    // letter's deadline can be (deadlines are at most two days after spawn).
    // Every src below except office 0 — which also has the six-hourly exact
    // route to office 1 added above — has no earlier departures, so its
    // trucks arrive with guaranteed-late cargo. Each route follows one of
    // the roads built above.
    const Tick late_start = 3 * kTicksPerDay;
    constexpr std::pair<PostOfficeId, PostOfficeId> kLateRoutes[] = {
        {0, 1}, {2, 0}, {5, 4}, {7, 5}, {11, 7}};
    for (const auto& [src, dst] : kLateRoutes)
        assert(w.add_schedule(src, dst, kTicksPerDay, late_start, all_rule));

    // Network events, brought forward (the first would otherwise wait four
    // days): one runs its course early on, so events_so_far is non-zero, and
    // a second is still running when the fixture is done, with its letters
    // in the buffers.
    w.schedule_next_event(1000);

    // Retune one schedule a day in, so its edit log is non-empty too.
    for (Tick t = 0; t < kTicksPerDay; ++t)
        w.advance_tick();
    assert(w.update_schedule(3, 0, 7 * kTicksPerHour, 7, all_rule));

    while (w.get_event().has_value())
        w.advance_tick();
    assert(w.get_events_so_far() == 1);                          // default: 0
    w.schedule_next_event(2 * kTicksPerDay);  // runs from day 2.5 to day 3.5

    // Run past the late batch's arrivals (the longest of these routes is
    // in flight for well under 500 ticks).
    while (w.get_tick() < late_start + 500)
        w.advance_tick();

    // Step until at least one truck is in flight so Truck state is exercised
    // (departures only spawn trucks when matching cargo is waiting).
    for (int i = 0; i < 20000 && w.get_trucks().empty(); ++i)
        w.advance_tick();

    // An office that fills up at once (a letter every tick into a buffer of
    // five), run just long enough to start its overflow timer: far short of
    // kOverflowLimit, and of any truck in flight arriving (the shortest
    // road above takes well over 20 ticks to drive).
    const PostOfficeId flooded = w.add_office("Flooded", "NJ/Flooded", {700, 1100}, kTicksPerDay,
                                              /*max_outbound_letters=*/5);
    for (int i = 0; i < 20; ++i)
        w.advance_tick();
    assert(w.get_post_offices()[flooded].overflow_ticks > 0);   // default: 0
    assert(!w.is_game_over());

    // One schedule switched off (its road, Hopewell - Princeton, stays).
    assert(w.set_schedule_enabled(3, 0, false));

    // The distinctness guarantees the round-trip test relies on: a forgotten
    // field would revert to a default that differs from every value below.
    assert(w.get_tick() > 0);                        // default: 0
    assert(w.is_paused());                           // default: false
    assert(w.get_speed_multiplier() == 4.0f);        // default: 1.0
    assert(w.get_money() != kStartingMoney);         // default: kStartingMoney
    assert(w.get_letters_delivered_on_time() > 0);   // default: 0
    assert(w.get_letters_delivered_late() > 0);      // default: 0
    assert(!w.get_trucks().empty());                 // default: empty
    assert(!w.get_roads().empty());                  // default: empty
    assert(w.get_next_office_spawn() != kOfficeSpawnIntervalStart);  // default: that
    assert(w.get_event().has_value() && w.get_event()->posted > 0);   // default: none
    assert(w.get_next_event_announce() != kNever);                     // default: kNever

    // Statistics and trails (stats.h, Letter::trail): every kind of stat
    // table has been written to, and every log holds entries.
    assert(w.get_stats() != GlobalStats{});
    assert(!w.get_recent_deliveries().empty());
    bool office_stats = false, schedule_stats = false, departure_log = false, edits = false,
         schedule_ids = false, trails = false, departed_stop = false, disabled = false;
    bool event_letters = false;
    for (const PostOffice& office : w.get_post_offices()) {
        office_stats |= office.stats != OfficeStats{};
        for (const Letter& letter : office.outbound_letters) {
            trails |= !letter.trail.empty();
            event_letters |= letter.event;
        }
        for (const TruckSchedule& schedule : office.outbound_schedules) {
            schedule_stats |= schedule.stats != ScheduleStats{};
            departure_log |= !schedule.departure_log.empty();
            edits |= !schedule.edits.empty();
            schedule_ids |= schedule.id != 0;  // default: kNoSchedule, and the first id is 0
            disabled |= !schedule.enabled;     // default: true
        }
    }
    bool truck_schedule = false;
    for (const auto& truck : w.get_trucks()) {
        truck_schedule |= truck->get_schedule() != kNoSchedule;
        for (const Letter& letter : truck->get_letters())
            for (const TrailStop& stop : letter.trail)
                departed_stop |= stop.schedule != kNoSchedule;
    }
    assert(office_stats && schedule_stats && departure_log && edits && schedule_ids && disabled);
    assert(trails && departed_stop && truck_schedule && event_letters);
    return w;
}

// The fixture world, built once (it simulates days of a 300-office world)
// and handed out as a fresh load of its save each time: copying a World
// would share its trucks between the copies.
World fixture_world() {
    static const std::string saved = dump(build_fixture_world());
    return *parse(saved);
}

// --- the tests -------------------------------------------------------------

void round_trip_preserves_every_field() {
    const World original = build_fixture_world();
    const std::string saved = dump(original);

    const auto loaded = parse(saved);
    assert(loaded.has_value());

    // The core check: full-state equality. Any field the fixture populated
    // but serialization.cpp forgot comes back as its default -> fails here.
    assert(*loaded == original);

    // Re-saving a loaded world must be byte-identical (nothing drifts
    // through the parse/dump cycle).
    assert(dump(*loaded) == saved);

    // Sanity check on the checker: equality must actually discriminate, or
    // the asserts above would prove nothing.
    World advanced = *loaded;
    advanced.advance_tick();
    assert(advanced != *loaded);
}

// An ended game saves and loads as ended, still frozen.
void game_over_round_trips() {
    World w;
    const PostOfficeId a = w.add_office("A", "P/A", {0, 0}, kTicksPerDay, /*max_outbound_letters=*/1);
    w.add_office("B", "P/B", {100, 0}, 0, 100);
    for (Tick t = 0; t < 2 * kOverflowLimit && !w.is_game_over(); ++t)
        w.advance_tick();
    assert(w.get_game_over_office() == a);

    const std::string saved = dump(w);
    const auto loaded = parse(saved);
    assert(loaded.has_value());
    assert(*loaded == w);
    assert(loaded->is_game_over());
    assert(dump(*loaded) == saved);

    // A game-over office that does not exist makes the save unusable.
    std::string s = saved;
    const auto at = s.find("\"game_over_office\": 0");
    assert(at != std::string::npos);
    s.replace(at, 21, "\"game_over_office\": 99");
    assert(!parse(s));
}

void determinism_survives_round_trip() {
    World a = fixture_world();
    auto b = parse(dump(a));
    assert(b.has_value());

    // Simulate both copies for two more in-game days: with the RNG state
    // restored, the loaded copy must follow the identical trajectory.
    for (Tick t = 0; t < 2 * kTicksPerDay; ++t) {
        a.advance_tick();
        b->advance_tick();
    }
    assert(a == *b);
    assert(dump(a) == dump(*b));
}

void unusable_saves_load_as_nullopt() {
    // Start from a known-good save to corrupt.
    const World original = fixture_world();
    const std::string good_json = dump(original);

    // Not JSON at all.
    assert(!parse("{this is not json"));

    // Truncated mid-file.
    assert(!parse(good_json.substr(0, 500)));

    // JSON, but not an object.
    assert(!parse("[1, 2, 3]"));

    // Incompatible save version.
    assert(!parse(R"({"format_version": 999})"));

    // Unknown routing-rule string.
    {
        std::string s = good_json;
        const auto at = s.find("\"exact_destination\"");
        assert(at != std::string::npos);
        s.replace(at, 19, "\"not_a_real_rule__\"");  // same length, still valid JSON
        assert(!parse(s));
    }

    // A postal-prefix rule with an empty prefix in its list.
    {
        std::string s = good_json;
        const auto at = s.find("\"NJ/Mercer\"");
        assert(at != std::string::npos);
        s.replace(at, 11, "\"\"");
        assert(!parse(s));
    }

    // An event aimed at an office that does not exist.
    {
        nlohmann::json j = nlohmann::json::parse(good_json);
        assert(j.contains("event"));
        j["event"]["dst"] = 9999;
        assert(!WorldJson::from_json(j));
    }

    // A schedule switched on with no road to run on. Hopewell's schedule to
    // Princeton is off in the fixture, so dropping their road still loads;
    // switching the schedule on as well does not.
    {
        nlohmann::json j = nlohmann::json::parse(good_json);
        nlohmann::json& roads = j["roads"];
        const auto road = std::find_if(roads.begin(), roads.end(), [](const nlohmann::json& r) {
            return r["src"] == 1 && r["dst"] == 3;
        });
        assert(road != roads.end());
        roads.erase(road);
        nlohmann::json& schedule = j["post_offices"][3]["outbound_schedules"][0];
        assert(schedule["dst"] == 1 && schedule["enabled"] == false);
        assert(WorldJson::from_json(j));
        schedule["enabled"] = true;
        assert(!WorldJson::from_json(j));
    }

    // Dangling post-office reference (office 9999 does not exist).
    {
        std::string s = good_json;
        const auto at = s.find("\"dst\": 1,");
        assert(at != std::string::npos);
        s.replace(at, 9, "\"dst\": 9999,");
        assert(!parse(s));
    }
}

// Saves from before multi-prefix rules hold a single "prefix" string; it
// loads as a one-prefix list.
void legacy_single_prefix_loads() {
    const World original = fixture_world();
    std::string s = dump(original);
    const auto mercer = s.find("\"NJ/Mercer\"");
    assert(mercer != std::string::npos);
    const auto start = s.rfind("\"prefixes\"", mercer);
    const auto end = s.find(']', mercer);
    assert(start != std::string::npos && end != std::string::npos);
    s.replace(start, end + 1 - start, "\"prefix\": \"NJ/Mercer\"");

    const auto loaded = parse(s);
    assert(loaded.has_value());
    const RoutingRule legacy{RoutingRuleType::PostalPrefix, {"NJ/Mercer"}};
    bool found = false;
    for (const PostOffice& office : loaded->get_post_offices())
        for (const TruckSchedule& schedule : office.outbound_schedules)
            found |= schedule.rule == legacy;
    assert(found);
}

// Saves from before statistics, trails, schedule ids and the enabled flag
// still load: with empty history, every schedule given a distinct fresh id,
// and every schedule switched on.
void pre_stats_save_loads() {
    const World original = fixture_world();

    // Drop every line holding a key that did not exist before (dump(2) puts
    // each scalar key on its own line), then every "stats"/"trail"/log
    // value, which spans several lines: cut from its key to the matching
    // close bracket.
    std::string s = dump(original);
    std::string out;
    std::istringstream lines(s);
    for (std::string line; std::getline(lines, line);) {
        if (line.find("\"id\":") != std::string::npos ||
            line.find("\"next_schedule_id\":") != std::string::npos ||
            line.find("\"schedule\":") != std::string::npos ||
            line.find("\"enabled\":") != std::string::npos)
            continue;
        out += line + "\n";
    }
    s = out;
    for (const std::string key : {"\"stats\": ", "\"trail\": ", "\"departure_log\": ",
                                  "\"edits\": ", "\"recent_deliveries\": "}) {
        for (auto at = s.find(key); at != std::string::npos; at = s.find(key)) {
            // Find the bracket that closes the value, then the comma (or
            // newline) after it.
            std::size_t i = at + key.size();
            int depth = 0;
            do {
                if (s[i] == '{' || s[i] == '[') ++depth;
                if (s[i] == '}' || s[i] == ']') --depth;
                ++i;
            } while (depth > 0);
            if (s[i] == ',')
                ++i;
            else {
                // Last key in its object: drop the comma before it instead.
                const auto comma = s.rfind(',', at);
                s.erase(comma, 1);
                --at;
                --i;
            }
            s.erase(at, i - at);
        }
    }
    // JSON allows the trailing whitespace this leaves behind.
    const auto loaded = parse(s);
    assert(loaded.has_value());
    assert(loaded->get_stats() == GlobalStats{});
    assert(loaded->get_recent_deliveries().empty());
    std::vector<ScheduleId> ids;
    for (const PostOffice& office : loaded->get_post_offices()) {
        assert(office.stats == OfficeStats{});
        for (const Letter& letter : office.outbound_letters)
            assert(letter.trail.empty());
        for (const TruckSchedule& schedule : office.outbound_schedules) {
            assert(schedule.stats == ScheduleStats{});
            assert(schedule.enabled);
            ids.push_back(schedule.id);
        }
    }
    std::sort(ids.begin(), ids.end());
    assert(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    assert(ids.back() != kNoSchedule);

    // ...and it keeps running, recording fresh history.
    World w = *loaded;
    for (Tick t = 0; t < kTicksPerDay; ++t)
        w.advance_tick();
    assert(w.get_stats() != GlobalStats{});
}

// A dumped world minus its "rng" line (dump(2) puts each key on its own line).
std::string without_rng_line(const std::string& json) {
    const auto start = json.find("\"rng\": \"");
    assert(start != std::string::npos);
    const auto end = json.find('\n', start);
    assert(end != std::string::npos);
    return json.substr(0, start) + json.substr(end + 1);
}

// A corrupted RNG dump is not fatal: the game loads with a freshly seeded
// RNG and everything else intact (only the random sequence changes).
void corrupt_rng_falls_back_to_fresh_rng() {
    const World original = fixture_world();
    const std::string good = dump(original);
    std::string s = good;

    const auto at = s.find("\"rng\": \"");
    assert(at != std::string::npos);
    s[at + 8] = 'x';  // first character of the state dump: no longer a number

    const auto loaded = parse(s);
    assert(loaded.has_value());
    assert(*loaded != original);  // the RNG differs...

    // ...and nothing else does.
    assert(without_rng_line(dump(*loaded)) == without_rng_line(good));
}

}  // namespace

int main() {
    round_trip_preserves_every_field();
    printf("PASS round_trip_preserves_every_field\n");
    game_over_round_trips();
    printf("PASS game_over_round_trips\n");
    determinism_survives_round_trip();
    printf("PASS determinism_survives_round_trip\n");
    unusable_saves_load_as_nullopt();
    printf("PASS unusable_saves_load_as_nullopt\n");
    corrupt_rng_falls_back_to_fresh_rng();
    printf("PASS corrupt_rng_falls_back_to_fresh_rng\n");
    legacy_single_prefix_loads();
    printf("PASS legacy_single_prefix_loads\n");
    pre_stats_save_loads();
    printf("PASS pre_stats_save_loads\n");

    printf("all serialization tests passed\n");
    return 0;
}
