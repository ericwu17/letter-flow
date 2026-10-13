// ui_internal.h — helpers shared between the presentation-layer files
// (ui.cpp, ui_stats.cpp and ui_tutorial.cpp). Not part of ui.h's public
// surface: main.cpp and the simulation never see these.
#pragma once

#include "entities.h"
#include "game_types.h"
#include "tutorial.h"
#include "imgui.h"

#include <cstddef>
#include <vector>

class World;

namespace ui {

// --- ui.cpp -----------------------------------------------------------------

// Selects an office (and drops any truck selection), as a map click does.
void SelectOffice(PostOfficeId id);

// Tick -> "Day N  HH:MM" / duration -> "HH:MM" (hours may exceed 23).
void FormatDayTime(Tick tick, char* buf, std::size_t buf_len);
void FormatDuration(Tick duration, char* buf, std::size_t buf_len);

// Asks the map to highlight this letter's journey (its trail, its truck if it
// is on one, and its destination) on the next frame. Call it every frame
// the letter is hovered in a list; the highlight lapses one frame after the
// last call.
void HighlightLetter(const Letter& letter);

// The visible map area: the dockspace's central node, which excludes the
// docked inspector (the whole display before the dockspace first exists).
void GetMapArea(ImVec2& min, ImVec2& size);

// Where an office is drawn on screen, with the current camera.
ImVec2 OfficeScreenPos(PostOfficeId office, const World& world);

// The current selection and road-building mode, for the tutorial's checks.
PostOfficeId SelectedOffice();
bool TruckSelected();
bool BuildingRoad();

// --- ui_stats.cpp -----------------------------------------------------------

// Opens or closes the statistics window (also bound to the P key).
void ToggleStatsWindow();

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

// Whether the statistics window is open.
bool StatsWindowOpen();

// Closes the statistics window and forgets its office / schedule picks
// (ResetGameUi, between games).
void ResetStatsUi();

// --- ui_tutorial.cpp --------------------------------------------------------

// Records where a tutorial anchor was drawn this frame, so the tutorial can
// spotlight it. NoteItemAnchor takes the last submitted item's rect. Cheap,
// and harmless outside the tutorial: a rect only counts in the frame it was
// noted in.
void NoteAnchor(TutorialAnchor anchor, ImVec2 min, ImVec2 max);
void NoteItemAnchor(TutorialAnchor anchor);

}  // namespace ui
