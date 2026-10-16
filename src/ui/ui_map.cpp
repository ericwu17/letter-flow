// ui_map.cpp — the map: the camera, offices, roads and trucks drawn into
// the background draw list, hover and click-to-select, road building and the
// road destroy menu, money popups, new offices' pulses, overflow warnings,
// the network event's markers and hovered letters' journeys.
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "ui/ui_theme.h"

#include "sim/balance.h"
#include "sim/entities.h"
#include "sim/game_types.h"
#include "sim/road.h"
#include "sim/road_builder.h"
#include "sim/truck.h"
#include "sim/world.h"
#include "imgui.h"
#include "imgui_internal.h"  // IM_PI

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <vector>

namespace {

using ui::g_game;
using ui::CameraState;
using ui::RoadBuildState;
using ui::MoneyPopup;
using ui::SpawnPulse;
using ui::RoadKey;
using ui::SelectOffice;
namespace theme = ui::theme;
using theme::U32;

struct ViewTransform {
    float scale = 1.0f;
    ImVec2 offset = {0.0f, 0.0f};
};

constexpr float kZoomStep = 1.2f;
constexpr float kPanStep = 5.0f;
constexpr float kZoomMin = 0.5f;
constexpr float kZoomMax = 8.0f;

// Map label for an office's postal code: a small chip under the office.
constexpr float kPostalCodeFontScale = 0.75f;

// The map's colors, named by role. Every value comes from the shared palette
// in ui_theme.h, so re-theming the app is an edit there.
constexpr ImU32 kColorText          = U32(theme::kInk);          // labels
constexpr ImU32 kColorTextDim       = U32(theme::kInkDim);       // postal codes
constexpr ImU32 kColorHalo          = U32(theme::kMap, 0.9f);    // outline around map labels
constexpr ImU32 kColorGrid          = U32(theme::kMapGrid);      // the map's paper grid
constexpr ImU32 kColorShadow        = U32(theme::kShadow, 0.22f);
constexpr ImU32 kColorChipBg        = U32(theme::kPaperLight, 0.9f);  // postal-code chips, cost label
constexpr ImU32 kColorChipBorder    = U32(theme::kPaper3);
constexpr ImU32 kColorOfficeBody    = U32(theme::kPaperLight);   // a truck's letters
constexpr ImU32 kColorOfficeEmpty   = U32(theme::kNavy);         // unfilled part of an office
constexpr ImU32 kColorOfficeFill    = U32(theme::kPaperLight);   // filled part, until it nears full
constexpr ImU32 kColorOfficeRing    = U32(theme::kNavy);
constexpr ImU32 kColorOverflow      = U32(theme::kBad);          // overflow timer, full office's name
constexpr ImU32 kColorOverflowTrack = U32(theme::kBad, 0.2f);    // the rest of the timer's circle
constexpr ImU32 kColorSpawnPulse    = U32(theme::kNavy);         // ring around a new office
constexpr ImU32 kColorEvent         = U32(theme::kWarn);         // network event markers
constexpr ImU32 kColorEventOutline  = U32(theme::kWarnText);
constexpr ImU32 kColorSelection     = U32(theme::kRed);          // selection rings, hovered journey
constexpr ImU32 kColorSelectionHalo = U32(theme::kRed, 0.22f);
constexpr ImU32 kColorDestination   = U32(theme::kNavy);         // a hovered letter's destination
constexpr ImU32 kColorTruck         = U32(theme::kRed);
constexpr ImU32 kColorMoneyIn       = U32(theme::kGood);
constexpr ImU32 kColorMoneyOut      = U32(theme::kBad);

// Road colors: a printed road-map look, an ink outline around a light fill.
constexpr ImU32 kColorRoadOutline = U32(theme::kInk, 0.8f);
constexpr ImU32 kColorRoadFill    = U32(theme::kPaperLight);
constexpr ImU32 kColorRoadPreview = U32(theme::kWarn);       // road-building preview
constexpr ImU32 kColorRoadVertex  = U32(theme::kWarnText);   // placed vertices in preview
constexpr ImU32 kColorRoadBad     = U32(theme::kBad);        // invalid preview (crossing)

// The map's paper grid, in world units.
constexpr float kGridStep = 100.0f;

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
    const float zoom = g_game.camera.zoom;
    vt.scale *= zoom;
    vt.offset.x = c.x - (c.x - fit_offset.x) * zoom + g_game.camera.pan.x;
    vt.offset.y = c.y - (c.y - fit_offset.y) * zoom + g_game.camera.pan.y;

    return vt;
}

// The camera's inputs: the mouse wheel zooms, and W A S D or a left-drag
// that started on the map pans. The wheel is ignored mid-drag.
void UpdateCamera(bool mouse_on_map) {
    ImGuiIO& io = ImGui::GetIO();
    CameraState& camera = g_game.camera;
    if (io.MouseClicked[0])
        camera.press_on_map = mouse_on_map;
    if (!io.MouseDown[0])
        camera.dragging = false;  // press_on_map lasts the release frame, for DrawWorld's click check
    else if (camera.press_on_map && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        camera.dragging = true;
    if (camera.dragging) {
        // `pan` is in screen pixels, so the map stays under the cursor.
        camera.pan.x += io.MouseDelta.x;
        camera.pan.y += io.MouseDelta.y;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    if (!io.WantCaptureMouse && !camera.dragging && io.MouseWheel != 0) {
        const float old_zoom = g_game.camera.zoom;
        const float new_zoom = std::clamp(old_zoom * std::pow(kZoomStep, io.MouseWheel),
                                          kZoomMin, kZoomMax);
        // `pan` is applied in screen pixels *after* the zoom, so once the view
        // has been panned, the world point under the display center is no
        // longer the world center. Rescaling the pan by the zoom ratio keeps
        // that under-center point pinned there through the zoom.
        const float pan_scale = new_zoom / old_zoom;
        g_game.camera.pan.x *= pan_scale;
        g_game.camera.pan.y *= pan_scale;
        g_game.camera.zoom = new_zoom;
    }
    if (!io.WantCaptureKeyboard) {
        if (ImGui::IsKeyDown(ImGuiKey::ImGuiKey_D)) {
            g_game.camera.pan.x -= kPanStep;
        }
        if (ImGui::IsKeyDown(ImGuiKey::ImGuiKey_A)) {
            g_game.camera.pan.x += kPanStep;
        }
        if (ImGui::IsKeyDown(ImGuiKey::ImGuiKey_S)) {
            g_game.camera.pan.y -= kPanStep;
        }
        if (ImGui::IsKeyDown(ImGuiKey::ImGuiKey_W)) {
            g_game.camera.pan.y += kPanStep;
        }
    }
}

ImVec2 WorldToScreen(const ViewTransform& vt, Position p) {
    return {vt.offset.x + static_cast<float>(p.x) * vt.scale,
            vt.offset.y + static_cast<float>(p.y) * vt.scale};
}

// Rounded to the nearest world unit and clamped to ±kMaxCoordinate, the
// range the exact road geometry is defined on.
Position ScreenToWorld(const ViewTransform& vt, ImVec2 screen) {
    constexpr double kMax = static_cast<double>(kMaxCoordinate);
    auto to_world = [&](float screen_coord, float offset) {
        const double world = static_cast<double>((screen_coord - offset) / vt.scale);
        return static_cast<int64_t>(std::llround(std::clamp(world, -kMax, kMax)));
    };
    return {to_world(screen.x, vt.offset.x), to_world(screen.y, vt.offset.y)};
}

// Squared distance from point p to the segment a-b (all in screen space).
float DistanceSqToSegment(ImVec2 p, ImVec2 a, ImVec2 b) {
    const ImVec2 ab = {b.x - a.x, b.y - a.y};
    const float len_sq = ab.x * ab.x + ab.y * ab.y;
    // Degenerate segment (a == b): distance to the point itself.
    float t = len_sq > 0.0f ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len_sq : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float dx = p.x - (a.x + t * ab.x);
    const float dy = p.y - (a.y + t * ab.y);
    return dx * dx + dy * dy;
}

// Index of the road whose polyline passes nearest to `screen_pos` within
// `tolerance_px`, or nullopt if none does. Hit-testing lives here (not in
// road.cpp) because it runs in screen space; the sim layer only knows int64
// world coordinates.
std::optional<std::size_t> HitTestRoad(const std::vector<Road>& roads,
                                       const ViewTransform& view, ImVec2 screen_pos,
                                       float tolerance_px) {
    std::optional<std::size_t> best;
    float best_dist_sq = tolerance_px * tolerance_px;
    for (std::size_t r = 0; r < roads.size(); ++r) {
        const std::vector<Position>& v = roads[r].vertices;
        for (std::size_t i = 1; i < v.size(); ++i) {
            const float dist_sq = DistanceSqToSegment(screen_pos,
                                                      WorldToScreen(view, v[i - 1]),
                                                      WorldToScreen(view, v[i]));
            if (dist_sq < best_dist_sq) {
                best_dist_sq = dist_sq;
                best = r;
            }
        }
    }
    return best;
}

// The first office within `hit_radius` pixels of `screen_pos`, or nullopt.
std::optional<PostOfficeId> HitTestOffice(const std::vector<PostOffice>& offices,
                                          const ViewTransform& view, ImVec2 screen_pos,
                                          float hit_radius) {
    for (PostOfficeId id = 0; id < offices.size(); ++id) {
        const ImVec2 c = WorldToScreen(view, offices[id].pos);
        const float dx = screen_pos.x - c.x;
        const float dy = screen_pos.y - c.y;
        if (dx * dx + dy * dy <= hit_radius * hit_radius)
            return id;
    }
    return std::nullopt;
}

// While drawing a road: the office other than the road's source
// nearest to `screen_pos` within `radius` pixels, or nullopt. The preview
// snaps to it and a click there completes the road.
std::optional<PostOfficeId> NearestRoadDestination(const std::vector<PostOffice>& offices,
                                                   const ViewTransform& view, ImVec2 screen_pos,
                                                   float radius, PostOfficeId src) {
    std::optional<PostOfficeId> nearest;
    float best_dist_sq = radius * radius;
    for (PostOfficeId id = 0; id < offices.size(); ++id) {
        if (id == src)
            continue;
        const ImVec2 c = WorldToScreen(view, offices[id].pos);
        const float dx = screen_pos.x - c.x;
        const float dy = screen_pos.y - c.y;
        const float dist_sq = dx * dx + dy * dy;
        if (dist_sq <= best_dist_sq) {
            best_dist_sq = dist_sq;
            nearest = id;
        }
    }
    return nearest;
}

// A truck's shape, in pixels along its heading (+x, toward the nose) and
// across it (+y): a cargo box from the tail to the shoulders, then a pointed
// nose. Fixed on-screen size, like the offices.
constexpr float kTruckTail = -12.0f;
constexpr float kTruckShoulder = 5.0f;
constexpr float kTruckNose = 12.0f;
constexpr float kTruckHalfWidth = 6.0f;

// The unit vector, in screen space, a truck faces at tick `now`. The view is
// a uniform scale, so the world heading only needs normalizing; a truck with
// no heading (a zero-length route) faces right.
ImVec2 TruckForward(const Truck& truck, Tick now) {
    const Position h = truck.get_heading(now);
    const float x = static_cast<float>(h.x);
    const float y = static_cast<float>(h.y);
    const float len = std::sqrt(x * x + y * y);
    return len > 0.0f ? ImVec2{x / len, y / len} : ImVec2{1.0f, 0.0f};
}

// The screen point `along` pixels ahead of `c` and `across` pixels to the
// side, for a truck at `c` facing `fwd`.
ImVec2 TruckPoint(ImVec2 c, ImVec2 fwd, float along, float across) {
    return {c.x + fwd.x * along - fwd.y * across, c.y + fwd.y * along + fwd.x * across};
}

// The truck's outline (convex, clockwise on screen from the nose), grown by
// `grow` pixels all round for the selection ring.
std::vector<ImVec2> TruckOutline(ImVec2 c, ImVec2 fwd, float grow = 0.0f) {
    const float w = kTruckHalfWidth + grow;
    return {TruckPoint(c, fwd, kTruckNose + grow * 1.5f, 0.0f),
            TruckPoint(c, fwd, kTruckShoulder, w),
            TruckPoint(c, fwd, kTruckTail - grow, w),
            TruckPoint(c, fwd, kTruckTail - grow, -w),
            TruckPoint(c, fwd, kTruckShoulder, -w)};
}

// The truck whose drawn shape (grown by `slack_px`, screen space) contains
// `screen_pos`, or null. Trucks draw in vector order, so the last hit is the
// one on top.
std::shared_ptr<Truck> HitTestTruck(const World& world, const ViewTransform& view,
                                    ImVec2 screen_pos, float slack_px) {
    std::shared_ptr<Truck> hit;
    for (const std::shared_ptr<Truck>& truck : world.get_trucks()) {
        const ImVec2 c = WorldToScreen(view, truck->get_position(world.get_tick()));
        const ImVec2 fwd = TruckForward(*truck, world.get_tick());
        // The cursor in the truck's frame; the nose counts as part of its box.
        const float dx = screen_pos.x - c.x;
        const float dy = screen_pos.y - c.y;
        const float along = dx * fwd.x + dy * fwd.y;
        const float across = -dx * fwd.y + dy * fwd.x;
        if (along >= kTruckTail - slack_px && along <= kTruckNose + slack_px &&
            std::abs(across) <= kTruckHalfWidth + slack_px)
            hit = truck;
    }
    return hit;
}

// Selects a truck, with its destination office as the backup shown once the
// truck arrives.
void SelectTruck(const std::shared_ptr<const Truck>& truck) {
    SelectOffice(truck->get_dst());
    g_game.selected_truck = truck;
}

// ---------------------------------------------------------------------------
// Road destroy menu: a small ImGui popup anchored at the point where the road
// was clicked, armed by DrawWorld's click handling. Two-step confirm — the
// first click on the entry turns it into "Confirm?", the second destroys the
// road. Esc or a click elsewhere dismisses it; either way the
// state resets so the highlight and the menu never outlive each other.
// ---------------------------------------------------------------------------

void DrawRoadMenu(World& world) {
    if (!g_game.road_menu.road.has_value())
        return;

    // OpenPopup dereferences the current window, and the map lives outside any
    // Begin/End pair — so host the popup in an invisible, zero-size,
    // input-transparent window placed exactly at the click point.
    ImGui::SetNextWindowPos(g_game.road_menu.pos);
    ImGui::Begin("##road_menu_host", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoInputs);

    if (g_game.road_menu.open_requested) {
        ImGui::OpenPopup("road_destroy_menu");
        g_game.road_menu.open_requested = false;
    }

    if (ImGui::BeginPopup("road_destroy_menu")) {
        if (ImGui::IsWindowAppearing())
            g_game.road_menu.confirm = false;

        const RoadKey key = g_game.road_menu.road.value();
        const std::vector<PostOffice>& offices = world.get_post_offices();
        char title[128];
        std::snprintf(title, sizeof(title), "%s <-> %s",
                      offices[key.src].name.c_str(), offices[key.dst].name.c_str());
        ImGui::TextDisabled("%s", title);
        ImGui::Separator();

        // NoAutoClosePopups keeps the first click from closing the popup, so
        // the same entry can serve as the two-step confirm.
        if (ImGui::Selectable(g_game.road_menu.confirm ? "Confirm?" : "Destroy road",
                              false, ImGuiSelectableFlags_NoAutoClosePopups)) {
            if (!g_game.road_menu.confirm) {
                g_game.road_menu.confirm = true;
            } else {
                world.remove_road(key.src, key.dst);
                ImGui::CloseCurrentPopup();
                g_game.road_menu.road.reset();
                g_game.road_menu.confirm = false;
            }
        }
        ImGui::EndPopup();
    } else if (!g_game.road_menu.open_requested) {
        // The popup was closed externally (Esc or a click outside it). ImGui
        // consumes the closing click, so it cannot leak through to the map
        // below; just reset the menu state.
        g_game.road_menu.road.reset();
        g_game.road_menu.confirm = false;
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Map drawing helpers
// ---------------------------------------------------------------------------


// Money popups (see ui::MoneyPopup): how long one lasts and how far it rises.
constexpr float kPopupDuration = 1.6f;
constexpr float kPopupRise = 55.0f;

void DrawMoneyPopups(ImDrawList* draw, const ViewTransform& view, World& world) {
    std::vector<MoneyEvent> events = world.take_money_events();
    // we first accumulate the events, by postoffice ID.
    size_t num_events_saved = 0;
    for (size_t i = 0; i < events.size(); i++) {
        bool found_dup = false;
        for (size_t j = 0; j < num_events_saved; j++) {
            if (events[j].office == events[i].office) {
                events[j].amount += events[i].amount;
                found_dup = true;
                break;
            }
        }
        if (!found_dup) {
            events[num_events_saved] = events[i];
            num_events_saved++;
        }
    }
    events.resize(num_events_saved);
    for (const MoneyEvent& e : events)
        g_game.money_popups.push_back({e.office, e.amount, 0.0f});

    const float dt = ImGui::GetIO().DeltaTime;
    const std::vector<PostOffice>& offices = world.get_post_offices();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
    for (MoneyPopup& p : g_game.money_popups) {
        p.age += dt;
        const float t = p.age / kPopupDuration;
        if (t >= 1.0f || p.office >= offices.size())
            continue;
        const float rise = 1.0f - (1.0f - t) * (1.0f - t);          // ease-out
        const float alpha = t < 0.6f ? 1.0f : (1.0f - t) / 0.4f;     // fade over the last 40%
        char text[32];
        std::snprintf(text, sizeof(text), "%s$%d", p.amount < 0 ? "-" : "+", std::abs(p.amount));
        const ImVec2 c = WorldToScreen(view, offices[p.office].pos);
        const ImVec2 size = ImGui::CalcTextSize(text);
        const ImVec2 pos(c.x - size.x * 0.5f, c.y - 62.0f - kPopupRise * rise);
        const ImU32 color = p.amount < 0 ? kColorMoneyOut : kColorMoneyIn;
        theme::DrawHaloText(draw, pos, (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(255.0f * alpha) << IM_COL32_A_SHIFT),
                            U32(theme::kPaperLight, alpha), text);
    }
    ImGui::PopFont();
    std::erase_if(g_game.money_popups, [](const MoneyPopup& p) { return p.age >= kPopupDuration; });
}

// New offices: each one that appeared since last frame sends out a few
// expanding rings, so the player notices it wherever it landed.
constexpr float kSpawnPulseDuration = 2.4f;  // seconds, real time
constexpr int kSpawnPulseRings = 2;          // rings sent out one after another
constexpr float kSpawnPulseReach = 44.0f;    // pixels past the office's rim

void DrawSpawnPulses(ImDrawList* draw, const ViewTransform& view, const World& world, float office_radius) {
    const std::vector<PostOffice>& offices = world.get_post_offices();
    if (!g_game.known_offices.has_value())
        g_game.known_offices = offices.size();
    for (PostOfficeId id = *g_game.known_offices; id < offices.size(); ++id)
        g_game.spawn_pulses.push_back({id, 0.0f});
    g_game.known_offices = offices.size();

    const float dt = ImGui::GetIO().DeltaTime;
    const float ring_duration = kSpawnPulseDuration / static_cast<float>(kSpawnPulseRings);
    for (SpawnPulse& p : g_game.spawn_pulses) {
        p.age += dt;
        if (p.age >= kSpawnPulseDuration || p.office >= offices.size())
            continue;
        const float t = std::fmod(p.age, ring_duration) / ring_duration;  // this ring's progress
        const float grow = 1.0f - (1.0f - t) * (1.0f - t);                  // ease-out
        const ImVec2 c = WorldToScreen(view, offices[p.office].pos);
        const ImU32 color = (kColorSpawnPulse & ~IM_COL32_A_MASK) |
                            (static_cast<ImU32>(255.0f * (1.0f - t)) << IM_COL32_A_SHIFT);
        draw->AddCircle(c, office_radius + 4.0f + kSpawnPulseReach * grow, color, 0, 3.0f);
    }
    std::erase_if(g_game.spawn_pulses, [](const SpawnPulse& p) { return p.age >= kSpawnPulseDuration; });
}

// A dashed arrow straight from `a` to `b`, starting `tail_gap` pixels past `a`
// and with its tip `tip_gap` pixels short of `b` (to clear the markers drawn
// there). The dashes march toward `b` at `march_speed` pixels a second.
void DrawDashedArrow(ImDrawList* draw, ImVec2 a, ImVec2 b, float tail_gap, float tip_gap, ImU32 color,
                     float march_speed) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    const float from = tail_gap;
    const float to = length - tip_gap;  // where the arrowhead's tip is
    if (to <= from)
        return;
    const ImVec2 dir(dx / length, dy / length);
    auto at = [&](float d) { return ImVec2(a.x + dir.x * d, a.y + dir.y * d); };

    constexpr float kDash = 10.0f;
    constexpr float kGap = 7.0f;
    const float shift = std::fmod(static_cast<float>(ImGui::GetTime()) * march_speed, kDash + kGap);
    for (float d = from - (kDash + kGap) + shift; d < to - 10.0f; d += kDash + kGap) {
        const float d0 = std::max(d, from);
        const float d1 = std::min(d + kDash, to - 10.0f);
        if (d1 > d0)
            draw->AddLine(at(d0), at(d1), color, 3.0f);
    }
    // The arrowhead.
    const ImVec2 tip = at(to);
    const ImVec2 base = at(to - 12.0f);
    const ImVec2 side(-dir.y * 6.5f, dir.x * 6.5f);
    draw->AddTriangleFilled(tip, {base.x + side.x, base.y + side.y}, {base.x - side.x, base.y - side.y}, color);
}

// The network event's markers under the offices: for a corridor, a dashed
// arrow straight from one office to the other (roads or not), marching while
// the event runs; for a running surge, a slow pulsing ring round its target.
void DrawEventRoute(ImDrawList* draw, const ViewTransform& view, const World& world, float office_radius) {
    const std::optional<NetworkEvent>& event = world.get_event();
    const std::vector<PostOffice>& offices = world.get_post_offices();
    if (!event || event->dst >= offices.size())
        return;
    const bool running = world.get_tick() >= event->start;
    const auto time = static_cast<float>(ImGui::GetTime());
    const ImVec2 b = WorldToScreen(view, offices[event->dst].pos);

    if (event->type == EventType::Surge) {
        if (running) {
            const float t = std::fmod(time, 1.6f) / 1.6f;
            draw->AddCircle(b, office_radius + 8.0f + 14.0f * t, U32(theme::kWarn, 0.8f * (1.0f - t)), 0, 3.0f);
        }
        return;
    }
    if (event->src >= offices.size())
        return;
    const ImVec2 a = WorldToScreen(view, offices[event->src].pos);
    DrawDashedArrow(draw, a, b, office_radius + 8.0f, office_radius + 12.0f, kColorEvent, running ? 30.0f : 0.0f);
}

// A five-pointed star centered on `c`, `outer` pixels to its tips.
void DrawStar(ImDrawList* draw, ImVec2 c, float outer, ImU32 fill, ImU32 outline) {
    ImVec2 points[10];
    for (int i = 0; i < 10; ++i) {
        const float angle = -IM_PI * 0.5f + static_cast<float>(i) * IM_PI / 5.0f;
        const float r = i % 2 == 0 ? outer : outer * 0.45f;
        points[i] = ImVec2(c.x + r * std::cos(angle), c.y + r * std::sin(angle));
    }
    for (const ImVec2& p : points)
        draw->PathLineTo(p);
    draw->PathFillConcave(fill);
    draw->AddPolyline(points, 10, outline, ImDrawFlags_Closed, 1.2f);
}

// The network event's badges, over the offices: a star on a surge's target,
// and on both ends of a corridor.
void DrawEventBadges(ImDrawList* draw, const ViewTransform& view, const World& world, float office_radius) {
    const std::optional<NetworkEvent>& event = world.get_event();
    const std::vector<PostOffice>& offices = world.get_post_offices();
    if (!event)
        return;
    for (const PostOfficeId id : {event->src, event->dst}) {
        if (id >= offices.size())
            continue;  // a surge has no src
        const ImVec2 c = WorldToScreen(view, offices[id].pos);
        DrawStar(draw, {c.x + office_radius * 0.85f, c.y - office_radius * 0.85f}, 7.0f, kColorEvent,
                 kColorEventOutline);
    }
}

// A hovered letter's journey on the map: the legs it has travelled, along the
// roads it took; the truck it is on, if any, with the road it has driven so
// far; a dashed arrow from where it is now straight to where it is going; and
// rings around the office it was posted at and the one it is addressed to.
void DrawLetterHighlight(ImDrawList* draw, const ViewTransform& view, const World& world, float office_radius) {
    if (!g_game.highlight_letter.has_value())
        return;
    const Letter& letter = *g_game.highlight_letter;
    const std::vector<PostOffice>& offices = world.get_post_offices();
    if (letter.src >= offices.size() || letter.dst >= offices.size())
        return;

    for (std::size_t i = 1; i < letter.trail.size(); ++i) {
        const TrailStop& from = letter.trail[i - 1];
        const TrailStop& to = letter.trail[i];
        if (from.event != TrailEvent::Departed || to.event != TrailEvent::Arrived ||
            from.office >= offices.size() || to.office >= offices.size())
            continue;
        std::vector<Position> leg = world.road_path(from.office, to.office);
        if (leg.empty())  // the road has been destroyed since
            leg = {offices[from.office].pos, offices[to.office].pos};
        for (std::size_t k = 1; k < leg.size(); ++k)
            draw->AddLine(WorldToScreen(view, leg[k - 1]), WorldToScreen(view, leg[k]), kColorSelection, 5.0f);
    }

    const ImVec2 dst = WorldToScreen(view, offices[letter.dst].pos);
    if (!letter.trail.empty() && letter.trail.back().event == TrailEvent::Departed) {
        // On a truck right now: trace the leg so far along its road, point
        // on to the destination, and ring the truck.
        for (const std::shared_ptr<Truck>& truck : world.get_trucks()) {
            const std::vector<Letter>& cargo = truck->get_letters();
            if (std::find(cargo.begin(), cargo.end(), letter) == cargo.end())
                continue;
            const std::vector<Position> leg = truck->route_travelled(world.get_tick());
            for (std::size_t k = 1; k < leg.size(); ++k)
                draw->AddLine(WorldToScreen(view, leg[k - 1]), WorldToScreen(view, leg[k]), kColorSelection, 3.0f);
            const ImVec2 at = WorldToScreen(view, leg.back());
            DrawDashedArrow(draw, at, dst, 21.0f, office_radius + 12.0f, kColorSelection, 30.0f);
            draw->AddCircleFilled(at, 19.0f, kColorSelectionHalo);
            draw->AddCircle(at, 16.0f, kColorSelection, 0, 3.0f);
            break;
        }
    } else {
        // Waiting in an office (none to point from once it is delivered).
        const PostOfficeId here = letter.trail.empty() ? letter.src : letter.trail.back().office;
        if (here != letter.dst && here < offices.size())
            DrawDashedArrow(draw, WorldToScreen(view, offices[here].pos), dst, office_radius + 8.0f,
                            office_radius + 12.0f, kColorSelection, 30.0f);
    }

    draw->AddCircle(WorldToScreen(view, offices[letter.src].pos), office_radius + 9.0f, kColorSelection, 0, 2.5f);
    draw->AddCircle(dst, office_radius + 9.0f, kColorDestination, 0, 4.0f);
}

// Faint grid lines every kGridStep world units across the whole display, like
// the squares on a paper map; they pan and zoom with the camera. Skipped when
// zoomed out so far that they would crowd together.
void DrawMapGrid(ImDrawList* draw, const ViewTransform& view) {
    const float step_px = kGridStep * view.scale;
    if (step_px < 12.0f)
        return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // First grid line at or left of / above the screen edge.
    const float first_x = view.offset.x + std::floor(-view.offset.x / step_px) * step_px;
    const float first_y = view.offset.y + std::floor(-view.offset.y / step_px) * step_px;
    for (float x = first_x; x < display.x; x += step_px)
        draw->AddLine({x, 0.0f}, {x, display.y}, kColorGrid, 1.0f);
    for (float y = first_y; y < display.y; y += step_px)
        draw->AddLine({0.0f, y}, {display.x, y}, kColorGrid, 1.0f);
}

// A polyline stroked in one color, with round joints: thick ImGui lines
// leave notches where segments meet, so each inner vertex gets a disc.
void StrokePolyline(ImDrawList* draw, const ViewTransform& view, const std::vector<Position>& points,
                    ImU32 color, float thickness) {
    for (std::size_t i = 1; i < points.size(); ++i)
        draw->AddLine(WorldToScreen(view, points[i - 1]), WorldToScreen(view, points[i]), color, thickness);
    for (std::size_t i = 1; i + 1 < points.size(); ++i)
        draw->AddCircleFilled(WorldToScreen(view, points[i]), thickness * 0.5f, color);
}

// A built road: an outline under a light fill, like a road on a printed map.
void DrawRoad(ImDrawList* draw, const ViewTransform& view, const std::vector<Position>& vertices,
              ImU32 outline) {
    StrokePolyline(draw, view, vertices, outline, 6.0f);
    StrokePolyline(draw, view, vertices, kColorRoadFill, 3.0f);
}

// Small text in a rounded chip centered on `center` (postal codes, the
// road-building cost readout). Uses the current font.
void DrawChip(ImDrawList* draw, ImVec2 center, const char* text, ImU32 text_color) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    const float pad_x = 5.0f;
    const float pad_y = 1.0f;
    const ImVec2 min(center.x - size.x * 0.5f - pad_x, center.y - size.y * 0.5f - pad_y);
    const ImVec2 max(center.x + size.x * 0.5f + pad_x, center.y + size.y * 0.5f + pad_y);
    const float rounding = (max.y - min.y) * 0.5f;
    draw->AddRectFilled(min, max, kColorChipBg, rounding);
    draw->AddRect(min, max, kColorChipBorder, rounding);
    draw->AddText({min.x + pad_x, min.y + pad_y}, text_color, text);
}

// Each office class (kOfficeClasses) has its own shape, so they tell apart at
// a glance: a circle for a small town, a triangle for a city and a square for
// a major city.
enum class OfficeShape { Circle, Triangle, Square };

OfficeShape OfficeShapeOf(const PostOffice& office) {
    static_assert(kNumOfficeClasses == 3, "one shape per office class");
    constexpr OfficeShape kShapes[] = {OfficeShape::Circle, OfficeShape::Triangle, OfficeShape::Square};
    return kShapes[office_class_of(office.letters_per_day)];
}

// How far an office's shape reaches from its center, for a circle of radius
// `r`: the triangle and square are sized to look about as heavy as it.
float OfficeShapeExtent(OfficeShape shape, float r) {
    switch (shape) {
    case OfficeShape::Triangle: return r * 1.35f;               // circumradius
    case OfficeShape::Square:   return r * 0.88f * std::sqrt(2.0f);  // half-diagonal
    case OfficeShape::Circle:   break;
    }
    return r;
}

// An office's outline around `c`, clockwise (screen y points down) from the
// point straight above `c`, so the clock-style fill can sweep along it from
// 12 o'clock. The triangle is centered on its centroid.
std::vector<ImVec2> OfficeOutline(OfficeShape shape, ImVec2 c, float r) {
    switch (shape) {
    case OfficeShape::Triangle: {
        const float R = OfficeShapeExtent(shape, r);
        const float half_side = R * std::sqrt(3.0f) * 0.5f;
        return {{c.x, c.y - R}, {c.x + half_side, c.y + R * 0.5f}, {c.x - half_side, c.y + R * 0.5f}};
    }
    case OfficeShape::Square: {
        const float h = r * 0.88f;
        return {{c.x, c.y - h}, {c.x + h, c.y - h}, {c.x + h, c.y + h}, {c.x - h, c.y + h}, {c.x - h, c.y - h}};
    }
    case OfficeShape::Circle:
        break;
    }
    constexpr int kSegments = 48;
    std::vector<ImVec2> points;
    for (int i = 0; i < kSegments; ++i) {
        const float a = 2.0f * IM_PI * static_cast<float>(i) / static_cast<float>(kSegments);
        points.emplace_back(c.x + r * std::sin(a), c.y - r * std::cos(a));
    }
    return points;
}

// The angle of `p` around `c`, clockwise from 12 o'clock, in [0, 2pi).
float ClockAngle(ImVec2 c, ImVec2 p) {
    const float a = std::atan2(p.x - c.x, c.y - p.y);
    return a < 0.0f ? a + 2.0f * IM_PI : a;
}

// Fills the wedge of `outline` (as OfficeOutline makes it) that sweeps
// clockwise from 12 o'clock through `fraction` of a full turn around `c`.
void FillOutlineWedge(ImDrawList* draw, ImVec2 c, const std::vector<ImVec2>& outline, float fraction,
                      ImU32 color) {
    const float sweep = 2.0f * IM_PI * fraction;
    const ImVec2 dir(std::sin(sweep), -std::cos(sweep));
    auto cross = [](ImVec2 u, ImVec2 v) { return u.x * v.y - u.y * v.x; };
    draw->PathLineTo(c);
    for (std::size_t i = 0; i < outline.size(); ++i) {
        const ImVec2 a = outline[i];
        const ImVec2 b = outline[(i + 1) % outline.size()];
        draw->PathLineTo(a);
        const float b_angle = i + 1 == outline.size() ? 2.0f * IM_PI : ClockAngle(c, b);
        if (b_angle < sweep)
            continue;
        // The sweep ends on this edge: where the ray from c at `sweep` meets it.
        const ImVec2 ab(b.x - a.x, b.y - a.y);
        const float denom = cross(dir, ab);
        const float t = denom == 0.0f ? 1.0f
            : std::clamp(-cross(dir, ImVec2(a.x - c.x, a.y - c.y)) / denom, 0.0f, 1.0f);
        draw->PathLineTo({a.x + ab.x * t, a.y + ab.y * t});
        break;
    }
    draw->PathFillConcave(color);
}

}  // namespace

void ui::HighlightLetter(const Letter& letter) {
    g_game.highlight_letter = letter;
}

ImVec2 ui::OfficeScreenPos(PostOfficeId office, const World& world) {
    return WorldToScreen(ComputeViewTransform(), world.get_post_offices()[office].pos);
}

void DrawWorld(World& world) {
    ImGuiIO& io = ImGui::GetIO();
    // The mouse is "on the map" unless an ImGui window or the transport bar
    // is under it.
    const bool mouse_on_map = !io.WantCaptureMouse && !ui::OverTransportBar(world, io.MousePos);
    UpdateCamera(mouse_on_map);
    ui::EnsureOfficeSelected(world);
    // Null once the selected truck has arrived (or if none is selected); the
    // backup office then wears the selection ring instead.
    const std::shared_ptr<const Truck> selected_truck = g_game.selected_truck.lock();

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ViewTransform view = ComputeViewTransform();
    // Fixed on-screen size: offices do not grow with zoom.
    const float office_radius = 14.0f;
    const std::vector<PostOffice>& offices = world.get_post_offices();

    Position mouse_world = ScreenToWorld(view, io.MousePos);

    DrawMapGrid(draw, view);

    // Roads (built, underneath schedule routes). The road whose destroy menu
    // is open is outlined with the selection color so it is unambiguous which
    // road the menu refers to.
    for (const Road& road : world.get_roads()) {
        const bool has_menu = g_game.road_menu.road.has_value()
                           && road.src == g_game.road_menu.road->src
                           && road.dst == g_game.road_menu.road->dst;
        DrawRoad(draw, view, road.vertices, has_menu ? kColorSelection : kColorRoadOutline);
    }

    // A letter hovered in a list (last frame): its journey, under the offices.
    DrawLetterHighlight(draw, view, world, office_radius);
    g_game.highlight_letter.reset();

    // Road-building preview.
    if (g_game.road_build_state == RoadBuildState::PlacingVertices) {
        // Snap to (and highlight) a destination office near the cursor.
        if (const std::optional<PostOfficeId> near_office =
                NearestRoadDestination(offices, view, io.MousePos, office_radius * 3.0f, g_game.road_builder.src)) {
            mouse_world = offices[*near_office].pos;
            draw->AddCircle(WorldToScreen(view, mouse_world), office_radius + 8.0f, kColorRoadPreview, 0, 3.0f);
        }

        g_game.road_builder.preview_to(mouse_world, world);

        const bool invalid = g_game.road_builder.is_invalid();
        const ImU32 road_color = invalid ? kColorRoadBad : kColorRoadPreview;

        // Draw the segments, with a dot on every vertex past the source.
        const std::vector<Position> full_path = g_game.road_builder.preview_vertices();
        for (std::size_t i = 1; i < full_path.size(); ++i) {
            const ImVec2 a = WorldToScreen(view, full_path[i - 1]);
            const ImVec2 b = WorldToScreen(view, full_path[i]);
            draw->AddLine(a, b, road_color, 4.0f);
            draw->AddCircleFilled(b, 3.5f, kColorRoadVertex);
        }

        // Show the live length, and under it why the road is illegal, if it is.
        char length_text[64];
        std::snprintf(length_text, sizeof(length_text), "%.0f units", polyline_length(full_path));
        const ImVec2 length_size = ImGui::CalcTextSize(length_text);
        DrawChip(draw, {io.MousePos.x + 16.0f + length_size.x * 0.5f, io.MousePos.y + 2.0f}, length_text,
                 invalid ? kColorRoadBad : kColorText);
        if (invalid) {
            const char* reason = g_game.road_builder.invalid_reason.c_str();
            const ImVec2 reason_size = ImGui::CalcTextSize(reason);
            DrawChip(draw,
                     {io.MousePos.x + 16.0f + reason_size.x * 0.5f,
                      io.MousePos.y + 2.0f + ImGui::GetTextLineHeight() + 6.0f},
                     reason, kColorRoadBad);
        }
    }

    // Post offices, with the rings announcing new ones and the network
    // event's route underneath.
    DrawSpawnPulses(draw, view, world, office_radius);
    DrawEventRoute(draw, view, world, office_radius);
    // Full offices flash in real time, so they keep flashing while paused.
    const float flash = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.0f * IM_PI * 1.25f);
    for (PostOfficeId id = 0; id < offices.size(); ++id) {
        const PostOffice& office = offices[id];
        const ImVec2 c = WorldToScreen(view, office.pos);
        const bool full = office.outbound_letters.size() >= office.max_outbound_letters;
        // Its class's shape; the rings around it clear the shape's corners.
        const OfficeShape shape = OfficeShapeOf(office);
        const float extent = OfficeShapeExtent(shape, office_radius);
        if (selected_truck == nullptr && id == g_game.selected_office) {
            draw->AddCircleFilled(c, extent + 9.0f, kColorSelectionHalo);
            draw->AddCircle(c, extent + 5.0f, kColorSelection, 0, 3.0f);
        }
        if (full)
            draw->AddCircleFilled(c, extent + 8.0f + 5.0f * flash, U32(theme::kBad, 0.15f + 0.35f * flash));
        const std::vector<ImVec2> shadow = OfficeOutline(shape, {c.x, c.y + 2.0f}, office_radius + 1.0f);
        draw->AddConvexPolyFilled(shadow.data(), static_cast<int>(shadow.size()), kColorShadow);

        // Clock-style fill
        const float fill = office.max_outbound_letters == 0 ? 0.0f
            : std::min(1.0f, static_cast<float>(office.outbound_letters.size())
                                 / static_cast<float>(office.max_outbound_letters));
        const ImU32 fill_color = fill >= theme::kWarnFraction ? U32(theme::FillColor(fill)) : kColorOfficeFill;
        const std::vector<ImVec2> outline = OfficeOutline(shape, c, office_radius);
        const int outline_size = static_cast<int>(outline.size());
        draw->AddConvexPolyFilled(outline.data(), outline_size, kColorOfficeEmpty);
        if (fill >= 1.0f)
            draw->AddConvexPolyFilled(outline.data(), outline_size, fill_color);
        else if (fill > 0.0f)
            FillOutlineWedge(draw, c, outline, fill, fill_color);
        draw->AddPolyline(outline.data(), outline_size, kColorOfficeRing, ImDrawFlags_Closed, 2.0f);

        // The overflow timer, just outside the rim: an arc sweeping clockwise
        // from 12 o'clock while the office is full and shrinking back once it
        // has room again. A closed circle is the end of the game.
        if (office.overflow_ticks > 0) {
            const float timer = std::min(1.0f, static_cast<float>(office.overflow_ticks)
                                                   / static_cast<float>(kOverflowLimit));
            const float timer_radius = extent + 4.5f;
            const float start = -IM_PI * 0.5f;
            draw->AddCircle(c, timer_radius, kColorOverflowTrack, 0, 3.0f);
            draw->PathArcTo(c, timer_radius, start, start + 2.0f * IM_PI * timer);
            draw->PathStroke(kColorOverflow, 0, 3.0f);
        }

        const ImVec2 name_size = ImGui::CalcTextSize(office.name.c_str());
        const float top = std::max(office_radius, c.y - outline.front().y);  // the triangle's tip pokes up
        const ImVec2 name_pos(c.x - name_size.x * 0.5f, c.y - top - name_size.y - 7.0f);
        theme::DrawHaloText(draw, name_pos, full ? kColorOverflow : kColorText, kColorHalo,
                            office.name.c_str());

        // Postal code, in a small chip centered below the circle.
        // PushFont(nullptr, size) rescales the current font (both
        // CalcTextSize and AddText honor it); FontSizeBase rather than
        // GetFontSize() so the DPI/global scale is applied exactly once.
        if (!office.postal_code.empty()) {
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kPostalCodeFontScale);
            DrawChip(draw, {c.x, c.y + office_radius + 13.0f}, office.postal_code.c_str(), kColorTextDim);
            ImGui::PopFont();
        }
    }

    DrawEventBadges(draw, view, world, office_radius);
    DrawMoneyPopups(draw, view, world);

    // A click acts on release, and only if the press began on the map and
    // stayed within the drag threshold: anything further was a drag (pan).
    const float drag_threshold = io.MouseDragThreshold;
    const bool clicked_map = io.MouseReleased[0] && g_game.camera.press_on_map && mouse_on_map &&
                             io.MouseDragMaxDistanceSqr[0] < drag_threshold * drag_threshold;

    // Hover (outside road-building), topmost first: trucks draw over offices,
    // and offices win over roads (they draw on top, and every road ends at an
    // office position). At most one thing is hovered; a click acts on exactly
    // that (see the click handling below), and the hand cursor says so. Hit
    // areas are a little larger than what is drawn (everything is drawn at a
    // fixed pixel size), so small things stay easy to click. Nothing is
    // hovered while the map is being dragged.
    const float truck_hit_slack = 1.0f;
    const float office_hit_radius = office_radius + 6.0f;
    const float road_hit_tolerance = 8.0f;
    std::shared_ptr<Truck> hovered_truck;
    std::optional<PostOfficeId> hovered_office;
    std::optional<std::size_t> hovered_road;
    const bool hovering = mouse_on_map && !g_game.camera.dragging;
    if (hovering && g_game.road_build_state == RoadBuildState::Idle) {
        hovered_truck = HitTestTruck(world, view, io.MousePos, truck_hit_slack);
        if (hovered_truck == nullptr)
            hovered_office = HitTestOffice(offices, view, io.MousePos, office_hit_radius);
        if (hovered_truck == nullptr && !hovered_office.has_value())
            hovered_road = HitTestRoad(world.get_roads(), view, io.MousePos, road_hit_tolerance);
    } else if (hovering && g_game.road_build_state == RoadBuildState::SelectSource) {
        // Picking a road's source office: only offices are clickable.
        hovered_office = HitTestOffice(offices, view, io.MousePos, office_hit_radius);
    }
    if (hovered_truck != nullptr || hovered_office.has_value() || hovered_road.has_value())
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    // Trucks in transit (position is derived from the tick, so they animate
    // smoothly as the simulation advances and freeze when it is paused). Each
    // points along its road, nose solid red; its cargo box fills red from the
    // tail as it carries more of kTruckCapacity. The hovered truck gets an
    // ink outline, the selected one a haloed ring.
    for (const std::shared_ptr<Truck>& truck : world.get_trucks()) {
        const ImVec2 c = WorldToScreen(view, truck->get_position(world.get_tick()));
        const ImVec2 fwd = TruckForward(*truck, world.get_tick());
        const bool hovered = hovered_truck != nullptr && hovered_truck == truck;
        if (truck == selected_truck) {
            const std::vector<ImVec2> halo = TruckOutline(c, fwd, 7.0f);
            const std::vector<ImVec2> ring = TruckOutline(c, fwd, 4.0f);
            draw->AddConvexPolyFilled(halo.data(), static_cast<int>(halo.size()), kColorSelectionHalo);
            draw->AddPolyline(ring.data(), static_cast<int>(ring.size()), kColorSelection, ImDrawFlags_Closed, 2.5f);
        }
        const std::vector<ImVec2> shadow = TruckOutline({c.x, c.y + 1.5f}, fwd);
        draw->AddConvexPolyFilled(shadow.data(), static_cast<int>(shadow.size()), kColorShadow);

        const std::vector<ImVec2> outline = TruckOutline(c, fwd);
        const int outline_size = static_cast<int>(outline.size());
        draw->AddConvexPolyFilled(outline.data(), outline_size, kColorTruck);
        // The letters in the truck
        const float w = kTruckHalfWidth;
        const float load = std::min(1.0f, static_cast<float>(truck->get_num_letters())
                                              / static_cast<float>(kTruckCapacity));
        if (load > 0.0f) {
            const float level = kTruckTail + (kTruckShoulder - kTruckTail) * load;
            draw->AddQuadFilled(TruckPoint(c, fwd, kTruckTail, -w), TruckPoint(c, fwd, level, -w),
                                TruckPoint(c, fwd, level, w), TruckPoint(c, fwd, kTruckTail, w),
                                kColorOfficeBody);
        }
        draw->AddPolyline(outline.data(), outline_size, hovered ? kColorText : kColorTruck,
                          ImDrawFlags_Closed, hovered ? 2.0f : 1.5f);
    }
    if (hovered_truck != nullptr) {
        const Truck& truck = *hovered_truck;
        ImGui::SetTooltip("%s -> %s  (%zu letters)", offices[truck.get_src()].name.c_str(),
                          offices[truck.get_dst()].name.c_str(), truck.get_num_letters());
    }

    // Click handling: road-building mode overrides normal click-to-select.

    if (g_game.road_build_state != RoadBuildState::Idle) {
        // Right-click on the map or Esc anywhere cancels road-building.
        const bool cancelled =
            ImGui::IsKeyPressed(ImGuiKey_Escape) ||
            (io.MouseClicked[1] && !io.WantCaptureMouse);
        if (cancelled) {
            g_game.road_build_state = RoadBuildState::Idle;
            g_game.road_builder = RoadBuilder{};
        } else if (clicked_map) {
            if (g_game.road_build_state == RoadBuildState::SelectSource) {
                // Click on a source office to start.
                if (hovered_office.has_value()) {
                    const PostOfficeId id = *hovered_office;
                    g_game.road_builder = RoadBuilder::from_source(offices[id].pos, id);
                    g_game.road_build_state = RoadBuildState::PlacingVertices;
                }
            } 
            // An illegal (red) preview takes no clicks: the player can only
            // move the cursor somewhere legal or cancel. The preview above
            // was checked against this frame's cursor.
            else if (g_game.road_build_state == RoadBuildState::PlacingVertices
                     && !g_game.road_builder.is_invalid()) {
                // A click near a destination office completes the road
                // there (the same office the preview snapped to).
                if (const std::optional<PostOfficeId> dst = NearestRoadDestination(
                        offices, view, io.MousePos, office_radius * 3.0f, g_game.road_builder.src)) {
                    // Finish a copy: the preview ran the same checks, so
                    // add_road should not fail, but if it does the player
                    // keeps drawing instead of the road vanishing silently.
                    RoadBuilder finished = g_game.road_builder;
                    finished.commit_to_destination(offices[*dst].pos, *dst);
                    if (world.add_road(finished.src, finished.dst, finished.into_road().vertices)) {
                        g_game.road_build_state = RoadBuildState::Idle;
                        g_game.road_builder = RoadBuilder{};
                        SelectOffice(*dst);
                    }
                } else {
                    // Place a vertex at the snapped point.
                    g_game.road_builder.commit_middle_and_head();
                }
            }
        }
    } else if (clicked_map) {
        // Normal click: act on whatever is hovered (see the hover pass above).
        if (hovered_truck != nullptr) {
            SelectTruck(hovered_truck);
        } else if (hovered_office.has_value()) {
            SelectOffice(*hovered_office);
        } else if (hovered_road.has_value()) {
            // Arm the road's destroy menu at the click point. While the popup
            // is open io.WantCaptureMouse is true, so clicked_map is false and
            // this cannot re-fire until the menu is dismissed.
            const Road& road = world.get_roads()[*hovered_road];
            g_game.road_menu.road = RoadKey{road.src, road.dst};
            g_game.road_menu.pos = io.MousePos;
            g_game.road_menu.open_requested = true;
            g_game.road_menu.confirm = false;
        }
    }

    DrawRoadMenu(world);
}
