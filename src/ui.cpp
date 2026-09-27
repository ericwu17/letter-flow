// ui.cpp — Dear ImGui presentation layer: reads World state, draws the map,
// HUD and inspector, and turns player clicks into World mutations.
#include "ui.h"

#include "entities.h"
#include "game_types.h"
#include "truck.h"
#include "world.h"
#include "imgui.h"

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
// Transport bar layout: play/pause + fast-forward buttons and the day/time
// readout, anchored top-center of the display. Every rect is recomputed from
// io.DisplaySize and font metrics each frame, so the bar stays centered and
// correctly sized across window resizes (and follows DPI scaling); on very
// narrow windows the day progress bar shrinks first.
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

TransportLayout ComputeTransportLayout(const World& world) {
    ImGuiIO& io = ImGui::GetIO();
    TransportLayout l;

    const float fh = ImGui::GetFrameHeight();  // follows font size / DPI scaling
    const float button = fh;
    const float gap = fh * 0.25f;
    const float pad = fh * 0.3f;
    const float separator_w = 1.0f;
    const bool fast_forward = world.get_speed_multiplier() > 1.0f;

    const unsigned long long tick = static_cast<unsigned long long>(world.get_tick());
    std::snprintf(l.time_text, sizeof(l.time_text), "Day %llu  (tick %llu)",
                  tick / kTicksPerDay + 1, tick);

    const float time_w = ImGui::CalcTextSize(l.time_text).x;
    const float speed_w = fast_forward ? ImGui::CalcTextSize("x4").x : 0.0f;
    const float fixed_w = button + gap + button
                        + (fast_forward ? gap + speed_w : 0.0f)
                        + gap + separator_w + gap + time_w + gap;
    float progress_w = std::min(140.0f, io.DisplaySize.x * 0.15f);

    // Shrink the progress bar first on narrow windows.
    const float avail_w = io.DisplaySize.x - 2.0f * pad - fh;
    if (fixed_w + progress_w > avail_w)
        progress_w = std::max(0.0f, avail_w - fixed_w);

    const float bar_w = fixed_w + progress_w + 2.0f * pad;
    const float bar_h = button + 2.0f * pad;
    l.button_size = button;
    l.bar_min = {std::max((io.DisplaySize.x - bar_w) * 0.5f, fh * 0.25f), fh * 0.4f};
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
    ImGui::Text("Money: %d", world.get_money());
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
                const long long remaining = static_cast<long long>(letter.deadline) - static_cast<long long>(world.get_tick());
                if (remaining >= 0)
                    ImGui::Text("%lld ticks", remaining);
                else
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "LATE by %lld", -remaining);
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
            const long long until = static_cast<long long>(schedule.next_departure) - static_cast<long long>(world.get_tick());
            ImGui::Text("to %s every %llu ticks (next in %lld)",
                        offices[schedule.dst].name.c_str(),
                        static_cast<unsigned long long>(schedule.period), until);
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("Remove"))
                schedule_to_remove = i;
            ImGui::PopID();
        }
        if (schedule_to_remove.has_value())
            world.remove_schedule(g_selected_office, *schedule_to_remove);

        ImGui::SeparatorText("Add route");
        static int new_period_ticks = 900;
        ImGui::InputInt("period (ticks)", &new_period_ticks);
        new_period_ticks = std::max(new_period_ticks, 60);
        for (PostOfficeId other = 0; other < offices.size(); ++other) {
            if (other == g_selected_office)
                continue;
            char label[96];
            std::snprintf(label, sizeof(label), "Route to %s", offices[other].name.c_str());
            if (ImGui::Button(label))
                world.add_schedule(g_selected_office, other, static_cast<Tick>(new_period_ticks));
            if (other + 1 < offices.size())
                ImGui::SameLine();
        }
    }

    ImGui::End();
}
