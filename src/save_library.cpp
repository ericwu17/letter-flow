// save_library.cpp — implements SaveLibrary (save_library.h): the saves file
// as a list of named JSON worlds.
// Pure simulation layer: no imgui/SDL/OpenGL headers (see game_types.h).
#include "save_library.h"

#include "scenario.h"
#include "world_json.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// Bump whenever the library's own layout (not the worlds inside it) changes
// incompatibly. The embedded worlds carry their own format_version.
constexpr int kLibraryFormatVersion = 1;

std::int64_t now_unix_seconds() {
    using namespace std::chrono;
    return static_cast<std::int64_t>(
        duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

json empty_library() {
    return json{{"format_version", kLibraryFormatVersion},
                {"next_save_id", SaveId{0}},
                {"saves", json::array()}};
}

// nullopt if the file is missing, unreadable or not JSON.
std::optional<json> read_json_file(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return std::nullopt;
    json j = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded())
        return std::nullopt;
    return j;
}

// Writes to a temp file, then renames it over the target, so a crash
// mid-write leaves the previous file intact instead of a half-written one.
// (Not proof against power loss: nothing fsyncs the file before the rename.)
bool write_json_file(const std::string& path, const json& j) {
    const std::string tmp_path = path + ".tmp";
    std::error_code ec;
    {
        std::ofstream out(tmp_path);
        if (!out)
            return false;
        // Compact, not indented: the library holds every save's whole world
        // (statistics included), and indenting nearly doubles its size.
        out << j.dump() << '\n';
        out.flush();
        if (!out) {
            fs::remove(tmp_path, ec);
            return false;
        }
    }
    fs::rename(tmp_path, path, ec);
    if (ec) {
        fs::remove(tmp_path, ec);
        return false;
    }
    return true;
}

// The library's own structure: version, id counter, and well-formed entries
// with unique ids below the counter. The worlds are only checked to be JSON
// objects — a broken world is that one save's problem (load() reports it),
// not the whole library's.
bool library_is_valid(const json& doc) {
    if (!doc.is_object())
        return false;
    const auto version = doc.find("format_version");
    if (version == doc.end() || !version->is_number_integer() ||
        version->get<int>() != kLibraryFormatVersion)
        return false;
    const auto next_id = doc.find("next_save_id");
    const auto saves = doc.find("saves");
    if (next_id == doc.end() || !next_id->is_number_unsigned() ||
        saves == doc.end() || !saves->is_array())
        return false;

    std::set<SaveId> ids;
    for (const json& entry : *saves) {
        if (!entry.is_object())
            return false;
        const auto id = entry.find("id");
        const auto name = entry.find("name");
        const auto last_played = entry.find("last_played");
        const auto world = entry.find("world");
        if (id == entry.end() || !id->is_number_unsigned() ||
            name == entry.end() || !name->is_string() ||
            last_played == entry.end() || !last_played->is_number_integer() ||
            world == entry.end() || !world->is_object())
            return false;
        if (id->get<std::uint64_t>() >= next_id->get<std::uint64_t>() ||
            !ids.insert(id->get<SaveId>()).second)
            return false;
    }
    return true;
}

// The library at `path`: an empty one if the file doesn't exist yet, nullopt
// if it exists but can't be used.
std::optional<json> read_library(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec))
        return empty_library();
    std::optional<json> doc = read_json_file(path);
    if (!doc || !library_is_valid(*doc))
        return std::nullopt;
    return doc;
}

std::vector<SaveSummary> summarize(const json& doc) {
    std::vector<SaveSummary> summaries;
    for (const json& entry : doc.at("saves")) {
        SaveSummary summary;
        summary.id = entry.at("id").get<SaveId>();
        summary.name = entry.at("name").get<std::string>();
        summary.last_played = entry.at("last_played").get<std::int64_t>();
        // Best effort: a world missing these still lists (as day 1, $0).
        const json& world = entry.at("world");
        if (const auto tick = world.find("current_tick"); tick != world.end() && tick->is_number_unsigned())
            summary.tick = tick->get<Tick>();
        if (const auto money = world.find("money"); money != world.end() && money->is_number_integer())
            summary.money = money->get<int>();
        summaries.push_back(std::move(summary));
    }
    std::sort(summaries.begin(), summaries.end(), [](const SaveSummary& a, const SaveSummary& b) {
        if (a.last_played != b.last_played)
            return a.last_played > b.last_played;
        return a.id > b.id;
    });
    return summaries;
}

json* find_entry(json& doc, SaveId id) {
    for (json& entry : doc.at("saves"))
        if (entry.at("id").get<SaveId>() == id)
            return &entry;
    return nullptr;
}

// Appends a new save holding `world_json` and returns its id.
SaveId add_entry(json& doc, const std::string& name, json world_json) {
    const SaveId id = doc.at("next_save_id").get<SaveId>();
    doc["next_save_id"] = id + 1;
    doc.at("saves").push_back(json{{"id", id},
                                   {"name", name},
                                   {"last_played", now_unix_seconds()},
                                   {"world", std::move(world_json)}});
    return id;
}

// Read-modify-write: reads the library, lets `change` edit it (returning
// false to abort without writing), then writes it back. Returns the written
// document, or nullopt if anything failed — the file is then untouched.
template <class Change>
std::optional<json> modify_library(const std::string& path, Change&& change) {
    std::optional<json> doc = read_library(path);
    if (!doc || !change(*doc) || !write_json_file(path, *doc))
        return std::nullopt;
    return doc;
}

}  // namespace

SaveLibrary::SaveLibrary(std::string path) : library_path(std::move(path)) {
    if (const std::optional<json> doc = read_library(library_path)) {
        summaries = summarize(*doc);
        return;
    }

    // Unusable: move it aside so no later write can overwrite it.
    const fs::path original(library_path);
    const fs::path aside = original.parent_path() /
        (original.stem().string() + ".corrupt-" + std::to_string(now_unix_seconds()) +
         original.extension().string());
    std::error_code ec;
    fs::rename(original, aside, ec);
    if (ec) {
        // Still in place, and still unreadable, so every operation will fail
        // its read and leave it alone.
        warning_text = "Your saves file " + library_path + " could not be read or moved aside. "
                       "Nothing can be saved until it is fixed or removed.";
    } else {
        warning_text = "Your saves file could not be read. It was moved to " + aside.string() +
                       " and a new one was started.";
    }
    std::cerr << warning_text << std::endl;
}

void SaveLibrary::import_legacy_save(const std::string& legacy_path) {
    std::error_code ec;
    if (fs::exists(library_path, ec) || !fs::exists(legacy_path, ec))
        return;
    const std::optional<json> legacy = read_json_file(legacy_path);
    if (!legacy)
        return;
    const std::optional<World> world = WorldJson::from_json(*legacy);
    if (!world)
        return;
    const auto doc = modify_library(library_path, [&](json& d) {
        add_entry(d, "Imported game", WorldJson::to_json(*world));
        return true;
    });
    if (doc) {
        summaries = summarize(*doc);
        std::cout << "Imported " << legacy_path << " into " << library_path << std::endl;
    }
}

std::optional<SaveId> SaveLibrary::create(const std::string& name) {
    if (name.empty())
        return std::nullopt;
    SaveId id = 0;
    const auto doc = modify_library(library_path, [&](json& d) {
        id = add_entry(d, name, WorldJson::to_json(create_default_world()));
        return true;
    });
    if (!doc)
        return std::nullopt;
    summaries = summarize(*doc);
    return id;
}

std::optional<SaveId> SaveLibrary::copy(SaveId id, const std::string& name) {
    if (name.empty())
        return std::nullopt;
    SaveId new_id = 0;
    const auto doc = modify_library(library_path, [&](json& d) {
        const json* source = find_entry(d, id);
        if (!source)
            return false;
        new_id = add_entry(d, name, source->at("world"));
        return true;
    });
    if (!doc)
        return std::nullopt;
    summaries = summarize(*doc);
    return new_id;
}

bool SaveLibrary::rename(SaveId id, const std::string& name) {
    if (name.empty())
        return false;
    const auto doc = modify_library(library_path, [&](json& d) {
        json* entry = find_entry(d, id);
        if (!entry)
            return false;
        (*entry)["name"] = name;
        return true;
    });
    if (!doc)
        return false;
    summaries = summarize(*doc);
    return true;
}

bool SaveLibrary::remove(SaveId id) {
    const auto doc = modify_library(library_path, [&](json& d) {
        json& saves = d.at("saves");
        const auto it = std::find_if(saves.begin(), saves.end(), [&](const json& entry) {
            return entry.at("id").get<SaveId>() == id;
        });
        if (it == saves.end())
            return false;
        saves.erase(it);
        return true;
    });
    if (!doc)
        return false;
    summaries = summarize(*doc);
    return true;
}

std::optional<World> SaveLibrary::load(SaveId id) {
    std::optional<json> doc = read_library(library_path);
    if (!doc)
        return std::nullopt;
    json* entry = find_entry(*doc, id);
    if (!entry)
        return std::nullopt;
    std::optional<World> world = WorldJson::from_json(entry->at("world"));
    if (!world)
        return std::nullopt;

    // Mark it as just played. Not fatal if this write fails: the game still
    // starts (and store() will try again on quit).
    (*entry)["last_played"] = now_unix_seconds();
    if (write_json_file(library_path, *doc))
        summaries = summarize(*doc);
    return world;
}

bool SaveLibrary::store(SaveId id, const World& world) {
    const auto doc = modify_library(library_path, [&](json& d) {
        json* entry = find_entry(d, id);
        if (!entry)
            return false;
        (*entry)["world"] = WorldJson::to_json(world);
        (*entry)["last_played"] = now_unix_seconds();
        return true;
    });
    if (!doc)
        return false;
    summaries = summarize(*doc);
    return true;
}
