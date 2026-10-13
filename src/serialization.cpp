// serialization.cpp — convert the complete simulation state to and from JSON.
// Pure simulation layer: no imgui/SDL/OpenGL headers (see game_types.h).
//
// Implements WorldJson::to_json / WorldJson::from_json (declared in
// world_json.h). Together with save_library.cpp, which stores those JSON
// worlds in the saves file, this is the only code that knows nlohmann/json
// exists (single-header library vendored under third_party/).
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
#include "world_json.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
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
// Statistics, letter trails, schedule ids and the recent-deliveries log were
// added within v3 as optional keys: a save without them still loads (with
// empty history and freshly assigned schedule ids), so no bump was needed.
constexpr int kSaveFormatVersion = 3;

// RoutingRuleType is saved as a readable string rather than a number, so
// reordering or extending the enum can never silently misread old saves. An
// unknown string fails the whole load (see WorldJson::from_json's catch-all).
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

// TrailEvent: a readable string, like RoutingRuleType above.
const char* trail_event_to_string(TrailEvent event) {
    switch (event) {
    case TrailEvent::Spawned:  return "spawned";
    case TrailEvent::Departed: return "departed";
    case TrailEvent::Arrived:  return "arrived";
    }
    throw std::runtime_error("unknown TrailEvent");  // unreachable
}

TrailEvent trail_event_from_string(const std::string& text) {
    if (text == "spawned")  return TrailEvent::Spawned;
    if (text == "departed") return TrailEvent::Departed;
    if (text == "arrived")  return TrailEvent::Arrived;
    throw std::runtime_error("unknown trail event in save file: " + text);
}

// A stat level's buckets as one space-separated string: a JSON array would
// spend a separator (and, pretty-printed, a whole line) on every one of the
// mostly zero numbers, bloating the file by an order of magnitude. Runs of zeros, the common case,
// shrink further to "0*<count>", e.g. "0*117 3 0 12".
std::string buckets_to_string(const std::array<int64_t, kStatBuckets>& buckets) {
    std::ostringstream out;
    for (std::size_t i = 0; i < buckets.size();) {
        out << (i == 0 ? "" : " ");
        std::size_t run = 0;
        while (i + run < buckets.size() && buckets[i + run] == 0)
            ++run;
        if (run > 1) {
            out << "0*" << run;
            i += run;
        } else {
            out << buckets[i++];
        }
    }
    return out.str();
}

std::array<int64_t, kStatBuckets> buckets_from_string(const std::string& text) {
    std::array<int64_t, kStatBuckets> buckets{};
    std::istringstream in(text);
    std::string token;
    std::size_t i = 0;
    while (in >> token) {
        std::size_t used = 0;
        const int64_t value = std::stoll(token, &used);
        std::size_t repeat = 1;
        if (used != token.size()) {
            if (value != 0 || token[used] != '*')
                throw std::runtime_error("stat buckets: bad token " + token);
            std::size_t count_used = 0;
            repeat = std::stoul(token.substr(used + 1), &count_used);
            if (count_used != token.size() - used - 1)
                throw std::runtime_error("stat buckets: bad token " + token);
        }
        if (repeat > buckets.size() - i)
            throw std::runtime_error("stat buckets: too many values");
        for (std::size_t k = 0; k < repeat; ++k)
            buckets[i++] = value;
    }
    if (i != buckets.size())
        throw std::runtime_error("stat buckets: too few values");
    return buckets;
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

// Statistics. A table is an object keyed by each metric's stable save key
// (stats.h), so adding or reordering metrics never misreads old saves: a
// metric missing from the file starts empty (which is also how never-written
// series are saved, by leaving them out), and unknown keys are ignored.
void to_json(nlohmann::json& j, const StatSeries& series) {
    nlohmann::json levels = nlohmann::json::array();
    for (const StatLevel& level : series.levels)
        levels.push_back({{"newest", level.newest}, {"buckets", buckets_to_string(level.buckets)}});
    j = {{"total", series.total}, {"levels", std::move(levels)}};
}

void from_json(const nlohmann::json& j, StatSeries& series) {
    j.at("total").get_to(series.total);
    const nlohmann::json& levels = j.at("levels");
    if (!levels.is_array() || levels.size() != kStatLevels)
        throw std::runtime_error("stat series: wrong number of levels");
    for (std::size_t i = 0; i < kStatLevels; ++i) {
        levels[i].at("newest").get_to(series.levels[i].newest);
        series.levels[i].buckets = buckets_from_string(levels[i].at("buckets").get<std::string>());
    }
}

template <typename Metric>
void to_json(nlohmann::json& j, const StatTable<Metric>& table) {
    j = nlohmann::json::object();
    const std::span<const StatInfo> infos = stat_infos(Metric{});
    for (std::size_t i = 0; i < infos.size(); ++i)
        if (table.series[i] != StatSeries{})  // never written: loads back empty anyway
            j[infos[i].key] = table.series[i];
}

template <typename Metric>
void from_json(const nlohmann::json& j, StatTable<Metric>& table) {
    const std::span<const StatInfo> infos = stat_infos(Metric{});
    for (std::size_t i = 0; i < infos.size(); ++i) {
        table.series[i] = StatSeries{};
        if (j.contains(infos[i].key))
            j.at(infos[i].key).get_to(table.series[i]);
    }
}

void to_json(nlohmann::json& j, const TrailStop& stop) {
    j = {{"tick", stop.tick},
         {"event", trail_event_to_string(stop.event)},
         {"office", stop.office}};
    if (stop.schedule != kNoSchedule)
        j["schedule"] = stop.schedule;
}

void from_json(const nlohmann::json& j, TrailStop& stop) {
    j.at("tick").get_to(stop.tick);
    stop.event = trail_event_from_string(j.at("event").get<std::string>());
    j.at("office").get_to(stop.office);
    stop.schedule = j.value("schedule", kNoSchedule);
}

void to_json(nlohmann::json& j, const Letter& letter) {
    j = {{"src", letter.src},
         {"dst", letter.dst},
         {"created", letter.created},
         {"deadline", letter.deadline},
         {"value", letter.value},
         {"max_bonus", letter.max_bonus},
         {"trail", letter.trail}};
}

void from_json(const nlohmann::json& j, Letter& letter) {
    j.at("src").get_to(letter.src);
    j.at("dst").get_to(letter.dst);
    j.at("created").get_to(letter.created);
    j.at("deadline").get_to(letter.deadline);
    j.at("value").get_to(letter.value);
    j.at("max_bonus").get_to(letter.max_bonus);
    letter.trail = j.value("trail", std::vector<TrailStop>{});
}

void to_json(nlohmann::json& j, const DeliveredLetter& delivered) {
    j = {{"letter", delivered.letter},
         {"delivered", delivered.delivered},
         {"earned", delivered.earned}};
}

void from_json(const nlohmann::json& j, DeliveredLetter& delivered) {
    j.at("letter").get_to(delivered.letter);
    j.at("delivered").get_to(delivered.delivered);
    j.at("earned").get_to(delivered.earned);
}

// A departure record is a compact [tick, loaded, left_behind] triple: a
// schedule keeps up to kDepartureLogSize of them.
void to_json(nlohmann::json& j, const DepartureRecord& record) {
    j = nlohmann::json::array({record.tick, record.loaded, record.left_behind});
}

void from_json(const nlohmann::json& j, DepartureRecord& record) {
    j.at(0).get_to(record.tick);
    j.at(1).get_to(record.loaded);
    j.at(2).get_to(record.left_behind);
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
    j = {{"id", schedule.id},
         {"dst", schedule.dst},
         {"rule", schedule.rule},
         {"period", schedule.period},
         {"start_offset", schedule.start_offset},
         {"next_departure", schedule.next_departure},
         {"stats", schedule.stats},
         {"departure_log", schedule.departure_log},
         {"edits", schedule.edits}};
}

void from_json(const nlohmann::json& j, TruckSchedule& schedule) {
    // Saves from before schedule ids get theirs assigned in WorldJson::from_json.
    schedule.id = j.value("id", kNoSchedule);
    j.at("dst").get_to(schedule.dst);
    j.at("rule").get_to(schedule.rule);
    j.at("period").get_to(schedule.period);
    j.at("start_offset").get_to(schedule.start_offset);
    j.at("next_departure").get_to(schedule.next_departure);
    schedule.stats = j.value("stats", ScheduleStats{});
    schedule.departure_log = j.value("departure_log", std::vector<DepartureRecord>{});
    schedule.edits = j.value("edits", std::vector<Tick>{});
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
         {"outbound_schedules", office.outbound_schedules},
         {"stats", office.stats}};
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
    office.stats = j.value("stats", OfficeStats{});
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
// converted one by one in WorldJson::to_json / from_json.
void to_json(nlohmann::json& j, const TruckState& truck) {
    j = {{"src", truck.src},
         {"dst", truck.dst},
         {"vertices", truck.vertices},
         {"distances", truck.distances},
         {"departure_tick", truck.departure_tick},
         {"speed", truck.speed},
         {"carried_letters", truck.carried_letters}};
    if (truck.schedule != kNoSchedule)
        j["schedule"] = truck.schedule;
}

void from_json(const nlohmann::json& j, TruckState& truck) {
    j.at("src").get_to(truck.src);
    j.at("dst").get_to(truck.dst);
    j.at("vertices").get_to(truck.vertices);
    j.at("distances").get_to(truck.distances);
    j.at("departure_tick").get_to(truck.departure_tick);
    j.at("speed").get_to(truck.speed);
    j.at("carried_letters").get_to(truck.carried_letters);
    truck.schedule = j.value("schedule", kNoSchedule);
}

namespace {

// Cross-reference and invariant check for a freshly loaded world. nlohmann
// already guarantees the JSON shape; this catches structurally valid but
// logically broken saves (hand-edited or truncated files): dangling office
// references, schedules violating the invariants add_schedule() enforces
// (dst is another office, period > 0, non-empty distinct postal prefixes), and roads
// violating the invariants add_road() enforces (normalized src < dst, no
// duplicates, both endpoint offices built, at least two in-range vertices
// anchored at the endpoint offices), letters (and their trails) naming
// offices that do not exist, and schedule ids that repeat or that the
// World's id counter would hand out again.
bool letter_is_valid(const Letter& letter, PostOfficeId num_offices) {
    if (letter.src >= num_offices || letter.dst >= num_offices)
        return false;
    for (const TrailStop& stop : letter.trail)
        if (stop.office >= num_offices)
            return false;
    return true;
}

bool loaded_state_is_valid(const std::vector<PostOffice>& post_offices,
                           const std::vector<std::shared_ptr<Truck>>& trucks,
                           const std::vector<Road>& roads,
                           const std::vector<DeliveredLetter>& recent_deliveries,
                           ScheduleId next_schedule_id) {
    const PostOfficeId num_offices = post_offices.size();
    std::vector<ScheduleId> schedule_ids;
    for (PostOfficeId office_id = 0; office_id < num_offices; ++office_id) {
        const PostOffice& office = post_offices[office_id];
        for (const Letter& letter : office.outbound_letters)
            if (!letter_is_valid(letter, num_offices))
                return false;
        for (const TruckSchedule& schedule : office.outbound_schedules) {
            if (schedule.id >= next_schedule_id)
                return false;
            schedule_ids.push_back(schedule.id);
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
        for (const Letter& letter : truck->get_letters())
            if (!letter_is_valid(letter, num_offices))
                return false;
    }
    for (const DeliveredLetter& delivered : recent_deliveries)
        if (!letter_is_valid(delivered.letter, num_offices))
            return false;
    std::sort(schedule_ids.begin(), schedule_ids.end());
    if (std::adjacent_find(schedule_ids.begin(), schedule_ids.end()) != schedule_ids.end())
        return false;  // two schedules share an id
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
// WorldJson::to_json / WorldJson::from_json
// ---------------------------------------------------------------------------

nlohmann::json WorldJson::to_json(const World& world) {
    nlohmann::json j;
    j["format_version"] = kSaveFormatVersion;
    j["current_tick"] = world.current_tick;
    j["paused"] = world.paused;
    j["speed_multiplier"] = world.speed_multiplier;
    j["money"] = world.money;
    j["letters_delivered_on_time"] = world.letters_delivered_on_time;
    j["letters_delivered_late"] = world.letters_delivered_late;
    j["next_schedule_id"] = world.next_schedule_id;
    j["stats"] = world.stats;
    j["recent_deliveries"] = world.recent_deliveries;
    j["post_offices"] = world.post_offices;
    j["roads"] = world.roads;

    nlohmann::json trucks_json = nlohmann::json::array();
    for (const std::shared_ptr<Truck>& truck : world.trucks)
        trucks_json.push_back(truck->get_state());
    j["trucks"] = std::move(trucks_json);

    // The RNG's complete internal state, dumped with the standard-library
    // operator<< (624 state words plus the position index, as decimal text)
    // and restored with operator>> on load. This is what makes the
    // simulation continue the identical random sequence after loading.
    std::ostringstream rng_state;
    rng_state << world.rng;
    j["rng"] = rng_state.str();

    return j;
}

std::optional<World> WorldJson::from_json(const nlohmann::json& j) {
    using std::cerr;
    using std::endl;

    try {
        if (j.at("format_version").get<int>() != kSaveFormatVersion) {
            cerr << "load: incompatible save format version" << endl;
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
        world.stats = j.value("stats", GlobalStats{});
        world.recent_deliveries = j.value("recent_deliveries", std::vector<DeliveredLetter>{});

        // Schedule ids. Saves from before ids hold none: hand every
        // schedule a fresh one, starting past any id already in the file.
        world.next_schedule_id = j.value("next_schedule_id", ScheduleId{0});
        if (!j.contains("next_schedule_id")) {
            for (PostOffice& office : world.post_offices)
                for (TruckSchedule& schedule : office.outbound_schedules)
                    schedule.id = world.next_schedule_id++;
        }

        const nlohmann::json& trucks_json = j.at("trucks");
        if (!trucks_json.is_array()) {
            cerr << "load: \"trucks\" is not an array" << endl;
            return std::nullopt;
        }
        for (const nlohmann::json& truck_json : trucks_json) {
            world.trucks.push_back(std::make_shared<Truck>(truck_json.get<TruckState>()));
        }

        // Restore the RNG's full internal state (see to_json). operator>>
        // validates the dump and sets failbit on malformed input.
        std::istringstream rng_state(j.at("rng").get<std::string>());
        rng_state >> world.rng;
        if (rng_state.fail()) {
            cerr << "load: unreadable RNG state, continuing with a freshly seeded RNG" << endl;
        }

        if (!loaded_state_is_valid(world.post_offices, world.trucks, world.roads,
                                   world.recent_deliveries, world.next_schedule_id)) {
            cerr << "load: the saved world is inconsistent (see loaded_state_is_valid)" << endl;
            return std::nullopt;
        }

        return world;
    } catch (const std::exception& e) {
        // Any problem — missing or mis-typed keys, unknown
        // enum strings, out-of-range numbers — means "no usable save": the
        // caller falls back to a fresh world instead of crashing or loading
        // a half-built one.
        cerr << "load: " << e.what() << endl;
        return std::nullopt;
    }
}
