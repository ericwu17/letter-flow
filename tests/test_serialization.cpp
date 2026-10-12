// test_serialization.cpp — headless round-trip tests for the JSON save system
// (World::save_to_file / World::load_from_file). Run with `make test`.
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
// Test artifacts are written under build/test-saves/ (gitignored); the test
// must run with the repo root as working directory (`make test` does).
#include "balance.h"
#include "scenario.h"
#include "world.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace {

namespace fs = std::filesystem;

const fs::path kSaveDir = "build/test-saves";

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

std::string slurp(const fs::path& path) {
    std::ifstream in(path);
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream out(path);
    out << contents;
}

// A world in which every serialized field holds a distinct, non-default
// value. Fully deterministic (fixed RNG seed + fixed tick counts), so the
// fixture asserts at the end hold on every run.
World build_fixture_world() {
    World w = create_default_world();  // offices: names, postal codes, positions,
                                       // spawn rates and build costs all non-default

    // The fixture builds far more than a real opening could afford, so
    // fund it directly instead of depending on kStartingMoney.
    w.set_money(100'000);

    // Build only the offices the roads below join (paying their build costs),
    // so the save holds both built and unbuilt offices, then seed their mail.
    for (const PostOfficeId id : {0u, 1u, 2u, 3u, 4u, 5u, 7u, 11u})
        assert(w.build_office(id));
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
    const RoutingRule prefix_rule{RoutingRuleType::PostalPrefix, "NJ/Mercer"};
    const RoutingRule all_rule{RoutingRuleType::AllLetters, ""};
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

    // Run past the late batch's arrivals (the longest of these routes is
    // in flight for well under 500 ticks).
    for (Tick t = 0; t < late_start + 500; ++t)
        w.advance_tick();

    // Step until at least one truck is in flight so Truck state is exercised
    // (departures only spawn trucks when matching cargo is waiting).
    for (int i = 0; i < 20000 && w.get_trucks().empty(); ++i)
        w.advance_tick();

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
    return w;
}

// --- the tests -------------------------------------------------------------

void round_trip_preserves_every_field() {
    const World original = build_fixture_world();
    const fs::path path = kSaveDir / "fixture.json";
    assert(original.save_to_file(path.string()));

    const auto loaded = World::load_from_file(path.string());
    assert(loaded.has_value());

    // The core check: full-state equality. Any field the fixture populated
    // but serialization.cpp forgot comes back as its default -> fails here.
    assert(*loaded == original);

    // Re-saving a loaded world must be byte-identical (nothing drifts
    // through the parse/dump cycle).
    const fs::path resaved = kSaveDir / "fixture_resaved.json";
    assert(loaded->save_to_file(resaved.string()));
    assert(slurp(path) == slurp(resaved));

    // Sanity check on the checker: equality must actually discriminate, or
    // the asserts above would prove nothing.
    World advanced = *loaded;
    advanced.advance_tick();
    assert(advanced != *loaded);
}

void determinism_survives_round_trip() {
    World a = build_fixture_world();
    const fs::path path = kSaveDir / "determinism.json";
    assert(a.save_to_file(path.string()));
    auto b = World::load_from_file(path.string());
    assert(b.has_value());

    // Simulate both copies for two more in-game days: with the RNG state
    // restored, the loaded copy must follow the identical trajectory.
    for (Tick t = 0; t < 2 * kTicksPerDay; ++t) {
        a.advance_tick();
        b->advance_tick();
    }
    assert(a == *b);
    assert(a.save_to_file((kSaveDir / "determinism_a.json").string()));
    assert(b->save_to_file((kSaveDir / "determinism_b.json").string()));
    assert(slurp(kSaveDir / "determinism_a.json") == slurp(kSaveDir / "determinism_b.json"));
}

void unusable_files_load_as_nullopt() {
    // Start from a known-good save to corrupt.
    const World original = build_fixture_world();
    const fs::path good = kSaveDir / "good.json";
    assert(original.save_to_file(good.string()));
    const std::string good_json = slurp(good);

    // Missing file.
    assert(!World::load_from_file((kSaveDir / "does_not_exist.json").string()));

    // Not JSON at all.
    write_file(kSaveDir / "garbage.json", "{this is not json");
    assert(!World::load_from_file((kSaveDir / "garbage.json").string()));

    // Structurally valid JSON, truncated mid-file.
    write_file(kSaveDir / "truncated.json", good_json.substr(0, 500));
    assert(!World::load_from_file((kSaveDir / "truncated.json").string()));

    // Incompatible save version.
    write_file(kSaveDir / "bad_version.json", R"({"format_version": 999})");
    assert(!World::load_from_file((kSaveDir / "bad_version.json").string()));

    // Unknown routing-rule string.
    {
        std::string s = good_json;
        const auto at = s.find("\"exact_destination\"");
        assert(at != std::string::npos);
        s.replace(at, 19, "\"not_a_real_rule__\"");  // same length, still valid JSON
        write_file(kSaveDir / "bad_rule.json", s);
        assert(!World::load_from_file((kSaveDir / "bad_rule.json").string()));
    }

    // Dangling post-office reference (office 99 does not exist).
    {
        std::string s = good_json;
        const auto at = s.find("\"dst\": 1,");
        assert(at != std::string::npos);
        s.replace(at, 9, "\"dst\": 99,");
        write_file(kSaveDir / "bad_ref.json", s);
        assert(!World::load_from_file((kSaveDir / "bad_ref.json").string()));
    }
}

// The save file minus its "rng" line (dump(2) puts each key on its own line).
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
    const World original = build_fixture_world();
    const fs::path good = kSaveDir / "good_rng.json";
    assert(original.save_to_file(good.string()));
    std::string s = slurp(good);

    const auto at = s.find("\"rng\": \"");
    assert(at != std::string::npos);
    s[at + 8] = 'x';  // first character of the state dump: no longer a number
    write_file(kSaveDir / "bad_rng.json", s);

    const auto loaded = World::load_from_file((kSaveDir / "bad_rng.json").string());
    assert(loaded.has_value());
    assert(*loaded != original);  // the RNG differs...

    // ...and nothing else does.
    const fs::path resaved = kSaveDir / "bad_rng_resaved.json";
    assert(loaded->save_to_file(resaved.string()));
    assert(without_rng_line(slurp(resaved)) == without_rng_line(slurp(good)));
}

}  // namespace

int main() {
    fs::create_directories(kSaveDir);

    round_trip_preserves_every_field();
    printf("PASS round_trip_preserves_every_field\n");
    determinism_survives_round_trip();
    printf("PASS determinism_survives_round_trip\n");
    unusable_files_load_as_nullopt();
    printf("PASS unusable_files_load_as_nullopt\n");
    corrupt_rng_falls_back_to_fresh_rng();
    printf("PASS corrupt_rng_falls_back_to_fresh_rng\n");

    printf("all serialization tests passed\n");
    return 0;
}
