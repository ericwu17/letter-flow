// save_library.cpp — implements SaveLibrary (save_library.h): a directory of
// saves, one JSON file each.
// Pure simulation layer: no imgui/SDL/OpenGL headers (see game_types.h).
#include "persistence/save_library.h"

#include "sim/scenario.h"
#include "persistence/world_json.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// Bump whenever a save file's own layout (not the world inside it) changes
// incompatibly. The embedded world carries its own format_version.
constexpr int kSaveFormatVersion = 1;

// Longest filename stem save_file_stem() produces (before any "-N").
constexpr std::size_t kMaxStemLength = 40;

std::int64_t now_unix_seconds() {
    using namespace std::chrono;
    return static_cast<std::int64_t>(
        duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
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
        // Compact, not indented: a save holds its whole world (statistics
        // included), and indenting nearly doubles its size.
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

json make_save(const std::string& name, json world) {
    return json{{"format_version", kSaveFormatVersion},
                {"name", name},
                {"last_played", now_unix_seconds()},
                {"world", std::move(world)}};
}

// The save file's own structure. The world is only checked to be a JSON
// object — a broken world is reported by load(), and the save still lists.
bool save_is_valid(const json& doc) {
    if (!doc.is_object())
        return false;
    const auto version = doc.find("format_version");
    const auto name = doc.find("name");
    const auto last_played = doc.find("last_played");
    const auto world = doc.find("world");
    return version != doc.end() && version->is_number_integer() &&
           version->get<int>() == kSaveFormatVersion &&
           name != doc.end() && name->is_string() &&
           last_played != doc.end() && last_played->is_number_integer() &&
           world != doc.end() && world->is_object();
}

// The save at `path`, or nullopt if it is missing or not a usable save file.
std::optional<json> read_save(const std::string& path) {
    std::optional<json> doc = read_json_file(path);
    if (!doc || !save_is_valid(*doc))
        return std::nullopt;
    return doc;
}

SaveSummary summarize(const json& save) {
    SaveSummary summary;
    summary.name = save.at("name").get<std::string>();
    summary.last_played = save.at("last_played").get<std::int64_t>();
    // Best effort: a world missing these still lists (as day 1, $0).
    const json& world = save.at("world");
    if (const auto tick = world.find("current_tick"); tick != world.end() && tick->is_number_unsigned())
        summary.tick = tick->get<Tick>();
    if (const auto money = world.find("money"); money != world.end() && money->is_number_integer())
        summary.money = money->get<int>();
    return summary;
}

bool is_windows_reserved(const std::string& stem) {
    static const std::array<const char*, 4> kDevices = {"con", "prn", "aux", "nul"};
    if (std::find(kDevices.begin(), kDevices.end(), stem) != kDevices.end())
        return true;
    return stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
           stem[3] >= '1' && stem[3] <= '9';
}

// Whether the file at `path` is named for `stem`: "<stem>.json" or one of
// its numbered variants "<stem>-N.json".
bool named_for_stem(const fs::path& path, const std::string& stem) {
    const std::string file_stem = path.stem().string();
    if (file_stem == stem)
        return true;
    if (!file_stem.starts_with(stem + "-") || file_stem.size() == stem.size() + 1)
        return false;
    return std::all_of(file_stem.begin() + static_cast<std::ptrdiff_t>(stem.size() + 1), file_stem.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
}

// "<stem>.json" in `dir`, or else the first "<stem>-N.json" (N = 2, 3, ...)
// that doesn't exist yet. nullopt if the directory can't be checked.
std::optional<fs::path> unique_save_path(const fs::path& dir, const std::string& stem) {
    for (int n = 1;; ++n) {
        const fs::path candidate = dir / (n == 1 ? stem + ".json" : stem + "-" + std::to_string(n) + ".json");
        std::error_code ec;
        const bool taken = fs::exists(candidate, ec);
        if (ec)
            return std::nullopt;
        if (!taken)
            return candidate;
    }
}

// Writes a new save file for `name` holding `world` into `dir` (created if
// need be). Returns its path and contents, or nullopt if it couldn't be written.
std::optional<std::pair<std::string, json>> write_new_save(const std::string& dir, const std::string& name,
                                                           json world) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        return std::nullopt;
    const std::optional<fs::path> path = unique_save_path(dir, save_file_stem(name));
    if (!path)
        return std::nullopt;
    json save = make_save(name, std::move(world));
    if (!write_json_file(path->string(), save))
        return std::nullopt;
    return std::make_pair(path->string(), std::move(save));
}

}  // namespace

std::string save_file_stem(const std::string& name) {
    std::string stem;
    for (const char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            stem += c;
        else if (c >= 'A' && c <= 'Z')
            stem += static_cast<char>(c - 'A' + 'a');
        else if (!stem.empty() && stem.back() != '_')
            stem += '_';  // never leading, never doubled
    }
    if (stem.size() > kMaxStemLength)
        stem.resize(kMaxStemLength);
    while (!stem.empty() && stem.back() == '_')
        stem.pop_back();
    if (stem.empty())
        return "save";
    if (is_windows_reserved(stem))
        return "save_" + stem;
    return stem;
}

SaveLibrary::SaveLibrary(std::string dir) : save_dir(std::move(dir)) {
    // Sorted, so ids follow the filenames rather than the directory's order.
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(save_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (std::error_code file_ec; it->path().extension() == ".json" && it->is_regular_file(file_ec))
            files.push_back(it->path());
    std::sort(files.begin(), files.end());

    if (std::error_code exists_ec; ec && fs::exists(save_dir, exists_ec)) {
        warning_text = "Your saves folder " + save_dir + " could not be read.";
        std::cerr << warning_text << std::endl;
        return;
    }

    std::string skipped;
    for (const fs::path& file : files) {
        const std::optional<json> save = read_save(file.string());
        if (!save) {
            // Left where it is: new saves never take an existing filename,
            // so nothing will overwrite it.
            skipped += (skipped.empty() ? "" : ", ") + file.filename().string();
            continue;
        }
        set_summary(next_id++, file.string(), summarize(*save));
    }
    if (!skipped.empty()) {
        warning_text = "Some save files in " + save_dir + " could not be read and were skipped "
                       "(they were left as they are): " + skipped;
        std::cerr << warning_text << std::endl;
    }
}

void SaveLibrary::set_summary(SaveId id, const std::string& path, SaveSummary summary) {
    summary.id = id;
    paths[id] = path;
    const auto existing = std::find_if(summaries.begin(), summaries.end(),
                                       [&](const SaveSummary& s) { return s.id == id; });
    if (existing != summaries.end())
        *existing = std::move(summary);
    else
        summaries.push_back(std::move(summary));
    std::sort(summaries.begin(), summaries.end(), [](const SaveSummary& a, const SaveSummary& b) {
        if (a.last_played != b.last_played)
            return a.last_played > b.last_played;
        return a.id > b.id;
    });
}

std::optional<SaveId> SaveLibrary::create(const std::string& name) {
    if (name.empty())
        return std::nullopt;
    const auto written = write_new_save(save_dir, name, WorldJson::to_json(create_default_world()));
    if (!written)
        return std::nullopt;
    const SaveId id = next_id++;
    set_summary(id, written->first, summarize(written->second));
    return id;
}

std::optional<SaveId> SaveLibrary::copy(SaveId id, const std::string& name) {
    const auto source_path = paths.find(id);
    if (name.empty() || source_path == paths.end())
        return std::nullopt;
    const std::optional<json> source = read_save(source_path->second);
    if (!source)
        return std::nullopt;
    const auto written = write_new_save(save_dir, name, source->at("world"));
    if (!written)
        return std::nullopt;
    const SaveId new_id = next_id++;
    set_summary(new_id, written->first, summarize(written->second));
    return new_id;
}

bool SaveLibrary::rename(SaveId id, const std::string& name) {
    const auto path_it = paths.find(id);
    if (name.empty() || path_it == paths.end())
        return false;
    std::string path = path_it->second;
    std::optional<json> save = read_save(path);
    if (!save)
        return false;
    (*save)["name"] = name;
    if (!write_json_file(path, *save))
        return false;

    // Then move it to a file named for the new name. Not fatal if this
    // fails: the save is renamed either way, its filename just lags behind.
    const std::string stem = save_file_stem(name);
    if (!named_for_stem(path, stem)) {
        if (const std::optional<fs::path> target = unique_save_path(save_dir, stem)) {
            std::error_code ec;
            fs::rename(path, *target, ec);
            if (!ec)
                path = target->string();
        }
    }
    set_summary(id, path, summarize(*save));
    return true;
}

bool SaveLibrary::remove(SaveId id) {
    const auto path = paths.find(id);
    if (path == paths.end())
        return false;
    // Already gone from disk counts as removed.
    std::error_code ec;
    fs::remove(path->second, ec);
    if (ec)
        return false;
    paths.erase(path);
    std::erase_if(summaries, [&](const SaveSummary& s) { return s.id == id; });
    return true;
}

std::optional<World> SaveLibrary::load(SaveId id) {
    const auto path = paths.find(id);
    if (path == paths.end())
        return std::nullopt;
    std::optional<json> save = read_save(path->second);
    if (!save)
        return std::nullopt;
    std::optional<World> world = WorldJson::from_json(save->at("world"));
    if (!world)
        return std::nullopt;

    // Mark it as just played. Not fatal if this write fails: the game still
    // starts (and store() will try again on quit).
    (*save)["last_played"] = now_unix_seconds();
    if (write_json_file(path->second, *save))
        set_summary(id, path->second, summarize(*save));
    return world;
}

bool SaveLibrary::store(SaveId id, const World& world) {
    const auto path = paths.find(id);
    if (path == paths.end())
        return false;
    std::optional<json> save = read_save(path->second);
    if (!save)
        return false;
    (*save)["world"] = WorldJson::to_json(world);
    (*save)["last_played"] = now_unix_seconds();
    if (!write_json_file(path->second, *save))
        return false;
    set_summary(id, path->second, summarize(*save));
    return true;
}
