// ui.cpp — the presentation layer's shared core: the per-game UI state and
// its reset, selection, time and rule formatting, and the dockspace that
// hosts the inspector beside the map. The map, HUD, inspector, statistics,
// menu and tutorial each live in their own ui_*.cpp.
#include "ui.h"
#include "ui_internal.h"

#include "entities.h"
#include "game_types.h"
#include "world.h"
#include "imgui.h"
#include "imgui_internal.h"  // DockBuilder* (imgui 'docking' branch)

#include <cstdio>
#include <string>
#include <vector>

ui::GameUiState ui::g_game;

namespace {

ImGuiID MainDockSpaceId() { return ImGui::GetID("MainDockSpace"); }

// The left dock node the inspector is pinned into; set by DrawDockspace on
// the first frame and used by DrawInspector to re-dock it every frame. Part
// of the window layout rather than of one game, so ResetGameUi keeps it.
ImGuiID g_inspector_dock_id = 0;

}  // namespace

void ResetGameUi() {
    ui::g_game = ui::GameUiState{};
}

void ui::SelectOffice(PostOfficeId id) {
    g_game.selected_truck.reset();
    g_game.selected_office = id;
}

void ui::EnsureOfficeSelected(const World& world) {
    if (g_game.selected_office >= world.get_post_offices().size())
        g_game.selected_office = 0;
}

// ---------------------------------------------------------------------------
// Tick -> 24-hour clock formatting. Every time shown to the player goes
// through these helpers; internally the simulation only ever uses ticks.
// ---------------------------------------------------------------------------

// Format an absolute tick as "Day N HH:MM".
void ui::FormatDayTime(Tick tick, char* buf, std::size_t buf_len) {
    const ClockTime t = tick_to_time_of_day(tick);
    std::snprintf(buf, buf_len, "Day %llu  %02d:%02d",
                  static_cast<unsigned long long>(tick / kTicksPerDay) + 1, t.hour, t.minute);
}

// Format a tick duration as "HH:MM" (hours may exceed 23).
void ui::FormatDuration(Tick duration, char* buf, std::size_t buf_len) {
    const ClockTime t = duration_to_hours_minutes(duration);
    std::snprintf(buf, buf_len, "%02d:%02d", t.hour, t.minute);
}

std::string ui::RuleSummary(const RoutingRule& rule, const std::vector<PostOffice>& offices,
                            PostOfficeId dst) {
    switch (rule.type) {
    case RoutingRuleType::ExactDestination:
        return offices[dst].name + " mail";
    case RoutingRuleType::PostalPrefix: {
        std::string text = "Prefix ";
        for (std::size_t i = 0; i < rule.prefixes.size(); ++i)
            text += (i == 0 ? "" : ", ") + rule.prefixes[i];
        return text;
    }
    case RoutingRuleType::AllLetters:
        return "All mail";
    }
    return "";
}

ImGuiID ui::InspectorDockId() { return g_inspector_dock_id; }

void ui::GetMapArea(ImVec2& min, ImVec2& size) {
    min = {0.0f, 0.0f};
    size = ImGui::GetIO().DisplaySize;
    if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(MainDockSpaceId())) {
        min = central->Pos;
        size = central->Size;
    }
}

// ---------------------------------------------------------------------------
// Dockspace: a fullscreen docking area over the main viewport.
//
// The central node is passthrough (ImGuiDockNodeFlags_PassthruCentralNode), so
// the map — drawn into the background draw list — stays visible and clickable
// through it. The layout is fixed: the inspector ("###Inspector") is pinned
// into a left split (DrawInspector re-docks it every frame and its node
// can't be undocked), and no other window can dock anywhere. Only the split
// width is user-adjustable, and it persists in imgui.ini.
// ---------------------------------------------------------------------------

void DrawDockspace() {
    const ImGuiID dockspace_id = MainDockSpaceId();

    // Decided once, on the first frame (imgui.ini is loaded by then). An ini
    // layout with a left split is reused to keep the user's panel width;
    // anything else (first run, an ini from an older build) is rebuilt.
    if (g_inspector_dock_id == 0) {
        const ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspace_id);
        if (root != nullptr && root->SplitAxis == ImGuiAxis_X && root->ChildNodes[0] != nullptr &&
            !root->ChildNodes[0]->IsCentralNode() && root->ChildNodes[1] != nullptr &&
            root->ChildNodes[1]->IsCentralNode())
            g_inspector_dock_id = root->ChildNodes[0]->ID;
    }

    // DockBuilder* must run before the dockspace node is submitted this frame.
    if (g_inspector_dock_id == 0) {
        ImGui::DockBuilderRemoveNode(dockspace_id);  // drops any stale layout; no-op on a fresh node
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

        // Split off the left third for the inspector; the remainder stays
        // as the passthrough central node over the map.
        ImGuiID central_id = dockspace_id;
        g_inspector_dock_id =
            ImGui::DockBuilderSplitNode(central_id, ImGuiDir_Left, 0.35f, nullptr, &central_id);
        ImGui::DockBuilderFinish(dockspace_id);
    }

    // NoDockingSplit / NoDockingOverCentralNode: nothing can dock into the
    // map area or split off new nodes.
    ImGui::DockSpaceOverViewport(dockspace_id, nullptr,
                                 ImGuiDockNodeFlags_PassthruCentralNode |
                                     ImGuiDockNodeFlags_NoDockingSplit |
                                     ImGuiDockNodeFlags_NoDockingOverCentralNode);
}
