// ui.cpp — Dear ImGui presentation layer: reads World state, draws the map,
// HUD and inspector, and turns player clicks into World mutations.
#include "ui.h"

#include "game.h"
#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <optional>

namespace {

// Currently selected office (clicked on the map or picked in the inspector).
PostOfficeId g_selected_office = 0;

// ---------------------------------------------------------------------------
// World -> screen mapping: scale the fixed-size world to fit the display.
// ---------------------------------------------------------------------------

struct ViewTransform {
    float scale = 1.0f;
    ImVec2 offset = {0.0f, 0.0f};
};

ViewTransform ComputeViewTransform() {
    ImGuiIO& io = ImGui::GetIO();
    const float margin = 24.0f;
    const float avail_w = io.DisplaySize.x - 2.0f * margin;
    const float avail_h = io.DisplaySize.y - 2.0f * margin;
    ViewTransform vt;
    vt.scale = std::min(avail_w / kWorldWidth, avail_h / kWorldHeight);
    vt.offset.x = margin + (avail_w - kWorldWidth * vt.scale) * 0.5f;
    vt.offset.y = margin + (avail_h - kWorldHeight * vt.scale) * 0.5f;
    return vt;
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

}  // namespace

// ---------------------------------------------------------------------------
// The map
// ---------------------------------------------------------------------------

void DrawWorld(const World& world) {
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
    // ImGui window (HUD/inspector), so the map ignores those clicks.
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseClicked[0] && !io.WantCaptureMouse) {
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
// HUD
// ---------------------------------------------------------------------------

void DrawHUD(World& world) {
    static bool show_imgui_demo = false;

    ImGui::Begin("Letter Flow");
    const unsigned long long day = world.get_tick() / kTicksPerDay + 1;
    const float day_fraction = static_cast<float>(world.get_tick() % kTicksPerDay) / static_cast<float>(kTicksPerDay);
    ImGui::Text("Day %llu  (tick %llu)", day, static_cast<unsigned long long>(world.get_tick()));
    ImGui::ProgressBar(day_fraction);
    ImGui::Separator();
    ImGui::Text("Money: %d", world.get_money());
    ImGui::Text("Delivered on time: %zu", world.get_letters_delivered_on_time());
    ImGui::Text("Delivered late: %zu", world.get_letters_delivered_late());
    ImGui::Text("Trucks en route: %zu", world.get_trucks().size());
    ImGui::Separator();
    if (ImGui::Button(world.is_paused() ? "Resume" : "Pause", ImVec2(90.0f, 0.0f)))
        world.set_paused(!world.is_paused());
    ImGui::SameLine();
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
