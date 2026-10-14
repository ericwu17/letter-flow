// save_library.h — every saved game, one JSON file per save.
// Pure simulation layer: no imgui/SDL/OpenGL headers (see game_types.h).
//
// The saves live in one directory (persistent_saves/ in SDL's per-user
// app-data directory), one file each, named after the save:
//
//   persistent_saves/game_1.json
//     { "format_version": 1, "name": "Game 1", "last_played": 1760275200,
//       "world": { ... } }
//
// The filename is only a sanitized form of the name (see save_file_stem), so
// the real name, special characters and all, is kept inside the file. Names
// need not be unique: when two saves would share a filename, the later one
// gets a numbered one instead (game_1-2.json, ...). A file that is already on
// disk is never overwritten by another save.
//
// The files are the source of truth: every operation reads or writes just
// its own save's file (temp file + rename, so a crash mid-write leaves the
// previous file intact). saves() is a cached summary of what this object
// last read or wrote.
#pragma once

#include "sim/game_types.h"
#include "sim/world.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Identifies a save within one SaveLibrary object. Ids are handed out as the
// library finds and creates saves and never reused, but they are not stored
// anywhere: a new SaveLibrary on the same directory numbers its saves afresh.
using SaveId = std::uint32_t;

// What the save list shows, without loading the world itself.
struct SaveSummary {
    SaveId id = 0;
    std::string name;
    Tick tick = 0;                 // the world's in-game clock
    int money = 0;
    std::int64_t last_played = 0;  // unix seconds (created, loaded or stored)
};

// The filename (without ".json") a save called `name` would get, before any
// numbering to avoid an existing file: lowercase ASCII letters and digits,
// with every run of anything else (spaces, punctuation, non-ASCII) turned
// into a single '_', trimmed and capped in length. Never empty, never
// contains '-', and never a name Windows reserves (con, nul, com1, ...).
std::string save_file_stem(const std::string& name);

class SaveLibrary {
public:
    // Reads every save in `dir`, if it exists (it is created by the first
    // write). A .json file there that isn't a usable save — corrupt JSON,
    // wrong format_version, missing name — is left exactly as it is and
    // skipped, with warning() naming it.
    explicit SaveLibrary(std::string dir);

    // Most recently played first.
    const std::vector<SaveSummary>& saves() const { return summaries; }
    // Non-empty when the constructor had to skip unreadable files.
    const std::string& warning() const { return warning_text; }

    // Each returns nullopt / false (and leaves the files as they were) if a
    // file can't be read or written, the id is unknown, or the name is empty.

    // A new save holding the starter scenario (create_default_world()).
    std::optional<SaveId> create(const std::string& name);
    // A new save holding a copy of save `id`'s world.
    std::optional<SaveId> copy(SaveId id, const std::string& name);
    // Also moves the save to a file named after the new name.
    bool rename(SaveId id, const std::string& name);
    bool remove(SaveId id);
    // The save's world, or nullopt if it is unusable (see WorldJson::from_json);
    // an unusable save stays in the list, so the player can still delete it.
    // Marks the save as just played.
    std::optional<World> load(SaveId id);
    // Overwrites save `id`'s world and marks it as just played.
    bool store(SaveId id, const World& world);

private:
    // Lists a save just read from or written to `path` (replacing any
    // previous summary for `id`) and keeps the list sorted.
    void set_summary(SaveId id, const std::string& path, SaveSummary summary);

    std::string save_dir;
    SaveId next_id = 0;
    std::map<SaveId, std::string> paths;  // each save's file
    std::vector<SaveSummary> summaries;
    std::string warning_text;
};
