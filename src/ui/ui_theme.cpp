// ui_theme.cpp — see ui_theme.h.
#include "ui/ui_theme.h"

#include "implot.h"

#include <algorithm>

// Generated from imgui/misc/fonts/Roboto-Medium.ttf by the makefiles:
// defines kRobotoTtf[].
#include "roboto_ttf.h"

namespace ui::theme {

namespace {

void LoadFont() {
    ImFontConfig config;
    // The array is static data in the executable: the atlas must not free it.
    config.FontDataOwnedByAtlas = false;
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kRobotoTtf),
                                               static_cast<int>(sizeof(kRobotoTtf)), kFontBody, &config);
}

void ApplyImGuiStyle(float dpi_scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsLight(&style);  // fills the few colors not set below

    style.FontSizeBase = kFontBody;
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.CellPadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.IndentSpacing = 16.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 5.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextBorderSize = 2.0f;
    style.SeparatorTextPadding = ImVec2(10.0f, 4.0f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                  = kInk;
    c[ImGuiCol_TextDisabled]          = kInkDim;
    c[ImGuiCol_WindowBg]              = kPaper;
    c[ImGuiCol_ChildBg]               = Rgb(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]               = kPaperLight;
    c[ImGuiCol_Border]                = kPaper3;
    c[ImGuiCol_BorderShadow]          = Rgb(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]               = kPaper2;
    c[ImGuiCol_FrameBgHovered]        = kPaper3;
    c[ImGuiCol_FrameBgActive]         = kPaper4;
    c[ImGuiCol_TitleBg]               = kPaper2;
    c[ImGuiCol_TitleBgActive]         = kPaper3;
    c[ImGuiCol_TitleBgCollapsed]      = kPaper2;
    c[ImGuiCol_MenuBarBg]             = kPaper2;
    c[ImGuiCol_ScrollbarBg]           = Rgb(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]         = kPaper3;
    c[ImGuiCol_ScrollbarGrabHovered]  = kPaper4;
    c[ImGuiCol_ScrollbarGrabActive]   = kInkDim;
    c[ImGuiCol_CheckMark]             = kNavy;
    c[ImGuiCol_CheckboxSelectedBg]    = kPaper2;
    c[ImGuiCol_SliderGrab]            = kNavy;
    c[ImGuiCol_SliderGrabActive]      = kRed;
    c[ImGuiCol_Button]                = kPaper2;
    c[ImGuiCol_ButtonHovered]         = kPaper3;
    c[ImGuiCol_ButtonActive]          = kPaper4;
    c[ImGuiCol_Header]                = kPaper2;
    c[ImGuiCol_HeaderHovered]         = kPaper3;
    c[ImGuiCol_HeaderActive]          = kPaper4;
    c[ImGuiCol_Separator]             = kPaper3;
    c[ImGuiCol_SeparatorHovered]      = kRedHover;
    c[ImGuiCol_SeparatorActive]       = kRed;
    c[ImGuiCol_ResizeGrip]            = Rgb(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]     = kPaper4;
    c[ImGuiCol_ResizeGripActive]      = kRed;
    c[ImGuiCol_InputTextCursor]       = kInk;
    c[ImGuiCol_Tab]                   = kPaper2;
    c[ImGuiCol_TabHovered]            = kPaper3;
    c[ImGuiCol_TabSelected]           = kPaper;
    c[ImGuiCol_TabSelectedOverline]   = kRed;
    c[ImGuiCol_TabDimmed]             = kPaper2;
    c[ImGuiCol_TabDimmedSelected]     = kPaper;
    c[ImGuiCol_TabDimmedSelectedOverline] = kPaper4;
    c[ImGuiCol_DockingPreview]        = Rgb(196, 52, 44, 90);
    c[ImGuiCol_DockingEmptyBg]        = kMap;
    c[ImGuiCol_PlotLines]             = kNavy;
    c[ImGuiCol_PlotLinesHovered]      = kRed;
    c[ImGuiCol_PlotHistogram]         = kNavy;
    c[ImGuiCol_PlotHistogramHovered]  = kRed;
    c[ImGuiCol_TableHeaderBg]         = kPaper2;
    c[ImGuiCol_TableBorderStrong]     = kPaper3;
    c[ImGuiCol_TableBorderLight]      = Rgb(222, 210, 188, 150);
    c[ImGuiCol_TableRowBg]            = Rgb(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]         = Rgb(120, 95, 60, 14);
    c[ImGuiCol_TextLink]              = kNavy;
    c[ImGuiCol_TextSelectedBg]        = Rgb(44, 66, 112, 60);
    c[ImGuiCol_TreeLines]             = kPaper3;
    c[ImGuiCol_DragDropTarget]        = kRed;
    c[ImGuiCol_NavCursor]             = kRed;
    c[ImGuiCol_NavWindowingHighlight] = kRed;
    c[ImGuiCol_NavWindowingDimBg]     = Rgb(32, 42, 68, 50);
    c[ImGuiCol_ModalWindowDimBg]      = Rgb(32, 42, 68, 80);

    // Sizes above are in unscaled pixels; scale them (and the fonts) once.
    style.ScaleAllSizes(dpi_scale);
    style.FontScaleDpi = dpi_scale;
}

void ApplyImPlotStyle() {
    ImPlotStyle& style = ImPlot::GetStyle();
    ImPlot::StyleColorsLight(&style);
    style.PlotPadding = ImVec2(8.0f, 8.0f);
    style.LegendPadding = ImVec2(8.0f, 6.0f);
    style.MinorAlpha = 0.4f;

    ImVec4* c = style.Colors;
    c[ImPlotCol_FrameBg]      = Rgb(0, 0, 0, 0);
    c[ImPlotCol_PlotBg]       = kPaperLight;
    c[ImPlotCol_PlotBorder]   = kPaper3;
    c[ImPlotCol_LegendBg]     = Rgb(252, 249, 242, 230);
    c[ImPlotCol_LegendBorder] = kPaper3;
    c[ImPlotCol_LegendText]   = kInk;
    c[ImPlotCol_TitleText]    = kInk;
    c[ImPlotCol_InlayText]    = kInk;
    c[ImPlotCol_AxisText]     = kInkDim;
    c[ImPlotCol_AxisGrid]     = Rgb(222, 210, 188, 200);
    c[ImPlotCol_AxisTick]     = kPaper3;
    c[ImPlotCol_Selection]    = kRed;
    c[ImPlotCol_Crosshairs]   = kInkDim;

    // Series without a semantic color cycle through the palette's inks.
    static const ImVec4 kPostalColormap[] = {kNavy, kRed, kWarn, kGood, kTeal, kPlum, kInkDim};
    style.Colormap = ImPlot::AddColormap("Postal", kPostalColormap,
                                         static_cast<int>(sizeof(kPostalColormap) / sizeof(kPostalColormap[0])));
}

}  // namespace

void ApplyTheme(float dpi_scale) {
    LoadFont();
    ApplyImGuiStyle(dpi_scale);
    ApplyImPlotStyle();
}

// ---------------------------------------------------------------------------
// Styled widgets
// ---------------------------------------------------------------------------

bool PrimaryButton(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, kRed);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRedHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kRedActive);
    ImGui::PushStyleColor(ImGuiCol_Text, kPaperLight);
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

bool DangerSmallButton(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button, Rgb(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgb(186, 48, 40, 40));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgb(186, 48, 40, 80));
    ImGui::PushStyleColor(ImGuiCol_Text, kBad);
    const bool pressed = ImGui::SmallButton(label);
    ImGui::PopStyleColor(4);
    return pressed;
}

bool SegmentedButton(const char* label, bool selected, ImVec2 size) {
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, kNavy);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kNavyHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kNavy);
        ImGui::PushStyleColor(ImGuiCol_Text, kPaperLight);
    }
    const bool pressed = ImGui::Button(label, size);
    if (selected)
        ImGui::PopStyleColor(4);
    return pressed;
}

void Pill(const char* text, ImVec4 bg, ImVec4 fg) {
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const float pad_x = ImGui::GetFontSize() * 0.45f;
    const float pad_y = 1.0f;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + text_size.x + 2.0f * pad_x, min.y + text_size.y + 2.0f * pad_y);
    ImGui::Dummy(ImVec2(max.x - min.x, max.y - min.y));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, U32(bg), (max.y - min.y) * 0.5f);
    draw->AddText(ImVec2(min.x + pad_x, min.y + pad_y), U32(fg), text);
}

bool PencilButton(const char* str_id) {
    const float s = ImGui::GetTextLineHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(str_id, ImVec2(s, s));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered)
        draw->AddRectFilled(min, ImVec2(min.x + s, min.y + s), U32(ImGui::IsItemActive() ? kPaper4 : kPaper3),
                            s * 0.2f);

    // A pencil leaning 45 degrees: the tip at the bottom left, the body
    // running up to the top right, ending in a short eraser band.
    const ImU32 color = U32(hovered ? kInk : kInkDim);
    auto at = [&](float x, float y) { return ImVec2(min.x + s * x, min.y + s * y); };
    const float w = 0.11f;  // half the body's width, along the perpendicular
    draw->AddQuadFilled(at(0.38f - w, 0.62f - w), at(0.66f - w, 0.34f - w), at(0.66f + w, 0.34f + w),
                        at(0.38f + w, 0.62f + w), color);
    draw->AddQuadFilled(at(0.71f - w, 0.29f - w), at(0.78f - w, 0.22f - w), at(0.78f + w, 0.22f + w),
                        at(0.71f + w, 0.29f + w), color);
    draw->AddTriangleFilled(at(0.38f - w, 0.62f - w), at(0.38f + w, 0.62f + w), at(0.2f, 0.8f), color);
    return pressed;
}

void HelpMarker(const char* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void SectionHeading(const char* text) {
    ImGui::Spacing();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::PushFont(nullptr, kFontTitle * 0.9f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    const ImVec2 min = ImGui::GetItemRectMin();
    const float y = ImGui::GetItemRectMax().y + 3.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(min.x, y), ImVec2(min.x + width, y), U32(kPaper3), 1.0f);
    draw->AddLine(ImVec2(min.x, y), ImVec2(min.x + std::min(ImGui::GetFontSize() * 2.5f, width), y),
                  U32(kRed), 2.0f);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
}

void DrawShadowedPanel(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding) {
    // A few stacked, offset, slightly larger translucent rects fake a blur.
    for (int i = 4; i >= 1; --i) {
        const float spread = static_cast<float>(i);
        draw->AddRectFilled(ImVec2(min.x - spread, min.y - spread + 2.0f),
                            ImVec2(max.x + spread, max.y + spread + 2.0f),
                            U32(kShadow, 0.05f), rounding + spread);
    }
    draw->AddRectFilled(min, max, U32(kPaper), rounding);
    draw->AddRect(min, max, U32(kPaper3), rounding);
}

void DrawHaloText(ImDrawList* draw, ImVec2 pos, ImU32 color, ImU32 halo, const char* text) {
    constexpr float kOffsets[][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
    for (const auto& o : kOffsets)
        draw->AddText(ImVec2(pos.x + o[0], pos.y + o[1]), halo, text);
    draw->AddText(pos, color, text);
}

}  // namespace ui::theme
