// ui_hud.cpp — the overlays on the map: the transport bar (play/pause,
// speed, day/time) and the HUD card (money, deliveries, statistics button).
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "ui/ui_theme.h"

#include "sim/game_types.h"
#include "sim/world.h"
#include "imgui.h"
#include "implot.h"  // ImPlot::ShowDemoWindow (the HUD's demo toggle)

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>

namespace {

using ui::FormatDayTime;
using ui::g_game;
namespace theme = ui::theme;
using theme::U32;

// The transport bar's colors, named by role (see the palette in ui_theme.h).
constexpr ImU32 kColorText          = U32(theme::kInk);          // icons, time readout
constexpr ImU32 kColorTextDim       = U32(theme::kInkDim);       // speed label at 1x
constexpr ImU32 kColorHoverBg       = U32(theme::kPaper3);       // hovered transport buttons
constexpr ImU32 kColorDisabledIcon  = U32(theme::kPaper4);
constexpr ImU32 kColorPlayBg        = U32(theme::kRed);          // play button while paused
constexpr ImU32 kColorPlayBgHover   = U32(theme::kRedHover);
constexpr ImU32 kColorPlayIcon      = U32(theme::kPaperLight);
constexpr ImU32 kColorFastForward   = U32(theme::kRed);          // speed label above 1x
constexpr ImU32 kColorSeparator     = U32(theme::kPaper3);
constexpr ImU32 kColorProgressBg    = U32(theme::kPaper2);
constexpr ImU32 kColorProgressFill  = U32(theme::kNavy);
constexpr ImU32 kColorProgressTick  = U32(theme::kPaper4);

// ---------------------------------------------------------------------------
// Transport bar layout: play/pause, slow-down/speed-up buttons around the
// current speed label, and the day/time
// readout, anchored top-center of the map area — the dockspace's central
// node, i.e. the display minus the docked inspector panel. Every rect is
// recomputed from that area and font metrics each frame, so the bar stays
// centered and correctly sized across window resizes, dock-layout changes and
// DPI scaling; on very narrow areas the day progress bar shrinks first.
// ---------------------------------------------------------------------------

// Simulation speeds the slow-down / speed-up buttons step through.
constexpr float kSpeedSteps[] = {1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f};
constexpr int kSpeedStepCount = sizeof(kSpeedSteps) / sizeof(kSpeedSteps[0]);
constexpr const char* kWidestSpeedText = "32x";  // reserves a fixed label width

// Next step above / below the current multiplier. Works from the multiplier
// itself rather than an index, so a value loaded from a save that isn't one of
// the steps still snaps to the neighbouring step.
float NextFasterSpeed(float current) {
    for (float s : kSpeedSteps)
        if (s > current) return s;
    return kSpeedSteps[kSpeedStepCount - 1];
}

float NextSlowerSpeed(float current) {
    for (int i = kSpeedStepCount - 1; i >= 0; --i)
        if (kSpeedSteps[i] < current) return kSpeedSteps[i];
    return kSpeedSteps[0];
}

struct TransportLayout {
    char time_text[64];
    float button_size = 0.0f;
    ImVec2 bar_min = {0.0f, 0.0f}, bar_max = {0.0f, 0.0f};
    ImVec2 play_min = {0.0f, 0.0f}, play_max = {0.0f, 0.0f};
    char speed_text[16];
    ImVec2 slower_min = {0.0f, 0.0f}, slower_max = {0.0f, 0.0f};
    ImVec2 speed_text_pos = {0.0f, 0.0f};  // "4x" label, centered in a fixed-width slot
    ImVec2 faster_min = {0.0f, 0.0f}, faster_max = {0.0f, 0.0f};
    ImVec2 separator_top = {0.0f, 0.0f}, separator_bottom = {0.0f, 0.0f};
    ImVec2 text_pos = {0.0f, 0.0f};
    ImVec2 progress_min = {0.0f, 0.0f}, progress_max = {0.0f, 0.0f};
};

// Manual point-in-rect instead of ImGui::IsMouseHoveringRect: the latter
// clips against the current window, and the transport bar is drawn at the
// top level, outside any Begin/End pair.
bool PointInRect(ImVec2 p, ImVec2 r_min, ImVec2 r_max) {
    return p.x >= r_min.x && p.y >= r_min.y && p.x < r_max.x && p.y < r_max.y;
}

TransportLayout ComputeTransportLayout(const World& world) {
    TransportLayout l;

    // Anchor the bar to the visible map area, which excludes the docked
    // inspector panel
    ImVec2 area_min, area_size;
    ui::GetMapArea(area_min, area_size);

    const float fh = ImGui::GetFrameHeight();  // follows font size / DPI scaling
    const float button = fh;
    const float gap = fh * 0.25f;
    const float pad = fh * 0.3f;
    const float separator_w = 1.0f;

    FormatDayTime(world.get_tick(), l.time_text, sizeof(l.time_text));
    snprintf(l.speed_text, sizeof(l.speed_text), "%gx", world.get_speed_multiplier());

    const float time_w = ImGui::CalcTextSize(l.time_text).x;
    const float speed_w = ImGui::CalcTextSize(kWidestSpeedText).x;  // fixed, so the bar doesn't jump
    const float fixed_w = button + gap + button + gap + speed_w + gap + button
                        + gap + separator_w + gap + time_w + gap;
    float progress_w = std::min(140.0f, area_size.x * 0.15f);

    // Shrink the progress bar first on narrow areas.
    const float avail_w = area_size.x - 2.0f * pad - fh;
    if (fixed_w + progress_w > avail_w)
        progress_w = std::max(0.0f, avail_w - fixed_w);

    const float bar_w = fixed_w + progress_w + 2.0f * pad;
    const float bar_h = button + 2.0f * pad;
    l.button_size = button;
    l.bar_min = {area_min.x + std::max((area_size.x - bar_w) * 0.5f, fh * 0.25f),
                 area_min.y + fh * 0.4f};
    l.bar_max = {l.bar_min.x + bar_w, l.bar_min.y + bar_h};

    const float cy = (l.bar_min.y + l.bar_max.y) * 0.5f;
    float x = l.bar_min.x + pad;

    l.play_min = {x, cy - button * 0.5f};
    l.play_max = {x + button, cy + button * 0.5f};
    x += button + gap;

    l.slower_min = {x, cy - button * 0.5f};
    l.slower_max = {x + button, cy + button * 0.5f};
    x += button + gap;

    const ImVec2 speed_size = ImGui::CalcTextSize(l.speed_text);
    l.speed_text_pos = {x + (speed_w - speed_size.x) * 0.5f, cy - speed_size.y * 0.5f};
    x += speed_w + gap;

    l.faster_min = {x, cy - button * 0.5f};
    l.faster_max = {x + button, cy + button * 0.5f};
    x += button + gap;

    l.separator_top = {x, cy - button * 0.4f};
    l.separator_bottom = {x, cy + button * 0.4f};
    x += separator_w + gap;

    l.text_pos = {x, cy - ImGui::CalcTextSize(l.time_text).y * 0.5f};
    x += time_w + gap;

    const float progress_h = std::max(4.0f, fh * 0.2f);
    l.progress_min = {x, cy - progress_h * 0.5f};
    l.progress_max = {x + progress_w, cy + progress_h * 0.5f};

    return l;
}

// This frame's layout: computed on first use (the map's hit test asks before
// the bar is drawn) and shared by the map, the bar and the HUD. Its geometry
// depends only on the map area, the font and the clock, none of which
// change within a frame.
const TransportLayout& FrameTransportLayout(const World& world) {
    static TransportLayout layout;
    static int layout_frame = -1;
    if (layout_frame != ImGui::GetFrameCount()) {
        layout = ComputeTransportLayout(world);
        layout_frame = ImGui::GetFrameCount();
    }
    return layout;
}

// One "label ....... value" row of the HUD's stat table, value right-aligned.
void HudStatRow(const char* label, const char* value, ImVec4 value_color) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    const float value_w = ImGui::CalcTextSize(value).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - value_w));
    ImGui::TextColored(value_color, "%s", value);
}

}  // namespace

bool ui::OverTransportBar(const World& world, ImVec2 p) {
    const TransportLayout& bar = FrameTransportLayout(world);
    return PointInRect(p, bar.bar_min, bar.bar_max);
}

// ---------------------------------------------------------------------------
// Transport bar (play/pause, slow-down/speed-up, day/time) — drawn into the
// background draw list, on top of the map but underneath ImGui windows.
// ---------------------------------------------------------------------------

void DrawTransportBar(World& world) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const TransportLayout& l = FrameTransportLayout(world);
    const float speed = world.get_speed_multiplier();
    const bool fast_forward = speed > 1.0f;
    const bool can_slow = speed > kSpeedSteps[0];
    const bool can_speed_up = speed < kSpeedSteps[kSpeedStepCount - 1];
    const float b = l.button_size;
    const float rounding = b * 0.2f;

    // Panel.
    theme::DrawShadowedPanel(draw, l.bar_min, l.bar_max, rounding * 1.5f);
    ui::NoteAnchor(TutorialAnchor::TransportBar, l.bar_min, l.bar_max);

    // Background drawing has no widget behaviour, so the buttons are plain
    // rects hit-tested against the mouse. WantCaptureMouse keeps clicks that
    // landed on an ImGui window from leaking through to the bar.
    const bool interactive = !io.WantCaptureMouse;
    const bool hover_play = interactive && PointInRect(io.MousePos, l.play_min, l.play_max);
    const bool hover_slower = interactive && can_slow && PointInRect(io.MousePos, l.slower_min, l.slower_max);
    const bool hover_faster = interactive && can_speed_up && PointInRect(io.MousePos, l.faster_min, l.faster_max);
    if (hover_play || hover_slower || hover_faster)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const ImU32 icon_color = kColorText;
    const ImU32 hover_bg = kColorHoverBg;

    // Play/pause: a triangle when paused, two bars when running. While paused
    // the button is a red disc, so the way to resume stands out.
    const ImVec2 play_center = {(l.play_min.x + l.play_max.x) * 0.5f, (l.play_min.y + l.play_max.y) * 0.5f};
    if (world.is_paused())
        draw->AddCircleFilled(play_center, b * 0.5f, hover_play ? kColorPlayBgHover : kColorPlayBg);
    else if (hover_play)
        draw->AddRectFilled(l.play_min, l.play_max, hover_bg, rounding);
    if (world.is_paused())
        draw->AddTriangleFilled({l.play_min.x + b * 0.38f, l.play_min.y + b * 0.28f},
                                {l.play_min.x + b * 0.38f, l.play_min.y + b * 0.72f},
                                {l.play_min.x + b * 0.74f, l.play_min.y + b * 0.50f}, kColorPlayIcon);
    else {
        draw->AddRectFilled({l.play_min.x + b * 0.30f, l.play_min.y + b * 0.28f},
                            {l.play_min.x + b * 0.44f, l.play_min.y + b * 0.72f}, icon_color);
        draw->AddRectFilled({l.play_min.x + b * 0.56f, l.play_min.y + b * 0.28f},
                            {l.play_min.x + b * 0.70f, l.play_min.y + b * 0.72f}, icon_color);
    }

    // Slow-down / speed-up: double triangles pointing left / right around the
    // speed label, which turns orange while fast-forwarding. A button at the
    // end of the range is drawn faded and ignores the mouse.
    if (hover_slower)
        draw->AddRectFilled(l.slower_min, l.slower_max, hover_bg, rounding);
    const ImU32 slower_color = can_slow ? kColorText : kColorDisabledIcon;
    draw->AddTriangleFilled({l.slower_min.x + b * 0.82f, l.slower_min.y + b * 0.30f},
                            {l.slower_min.x + b * 0.82f, l.slower_min.y + b * 0.70f},
                            {l.slower_min.x + b * 0.52f, l.slower_min.y + b * 0.50f}, slower_color);
    draw->AddTriangleFilled({l.slower_min.x + b * 0.52f, l.slower_min.y + b * 0.30f},
                            {l.slower_min.x + b * 0.52f, l.slower_min.y + b * 0.70f},
                            {l.slower_min.x + b * 0.22f, l.slower_min.y + b * 0.50f}, slower_color);

    draw->AddText(l.speed_text_pos, fast_forward ? kColorFastForward : kColorTextDim, l.speed_text);

    if (hover_faster)
        draw->AddRectFilled(l.faster_min, l.faster_max, hover_bg, rounding);
    const ImU32 faster_color = can_speed_up ? kColorText : kColorDisabledIcon;
    draw->AddTriangleFilled({l.faster_min.x + b * 0.18f, l.faster_min.y + b * 0.30f},
                            {l.faster_min.x + b * 0.18f, l.faster_min.y + b * 0.70f},
                            {l.faster_min.x + b * 0.48f, l.faster_min.y + b * 0.50f}, faster_color);
    draw->AddTriangleFilled({l.faster_min.x + b * 0.48f, l.faster_min.y + b * 0.30f},
                            {l.faster_min.x + b * 0.48f, l.faster_min.y + b * 0.70f},
                            {l.faster_min.x + b * 0.78f, l.faster_min.y + b * 0.50f}, faster_color);

    // Separator, day/time readout, day progress (with marks at 06, 12 and
    // 18 h, so the time of day reads at a glance).
    draw->AddLine(l.separator_top, l.separator_bottom, kColorSeparator, 1.0f);
    draw->AddText(l.text_pos, kColorText, l.time_text);
    const float progress_w = l.progress_max.x - l.progress_min.x;
    const float progress_rounding = (l.progress_max.y - l.progress_min.y) * 0.5f;
    draw->AddRectFilled(l.progress_min, l.progress_max, kColorProgressBg, progress_rounding);
    const float day_fraction = static_cast<float>(world.get_tick() % kTicksPerDay) / static_cast<float>(kTicksPerDay);
    if (day_fraction > 0.0f)
        draw->AddRectFilled(l.progress_min, {l.progress_min.x + progress_w * day_fraction, l.progress_max.y},
                            kColorProgressFill, progress_rounding);
    if (progress_w > 0.0f) {
        for (int quarter = 1; quarter < 4; ++quarter) {
            const float x = l.progress_min.x + progress_w * static_cast<float>(quarter) * 0.25f;
            draw->AddLine({x, l.progress_min.y - 3.0f}, {x, l.progress_min.y - 1.0f}, kColorProgressTick, 1.0f);
            draw->AddLine({x, l.progress_max.y + 1.0f}, {x, l.progress_max.y + 3.0f}, kColorProgressTick, 1.0f);
        }
    }

    // Clicks.
    if (interactive && io.MouseClicked[0]) {
        if (hover_play)
            world.set_paused(!world.is_paused());
        else if (hover_slower)
            world.set_speed_multiplier(NextSlowerSpeed(speed));
        else if (hover_faster)
            world.set_speed_multiplier(NextFasterSpeed(speed));
    }
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------


void DrawHUD(const World& world) {
    static bool show_imgui_demo = false;
    static bool show_implot_demo = false;
    static float last_hud_width = 0.0f;  // last frame's, to dodge the transport bar

    // Widget reference windows, kept handy while developing: F1 / F2.
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureKeyboard) {
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
            show_imgui_demo = !show_imgui_demo;
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
            show_implot_demo = !show_implot_demo;
    }

    // A card pinned to the top-right corner of the map area (the dockspace's
    // central node), like the transport bar, so it follows window resizes
    // and the inspector's width. When the area is too narrow for both side
    // by side, the card drops below the bar.
    ImVec2 area_min, area_size;
    ui::GetMapArea(area_min, area_size);
    const float margin = ImGui::GetFrameHeight() * 0.4f;
    const float right = area_min.x + area_size.x - margin;
    const TransportLayout& bar = FrameTransportLayout(world);
    const float top = right - last_hud_width < bar.bar_max.x + margin ? bar.bar_max.y + margin
                                                                      : area_min.y + margin;
    ImGui::SetNextWindowPos({right, top}, ImGuiCond_Always, {1.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::Begin("##hud", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    const ImVec2 hud_min = ImGui::GetWindowPos();
    const ImVec2 hud_size = ImGui::GetWindowSize();
    last_hud_width = hud_size.x;
    // The card's drop shadow goes underneath the window, on top of the map.
    theme::DrawShadowedPanel(ImGui::GetBackgroundDrawList(), hud_min,
                             {hud_min.x + hud_size.x, hud_min.y + hud_size.y}, ImGui::GetStyle().WindowRounding);

    ImGui::PushFont(nullptr, theme::kFontSmall);
    ImGui::TextColored(theme::kRed, "LETTER FLOW");
    ImGui::PopFont();

    // Money: green while above the starting balance, red once in debt.
    const int money = world.get_money();
    const ImVec4 money_color = money < 0 ? theme::kBad : money >= kStartingMoney ? theme::kGood : theme::kInk;
    ImGui::PushFont(nullptr, theme::kFontHuge);
    ImGui::TextColored(money_color, "%s$%d", money < 0 ? "-" : "", std::abs(money));
    ImGui::PopFont();

    if (ImGui::BeginTable("hud_stats", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00000").x);
        char value[32];
        std::snprintf(value, sizeof(value), "%zu", world.get_letters_delivered_on_time());
        HudStatRow("Delivered on time", value, theme::kGood);
        const std::size_t late = world.get_letters_delivered_late();
        std::snprintf(value, sizeof(value), "%zu", late);
        HudStatRow("Delivered late", value, late > 0 ? theme::kBad : theme::kInk);
        std::snprintf(value, sizeof(value), "%zu", world.get_trucks().size());
        HudStatRow("Trucks en route", value, theme::kInk);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (theme::PrimaryButton("Statistics  (P)", ImVec2(-FLT_MIN, 0.0f)))
        g_game.stats_open = !g_game.stats_open;
    ui::NoteItemAnchor(TutorialAnchor::StatsButton);
    ImGui::End();

    if (show_imgui_demo)
        ImGui::ShowDemoWindow(&show_imgui_demo);
    if (show_implot_demo)
        ImPlot::ShowDemoWindow(&show_implot_demo);
}
