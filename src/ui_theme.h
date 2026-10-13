// ui_theme.h — the warm "postal" look shared by every presentation file:
// the palette, the Dear ImGui / ImPlot style and embedded UI font
// (ApplyTheme), and a handful of styled widgets built on top of them.
//
// Every color the UI draws comes from here, so re-theming the app is an edit
// to this file alone.
#pragma once

#include "imgui.h"

namespace ui::theme {

// 0-255 channels -> ImVec4, usable in constant expressions.
constexpr ImVec4 Rgb(int r, int g, int b, int a = 255) {
    return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                  static_cast<float>(b) / 255.0f, static_cast<float>(a) / 255.0f);
}

// ImVec4 -> packed ImU32 for draw-list calls, with an optional alpha multiplier.
constexpr ImU32 U32(ImVec4 c, float alpha = 1.0f) {
    auto channel = [](float v) { return static_cast<ImU32>(v * 255.0f + 0.5f); };
    return IM_COL32(channel(c.x), channel(c.y), channel(c.z), channel(c.w * alpha));
}

// ---------------------------------------------------------------------------
// Palette: cream paper, navy ink, postal red.
// ---------------------------------------------------------------------------

constexpr ImVec4 kPaper      = Rgb(246, 240, 228);  // window and panel background
constexpr ImVec4 kPaperLight = Rgb(252, 249, 242);  // plot areas, inputs on paper
constexpr ImVec4 kPaper2     = Rgb(236, 227, 210);  // frames, table headers, inset tiles
constexpr ImVec4 kPaper3     = Rgb(222, 210, 188);  // hovered frames, borders, road casing
constexpr ImVec4 kPaper4     = Rgb(204, 189, 162);  // pressed frames, scrollbar grabs
constexpr ImVec4 kMap        = Rgb(231, 222, 203);  // map background (the GL clear color)
constexpr ImVec4 kMapGrid    = Rgb(218, 207, 186);  // map grid lines
constexpr ImVec4 kInk        = Rgb(32, 42, 68);     // text, icons, roads
constexpr ImVec4 kInkDim     = Rgb(118, 110, 96);   // secondary text, inactive icons
constexpr ImVec4 kShadow     = Rgb(70, 50, 25);     // drop shadows (used at low alpha)

constexpr ImVec4 kRed        = Rgb(196, 52, 44);    // postal red: primary accent, trucks, selection
constexpr ImVec4 kRedHover   = Rgb(214, 72, 62);
constexpr ImVec4 kRedActive  = Rgb(164, 40, 34);
constexpr ImVec4 kNavy       = Rgb(44, 66, 112);    // secondary accent: office fill, progress
constexpr ImVec4 kNavyHover  = Rgb(62, 88, 140);

constexpr ImVec4 kGood       = Rgb(58, 135, 88);    // income, on-time deliveries
constexpr ImVec4 kBad        = Rgb(186, 48, 40);    // losses, late deliveries, errors
constexpr ImVec4 kWarn       = Rgb(214, 158, 46);   // nearly full, left behind, bonuses
constexpr ImVec4 kWarnText   = Rgb(160, 110, 18);   // kWarn darkened enough to read on paper
constexpr ImVec4 kTeal       = Rgb(52, 128, 140);
constexpr ImVec4 kPlum       = Rgb(120, 72, 110);

// An office's outbound buffer turns from navy to warn to bad as it fills up
// (the map wedge, the inspector's capacity bar and the offices table).
constexpr float kWarnFraction = 0.75f;
constexpr float kFullFraction = 0.9f;
constexpr ImVec4 FillColor(float fraction) {
    return fraction >= kFullFraction ? kBad : fraction >= kWarnFraction ? kWarn : kNavy;
}

// Unscaled font sizes, as passed to ImGui::PushFont(nullptr, size); the DPI
// scale is applied on top by style.FontScaleDpi.
constexpr float kFontSmall = 12.5f;
constexpr float kFontBody  = 16.0f;
constexpr float kFontTitle = 20.0f;
constexpr float kFontHuge  = 30.0f;

// Loads the embedded UI font and installs the ImGui and ImPlot styles. Call
// once, after both contexts exist and before the first frame.
void ApplyTheme(float dpi_scale);

// ---------------------------------------------------------------------------
// Styled widgets
// ---------------------------------------------------------------------------

// The main action of a panel: postal-red fill, paper text.
bool PrimaryButton(const char* label, ImVec2 size = ImVec2(0.0f, 0.0f));

// A small, borderless button in the "bad" color, for removing things.
bool DangerSmallButton(const char* label);

// One option of a segmented control (lay several out with SameLine): the
// selected one is filled navy. Returns true when clicked.
bool SegmentedButton(const char* label, bool selected, ImVec2 size = ImVec2(0.0f, 0.0f));

// A small rounded tag ("LATE", a postal code), laid out like a text item.
void Pill(const char* text, ImVec4 bg, ImVec4 fg);

// A dimmed "(?)" that shows `text` in a wrapped tooltip on hover.
void HelpMarker(const char* text);

// A larger heading underlined with a thin rule and a short red accent.
void SectionHeading(const char* text);

// A paper card with a soft drop shadow, drawn straight into a draw list (the
// transport bar and the HUD card).
void DrawShadowedPanel(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding);

// Text with a one-pixel halo all around, so map labels stay readable over
// roads and the grid.
void DrawHaloText(ImDrawList* draw, ImVec2 pos, ImU32 color, ImU32 halo, const char* text);

}  // namespace ui::theme
