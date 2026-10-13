// serialization.cpp — save/load the complete simulation state as a JSON file.
// Pure simulation layer: no imgui/SDL/OpenGL headers (see game_types.h).
//
// Implements World::save_to_file / World::load_from_file (declared in
// world.h). This is the only translation unit that knows nlohmann/json exists
// (single-header library vendored under third_party/).
//
// The to_json/from_json overloads below live in the global namespace, right
// next to the entity types they convert — that is where nlohmann finds them
// through argument-dependent lookup when dumping or parsing entities and
// vectors of entities.
//
// Save files embed the mt19937's full internal state (all 624 words), so a
// loaded game continues the exact same random sequence as the saved one:
// determinism survives save/load, and two copies of a save simulate
// identically forever.
#include "world.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <iostream>
#include <memory>

namespace {

// Bump whenever the save layout changes incompatibly; files with any other
// version are rejected on load rather than half-parsed.
// v2: roads added; TruckState stores a route polyline (vertices/distances)
//     instead of straight-line from/to.
// v3: post offices store build_cost and built.
constexpr int kSaveFormatVersion = 3;

// RoutingRuleType is saved as a readable string rather than a number, so
// reordering or extending the enum can never silently misread old saves. An
// unknown string fails the whole load (see load_from_file's catch-all).
const char* routing_rule_type_to_string(RoutingRuleType type) {
    switch (type) {
    case RoutingRuleType::ExactDestination: return "exact_destination";
    case RoutingRuleType::PostalPrefix:     return "postal_prefix";
    case RoutingRuleType::AllLetters:       return "all_letters";
    }
    throw std::runtime_error("unknown RoutingRuleType");  // unreachable
}

RoutingRuleType routing_rule_type_from_string(const std::string& text) {
    if (text == "exact_destination") return RoutingRuleType::ExactDestination;
    if (text == "postal_prefix")     return RoutingRuleType::PostalPrefix;
    if (text == "all_letters")       return RoutingRuleType::AllLetters;
    throw std::runtime_error("unknown routing rule type in save file: " + text);
}

}  // namespace

// ---------------------------------------------------------------------------
// Entity conversions (global namespace — nlohmann's ADL hooks)
// ---------------------------------------------------------------------------

void to_json(nlohmann::json& j, const Position& pos) {
    j = {{"x", pos.x}, {"y", pos.y}};
}

void from_json(const nlohmann::json& j, Position& pos) {
    j.at("x").get_to(pos.x);
    j.at("y").get_to(pos.y);
}

void to_json(nlohmann::json& j, const Letter& letter) {
    j = {{"src", letter.src},
         {"dst", letter.dst},
         {"created", letter.created},
         {"deadline", letter.deadline},
         {"value", letter.value},
         {"max_bonus", letter.max_bonus}};
}

void from_json(const nlohmann::json& j, Letter& letter) {
    j.at("src").get_to(letter.src);
    j.at("dst").get_to(letter.dst);
    j.at("created").get_to(letter.created);
    j.at("deadline").get_to(letter.deadline);
    j.at("value").get_to(letter.value);
    j.at("max_bonus").get_to(letter.max_bonus);
}

void to_json(nlohmann::json& j, const RoutingRule& rule) {
    j = {{"type", routing_rule_type_to_string(rule.type)},
         {"prefixes", rule.prefixes}};
}

void from_json(const nlohmann::json& j, RoutingRule& rule) {
    rule.type = routing_rule_type_from_string(j.at("type").get<std::string>());
    // Only meaningful for PostalPrefix. Saves from before multi-prefix rules
    // hold a single "prefix" string instead of the list.
    rule.prefixes.clear();
    if (j.contains("prefixes")) {
        j.at("prefixes").get_to(rule.prefixes);
    } else {
        std::string legacy_prefix = j.value("prefix", std::string());
        if (!legacy_prefix.empty())
            rule.prefixes.push_back(std::move(legacy_prefix));
    }
}

void to_json(nlohmann::json& j, const TruckSchedule& schedule) {
    j = {{"dst", schedule.dst},
         {"rule", schedule.rule},
         {"period", schedule.period},
         {"start_offset", schedule.start_offset},
         {"next_departure", schedule.next_departure}};
}

void from_json(const nlohmann::json& j, TruckSchedule& schedule) {
    j.at("dst").get_to(schedule.dst);
    j.at("rule").get_to(schedule.rule);
    j.at("period").get_to(schedule.period);
    j.at("start_offset").get_to(schedule.start_offset);
    j.at("next_departure").get_to(schedule.next_departure);
}

void to_json(nlohmann::json& j, const PostOffice& office) {
    j = {{"name", office.name},
         {"postal_code", office.postal_code},
         {"pos", office.pos},
         {"max_outbound_letters", office.max_outbound_letters},
         {"letters_per_day", office.letters_per_day},
         {"build_cost", office.build_cost},
         {"built", office.built},
         {"outbound_letters", office.outbound_letters},
         {"outbound_schedules", office.outbound_schedules}};
}

void from_json(const nlohmann::json& j, PostOffice& office) {
    j.at("name").get_to(office.name);
    j.at("postal_code").get_to(office.postal_code);
    j.at("pos").get_to(office.pos);
    j.at("max_outbound_letters").get_to(office.max_outbound_letters);
    j.at("letters_per_day").get_to(office.letters_per_day);
    j.at("build_cost").get_to(office.build_cost);
    j.at("built").get_to(office.built);
    j.at("outbound_letters").get_to(office.outbound_letters);
    j.at("outbound_schedules").get_to(office.outbound_schedules);
}

void to_json(nlohmann::json& j, const Road& road) {
    j = {{"src", road.src},
         {"dst", road.dst},
         {"vertices", road.vertices}};
}

void from_json(const nlohmann::json& j, Road& road) {
    j.at("src").get_to(road.src);
    j.at("dst").get_to(road.dst);
    j.at("vertices").get_to(road.vertices);
}

// TruckState converts like any other entity; Truck itself goes through it
// (get_state() / the state constructor) because it has no default
// constructor, so it cannot use the vector-wide from_json path — trucks are
// converted one by one in save_to_file / load_from_file.
void to_json(nlohmann::json& j, const TruckState& truck) {
    j = {{"src", truck.src},
         {"dst", truck.dst},
         {"vertices", truck.vertices},
         {"distances", truck.distances},
         {"departure_tick", truck.departure_tick},
         {"speed", truck.speed},
         {"carried_letters", truck.carried_letters}};
}

void from_json(const nlohmann::json& j, TruckState& truck) {
    j.at("src").get_to(truck.src);
    j.at("dst").get_to(truck.dst);
    j.at("vertices").get_to(truck.vertices);
    j.at("distances").get_to(truck.distances);
    j.at("departure_tick").get_to(truck.departure_tick);
    j.at("speed").get_to(truck.speed);
    j.at("carried_letters").get_to(truck.carried_letters);
}

namespace {

// Cross-reference and invariant check for a freshly loaded world. nlohmann
// already guarantees the JSON shape; this catches structurally valid but
// logically broken saves (hand-edited or truncated files): dangling office
// references, schedules violating the invariants add_schedule() enforces
// (dst is another office, period > 0, non-empty distinct postal prefixes), and roads
// violating the invariants add_road() enforces (normalized src < dst, no
// duplicates, both endpoint offices built, at least two in-range vertices
// anchored at the endpoint offices).
bool loaded_state_is_valid(const std::vector<PostOffice>& post_offices,
                           const std::vector<std::shared_ptr<Truck>>& trucks,
                           const std::vector<Road>& roads) {
    const PostOfficeId num_offices = post_offices.size();
    for (PostOfficeId office_id = 0; office_id < num_offices; ++office_id) {
        const PostOffice& office = post_offices[office_id];
        for (const Letter& letter : office.outbound_letters)
            if (letter.src >= num_offices || letter.dst >= num_offices)
                return false;
        for (const TruckSchedule& schedule : office.outbound_schedules) {
            if (schedule.dst >= num_offices || schedule.dst == office_id)
                return false;
            if (schedule.period == 0)
                return false;
            if (!schedule.rule.is_well_formed())
                return false;
        }
    }
    for (const std::shared_ptr<Truck>& truck : trucks) {
        if (truck->get_src() >= num_offices || truck->get_dst() >= num_offices)
            return false;
        if (truck->get_state().distances.size() != truck->get_state().vertices.size() - 1) 
            return false;
    }
    for (std::size_t i = 0; i < roads.size(); ++i) {
        const Road& road = roads[i];
        if (road.src >= num_offices || road.dst >= num_offices || road.src >= road.dst)
            return false;
        if (!post_offices[road.src].built || !post_offices[road.dst].built)
            return false;
        if (road.vertices.size() < 2 || !all_vertices_in_range(road.vertices))
            return false;
        if (road.vertices.front() != post_offices[road.src].pos ||
            road.vertices.back() != post_offices[road.dst].pos)
            return false;
        for (std::size_t j = i + 1; j < roads.size(); ++j)
            if (roads[j].src == road.src && roads[j].dst == road.dst)
                return false;  // duplicate road between the same offices
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// World::save_to_file / World::load_from_file
// ---------------------------------------------------------------------------

bool World::save_to_file(const std::string& path) const {
    nlohmann::json j;
    j["format_version"] = kSaveFormatVersion;
    j["current_tick"] = current_tick;
    j["paused"] = paused;
    j["speed_multiplier"] = speed_multiplier;
    j["money"] = money;
    j["letters_delivered_on_time"] = letters_delivered_on_time;
    j["letters_delivered_late"] = letters_delivered_late;
    j["post_offices"] = post_offices;
    j["roads"] = roads;

    nlohmann::json trucks_json = nlohmann::json::array();
    for (const std::shared_ptr<Truck>& truck : trucks)
        trucks_json.push_back(truck->get_state());
    j["trucks"] = std::move(trucks_json);

    // The RNG's complete internal state, dumped with the standard-library
    // operator<< (624 state words plus the position index, as decimal text)
    // and restored with operator>> on load. This is what makes the
    // simulation continue the identical random sequence after loading.
    std::ostringstream rng_state;
    rng_state << rng;
    j["rng"] = rng_state.str();

    // Write to a temp file, then rename over the target, so a crash mid-write
    // leaves the previous save intact instead of a half-written one. (Not
    // proof against power loss: nothing fsyncs the file before the rename.)
    const std::string tmp_path = path + ".tmp";
    std::error_code ec;
    {
        std::ofstream out(tmp_path);
        if (!out)
            return false;
        out << j.dump(2) << '\n';
        out.flush();
        if (!out) {
            std::filesystem::remove(tmp_path, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    return true;
}

std::optional<World> World::load_from_file(const std::string& path) {
    using std::cerr;
    using std::endl;

    std::ifstream in(path);
    if (!in) {
        cerr << "No save file found at " << path << "." << endl;
        return std::nullopt;
    }

    try {
        const nlohmann::json j = nlohmann::json::parse(in);

        if (j.at("format_version").get<int>() != kSaveFormatVersion) {
            cerr << "Incompatible save version." << endl;
            return std::nullopt;
        }

        World world;
        j.at("current_tick").get_to(world.current_tick);
        j.at("paused").get_to(world.paused);
        j.at("speed_multiplier").get_to(world.speed_multiplier);
        j.at("money").get_to(world.money);
        j.at("letters_delivered_on_time").get_to(world.letters_delivered_on_time);
        j.at("letters_delivered_late").get_to(world.letters_delivered_late);
        j.at("post_offices").get_to(world.post_offices);
        j.at("roads").get_to(world.roads);

        const nlohmann::json& trucks_json = j.at("trucks");
        if (!trucks_json.is_array()) {
            cerr << "trucks is not an array." << endl;
            return std::nullopt;
        }
        for (const nlohmann::json& truck_json : trucks_json) {
            world.trucks.push_back(std::make_shared<Truck>(truck_json.get<TruckState>()));
        }

        // Restore the RNG's full internal state (see save_to_file). operator>>
        // validates the dump and sets failbit on malformed input.
        std::istringstream rng_state(j.at("rng").get<std::string>());
        rng_state >> world.rng;
        if (rng_state.fail()) {
            cerr << "rng failed. Using a new RNG." << endl;
        }

        if (!loaded_state_is_valid(world.post_offices, world.trucks, world.roads)) {
            cerr << "loaded state not valid" << endl;
            return std::nullopt;
        }

        return world;
    } catch (const std::exception& e) {
        // Any problem — malformed JSON, missing or mis-typed keys, unknown
        // enum strings, out-of-range numbers — means "no usable save": the
        // caller falls back to a fresh world instead of crashing or loading
        // a half-built one.
        cerr << "caught exception" << e.what() << endl;
        return std::nullopt;
    }
}
