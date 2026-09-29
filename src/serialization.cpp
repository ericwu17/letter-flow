// serialization.cpp — save/load the complete simulation state as a JSON file.
// Pure simulation layer: no imgui/SDL/Metal headers (see game_types.h).
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

namespace {

// Bump whenever the save layout changes incompatibly; files with any other
// version are rejected on load rather than half-parsed.
constexpr int kSaveFormatVersion = 1;

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
         {"deadline", letter.deadline},
         {"value", letter.value},
         {"fine", letter.fine}};
}

void from_json(const nlohmann::json& j, Letter& letter) {
    j.at("src").get_to(letter.src);
    j.at("dst").get_to(letter.dst);
    j.at("deadline").get_to(letter.deadline);
    j.at("value").get_to(letter.value);
    j.at("fine").get_to(letter.fine);
}

void to_json(nlohmann::json& j, const RoutingRule& rule) {
    j = {{"type", routing_rule_type_to_string(rule.type)},
         {"prefix", rule.prefix}};
}

void from_json(const nlohmann::json& j, RoutingRule& rule) {
    rule.type = routing_rule_type_from_string(j.at("type").get<std::string>());
    rule.prefix = j.value("prefix", std::string());  // only meaningful for PostalPrefix
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
         {"outbound_letters", office.outbound_letters},
         {"outbound_schedules", office.outbound_schedules}};
}

void from_json(const nlohmann::json& j, PostOffice& office) {
    j.at("name").get_to(office.name);
    j.at("postal_code").get_to(office.postal_code);
    j.at("pos").get_to(office.pos);
    j.at("max_outbound_letters").get_to(office.max_outbound_letters);
    j.at("letters_per_day").get_to(office.letters_per_day);
    j.at("outbound_letters").get_to(office.outbound_letters);
    j.at("outbound_schedules").get_to(office.outbound_schedules);
}

namespace {

// Cross-reference and invariant check for a freshly loaded world. nlohmann
// already guarantees the JSON shape; this catches structurally valid but
// logically broken saves (hand-edited or truncated files): dangling office
// references, and schedules violating the invariants add_schedule() enforces
// (dst is another office, period > 0, non-empty postal prefix).
bool loaded_state_is_valid(const std::vector<PostOffice>& post_offices,
                           const std::vector<Truck>& trucks) {
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
            if (schedule.rule.type == RoutingRuleType::PostalPrefix &&
                schedule.rule.prefix.empty())
                return false;
        }
    }
    for (const Truck& truck : trucks)
        if (truck.get_src() >= num_offices || truck.get_dst() >= num_offices)
            return false;
    return true;
}

}  // namespace

// Friend of Truck (see truck.h), at global scope so the friendship declared
// in truck.h refers to exactly this type. Saving reads the private motion
// fields directly; loading rebuilds a truck through its public constructor.
// (Trucks have no default constructor, so they cannot use the vector-wide
// from_json path above and are converted one by one in load_from_file.)
struct TruckSerializer {
    static nlohmann::json to_json(const Truck& truck) {
        return {{"src", truck.src},
                {"dst", truck.dst},
                {"from", truck.from},
                {"to", truck.to},
                {"departure_tick", truck.departure_tick},
                {"speed", truck.speed},
                {"carried_letters", truck.carried_letters}};
    }

    static Truck from_json(const nlohmann::json& j) {
        Truck truck(j.at("src").get<PostOfficeId>(),
                    j.at("dst").get<PostOfficeId>(),
                    j.at("from").get<Position>(),
                    j.at("to").get<Position>(),
                    j.at("departure_tick").get<Tick>(),
                    j.at("speed").get<float>());
        for (const Letter& letter : j.at("carried_letters").get<std::vector<Letter>>())
            truck.push_back_letter(letter);
        return truck;
    }
};

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

    nlohmann::json trucks_json = nlohmann::json::array();
    for (const Truck& truck : trucks)
        trucks_json.push_back(TruckSerializer::to_json(truck));
    j["trucks"] = std::move(trucks_json);

    // The RNG's complete internal state, dumped with the standard-library
    // operator<< (624 state words plus the position index, as decimal text)
    // and restored with operator>> on load. This is what makes the
    // simulation continue the identical random sequence after loading.
    std::ostringstream rng_state;
    rng_state << rng;
    j["rng"] = rng_state.str();

    // Write to a temp file, then rename over the target. rename() is atomic
    // on macOS, so a crash or power loss mid-write leaves the previous save
    // intact instead of a half-written one.
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
    std::ifstream in(path);
    if (!in)
        return std::nullopt;  // no save file yet: caller starts a fresh world

    try {
        const nlohmann::json j = nlohmann::json::parse(in);

        if (j.at("format_version").get<int>() != kSaveFormatVersion)
            return std::nullopt;  // incompatible save version

        World world;
        j.at("current_tick").get_to(world.current_tick);
        j.at("paused").get_to(world.paused);
        j.at("speed_multiplier").get_to(world.speed_multiplier);
        j.at("money").get_to(world.money);
        j.at("letters_delivered_on_time").get_to(world.letters_delivered_on_time);
        j.at("letters_delivered_late").get_to(world.letters_delivered_late);
        j.at("post_offices").get_to(world.post_offices);

        const nlohmann::json& trucks_json = j.at("trucks");
        if (!trucks_json.is_array())
            return std::nullopt;
        for (const nlohmann::json& truck_json : trucks_json)
            world.trucks.push_back(TruckSerializer::from_json(truck_json));

        // Restore the RNG's full internal state (see save_to_file). operator>>
        // validates the dump and sets failbit on malformed input.
        std::istringstream rng_state(j.at("rng").get<std::string>());
        rng_state >> world.rng;
        if (rng_state.fail())
            return std::nullopt;

        if (!loaded_state_is_valid(world.post_offices, world.trucks))
            return std::nullopt;

        return world;
    } catch (const std::exception&) {
        // Any problem — malformed JSON, missing or mis-typed keys, unknown
        // enum strings, out-of-range numbers — means "no usable save": the
        // caller falls back to a fresh world instead of crashing or loading
        // a half-built one.
        return std::nullopt;
    }
}
