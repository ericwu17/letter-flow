// save_library.h — every saved game, kept together in one JSON file.
// Pure simulation layer: no imgui/SDL/OpenGL headers (see game_types.h).
//
// The file (saves.json in SDL's per-user app-data directory) holds a list of
// named saves, each embedding a whole World as JSON (see world_json.h):
//
//   { "format_version": 1, "next_save_id": 3,
//     "saves": [ { "id": 0, "name": "Game 1", "last_played": 1760275200,
//                  "world": { ... } } ] }
//
// The file is the single source of truth: every operation that changes it
// reads it, edits the JSON and writes it back (temp file + rename, so a
// crash mid-write leaves the previous file intact). saves() is a cached
// summary of the last file this object read or wrote.
#pragma once

#include "sim/game_types.h"
#include "sim/world.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using SaveId = std::uint32_t;

// What the save list shows, without loading the world itself.
struct SaveSummary {
    SaveId id = 0;
    std::string name;
    Tick tick = 0;                 // the world's in-game clock
    int money = 0;
    std::int64_t last_played = 0;  // unix seconds (created, loaded or stored)
};

class SaveLibrary {
public:
    // Reads the library at `path` if it exists (it is created by the first
    // write). A file that exists but can't be used — corrupt JSON, wrong
    // format_version, malformed entries — is renamed aside (e.g.
    // saves.corrupt-1760275200.json) rather than ever being overwritten, and
    // the library starts empty with warning() explaining where it went.
    explicit SaveLibrary(std::string path);

    // One-time migration from the single save.json of earlier versions: if
    // the library file doesn't exist yet and `legacy_path` holds a loadable
    // world, adds it as "Imported game". The legacy file is left untouched.
    void import_legacy_save(const std::string& legacy_path);

    // Most recently played first.
    const std::vector<SaveSummary>& saves() const { return summaries; }
    // Non-empty when the constructor had to set an unreadable file aside.
    const std::string& warning() const { return warning_text; }

    // Each returns nullopt / false (and leaves the file as it was) if the
    // library can't be read or written, the id is unknown, or the name is
    // empty. Names need not be unique; ids are, and are never reused.

    // A new save holding the starter scenario (create_default_world()).
    std::optional<SaveId> create(const std::string& name);
    // A new save holding a copy of save `id`'s world.
    std::optional<SaveId> copy(SaveId id, const std::string& name);
    bool rename(SaveId id, const std::string& name);
    bool remove(SaveId id);
    // The save's world, or nullopt if it is unusable (see WorldJson::from_json);
    // an unusable save stays in the list, so the player can still delete it.
    // Marks the save as just played.
    std::optional<World> load(SaveId id);
    // Overwrites save `id`'s world and marks it as just played.
    bool store(SaveId id, const World& world);

private:
    std::string library_path;
    std::vector<SaveSummary> summaries;
    std::string warning_text;
};
