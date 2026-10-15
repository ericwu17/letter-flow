// test_save_library.cpp — headless tests for SaveLibrary, the directory
// holding one JSON file per saved game. Run with `make test`.
// Pure simulation layer: no SDL/imgui/OpenGL — no window is needed.
//
// Each test works in its own fresh directory under build/test-saves/library/
// (gitignored); the test must run with the repo root as working directory
// (`make test` does). Reopening a SaveLibrary on the same directory is how
// the tests check that something really reached the files. Ids don't
// survive reopening, so reopened saves are found by name.
#include "sim/balance.h"
#include "persistence/save_library.h"
#include "sim/scenario.h"
#include "sim/world.h"
#include "persistence/world_json.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

const fs::path kLibraryDir = "build/test-saves/library";

// A directory for one test, cleared of any previous run's files. The saves
// directory itself (returned) doesn't exist yet: SaveLibrary creates it.
fs::path fresh_dir(const std::string& name) {
    const fs::path dir = kLibraryDir / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir / "saves";
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

// The filenames in `dir`.
std::set<std::string> files_in(const fs::path& dir) {
    std::set<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir))
        names.insert(entry.path().filename().string());
    return names;
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

// The one save called `name` (asserts there is exactly one).
SaveId find_by_name(const SaveLibrary& library, const std::string& name) {
    const auto matches = std::count_if(library.saves().begin(), library.saves().end(),
                                       [&](const SaveSummary& s) { return s.name == name; });
    assert(matches == 1);
    for (const SaveSummary& summary : library.saves())
        if (summary.name == name)
            return summary.id;
    return 0;
}

// --- the tests -------------------------------------------------------------

void create_lists_and_persists() {
    const fs::path dir = fresh_dir("create");
    SaveLibrary library(dir.string());
    assert(library.saves().empty());
    assert(library.warning().empty());
    assert(!fs::exists(dir));  // nothing written until something changes

    assert(!library.create(""));
    const auto id = library.create("Game 1");
    assert(id.has_value());
    assert(files_in(dir) == std::set<std::string>{"game_1.json"});
    assert(library.saves().size() == 1);

    // A fresh library on the same directory sees the same save...
    SaveLibrary reopened(dir.string());
    assert(reopened.warning().empty());
    assert(reopened.saves().size() == 1);
    const SaveSummary& summary = reopened.saves()[0];
    assert(summary.name == "Game 1");
    assert(summary.tick == 0);
    assert(summary.money == kStartingMoney);
    assert(summary.last_played > 0);

    // ...holding exactly the starter scenario.
    const auto world = reopened.load(summary.id);
    assert(world.has_value());
    assert(*world == create_default_world());

    // Newest first when created within the same second (ties go to the higher id).
    const auto second = reopened.create("Game 2");
    assert(second.has_value() && *second != summary.id);
    assert(reopened.saves()[0].id == *second);
    assert(files_in(dir) == (std::set<std::string>{"game_1.json", "game_2.json"}));
}

void store_round_trips() {
    const fs::path dir = fresh_dir("store");
    SaveLibrary library(dir.string());
    const auto id = library.create("Game 1");
    assert(id.has_value());

    const World played = played_world(1234);
    assert(library.store(*id, played));
    assert(!library.store(*id + 100, played));  // unknown id

    SaveLibrary reopened(dir.string());
    const SaveId reopened_id = find_by_name(reopened, "Game 1");
    const auto loaded = reopened.load(reopened_id);
    assert(loaded.has_value());
    assert(*loaded == played);
    assert(reopened.saves()[0].tick == played.get_tick());
    assert(reopened.saves()[0].money == 1234);
    assert(!reopened.load(reopened_id + 100));
}

void copy_is_independent() {
    const fs::path dir = fresh_dir("copy");
    SaveLibrary library(dir.string());
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
    assert(files_in(dir) == (std::set<std::string>{"original.json", "original_copy.json"}));

    // Playing on in the copy leaves the original as it was.
    const World second = played_world(222);
    assert(library.store(*copy, second));
    SaveLibrary reopened(dir.string());
    assert(*reopened.load(find_by_name(reopened, "Original")) == first);
    assert(*reopened.load(find_by_name(reopened, "Original (copy)")) == second);
}

void rename_and_remove() {
    const fs::path dir = fresh_dir("rename");
    SaveLibrary library(dir.string());
    const auto a = library.create("A");
    const auto b = library.create("B");
    assert(a.has_value() && b.has_value());

    // Renaming moves the save to a file named after its new name.
    assert(library.rename(*a, "Renamed"));
    assert(!library.rename(*a, ""));
    assert(!library.rename(*b + 100, "Nope"));
    assert(find_summary(library, *a)->name == "Renamed");
    assert(files_in(dir) == (std::set<std::string>{"b.json", "renamed.json"}));
    {
        SaveLibrary reopened(dir.string());
        find_by_name(reopened, "Renamed");
    }

    // A rename that keeps the filename stem keeps the file.
    assert(library.rename(*a, "RENAMED!"));
    assert(files_in(dir) == (std::set<std::string>{"b.json", "renamed.json"}));
    assert(SaveLibrary(dir.string()).saves().size() == 2);

    assert(library.remove(*b));
    assert(!library.remove(*b));  // already gone
    assert(library.saves().size() == 1);
    assert(!library.load(*b));
    assert(files_in(dir) == std::set<std::string>{"renamed.json"});
    assert(SaveLibrary(dir.string()).saves().size() == 1);

    // Ids are never reused, even after the highest one is deleted.
    const auto c = library.create("C");
    assert(c.has_value() && *c != *a && *c != *b);

    assert(library.remove(*a));
    assert(library.remove(*c));
    assert(library.saves().empty());
    assert(files_in(dir).empty());
    assert(SaveLibrary(dir.string()).saves().empty());
}

void filenames_from_special_names() {
    assert(save_file_stem("Game 1") == "game_1");
    assert(save_file_stem("Bob's Game #2") == "bob_s_game_2");
    assert(save_file_stem("  --Spaced  out--  ") == "spaced_out");
    assert(save_file_stem("../../etc/passwd") == "etc_passwd");
    assert(save_file_stem("!!!") == "save");
    assert(save_file_stem("Café Größe") == "caf_gr_e");
    assert(save_file_stem("日本") == "save");
    assert(save_file_stem("CON") == "save_con");
    assert(save_file_stem("lpt1") == "save_lpt1");
    assert(save_file_stem("console") == "console");
    assert(save_file_stem(std::string(100, 'x')) == std::string(40, 'x'));
    assert(save_file_stem(std::string(39, 'x') + " y") == std::string(39, 'x'));

    // The real name, special characters and all, survives in the file.
    const fs::path dir = fresh_dir("special");
    SaveLibrary library(dir.string());
    const std::vector<std::string> names = {"Bob's Game #2", "!!!", "日本", "日本 / Café", "CON",
                                            "a\"b\\c\nd"};
    for (const std::string& name : names)
        assert(library.create(name).has_value());
    assert(files_in(dir) == (std::set<std::string>{"bob_s_game_2.json", "save.json", "save-2.json", "caf.json",
                                                   "save_con.json", "a_b_c_d.json"}));

    SaveLibrary reopened(dir.string());
    assert(reopened.warning().empty());
    assert(reopened.saves().size() == names.size());
    for (const std::string& name : names)
        assert(reopened.load(find_by_name(reopened, name)).has_value());
}

void duplicate_filenames_get_numbered() {
    const fs::path dir = fresh_dir("duplicates");
    SaveLibrary library(dir.string());
    const auto first = library.create("Game 1");
    const auto second = library.create("Game 1");
    const auto third = library.create("game 1!");
    assert(first && second && third);
    assert(*first != *second && *second != *third);
    assert(files_in(dir) == (std::set<std::string>{"game_1.json", "game_1-2.json", "game_1-3.json"}));

    // Each is its own save.
    assert(library.store(*second, played_world(2)));
    assert(library.load(*first)->get_money() == kStartingMoney);
    assert(library.load(*second)->get_money() == 2);

    // Copies get their own file too, never the source's.
    const auto copy = library.copy(*first, "Game 1");
    assert(copy.has_value());
    assert(files_in(dir).count("game_1-4.json") == 1);

    // Renaming into a taken name gets a numbered file; renaming a numbered
    // file to a name with the same stem keeps it where it is.
    const auto other = library.create("Other");
    assert(other.has_value());
    assert(library.rename(*other, "GAME 1"));
    assert(files_in(dir).count("other.json") == 0);
    assert(files_in(dir).count("game_1-5.json") == 1);
    assert(library.rename(*third, "Game 1"));
    assert(files_in(dir).size() == 5);

    // A freed name is reused by the next save that wants it.
    assert(library.remove(*first));
    assert(library.create("Game_1").has_value());
    assert(files_in(dir).count("game_1.json") == 1);

    SaveLibrary reopened(dir.string());
    assert(reopened.warning().empty());
    assert(reopened.saves().size() == 5);
    for (const SaveSummary& save : reopened.saves())
        assert(save_file_stem(save.name) == "game_1");
}

void corrupt_files_are_skipped_and_kept() {
    const fs::path dir = fresh_dir("corrupt");
    fs::create_directories(dir);
    const std::string garbage = "{this is not json";
    const std::string future = R"({"format_version": 999, "name": "Game 1", "last_played": 0, "world": {}})";
    write_file(dir / "game_1.json", garbage);
    write_file(dir / "game_1-2.json", future);
    write_file(dir / "notes.txt", "not a save, not a .json file");
    write_file(dir / "game_2.json.tmp", "a leftover temp file");

    SaveLibrary library(dir.string());
    assert(library.saves().empty());
    assert(!library.warning().empty());
    assert(library.warning().find("game_1.json") != std::string::npos);
    assert(library.warning().find("game_1-2.json") != std::string::npos);
    assert(library.warning().find("notes.txt") == std::string::npos);

    // New saves go around them rather than over them.
    const auto id = library.create("Game 1");
    assert(id.has_value());
    assert(files_in(dir).count("game_1-3.json") == 1);
    assert(slurp(dir / "game_1.json") == garbage);
    assert(slurp(dir / "game_1-2.json") == future);

    SaveLibrary reopened(dir.string());
    assert(!reopened.warning().empty());
    assert(reopened.saves().size() == 1);
    assert(reopened.load(reopened.saves()[0].id).has_value());
}

void broken_world_still_lists() {
    const fs::path dir = fresh_dir("broken_world");
    SaveLibrary library(dir.string());
    assert(library.create("Broken").has_value());
    assert(library.create("Fine").has_value());

    // Damage one save's world by hand; the file itself stays well-formed.
    nlohmann::json doc = nlohmann::json::parse(slurp(dir / "broken.json"));
    doc.at("world")["format_version"] = 999;
    write_file(dir / "broken.json", doc.dump(2));

    SaveLibrary reopened(dir.string());
    assert(reopened.warning().empty());
    assert(reopened.saves().size() == 2);
    const SaveId broken = find_by_name(reopened, "Broken");
    assert(!reopened.load(broken));
    assert(reopened.load(find_by_name(reopened, "Fine")).has_value());
    assert(reopened.remove(broken));
    assert(reopened.saves().size() == 1);
    assert(files_in(dir) == std::set<std::string>{"fine.json"});
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
    filenames_from_special_names();
    printf("PASS filenames_from_special_names\n");
    duplicate_filenames_get_numbered();
    printf("PASS duplicate_filenames_get_numbered\n");
    corrupt_files_are_skipped_and_kept();
    printf("PASS corrupt_files_are_skipped_and_kept\n");
    broken_world_still_lists();
    printf("PASS broken_world_still_lists\n");

    printf("all save library tests passed\n");
    return 0;
}
