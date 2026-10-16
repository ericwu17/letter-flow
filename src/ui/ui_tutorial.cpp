// ui_tutorial.cpp — Dear ImGui presentation of the tutorial (tutorial.h):
// the step cards and the spotlight on what each step asks the player to use.
//
// The first and last steps are centered modal popups; every other step is a
// card pinned to the bottom of the map area that leaves the game fully
// playable. The spotlight is a pulsing red outline around an anchor the
// other UI files recorded this frame (ui::NoteAnchor), or a ring around an
// office on the map. No window here saves settings to imgui.ini, and nothing
// about the tutorial is kept once the player leaves it.
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "ui/ui_theme.h"

#include "sim/entities.h"
#include "sim/tutorial.h"
#include "sim/world.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>

namespace {

namespace theme = ui::theme;
using theme::U32;

// Where each anchor was drawn, and in which frame: a rect only counts in the
// frame it was noted in, so a widget that is not drawn (another office is
// selected, a section is collapsed) gets no spotlight.
struct AnchorRect {
    ImVec2 min = {0.0f, 0.0f};
    ImVec2 max = {0.0f, 0.0f};
    int frame = -1;
};
std::array<AnchorRect, static_cast<std::size_t>(TutorialAnchor::Count)> g_anchors;

// "Exit tutorial" takes two clicks on a mid-tutorial card; the first arms
// it. Disarmed whenever the step changes.
bool g_exit_armed = false;
std::size_t g_last_step = std::numeric_limits<std::size_t>::max();

enum class CardAction { None, Next, Exit };

constexpr ImGuiWindowFlags kCardFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

const AnchorRect* FindAnchor(TutorialAnchor anchor) {
    if (anchor == TutorialAnchor::None || anchor == TutorialAnchor::Count)
        return nullptr;
    const AnchorRect& rect = g_anchors[static_cast<std::size_t>(anchor)];
    return rect.frame == ImGui::GetFrameCount() ? &rect : nullptr;
}

// 0..1, about twice a second, in real time.
float Pulse() {
    return 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f);
}

// A pulsing outline around a widget, over every window.
void SpotlightRect(ImVec2 min, ImVec2 max) {
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const float grow = 3.0f + 3.0f * Pulse();
    const ImVec2 a(min.x - grow, min.y - grow);
    const ImVec2 b(max.x + grow, max.y + grow);
    draw->AddRectFilled(a, b, U32(theme::kRed, 0.08f), 6.0f);
    draw->AddRect(a, b, U32(theme::kRed, 0.55f + 0.45f * Pulse()), 6.0f, 0, 3.0f);
}

// A pulsing ring around an office on the map: drawn into the background
// list after the map, so it sits on the map but under every window.
void RingOffice(const World& world, PostOfficeId office) {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImVec2 c = ui::OfficeScreenPos(office, world);
    const float r = 24.0f + 5.0f * Pulse();  // outside the office disc and its selection ring
    draw->AddCircleFilled(c, r + 4.0f, U32(theme::kRed, 0.12f));
    draw->AddCircle(c, r, U32(theme::kRed, 0.55f + 0.45f * Pulse()), 0, 3.0f);
}

// The step's spotlights: its extra rings, then either its office (until the
// inspector shows it) or its anchor.
void DrawSpotlights(const TutorialStep& step, const World& world) {
    for (const PostOfficeId office : step.rings)
        if (office != kNoPostOffice)
            RingOffice(world, office);
    const bool office_shown = ui::g_game.selected_truck.expired() && ui::g_game.selected_office == step.office;
    if (step.office != kNoPostOffice && !office_shown) {
        RingOffice(world, step.office);
        return;
    }
    const AnchorRect* rect = FindAnchor(step.anchor);
    if (rect != nullptr)
        SpotlightRect(rect->min, rect->max);
}

// The step counter, title and body, wrapped to `width`.
void DrawStepText(const Tutorial& tutorial, float width) {
    const TutorialStep& step = tutorial.current();
    char counter[32];
    std::snprintf(counter, sizeof(counter), "TUTORIAL  %zu / %zu", tutorial.index() + 1, Tutorial::count());
    ImGui::PushFont(nullptr, theme::kFontSmall);
    theme::Pill(counter, theme::kRed, theme::kPaperLight);
    ImGui::PopFont();
    ImGui::PushFont(nullptr, theme::kFontTitle);
    ImGui::TextUnformatted(step.title);
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::TextUnformatted(step.body);
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(width, ImGui::GetStyle().ItemSpacing.y));  // holds the card at its full width
}

// Puts the next button flush with the right edge of a `width`-wide card.
void AlignRight(const char* label, float start_x, float width) {
    const float button_w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), start_x + width - button_w));
}

// Opens a paper card pinned to the bottom center of the map area (shadowed
// like the HUD). Always followed by ImGui::End().
void BeginBottomCard(const char* id) {
    ImVec2 area_min, area_size;
    ui::GetMapArea(area_min, area_size);
    const float margin = ImGui::GetFrameHeight() * 0.6f;
    ImGui::SetNextWindowPos({area_min.x + area_size.x * 0.5f, area_min.y + area_size.y - margin},
                            ImGuiCond_Always, {0.5f, 1.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    ImGui::Begin(id, nullptr, kCardFlags);
    ImGui::PopStyleVar();
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    theme::DrawShadowedPanel(ImGui::GetBackgroundDrawList(), pos, {pos.x + size.x, pos.y + size.y},
                             ImGui::GetStyle().WindowRounding);
}

// A mid-tutorial step: its text, Next (info cards) or Skip (action steps),
// and Exit, which asks once more before leaving.
CardAction DrawStepCard(const Tutorial& tutorial) {
    const float width = ImGui::GetFontSize() * 25.0f;
    BeginBottomCard("##tutorial_card");
    const float start_x = ImGui::GetCursorPosX();
    DrawStepText(tutorial, width);

    CardAction action = CardAction::None;
    if (tutorial.current().done == nullptr) {
        if (theme::PrimaryButton("Next"))
            action = CardAction::Next;
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::kInkDim, "Continues once you've done it.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Skip"))
            action = CardAction::Next;
        ImGui::SetItemTooltip("Move on without doing this step");
    }
    const char* exit_label = g_exit_armed ? "Really exit?" : "Exit tutorial";
    AlignRight(exit_label, start_x, width);
    if (g_exit_armed)
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kBad);
    if (ImGui::Button(exit_label)) {
        if (g_exit_armed)
            action = CardAction::Exit;
        g_exit_armed = true;
    }
    if (g_exit_armed)
        ImGui::PopStyleColor();
    ImGui::End();
    return action;
}

// The first and last steps: a centered modal popup over the dimmed game.
// OpenPopup needs a current window, so the popup is hosted in an invisible,
// input-transparent one (like the road menu's).
CardAction DrawModalCard(const Tutorial& tutorial) {
    const bool first = tutorial.index() == 0;
    ImGui::Begin("##tutorial_modal_host", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
    if (!ImGui::IsPopupOpen("##tutorial_modal"))
        ImGui::OpenPopup("##tutorial_modal");

    CardAction action = CardAction::None;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 18.0f));
    const bool open = ImGui::BeginPopupModal("##tutorial_modal", nullptr,
                                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_AlwaysAutoResize |
                                                 ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (open) {
        DrawStepText(tutorial, ImGui::GetFontSize() * 26.0f);
        if (first) {
            if (theme::PrimaryButton("Start the tutorial"))
                action = CardAction::Next;
            ImGui::SameLine();
            if (ImGui::Button("Back to main menu"))
                action = CardAction::Exit;
        } else {
            if (theme::PrimaryButton("Back to main menu"))
                action = CardAction::Exit;
            ImGui::SameLine();
            if (ImGui::Button("Keep playing"))
                action = CardAction::Next;
        }
        if (action != CardAction::None)
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
    return action;
}

// After "Keep playing": the tutorial world stays a sandbox, with a small
// card that leads back to the menu.
CardAction DrawFinishedCard() {
    BeginBottomCard("##tutorial_card");
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::kInkDim, "Tutorial complete. Nothing here is saved.");
    ImGui::SameLine();
    const bool exit = theme::PrimaryButton("Back to main menu");
    ImGui::End();
    return exit ? CardAction::Exit : CardAction::None;
}

}  // namespace

void ui::NoteAnchor(TutorialAnchor anchor, ImVec2 min, ImVec2 max) {
    if (anchor == TutorialAnchor::None || anchor == TutorialAnchor::Count)
        return;
    g_anchors[static_cast<std::size_t>(anchor)] = {min, max, ImGui::GetFrameCount()};
}

void ui::NoteItemAnchor(TutorialAnchor anchor) {
    // Clipped to the window, so a section scrolled partly out of the
    // inspector is outlined where it shows, and one scrolled away not at all.
    if (!ImGui::IsItemVisible())
        return;
    const ImVec2 win_min = ImGui::GetWindowPos();
    const ImVec2 win_max(win_min.x + ImGui::GetWindowWidth(), win_min.y + ImGui::GetWindowHeight());
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    NoteAnchor(anchor, {std::max(min.x, win_min.x), std::max(min.y, win_min.y)},
               {std::min(max.x, win_max.x), std::min(max.y, win_max.y)});
}

bool DrawTutorial(Tutorial& tutorial, const World& world) {
    const ui::GameUiState& ui_state = ui::g_game;
    const TutorialUiFacts facts{ui_state.selected_office, !ui_state.selected_truck.expired(),
                                ui_state.road_build_state != ui::RoadBuildState::Idle, ui_state.stats_open};
    tutorial.update(world, facts);
    if (tutorial.index() != g_last_step) {
        g_exit_armed = false;
        g_last_step = tutorial.index();
    }

    CardAction action = CardAction::None;
    if (tutorial.finished()) {
        action = DrawFinishedCard();
    } else {
        DrawSpotlights(tutorial.current(), world);
        const bool modal = tutorial.index() == 0 || tutorial.index() + 1 == Tutorial::count();
        action = modal ? DrawModalCard(tutorial) : DrawStepCard(tutorial);
    }

    if (action == CardAction::Next)
        tutorial.next();
    if (action == CardAction::Exit) {
        g_exit_armed = false;
        g_last_step = std::numeric_limits<std::size_t>::max();
        return true;
    }
    return false;
}
