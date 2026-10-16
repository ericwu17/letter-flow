// ui_internal.h — state and helpers shared between the presentation-layer
// files (ui.cpp, ui_map.cpp, ui_hud.cpp, ui_inspector.cpp, ui_stats.cpp and
// ui_tutorial.cpp). Not part of ui.h's public surface: main.cpp and the
// simulation never see these.
#pragma once

#include "sim/entities.h"
#include "sim/game_types.h"
#include "sim/road_builder.h"
#include "sim/truck.h"
#include "sim/tutorial.h"
#include "imgui.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class World;

namespace ui {

// ---------------------------------------------------------------------------
// Per-game UI state
// ---------------------------------------------------------------------------

struct CameraState {
    float zoom = 1.0f;
    ImVec2 pan = {0.0f, 0.0f};
    // The left button last went down on the map (not over a window or the
    // HUD), and whether that press has since moved past ImGui's drag
    // threshold and is panning the map.
    bool press_on_map = false;
    bool dragging = false;
};

// Road building: Idle, then picking the source office, then placing bends
// until the player clicks the destination office.
enum class RoadBuildState { Idle, SelectSource, PlacingVertices };

// The road whose destroy menu is open, keyed by its endpoint-id pair — never
// an index or pointer — because remove_road() erases from the roads vector.
struct RoadKey {
    PostOfficeId src = kNoPostOffice;
    PostOfficeId dst = kNoPostOffice;
};

struct RoadMenuState {
    std::optional<RoadKey> road;
    ImVec2 pos = {0.0f, 0.0f};    // click point, screen space
    bool open_requested = false;  // latch for OpenPopup in DrawRoadMenu
    bool confirm = false;         // two-step confirm: first click arms
};

// Floating money text that rises from an office and fades out: a green "+$N"
// when a truck delivers there, a red "-$N" when a truck departs from there.
// Animated in real time (frame delta), so it looks the same at any game
// speed; purely presentational.
struct MoneyPopup {
    PostOfficeId office = kNoPostOffice;
    int amount = 0;
    float age = 0.0f;  // seconds
};

// A ring expanding out of an office that just appeared on the map. Real
// time, like MoneyPopup; purely presentational.
struct SpawnPulse {
    PostOfficeId office = kNoPostOffice;
    float age = 0.0f;  // seconds
};

// The inspector's editable fields for one office. DrawOfficePanel resets
// them whenever the inspected office changes.
struct InspectorForms {
    PostOfficeId office = kNoPostOffice;  // whose values the fields below hold
    // Why the last schedule-table edit was refused, shown under the table
    // until the next accepted edit.
    std::string edit_error;
    // The schedule clicked in the schedule table (by id, so it survives
    // reorders), and the one under the mouse last frame: the outbound letters
    // the hovered one's rule matches are highlighted, else the selected one's.
    ScheduleId selected_schedule = kNoSchedule;
    ScheduleId hovered_schedule = kNoSchedule;
    // The header's postal code: shown as a tag until its pencil is clicked,
    // then as a field (focused on the frame after the click) until it loses
    // focus. InputText needs a mutable buffer. Every keystroke is committed
    // to the World, so the map label updates live.
    bool editing_postal_code = false;
    bool focus_postal_code = false;
    char postal_code[128] = "";
};

// Everything the presentation layer remembers about one game, in one place,
// so ResetGameUi() forgets it all with a single assignment. Each ui_*.cpp
// reads and writes its own part of g_game.
struct GameUiState {
    // The current selection (clicked on the map or picked in the
    // inspector). A selected truck is only observed: the World stays its sole
    // owner, and the weak_ptr expires the moment the World retires the truck
    // on arrival. While no live truck is selected the inspector shows
    // selected_office, which doubles as the backup: selecting a truck points
    // it at the truck's destination, so an arrival falls through to that
    // office with no extra bookkeeping. kNoPostOffice until the first
    // EnsureOfficeSelected.
    std::weak_ptr<const Truck> selected_truck;
    PostOfficeId selected_office = kNoPostOffice;

    CameraState camera;

    RoadBuildState road_build_state = RoadBuildState::Idle;
    RoadBuilder road_builder;
    RoadMenuState road_menu;

    std::vector<MoneyPopup> money_popups;
    std::vector<SpawnPulse> spawn_pulses;
    // How many offices the map has seen: any beyond this count are new and
    // get a SpawnPulse. Unset until the game's first frame, so the offices a
    // game opens or loads with don't all pulse at once.
    std::optional<std::size_t> known_offices;
    // The network event the HUD last saw (by its start tick), and how long
    // ago a new one was announced: its HUD section flashes for a moment.
    Tick known_event_start = 0;
    float event_flash_age = 1e9f;  // seconds; huge = not flashing
    // The letter hovered in a list last frame (HighlightLetter). DrawWorld
    // draws its journey and then clears it, so the highlight lapses as soon
    // as the list stops asking for it.
    std::optional<Letter> highlight_letter;

    InspectorForms forms;

    // The statistics window (ui_stats.cpp). Its time range is a viewing
    // preference that carries over between games, so it is not kept here.
    bool stats_open = false;
    PostOfficeId stats_office = kNoPostOffice;  // picked in the Offices tab
    ScheduleId stats_schedule = kNoSchedule;    // picked in the Schedules tab

    // The pause menu (ui_menu.cpp), opened with Esc or the HUD's Menu button.
    bool pause_menu_open = false;
    // How the menu's last Save went, shown under its buttons until it closes.
    std::string pause_menu_status;
    bool pause_menu_status_failed = false;
};

extern GameUiState g_game;  // defined in ui.cpp

// --- ui.cpp -----------------------------------------------------------------

// Selects an office (and drops any truck selection), as a map click does.
void SelectOffice(PostOfficeId id);

// Defaults the selection to the first office until the player picks one
// (and guards against an id from a different world).
void EnsureOfficeSelected(const World& world);

// Every routing rule type, in the order the UI offers them.
inline constexpr RoutingRuleType kRuleTypes[] = {RoutingRuleType::ExactDestination,
                                                 RoutingRuleType::PostalPrefix,
                                                 RoutingRuleType::AllLetters};

// A rule in a few words: "Princeton mail" / "Prefix NJ/Mercer, PA" / "All
// mail". `dst` is the destination of the schedule the rule belongs to.
std::string RuleSummary(const RoutingRule& rule, const std::vector<PostOffice>& offices,
                        PostOfficeId dst);

// Tick -> "Day N  HH:MM" / duration -> "HH:MM" (hours may exceed 23).
void FormatDayTime(Tick tick, char* buf, std::size_t buf_len);
void FormatDuration(Tick duration, char* buf, std::size_t buf_len);

// The visible map area: the dockspace's central node, which excludes the
// docked inspector (the whole display before the dockspace first exists).
void GetMapArea(ImVec2& min, ImVec2& size);

// The left dock node the inspector is pinned into (set by DrawDockspace).
ImGuiID InspectorDockId();

// --- ui_map.cpp -------------------------------------------------------------

// Asks the map to highlight this letter's journey (its trail, its truck if it
// is on one, and its destination) on the next frame. Call it every frame
// the letter is hovered in a list; the highlight lapses one frame after the
// last call.
void HighlightLetter(const Letter& letter);

// Where an office is drawn on screen, with the current camera.
ImVec2 OfficeScreenPos(PostOfficeId office, const World& world);

// --- ui_hud.cpp -------------------------------------------------------------

// Whether screen point `p` is over the transport bar (which the map must not
// treat as a click on itself).
bool OverTransportBar(const World& world, ImVec2 p);

// --- ui_stats.cpp -----------------------------------------------------------

// The statistics window's contents (range buttons and tabs), drawn into the
// current window: the floating window while the game runs, the game-over
// card's statistics popup once it has ended.
void DrawStatsContents(const World& world);

// The inspector's "Statistics" section for one office: range totals and
// charts over the time range picked in the statistics window.
void DrawOfficeStats(const World& world, PostOfficeId office);

// The inspector schedule table's "Load" cell: how full this schedule's
// trucks ran over the selected range, with a bar per recent departure and
// the details in a tooltip.
void DrawScheduleLoadCell(const TruckSchedule& schedule, Tick now);

// A tooltip body narrating a letter's trail: where it waited, when it left
// on which schedule, where it arrived, and where it is now (or when it was
// delivered). `offices` resolves office and schedule names.
void DrawLetterTrail(const Letter& letter, const std::vector<PostOffice>& offices, Tick now);

// --- ui_tutorial.cpp --------------------------------------------------------

// Records where a tutorial anchor was drawn this frame, so the tutorial can
// spotlight it. NoteItemAnchor takes the last submitted item's rect. Cheap,
// and harmless outside the tutorial: a rect only counts in the frame it was
// noted in.
void NoteAnchor(TutorialAnchor anchor, ImVec2 min, ImVec2 max);
void NoteItemAnchor(TutorialAnchor anchor);

}  // namespace ui
