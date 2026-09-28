// ui.cpp — Dear ImGui presentation layer: reads World state, draws the map,
// HUD and inspector, and turns player clicks into World mutations.
#include "ui.h"

#include "entities.h"
#include "game_types.h"
#include "truck.h"
#include "world.h"
#include "imgui.h"
#include "imgui_internal.h"  // DockBuilder* (imgui 'docking' branch)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

namespace {

struct ViewTransform {
    float scale = 1.0f;
    ImVec2 offset = {0.0f, 0.0f};
};

struct CameraState {
    float zoom = 1.0f;
    ImVec2 pan = {0.0f, 0.0f};
};

// Currently selected office (clicked on the map or picked in the inspector).
PostOfficeId g_selected_office = 0;
CameraState g_camera;
constexpr float kZoomStep = 1.2;
constexpr float kPanStep = 5.0;
constexpr float kZoomMin = 0.5;
constexpr float kZoomMax = 8.0;

// Map label for an office's postal code: smaller and muted-green so it reads
// as a subtitle under the (full-size, near-white) office name.
constexpr float kPostalCodeFontScale = 0.7f;
constexpr ImU32 kPostalCodeColor = IM_COL32(150, 205, 165, 255);

// ---------------------------------------------------------------------------
// World -> screen mapping: scale the fixed-size world to fit the display,
// then apply the camera on top.
// ---------------------------------------------------------------------------

ViewTransform ComputeViewTransform() {
    ImGuiIO& io = ImGui::GetIO();
    const float margin = 24.0f;
    const float avail_w = io.DisplaySize.x - 2.0f * margin;
    const float avail_h = io.DisplaySize.y - 2.0f * margin;
    ViewTransform vt;
    vt.scale = std::min(avail_w / kWorldWidth, avail_h / kWorldHeight);
    vt.offset.x = margin + (avail_w - kWorldWidth * vt.scale) * 0.5f;
    vt.offset.y = margin + (avail_h - kWorldHeight * vt.scale) * 0.5f;

    // Zoom about the display center c rather than about the world origin:
    //     screen = c + (fit(world) - c) * zoom + pan
    // The fit transform maps the world center to c, so this keeps the point
    // under the display center pinned there for any zoom. Expanding into the
    // usual screen = offset + world * scale form gives the offset below.
    const ImVec2 c = {io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f};
    const ImVec2 fit_offset = vt.offset;
    const float zoom = g_camera.zoom;
    vt.scale *= zoom;
    vt.offset.x = c.x - (c.x - fit_offset.x) * zoom + g_camera.pan.x;
    vt.offset.y = c.y - (c.y - fit_offset.y) * zoom + g_camera.pan.y;

    return vt;
}

void UpdateCamera() {
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureMouse && io.MouseWheel != 0) {
        const float old_zoom = g_camera.zoom;
        const float new_zoom = std::clamp(old_zoom * std::pow(kZoomStep, io.MouseWheel),
                                          kZoomMin, kZoomMax);
        // `pan` is applied in screen pixels *after* the zoom, so once the view
        // has been panned, the world point under the display center is no
        // longer the world center. Rescaling the pan by the zoom ratio keeps
        // that under-center point pinned there through the zoom.
        const float pan_scale = new_zoom / old_zoom;
        g_camera.pan.x *= pan_scale;
        g_camera.pan.y *= pan_scale;
        g_camera.zoom = new_zoom;
    }
    if (!io.WantCaptureKeyboard) {
        if (ImGui::IsKeyDown(ImGuiKey::ImGuiKey_D)) {
            g_camera.pan.x -= kPanStep;
        }
        if (ImGui::IsKeyDown(ImGuiKey::ImGuiKey_A)) {
            g_camera.pan.x += kPanStep;
        }
        if (ImGui::IsKeyDown(ImGuiKey::ImGuiKey_S)) {
            g_camera.pan.y -= kPanStep;
        }
        if (ImGui::IsKeyDown(ImGuiKey::ImGuiKey_W)) {
            g_camera.pan.y += kPanStep;
        }
    }
}

ImVec2 WorldToScreen(const ViewTransform& vt, Position p) {
    return {vt.offset.x + p.x * vt.scale, vt.offset.y + p.y * vt.scale};
}

void DrawCenteredText(ImDrawList* draw, ImVec2 center, const char* text) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 pos(center.x - size.x * 0.5f, center.y - size.y * 0.5f);
    draw->AddText({pos.x + 1.0f, pos.y + 1.0f}, IM_COL32(0, 0, 0, 255), text);  // shadow
    draw->AddText(pos, IM_COL32(235, 235, 240, 255), text);
}

// ---------------------------------------------------------------------------
// Tick -> 24-hour clock formatting. Every time shown to the player goes
// through these helpers; internally the simulation only ever uses ticks.
// ---------------------------------------------------------------------------

// Format an absolute tick as "Day N HH:MM".
void FormatDayTime(Tick tick, char* buf, std::size_t buf_len) {
    const ClockTime t = tick_to_time_of_day(tick);
    std::snprintf(buf, buf_len, "Day %llu  %02d:%02d",
                  static_cast<unsigned long long>(tick / kTicksPerDay) + 1, t.hour, t.minute);
}

// Format a tick duration as "HH:MM" (hours may exceed 23).
void FormatDuration(Tick duration, char* buf, std::size_t buf_len) {
    const ClockTime t = duration_to_hours_minutes(duration);
    std::snprintf(buf, buf_len, "%02d:%02d", t.hour, t.minute);
}

// ---------------------------------------------------------------------------
// Transport bar layout: play/pause + fast-forward buttons and the day/time
// readout, anchored top-center of the map area — the dockspace's central
// node, i.e. the display minus the docked "Post office" panel. Every rect is
// recomputed from that area and font metrics each frame, so the bar stays
// centered and correctly sized across window resizes, dock-layout changes and
// DPI scaling; on very narrow areas the day progress bar shrinks first.
// ---------------------------------------------------------------------------

constexpr float kFastForwardMultiplier = 4.0f;

struct TransportLayout {
    char time_text[64];
    float button_size = 0.0f;
    ImVec2 bar_min = {0.0f, 0.0f}, bar_max = {0.0f, 0.0f};
    ImVec2 play_min = {0.0f, 0.0f}, play_max = {0.0f, 0.0f};
    ImVec2 ff_min = {0.0f, 0.0f}, ff_max = {0.0f, 0.0f};
    ImVec2 speed_text_pos = {0.0f, 0.0f};  // "x4" label; only valid while fast-forwarding
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

ImGuiID MainDockSpaceId() { return ImGui::GetID("MainDockSpace"); }

TransportLayout ComputeTransportLayout(const World& world) {
    ImGuiIO& io = ImGui::GetIO();
    TransportLayout l;

    // Anchor the bar to the dockspace's central node — the visible map area,
    // which excludes the docked "Post office" panel
    ImVec2 area_min = {0.0f, 0.0f};
    ImVec2 area_size = io.DisplaySize;
    if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(MainDockSpaceId())) {
        area_min = central->Pos;
        area_size = central->Size;
    }

    const float fh = ImGui::GetFrameHeight();  // follows font size / DPI scaling
    const float button = fh;
    const float gap = fh * 0.25f;
    const float pad = fh * 0.3f;
    const float separator_w = 1.0f;
    const bool fast_forward = world.get_speed_multiplier() > 1.0f;

    FormatDayTime(world.get_tick(), l.time_text, sizeof(l.time_text));

    const float time_w = ImGui::CalcTextSize(l.time_text).x;
    const float speed_w = fast_forward ? ImGui::CalcTextSize("x4").x : 0.0f;
    const float fixed_w = button + gap + button
                        + (fast_forward ? gap + speed_w : 0.0f)
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

    l.ff_min = {x, cy - button * 0.5f};
    l.ff_max = {x + button, cy + button * 0.5f};
    x += button;
    if (fast_forward) {
        x += gap;
        l.speed_text_pos = {x, cy - ImGui::CalcTextSize("x4").y * 0.5f};
        x += speed_w;
    }
    x += gap;

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

}  // namespace

// ---------------------------------------------------------------------------
// Dockspace: a fullscreen docking area over the main viewport.
//
// The central node is passthrough (ImGuiDockNodeFlags_PassthruCentralNode), so
// the map — drawn into the background draw list — stays visible and clickable
// through it. On first run (when imgui.ini holds no docking data for the
// dockspace yet) the default layout is built programmatically: "Post office"
// is docked into a left split. From then on the layout lives in imgui.ini
// and the user can rearrange/undock windows freely without the code fighting
// it every frame.
// ---------------------------------------------------------------------------

void DrawDockspace() {
    const ImGuiID dockspace_id = MainDockSpaceId();

    // DockBuilder* must run before the dockspace node is submitted this frame.
    if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        ImGui::DockBuilderRemoveNode(dockspace_id);  // no-op on a fresh node
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

        // Split off the left third for the inspector; the remainder stays
        // as the passthrough central node over the map.
        ImGuiID central_id = dockspace_id;
        const ImGuiID dock_id =
            ImGui::DockBuilderSplitNode(central_id, ImGuiDir_Left, 0.35f, nullptr, &central_id);
        ImGui::DockBuilderFinish(dockspace_id);

        // Queued by name: applied when the window is submitted this frame and
        // persisted into imgui.ini, so it only happens on the first run.
        ImGui::DockBuilderDockWindow("Post office", dock_id);
    }

    ImGui::DockSpaceOverViewport(dockspace_id, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
}

// ---------------------------------------------------------------------------
// The map
// ---------------------------------------------------------------------------

void DrawWorld(const World& world) {

    UpdateCamera();

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ViewTransform view = ComputeViewTransform();
    const float office_radius = 14.0f * view.scale;
    const std::vector<PostOffice>& offices = world.get_post_offices();

    // Schedule routes (faint lines under everything).
    for (PostOfficeId src = 0; src < offices.size(); ++src) {
        const ImVec2 a = WorldToScreen(view, offices[src].pos);
        for (const TruckSchedule& schedule : offices[src].outbound_schedules) {
            const ImVec2 b = WorldToScreen(view, offices[schedule.dst].pos);
            draw->AddLine(a, b, IM_COL32(110, 120, 150, 70), 1.5f);
        }
    }

    // Post offices.
    for (PostOfficeId id = 0; id < offices.size(); ++id) {
        const PostOffice& office = offices[id];
        const ImVec2 c = WorldToScreen(view, office.pos);
        if (id == g_selected_office)
            draw->AddCircle(c, office_radius + 5.0f, IM_COL32(255, 200, 80, 255), 0, 2.5f);
        draw->AddCircleFilled(c, office_radius, IM_COL32(70, 130, 220, 255));
        draw->AddCircle(c, office_radius, IM_COL32(255, 255, 255, 140), 0, 1.5f);

        const ImVec2 name_size = ImGui::CalcTextSize(office.name.c_str());
        const ImVec2 name_pos(c.x - name_size.x * 0.5f, c.y - office_radius - name_size.y - 5.0f);
        draw->AddText({name_pos.x + 1.0f, name_pos.y + 1.0f}, IM_COL32(0, 0, 0, 255), office.name.c_str());
        draw->AddText(name_pos, IM_COL32(235, 235, 240, 255), office.name.c_str());

        // Postal code, centered directly below the name in a smaller, muted
        // green font. PushFont(nullptr, size) rescales the current font (both
        // CalcTextSize and AddText honor it); FontSizeBase rather than
        // GetFontSize() so the DPI/global scale is applied exactly once.
        if (!office.postal_code.empty()) {
            const float code_font_size = ImGui::GetStyle().FontSizeBase * kPostalCodeFontScale;
            ImGui::PushFont(nullptr, code_font_size);
            const ImVec2 code_extent = ImGui::CalcTextSize(office.postal_code.c_str());
            const ImVec2 code_pos(c.x - code_extent.x * 0.5f, name_pos.y + name_size.y + 1.0f);
            draw->AddText({code_pos.x + 1.0f, code_pos.y + 1.0f}, IM_COL32(0, 0, 0, 255), office.postal_code.c_str());
            draw->AddText(code_pos, kPostalCodeColor, office.postal_code.c_str());
            ImGui::PopFont();
        }

        char buf[64];
        std::snprintf(buf, sizeof(buf), "%zu letters", office.outbound_letters.size());
        DrawCenteredText(draw, {c.x, c.y + office_radius + 12.0f}, buf);
    }

    // Trucks in transit (position is derived from the tick, so they animate
    // smoothly as the simulation advances and freeze when it is paused).
    for (const Truck& truck : world.get_trucks()) {
        const ImVec2 c = WorldToScreen(view, truck.get_position(world.get_tick()));
        const float r = 5.5f * view.scale;
        draw->AddRectFilled({c.x - r, c.y - r}, {c.x + r, c.y + r}, IM_COL32(240, 150, 60, 255));
        draw->AddRect({c.x - r, c.y - r}, {c.x + r, c.y + r}, IM_COL32(30, 30, 30, 255));
    }

    // Click-to-select. io.WantCaptureMouse is true when the click landed on an
    // ImGui window (HUD/inspector), so the map ignores those clicks; clicks on
    // the transport bar are likewise not the map's business.
    ImGuiIO& io = ImGui::GetIO();
    const TransportLayout bar = ComputeTransportLayout(world);
    if (io.MouseClicked[0] && !io.WantCaptureMouse && !PointInRect(io.MousePos, bar.bar_min, bar.bar_max)) {
        for (PostOfficeId id = 0; id < offices.size(); ++id) {
            const ImVec2 c = WorldToScreen(view, offices[id].pos);
            const float dx = io.MousePos.x - c.x;
            const float dy = io.MousePos.y - c.y;
            const float hit_radius = office_radius + 6.0f;
            if (dx * dx + dy * dy <= hit_radius * hit_radius) {
                g_selected_office = id;
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Transport bar (play/pause, fast-forward, day/time) — drawn into the
// background draw list, on top of the map but underneath ImGui windows.
// ---------------------------------------------------------------------------

void DrawTransportBar(World& world) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const TransportLayout l = ComputeTransportLayout(world);
    const bool fast_forward = world.get_speed_multiplier() > 1.0f;
    const float b = l.button_size;
    const float rounding = b * 0.2f;

    // Panel.
    draw->AddRectFilled(l.bar_min, l.bar_max, IM_COL32(24, 27, 34, 220), rounding * 1.5f);
    draw->AddRect(l.bar_min, l.bar_max, IM_COL32(110, 120, 150, 100), rounding * 1.5f);

    // Background drawing has no widget behaviour, so the buttons are plain
    // rects hit-tested against the mouse. WantCaptureMouse keeps clicks that
    // landed on an ImGui window from leaking through to the bar.
    const bool interactive = !io.WantCaptureMouse;
    const bool hover_play = interactive && PointInRect(io.MousePos, l.play_min, l.play_max);
    const bool hover_ff = interactive && PointInRect(io.MousePos, l.ff_min, l.ff_max);
    if (hover_play || hover_ff)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const ImU32 icon_color = IM_COL32(235, 235, 240, 255);
    const ImU32 hover_bg = IM_COL32(70, 130, 220, 90);

    // Play/pause: a triangle when paused, two bars when running.
    if (hover_play)
        draw->AddRectFilled(l.play_min, l.play_max, hover_bg, rounding);
    if (world.is_paused())
        draw->AddTriangleFilled({l.play_min.x + b * 0.36f, l.play_min.y + b * 0.26f},
                                {l.play_min.x + b * 0.36f, l.play_min.y + b * 0.74f},
                                {l.play_min.x + b * 0.76f, l.play_min.y + b * 0.50f}, icon_color);
    else {
        draw->AddRectFilled({l.play_min.x + b * 0.30f, l.play_min.y + b * 0.28f},
                            {l.play_min.x + b * 0.44f, l.play_min.y + b * 0.72f}, icon_color);
        draw->AddRectFilled({l.play_min.x + b * 0.56f, l.play_min.y + b * 0.28f},
                            {l.play_min.x + b * 0.70f, l.play_min.y + b * 0.72f}, icon_color);
    }

    // Fast-forward: double triangle, orange while active, plus an "x4" label.
    if (hover_ff)
        draw->AddRectFilled(l.ff_min, l.ff_max, hover_bg, rounding);
    const ImU32 ff_color = fast_forward ? IM_COL32(240, 150, 60, 255) : IM_COL32(235, 235, 240, 200);
    draw->AddTriangleFilled({l.ff_min.x + b * 0.18f, l.ff_min.y + b * 0.30f},
                            {l.ff_min.x + b * 0.18f, l.ff_min.y + b * 0.70f},
                            {l.ff_min.x + b * 0.48f, l.ff_min.y + b * 0.50f}, ff_color);
    draw->AddTriangleFilled({l.ff_min.x + b * 0.48f, l.ff_min.y + b * 0.30f},
                            {l.ff_min.x + b * 0.48f, l.ff_min.y + b * 0.70f},
                            {l.ff_min.x + b * 0.78f, l.ff_min.y + b * 0.50f}, ff_color);
    if (fast_forward)
        draw->AddText(l.speed_text_pos, IM_COL32(240, 150, 60, 255), "x4");

    // Separator, day/time readout, day progress.
    draw->AddLine(l.separator_top, l.separator_bottom, IM_COL32(110, 120, 150, 140), 1.0f);
    draw->AddText(l.text_pos, IM_COL32(235, 235, 240, 255), l.time_text);
    const float progress_rounding = (l.progress_max.y - l.progress_min.y) * 0.5f;
    draw->AddRectFilled(l.progress_min, l.progress_max, IM_COL32(255, 255, 255, 40), progress_rounding);
    const float day_fraction = static_cast<float>(world.get_tick() % kTicksPerDay) / static_cast<float>(kTicksPerDay);
    if (day_fraction > 0.0f)
        draw->AddRectFilled(l.progress_min,
                            {l.progress_min.x + (l.progress_max.x - l.progress_min.x) * day_fraction, l.progress_max.y},
                            IM_COL32(70, 130, 220, 255), progress_rounding);

    // Clicks.
    if (interactive && io.MouseClicked[0]) {
        if (hover_play)
            world.set_paused(!world.is_paused());
        else if (hover_ff)
            world.set_speed_multiplier(fast_forward ? 1.0f : kFastForwardMultiplier);
    }
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------

void DrawHUD(const World& world) {
    static bool show_imgui_demo = false;

    // Day/time and play/pause now live in the background transport bar
    // (DrawTransportBar); this window is just the score board.
    ImGui::Begin("Letter Flow");
    ImGui::Text("Money: $%d", world.get_money());
    ImGui::Text("Delivered on time: %zu", world.get_letters_delivered_on_time());
    ImGui::Text("Delivered late: %zu", world.get_letters_delivered_late());
    ImGui::Text("Trucks en route: %zu", world.get_trucks().size());
    ImGui::Separator();
    ImGui::Checkbox("ImGui demo", &show_imgui_demo);
    ImGui::TextDisabled("Click an office to inspect it.");
    ImGui::End();

    // Widget reference window — keep handy while developing.
    if (show_imgui_demo)
        ImGui::ShowDemoWindow(&show_imgui_demo);
}

// ---------------------------------------------------------------------------
// Inspector for the selected office
// ---------------------------------------------------------------------------

void DrawInspector(World& world) {
    const std::vector<PostOffice>& offices = world.get_post_offices();
    if (offices.empty())
        return;
    if (g_selected_office >= offices.size())
        g_selected_office = 0;
    const PostOffice& office = offices[g_selected_office];

    ImGui::Begin("Post office");

    // Office picker (map clicking is the other way to change selection).
    if (ImGui::BeginCombo("Office", office.name.c_str())) {
        for (PostOfficeId id = 0; id < offices.size(); ++id) {
            if (ImGui::Selectable(offices[id].name.c_str(), id == g_selected_office))
                g_selected_office = id;
        }
        ImGui::EndCombo();
    }

    // Postal code, editable. InputText needs a mutable buffer, so keep one
    // static buffer that is reloaded whenever the selection changes, and
    // commit every keystroke to the World — the map label updates live.
    static char postal_buf[128];
    static PostOfficeId postal_buf_owner = kNoPostOffice;
    if (postal_buf_owner != g_selected_office) {
        std::snprintf(postal_buf, sizeof(postal_buf), "%s", office.postal_code.c_str());
        postal_buf_owner = g_selected_office;
    }
    if (ImGui::InputText("Postal code", postal_buf, sizeof(postal_buf)))
        world.set_postal_code(g_selected_office, postal_buf);

    ImGui::Text("Outbound: %zu / %zu   (%zu per day)",
                office.outbound_letters.size(), office.max_outbound_letters, office.letters_per_day);

    if (ImGui::CollapsingHeader("Outbound letters", ImGuiTreeNodeFlags_DefaultOpen)) {
        const ImGuiTableFlags flags =
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("outbound_letters", 4, flags, ImVec2(0.0f, 200.0f))) {
            ImGui::TableSetupColumn("To");
            ImGui::TableSetupColumn("Deadline");
            ImGui::TableSetupColumn("Value");
            ImGui::TableSetupColumn("Fine");
            ImGui::TableHeadersRow();
            for (const Letter& letter : office.outbound_letters) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(offices[letter.dst].name.c_str());
                ImGui::TableSetColumnIndex(1);
                char due[32];
                FormatDayTime(letter.deadline, due, sizeof(due));
                if (letter.deadline >= world.get_tick())
                    ImGui::TextUnformatted(due);
                else
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "LATE (was due %s)", due);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%d", letter.value);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%d", letter.fine);
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Truck schedules", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Record the removal and apply it after the loop: remove_schedule()
        // erases from the very vector we are iterating, which would invalidate
        // the `schedule` reference and shift the indices of every later row.
        std::optional<std::size_t> schedule_to_remove;
        for (std::size_t i = 0; i < office.outbound_schedules.size(); ++i) {
            const TruckSchedule& schedule = office.outbound_schedules[i];
            char every[16], start[16], next[32];
            FormatDuration(schedule.period, every, sizeof(every));
            FormatDuration(schedule.start_offset, start, sizeof(start));
            FormatDayTime(schedule.next_departure, next, sizeof(next));
            ImGui::Text("to %s every %s starting at %s (next: %s)",
                        offices[schedule.dst].name.c_str(), every, start, next);
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("Remove"))
                schedule_to_remove = i;
            ImGui::PopID();
        }
        if (schedule_to_remove.has_value())
            world.remove_schedule(g_selected_office, *schedule_to_remove);

        ImGui::SeparatorText("Add route");
        // Frequency and offset are entered as 24-hour HH:MM and converted to
        // ticks right away — the World only ever sees ticks. Defaults spell
        // out the canonical example: every 3 hours starting at 00:30.
        static int frequency_hhmm[2] = {3, 0};
        static int start_hhmm[2] = {0, 30};
        ImGui::InputInt2("Every (hh:mm)", frequency_hhmm);
        ImGui::InputInt2("Starting at (hh:mm)", start_hhmm);
        frequency_hhmm[0] = std::clamp(frequency_hhmm[0], 0, 24);
        frequency_hhmm[1] = std::clamp(frequency_hhmm[1], 0, 59);
        start_hhmm[0] = std::clamp(start_hhmm[0], 0, 23);
        start_hhmm[1] = std::clamp(start_hhmm[1], 0, 59);
        const Tick period = std::clamp(hours_minutes_to_ticks(frequency_hhmm[0], frequency_hhmm[1]),
                                       kMinSchedulePeriod, kTicksPerDay);
        const Tick start_offset = hours_minutes_to_ticks(start_hhmm[0], start_hhmm[1]);

        // Creating a schedule costs money up front, priced by frequency.
        const int cost = schedule_cost(period);
        const bool affordable = world.get_money() >= cost;
        if (affordable)
            ImGui::Text("Up-front cost: $%d", cost);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "Up-front cost: $%d (you only have $%d)", cost, world.get_money());

        for (PostOfficeId other = 0; other < offices.size(); ++other) {
            if (other == g_selected_office)
                continue;
            const bool route_exists =
                std::any_of(office.outbound_schedules.begin(), office.outbound_schedules.end(),
                            [other](const TruckSchedule& s) { return s.dst == other; });
            char label[96];
            std::snprintf(label, sizeof(label), "Route to %s ($%d)",
                          offices[other].name.c_str(), cost);
            // Grey out routes that already exist or that the player can't afford.
            ImGui::BeginDisabled(route_exists || !affordable);
            if (ImGui::Button(label))
                world.add_schedule(g_selected_office, other, period, start_offset);
            ImGui::EndDisabled();
            if (other + 1 < offices.size())
                ImGui::SameLine();
        }
    }

    ImGui::End();
}
