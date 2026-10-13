// test_save_library.cpp — headless tests for SaveLibrary, the single JSON
// file holding every saved game. Run with `make test`.
// Pure simulation layer: no SDL/imgui/OpenGL — no window is needed.
//
// Each test works in its own fresh directory under build/test-saves/library/
// (gitignored); the test must run with the repo root as working directory
// (`make test` does). Reopening a SaveLibrary on the same path is how the
// tests check that something really reached the file.
#include "sim/balance.h"
#include "persistence/save_library.h"
#include "sim/scenario.h"
#include "sim/world.h"
#include "persistence/world_json.h"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

const fs::path kLibraryDir = "build/test-saves/library";

// An empty directory for one test, cleared of any previous run's files.
fs::path fresh_dir(const std::string& name) {
    const fs::path dir = kLibraryDir / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
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

// A world that differs from the starter scenario: other money, a day of
// simulated time (so its clock and RNG have moved on too).
World played_world(int money) {
    World w = create_default_world();
    w.set_money(money);
    for (Tick t = 0; t < kTicksPerDay; ++t)
        w.advance_tick();
    return w;
}

const SaveSummary* find_summary(const SaveLibrary& library, SaveId id) {
    for (const SaveSummary& summary : library.saves())
        if (summary.id == id)
            return &summary;
    return nullptr;
}

// --- the tests -------------------------------------------------------------

void create_lists_and_persists() {
    const std::string path = (fresh_dir("create") / "saves.json").string();
    SaveLibrary library(path);
    assert(library.saves().empty());
    assert(library.warning().empty());
    assert(!fs::exists(path));  // nothing written until something changes

    assert(!library.create(""));
    const auto id = library.create("Game 1");
    assert(id.has_value());
    assert(fs::exists(path));
    assert(library.saves().size() == 1);

    // A fresh library on the same file sees the same save...
    SaveLibrary reopened(path);
    assert(reopened.saves().size() == 1);
    const SaveSummary& summary = reopened.saves()[0];
    assert(summary.id == *id);
    assert(summary.name == "Game 1");
    assert(summary.tick == 0);
    assert(summary.money == kStartingMoney);
    assert(summary.last_played > 0);

    // ...holding exactly the starter scenario.
    const auto world = reopened.load(*id);
    assert(world.has_value());
    assert(*world == create_default_world());

    // Newest first when created within the same second (ties go to the higher id).
    const auto second = reopened.create("Game 2");
    assert(second.has_value() && *second != *id);
    assert(reopened.saves()[0].id == *second);
}

void store_round_trips() {
    const std::string path = (fresh_dir("store") / "saves.json").string();
    SaveLibrary library(path);
    const auto id = library.create("Game 1");
    assert(id.has_value());

    const World played = played_world(1234);
    assert(library.store(*id, played));
    assert(!library.store(*id + 100, played));  // unknown id

    SaveLibrary reopened(path);
    const auto loaded = reopened.load(*id);
    assert(loaded.has_value());
    assert(*loaded == played);
    assert(reopened.saves()[0].tick == played.get_tick());
    assert(reopened.saves()[0].money == 1234);
    assert(!reopened.load(*id + 100));
}

void copy_is_independent() {
    const std::string path = (fresh_dir("copy") / "saves.json").string();
    SaveLibrary library(path);
    const auto original = library.create("Original");
    assert(original.has_value());
    const World first = played_world(111);
    assert(library.store(*original, first));

    const auto copy = library.copy(*original, "Original (copy)");
    assert(copy.has_value() && *copy != *original);
    assert(!library.copy(*original + 100, "Nope"));
    assert(!library.copy(*original, ""));
    assert(library.saves().size() == 2);
    assert(*library.load(*copy) == first);

    // Playing on in the copy leaves the original as it was.
    const World second = played_world(222);
    assert(library.store(*copy, second));
    SaveLibrary reopened(path);
    assert(*reopened.load(*original) == first);
    assert(*reopened.load(*copy) == second);
    assert(find_summary(reopened, *copy)->name == "Original (copy)");
}

void rename_and_remove() {
    const std::string path = (fresh_dir("rename") / "saves.json").string();
    SaveLibrary library(path);
    const auto a = library.create("A");
    const auto b = library.create("B");
    assert(a.has_value() && b.has_value());

    assert(library.rename(*a, "Renamed"));
    assert(!library.rename(*a, ""));
    assert(!library.rename(*b + 100, "Nope"));
    assert(find_summary(SaveLibrary(path), *a)->name == "Renamed");

    assert(library.remove(*b));
    assert(!library.remove(*b));  // already gone
    assert(library.saves().size() == 1);
    assert(!library.load(*b));
    assert(SaveLibrary(path).saves().size() == 1);

    // Ids are never reused, even after the highest one is deleted.
    const auto c = library.create("C");
    assert(c.has_value() && *c != *a && *c != *b);

    assert(library.remove(*a));
    assert(library.remove(*c));
    assert(library.saves().empty());
    assert(SaveLibrary(path).saves().empty());
}

void corrupt_library_is_set_aside() {
    const fs::path dir = fresh_dir("corrupt");
    const fs::path path = dir / "saves.json";
    const std::string garbage = "{this is not json";
    write_file(path, garbage);

    SaveLibrary library(path.string());
    assert(!library.warning().empty());
    assert(library.saves().empty());
    assert(!fs::exists(path));

    // The original bytes survive, under a new name.
    std::vector<fs::path> aside;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir))
        aside.push_back(entry.path());
    assert(aside.size() == 1);
    assert(aside[0].filename().string().rfind("saves.corrupt-", 0) == 0);
    assert(aside[0].extension() == ".json");
    assert(slurp(aside[0]) == garbage);

    // And the library carries on with a new file.
    assert(library.create("Fresh start").has_value());
    assert(SaveLibrary(path.string()).saves().size() == 1);

    // Valid JSON with the wrong library version is set aside too.
    const fs::path versioned = fresh_dir("corrupt_version") / "saves.json";
    write_file(versioned, R"({"format_version": 999, "next_save_id": 0, "saves": []})");
    const SaveLibrary future(versioned.string());
    assert(!future.warning().empty());
    assert(!fs::exists(versioned));
}

void legacy_save_is_imported_once() {
    const fs::path dir = fresh_dir("legacy");
    const std::string path = (dir / "saves.json").string();
    const std::string legacy = (dir / "save.json").string();
    const World played = played_world(4321);
    write_file(legacy, WorldJson::to_json(played).dump(2));

    SaveLibrary library(path);
    library.import_legacy_save(legacy);
    assert(library.saves().size() == 1);
    assert(library.saves()[0].name == "Imported game");
    assert(*library.load(library.saves()[0].id) == played);
    assert(fs::exists(legacy));  // left untouched

    // The library exists now, so a second import (next launch) does nothing,
    // even after the imported save is deleted.
    assert(library.remove(library.saves()[0].id));
    SaveLibrary next_launch(path);
    next_launch.import_legacy_save(legacy);
    assert(next_launch.saves().empty());

    // No legacy save: nothing is imported and no file is written.
    const fs::path empty = fresh_dir("legacy_missing");
    SaveLibrary nothing((empty / "saves.json").string());
    nothing.import_legacy_save((empty / "save.json").string());
    assert(nothing.saves().empty());
    assert(!fs::exists(empty / "saves.json"));

    // An unloadable legacy save isn't imported either.
    const fs::path broken = fresh_dir("legacy_broken");
    write_file(broken / "save.json", R"({"format_version": 999})");
    SaveLibrary skipped((broken / "saves.json").string());
    skipped.import_legacy_save((broken / "save.json").string());
    assert(skipped.saves().empty());
}

void broken_world_still_lists() {
    const fs::path path = fresh_dir("broken_world") / "saves.json";
    SaveLibrary library(path.string());
    const auto broken = library.create("Broken");
    const auto fine = library.create("Fine");
    assert(broken.has_value() && fine.has_value());

    // Damage one save's world by hand; the library itself stays well-formed.
    nlohmann::json doc = nlohmann::json::parse(slurp(path));
    for (nlohmann::json& entry : doc.at("saves"))
        if (entry.at("id").get<SaveId>() == *broken)
            entry.at("world")["format_version"] = 999;
    write_file(path, doc.dump(2));

    SaveLibrary reopened(path.string());
    assert(reopened.warning().empty());
    assert(reopened.saves().size() == 2);
    assert(!reopened.load(*broken));
    assert(reopened.load(*fine).has_value());
    assert(reopened.remove(*broken));
    assert(reopened.saves().size() == 1);
}

}  // namespace

int main() {
    create_lists_and_persists();
    printf("PASS create_lists_and_persists\n");
    store_round_trips();
    printf("PASS store_round_trips\n");
    copy_is_independent();
    printf("PASS copy_is_independent\n");
    rename_and_remove();
    printf("PASS rename_and_remove\n");
    corrupt_library_is_set_aside();
    printf("PASS corrupt_library_is_set_aside\n");
    legacy_save_is_imported_once();
    printf("PASS legacy_save_is_imported_once\n");
    broken_world_still_lists();
    printf("PASS broken_world_still_lists\n");

    printf("all save library tests passed\n");
    return 0;
}
