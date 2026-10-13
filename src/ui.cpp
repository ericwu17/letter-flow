// ui.cpp — Dear ImGui presentation layer: reads World state, draws the map,
// HUD and inspector, and turns player clicks into World mutations.
#include "ui.h"
#include "ui_internal.h"
#include "ui_theme.h"

#include "entities.h"
#include "balance.h"
#include "game_types.h"
#include "road.h"
#include "road_builder.h"
#include "truck.h"
#include "world.h"
#include "imgui.h"
#include "imgui_internal.h"  // DockBuilder* (imgui 'docking' branch)
#include "implot.h"          // ImPlot::ShowDemoWindow (the HUD's demo toggle)

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Shared with ui_stats.cpp through ui_internal.h; defined below.
using ui::FormatDayTime;
using ui::FormatDuration;
using ui::SelectOffice;

namespace {

struct ViewTransform {
    float scale = 1.0f;
    ImVec2 offset = {0.0f, 0.0f};
};

struct CameraState {
    float zoom = 1.0f;
    ImVec2 pan = {0.0f, 0.0f};
};

// Current selection (clicked on the map or picked in the inspector). A
// selected truck is only observed: the World stays its sole owner, and the
// weak_ptr expires the moment the World retires the truck on arrival. While
// no live truck is selected the inspector shows g_selected_office, which
// doubles as the backup: SelectTruck points it at the truck's destination,
// so an arrival falls through to that office with no extra bookkeeping.
// g_selected_office is kNoPostOffice until the first EnsureOfficeSelected.
std::weak_ptr<const Truck> g_selected_truck;
PostOfficeId g_selected_office = kNoPostOffice;
CameraState g_camera;
constexpr float kZoomStep = 1.2f;
constexpr float kPanStep = 5.0f;
constexpr float kZoomMin = 0.5f;
constexpr float kZoomMax = 8.0f;

namespace theme = ui::theme;
using theme::U32;

// Map label for an office's postal code: a small chip under the office.
constexpr float kPostalCodeFontScale = 0.75f;

// The map's and transport bar's colors, named by role. Every value comes from
// the shared palette in ui_theme.h, so re-theming the app is an edit there.
constexpr ImU32 kColorText          = U32(theme::kInk);          // labels, icons
constexpr ImU32 kColorTextDim       = U32(theme::kInkDim);       // speed label at 1x, postal codes
constexpr ImU32 kColorHalo          = U32(theme::kMap, 0.9f);    // outline around map labels
constexpr ImU32 kColorGrid          = U32(theme::kMapGrid);      // the map's paper grid
constexpr ImU32 kColorShadow        = U32(theme::kShadow, 0.22f);
constexpr ImU32 kColorChipBg        = U32(theme::kPaperLight, 0.9f);  // postal-code chips, cost label
constexpr ImU32 kColorChipBorder    = U32(theme::kPaper3);
constexpr ImU32 kColorOfficeBody    = U32(theme::kPaperLight);   // unfilled part of a built office
constexpr ImU32 kColorOfficeRing    = U32(theme::kNavy);
constexpr ImU32 kColorOfficeUnbuilt = U32(theme::kPaper2);       // offices not yet built
constexpr ImU32 kColorUnbuiltRing   = U32(theme::kInkDim);
constexpr ImU32 kColorSelection     = U32(theme::kRed);          // selection rings, hovered journey
constexpr ImU32 kColorSelectionHalo = U32(theme::kRed, 0.22f);
constexpr ImU32 kColorDestination   = U32(theme::kNavy);         // a hovered letter's destination
constexpr ImU32 kColorTruck         = U32(theme::kRed);
constexpr ImU32 kColorTruckOutline  = U32(theme::kPaperLight);
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
constexpr ImU32 kColorMoneyIn       = U32(theme::kGood);
constexpr ImU32 kColorMoneyOut      = U32(theme::kBad);
constexpr ImVec4 kColorErrorText    = theme::kBad;               // red inline messages

// Road colors: a printed road-map look, an ink outline around a light fill.
constexpr ImU32 kColorRoadOutline = U32(theme::kInk, 0.8f);
constexpr ImU32 kColorRoadFill    = U32(theme::kPaperLight);
constexpr ImU32 kColorRoadPreview = U32(theme::kWarn);       // road-building preview
constexpr ImU32 kColorRoadVertex  = U32(theme::kWarnText);   // placed vertices in preview
constexpr ImU32 kColorRoadBad     = U32(theme::kBad);        // invalid preview (crossing)

// The map's paper grid, in world units.
constexpr float kGridStep = 100.0f;

// Road-building state machine
enum class RoadBuildState { Idle, SelectSource, PlacingVertices };
RoadBuildState g_road_build_state = RoadBuildState::Idle;
RoadBuilder g_road_builder;

// Road-destroy menu state. The road is keyed by its endpoint-id pair — never
// an index or pointer — because remove_road() erases from the roads vector.
struct RoadKey {
    PostOfficeId src;
    PostOfficeId dst;
};
std::optional<RoadKey> g_road_menu_road;
ImVec2 g_road_menu_pos = {0.0f, 0.0f};    // click point, screen space
bool g_road_menu_open_requested = false;  // latch for OpenPopup in DrawRoadMenu
bool g_road_menu_confirm = false;         // two-step confirm: first click arms


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

// The truck whose drawn square (half-size `half_px`, screen space) contains
// `screen_pos`, or null. Trucks draw in vector order, so the last hit is the
// one on top.
std::shared_ptr<Truck> HitTestTruck(const World& world, const ViewTransform& view,
                                    ImVec2 screen_pos, float half_px) {
    std::shared_ptr<Truck> hit;
    for (const std::shared_ptr<Truck>& truck : world.get_trucks()) {
        const ImVec2 c = WorldToScreen(view, truck->get_position(world.get_tick()));
        if (std::abs(screen_pos.x - c.x) <= half_px && std::abs(screen_pos.y - c.y) <= half_px)
            hit = truck;
    }
    return hit;
}

// ---------------------------------------------------------------------------
// Selection helpers
// ---------------------------------------------------------------------------

}  // namespace

void ui::SelectOffice(PostOfficeId id) {
    g_selected_truck.reset();
    g_selected_office = id;
}

namespace {

// Selects a truck, with its destination office as the backup shown once the
// truck arrives.
void SelectTruck(const std::shared_ptr<const Truck>& truck) {
    SelectOffice(truck->get_dst());
    g_selected_truck = truck;
}

// Defaults the selection to the first office until the player picks one
// (and guards against an id from a different world).
void EnsureOfficeSelected(const World& world) {
    if (g_selected_office >= world.get_post_offices().size())
        g_selected_office = 0;
}

// ---------------------------------------------------------------------------
// Tick -> 24-hour clock formatting. Every time shown to the player goes
// through these helpers; internally the simulation only ever uses ticks.
// ---------------------------------------------------------------------------

}  // namespace

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

namespace {

// Every '/'-delimited prefix of every office's postal code ("NJ",
// "NJ/Mercer", "NJ/Mercer/Princeton", ...), sorted and deduplicated — the
// suggestion list for the prefix field of a PostalPrefix rule. Recomputed
// each frame, which is trivially cheap at this map size.
std::vector<std::string> CollectPostalPrefixes(const std::vector<PostOffice>& offices) {
    std::vector<std::string> prefixes;
    for (const PostOffice& office : offices) {
        const std::string& code = office.postal_code;
        if (code.empty())
            continue;
        for (std::size_t slash = code.find('/'); slash != std::string::npos;
             slash = code.find('/', slash + 1))
            prefixes.push_back(code.substr(0, slash));
        prefixes.push_back(code);  // the full code itself is a valid prefix
    }
    std::sort(prefixes.begin(), prefixes.end());
    prefixes.erase(std::unique(prefixes.begin(), prefixes.end()), prefixes.end());
    return prefixes;
}

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

ImGuiID MainDockSpaceId() { return ImGui::GetID("MainDockSpace"); }

// The left dock node the inspector is pinned into; set by DrawDockspace on
// the first frame and used by DrawInspector to re-dock it every frame.
ImGuiID g_inspector_dock_id = 0;

TransportLayout ComputeTransportLayout(const World& world) {
    ImGuiIO& io = ImGui::GetIO();
    TransportLayout l;

    // Anchor the bar to the dockspace's central node — the visible map area,
    // which excludes the docked inspector panel
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

}  // namespace

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

// ---------------------------------------------------------------------------
// Road destroy menu: a small ImGui popup anchored at the point where the road
// was clicked, armed by DrawWorld's click handling. Two-step confirm — the
// first click on the entry turns it into "Confirm?", the second destroys the
// road (no refund). Esc or a click elsewhere dismisses it; either way the
// state resets so the highlight and the menu never outlive each other.
// ---------------------------------------------------------------------------

static void DrawRoadMenu(World& world) {
    if (!g_road_menu_road.has_value())
        return;

    // OpenPopup dereferences the current window, and the map lives outside any
    // Begin/End pair — so host the popup in an invisible, zero-size,
    // input-transparent window placed exactly at the click point.
    ImGui::SetNextWindowPos(g_road_menu_pos);
    ImGui::Begin("##road_menu_host", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoInputs);

    if (g_road_menu_open_requested) {
        ImGui::OpenPopup("road_destroy_menu");
        g_road_menu_open_requested = false;
    }

    if (ImGui::BeginPopup("road_destroy_menu")) {
        if (ImGui::IsWindowAppearing())
            g_road_menu_confirm = false;

        const RoadKey key = g_road_menu_road.value();
        const std::vector<PostOffice>& offices = world.get_post_offices();
        char title[128];
        std::snprintf(title, sizeof(title), "%s <-> %s",
                      offices[key.src].name.c_str(), offices[key.dst].name.c_str());
        ImGui::TextDisabled("%s", title);
        ImGui::Separator();

        // NoAutoClosePopups keeps the first click from closing the popup, so
        // the same entry can serve as the two-step confirm.
        if (ImGui::Selectable(g_road_menu_confirm ? "Confirm? (no refund)" : "Destroy road",
                              false, ImGuiSelectableFlags_NoAutoClosePopups)) {
            if (!g_road_menu_confirm) {
                g_road_menu_confirm = true;
            } else {
                world.remove_road(key.src, key.dst);
                ImGui::CloseCurrentPopup();
                g_road_menu_road.reset();
                g_road_menu_confirm = false;
            }
        }
        ImGui::EndPopup();
    } else if (!g_road_menu_open_requested) {
        // The popup was closed externally (Esc or a click outside it). ImGui
        // consumes the closing click, so it cannot leak through to the map
        // below; just reset the menu state.
        g_road_menu_road.reset();
        g_road_menu_confirm = false;
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// The map
// ---------------------------------------------------------------------------

namespace {

// Floating money text that rises from an office and fades out: a green "+$N"
// when a truck delivers there, a red "-$N" when a truck departs from there.
// Animated in real time (frame delta), so it looks the same at any game
// speed; the list is purely presentational.
struct MoneyPopup {
    PostOfficeId office;
    int amount;
    float age;  // seconds
};
std::vector<MoneyPopup> g_money_popups;
constexpr float kPopupDuration = 1.6f;
constexpr float kPopupRise = 55.0f;

void DrawMoneyPopups(ImDrawList* draw, const ViewTransform& view, World& world) {
    for (const MoneyEvent& e : world.take_money_events())
        g_money_popups.push_back({e.office, e.amount, 0.0f});

    const float dt = ImGui::GetIO().DeltaTime;
    const std::vector<PostOffice>& offices = world.get_post_offices();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
    for (MoneyPopup& p : g_money_popups) {
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
    std::erase_if(g_money_popups, [](const MoneyPopup& p) { return p.age >= kPopupDuration; });
}

// The letter hovered in a list last frame (ui::HighlightLetter). DrawWorld
// draws its journey and then clears it, so the highlight lapses as soon as
// the list stops asking for it.
std::optional<Letter> g_highlight_letter;

// A hovered letter's journey on the map: the legs it has travelled, along the
// roads it took; the truck it is on, if any; and rings around the office it
// was posted at and the one it is addressed to.
void DrawLetterHighlight(ImDrawList* draw, const ViewTransform& view, const World& world, float office_radius) {
    if (!g_highlight_letter.has_value())
        return;
    const Letter& letter = *g_highlight_letter;
    const std::vector<PostOffice>& offices = world.get_post_offices();
    if (letter.src >= offices.size() || letter.dst >= offices.size())
        return;

    for (std::size_t i = 1; i < letter.trail.size(); ++i) {
        const TrailStop& from = letter.trail[i - 1];
        const TrailStop& to = letter.trail[i];
        if (from.event != TrailEvent::Departed || to.event != TrailEvent::Arrived ||
            from.office >= offices.size() || to.office >= offices.size())
            continue;
        std::vector<Position> leg = world.get_road_vertices_from_a_to_b(from.office, to.office);
        if (leg.empty())  // the road has been destroyed since
            leg = {offices[from.office].pos, offices[to.office].pos};
        for (std::size_t k = 1; k < leg.size(); ++k)
            draw->AddLine(WorldToScreen(view, leg[k - 1]), WorldToScreen(view, leg[k]), kColorSelection, 5.0f);
    }

    // On a truck right now: ring it, and trace the leg so far.
    if (!letter.trail.empty() && letter.trail.back().event == TrailEvent::Departed) {
        for (const std::shared_ptr<Truck>& truck : world.get_trucks()) {
            const std::vector<Letter>& cargo = truck->get_letters();
            if (std::find(cargo.begin(), cargo.end(), letter) == cargo.end())
                continue;
            const ImVec2 at = WorldToScreen(view, truck->get_position(world.get_tick()));
            draw->AddLine(WorldToScreen(view, offices[truck->get_src()].pos), at, kColorSelection, 3.0f);
            draw->AddCircleFilled(at, 13.0f, kColorSelectionHalo);
            draw->AddCircle(at, 11.0f, kColorSelection, 0, 3.0f);
            break;
        }
    }

    draw->AddCircle(WorldToScreen(view, offices[letter.src].pos), office_radius + 9.0f, kColorSelection, 0, 2.5f);
    draw->AddCircle(WorldToScreen(view, offices[letter.dst].pos), office_radius + 9.0f, kColorDestination, 0, 4.0f);
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

}  // namespace

void ui::HighlightLetter(const Letter& letter) {
    g_highlight_letter = letter;
}

void DrawWorld(World& world) {
    UpdateCamera();
    EnsureOfficeSelected(world);
    // Null once the selected truck has arrived (or if none is selected); the
    // backup office then wears the selection ring instead.
    const std::shared_ptr<const Truck> selected_truck = g_selected_truck.lock();

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ViewTransform view = ComputeViewTransform();
    // Fixed on-screen size: offices do not grow with zoom.
    const float office_radius = 14.0f;
    const std::vector<PostOffice>& offices = world.get_post_offices();

    ImGuiIO& io = ImGui::GetIO();
    Position mouse_world = ScreenToWorld(view, io.MousePos);

    DrawMapGrid(draw, view);

    // Roads (built, underneath schedule routes). The road whose destroy menu
    // is open is outlined with the selection color so it is unambiguous which
    // road the menu refers to.
    for (const Road& road : world.get_roads()) {
        const bool has_menu = g_road_menu_road.has_value()
                           && road.src == g_road_menu_road->src
                           && road.dst == g_road_menu_road->dst;
        DrawRoad(draw, view, road.vertices, has_menu ? kColorSelection : kColorRoadOutline);
    }

    // A letter hovered in a list (last frame): its journey, under the offices.
    DrawLetterHighlight(draw, view, world, office_radius);
    g_highlight_letter.reset();

    // Road-building preview.
    if (g_road_build_state == RoadBuildState::PlacingVertices) {
        // Highlight destination office near the cursor.
        {
            const float near_radius = office_radius * 3.0f;
            PostOfficeId near_office = kNoPostOffice;
            float best_dist = near_radius * near_radius;
            for (PostOfficeId id = 0; id < offices.size(); ++id) {
                if (id == g_road_builder.src || !offices[id].built) continue;
                const ImVec2 oc = WorldToScreen(view, offices[id].pos);
                const float dx = io.MousePos.x - oc.x;
                const float dy = io.MousePos.y - oc.y;
                const float dist_sq = dx * dx + dy * dy;
                if (dist_sq < best_dist) {
                    best_dist = dist_sq;
                    near_office = id;
                }
            }
            if (near_office != kNoPostOffice) {
                mouse_world = offices[near_office].pos;
                const ImVec2 oc = WorldToScreen(view, offices[near_office].pos);
                draw->AddCircle(oc, office_radius + 8.0f, kColorRoadPreview, 0, 3.0f);
            }
        }

        
        g_road_builder.build_to(mouse_world);
        g_road_builder.check_road(world);

        const ImU32 road_color = g_road_builder.is_invalid ? kColorRoadBad : kColorRoadPreview;

        // Draw placed vertices and segments.
        for (std::size_t i = 1; i < g_road_builder.vertices.size(); ++i) {
            const ImVec2 a = WorldToScreen(view, g_road_builder.vertices[i - 1]);
            const ImVec2 b = WorldToScreen(view, g_road_builder.vertices[i]);
            draw->AddLine(a, b, road_color, 4.0f);
            draw->AddCircleFilled(b, 3.5f, kColorRoadVertex);
        }
        Position last_vertex = g_road_builder.vertices.back();
        if (g_road_builder.middle.has_value()) {
            const ImVec2 a = WorldToScreen(view, last_vertex);
            const ImVec2 b = WorldToScreen(view, g_road_builder.middle.value());
            draw->AddLine(a, b, road_color, 4.0f);
            draw->AddCircleFilled(b, 3.5f, kColorRoadVertex);
            last_vertex = g_road_builder.middle.value();
        }
        if (g_road_builder.head.has_value()) {
            const ImVec2 a = WorldToScreen(view, last_vertex);
            const ImVec2 b = WorldToScreen(view, g_road_builder.head.value());
            draw->AddLine(a, b, road_color, 4.0f);
            draw->AddCircleFilled(b, 3.5f, kColorRoadVertex);
            last_vertex = g_road_builder.head.value();
        }

        // Show live cost.
        const std::vector<Position> full_path = [&]() {
            auto path = g_road_builder.vertices;
            if (g_road_builder.middle.has_value()) {
                path.push_back(g_road_builder.middle.value());
            }
            if (g_road_builder.head.has_value()) {
                path.push_back(g_road_builder.head.value());
            }
            return path;
        }();
        const double total_len = polyline_length(full_path);
        const int cost = road_cost(full_path);
        char cost_text[64];
        std::snprintf(cost_text, sizeof(cost_text), "$%d (%.0f units)", cost, total_len);
        const ImVec2 cost_size = ImGui::CalcTextSize(cost_text);
        DrawChip(draw, {io.MousePos.x + 16.0f + cost_size.x * 0.5f, io.MousePos.y + 2.0f}, cost_text,
                 g_road_builder.is_invalid ? kColorRoadBad : kColorText);
    }

    // Post offices.
    for (PostOfficeId id = 0; id < offices.size(); ++id) {
        const PostOffice& office = offices[id];
        const ImVec2 c = WorldToScreen(view, office.pos);
        if (selected_truck == nullptr && id == g_selected_office) {
            draw->AddCircleFilled(c, office_radius + 9.0f, kColorSelectionHalo);
            draw->AddCircle(c, office_radius + 5.0f, kColorSelection, 0, 3.0f);
        }
        draw->AddCircleFilled({c.x, c.y + 2.0f}, office_radius + 1.0f, kColorShadow);
        if (!office.built) {
            // Not built yet: a blank disc with a dashed rim.
            draw->AddCircleFilled(c, office_radius, kColorOfficeUnbuilt);
            constexpr int kDashes = 12;
            const float dash = 2.0f * IM_PI / static_cast<float>(kDashes);
            for (int i = 0; i < kDashes; ++i) {
                const float a0 = static_cast<float>(i) * dash;
                draw->PathArcTo(c, office_radius, a0, a0 + dash * 0.55f, 4);
                draw->PathStroke(kColorUnbuiltRing, 0, 1.5f);
            }
        } else {
            // Clock-style fill: a wedge sweeping clockwise from 12 o'clock
            // shows how full the outbound buffer is, turning warn then bad
            // as it nears max_outbound_letters.
            const float fill = office.max_outbound_letters == 0 ? 0.0f
                : std::min(1.0f, static_cast<float>(office.outbound_letters.size())
                                     / static_cast<float>(office.max_outbound_letters));
            const ImU32 fill_color = U32(theme::FillColor(fill));
            draw->AddCircleFilled(c, office_radius, kColorOfficeBody);
            if (fill >= 1.0f) {
                draw->AddCircleFilled(c, office_radius, fill_color);
            } else if (fill > 0.0f) {
                const float start = -IM_PI * 0.5f;
                draw->PathLineTo(c);
                draw->PathArcTo(c, office_radius, start, start + 2.0f * IM_PI * fill);
                draw->PathFillConcave(fill_color);
            }
            draw->AddCircle(c, office_radius, kColorOfficeRing, 0, 2.0f);
        }

        const ImVec2 name_size = ImGui::CalcTextSize(office.name.c_str());
        const ImVec2 name_pos(c.x - name_size.x * 0.5f, c.y - office_radius - name_size.y - 5.0f);
        theme::DrawHaloText(draw, name_pos, office.built ? kColorText : kColorTextDim, kColorHalo,
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

    DrawMoneyPopups(draw, view, world);

    // The mouse is "on the map" unless an ImGui window or the transport bar
    // is under it.
    const TransportLayout bar = ComputeTransportLayout(world);
    const bool mouse_on_map = !io.WantCaptureMouse
                           && !PointInRect(io.MousePos, bar.bar_min, bar.bar_max);
    const bool clicked_map = io.MouseClicked[0] && mouse_on_map;

    // Hover (outside road-building), topmost first: trucks draw over offices,
    // and offices win over roads (they draw on top, and every road ends at an
    // office position). At most one thing is hovered; a click acts on exactly
    // that (see the click handling below), and the hand cursor says so. Hit
    // areas have pixel floors so everything stays clickable when zoomed out.
    const float truck_half = 5.5f;
    const float office_hit_radius = office_radius + 6.0f;
    std::shared_ptr<Truck> hovered_truck;
    std::optional<PostOfficeId> hovered_office;
    std::optional<std::size_t> hovered_road;
    if (mouse_on_map && g_road_build_state == RoadBuildState::Idle) {
        hovered_truck = HitTestTruck(world, view, io.MousePos, std::max(truck_half, 6.0f));
        if (hovered_truck == nullptr)
            hovered_office = HitTestOffice(offices, view, io.MousePos, office_hit_radius);
        if (hovered_truck == nullptr && !hovered_office.has_value())
            hovered_road = HitTestRoad(world.get_roads(), view, io.MousePos,
                                       std::max(8.0f, 2.5f));
    } else if (mouse_on_map && g_road_build_state == RoadBuildState::SelectSource) {
        // Picking a road's source office: only built offices are clickable.
        hovered_office = HitTestOffice(offices, view, io.MousePos, office_hit_radius);
        if (hovered_office.has_value() && !offices[*hovered_office].built)
            hovered_office.reset();
    }
    if (hovered_truck != nullptr || hovered_office.has_value() || hovered_road.has_value())
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    // Trucks in transit (position is derived from the tick, so they animate
    // smoothly as the simulation advances and freeze when it is paused). The
    // hovered truck gets an ink outline, the selected one a haloed ring.
    for (const std::shared_ptr<Truck>& truck : world.get_trucks()) {
        const ImVec2 c = WorldToScreen(view, truck->get_position(world.get_tick()));
        const float r = truck_half;
        const bool hovered = hovered_truck != nullptr && hovered_truck == truck;
        if (truck == selected_truck) {
            const float ring = r + 4.0f;
            draw->AddRectFilled({c.x - ring - 3.0f, c.y - ring - 3.0f}, {c.x + ring + 3.0f, c.y + ring + 3.0f},
                                kColorSelectionHalo, 5.0f);
            draw->AddRect({c.x - ring, c.y - ring}, {c.x + ring, c.y + ring}, kColorSelection, 3.0f, 0, 2.5f);
        }
        draw->AddRectFilled({c.x - r, c.y - r + 1.5f}, {c.x + r, c.y + r + 1.5f}, kColorShadow, 2.0f);
        draw->AddRectFilled({c.x - r, c.y - r}, {c.x + r, c.y + r}, kColorTruck, 2.0f);
        draw->AddRect({c.x - r, c.y - r}, {c.x + r, c.y + r},
                      hovered ? kColorText : kColorTruckOutline, 2.0f, 0, hovered ? 2.0f : 1.5f);
    }
    if (hovered_truck != nullptr) {
        const Truck& truck = *hovered_truck;
        ImGui::SetTooltip("%s -> %s  (%zu letters)", offices[truck.get_src()].name.c_str(),
                          offices[truck.get_dst()].name.c_str(), truck.get_num_letters());
    }

    // Click handling: road-building mode overrides normal click-to-select.

    if (g_road_build_state != RoadBuildState::Idle) {
        // Right-click on the map or Esc anywhere cancels road-building.
        const bool cancelled =
            ImGui::IsKeyPressed(ImGuiKey_Escape) ||
            (io.MouseClicked[1] && !io.WantCaptureMouse);
        if (cancelled) {
            g_road_build_state = RoadBuildState::Idle;
            g_road_builder = RoadBuilder{};
        } else if (clicked_map) {
            if (g_road_build_state == RoadBuildState::SelectSource) {
                // Click on a source office to start.
                if (hovered_office.has_value()) {
                    const PostOfficeId id = *hovered_office;
                    g_road_builder = RoadBuilder::RoadBuilder_from_source(offices[id].pos, id);
                    g_road_build_state = RoadBuildState::PlacingVertices;
                }
            } 
            else if (g_road_build_state == RoadBuildState::PlacingVertices) {
                // Check if the snapped point or mouse cursor is on a
                // destination office to complete the road.
                bool hit_office = false;
                for (PostOfficeId id = 0; id < offices.size(); ++id) {
                    if (id == g_road_builder.src || !offices[id].built) continue;
                    const ImVec2 oc = WorldToScreen(view, offices[id].pos);
                    const float hit_radius = office_radius * 3.0f;
                    const float dx_mouse = io.MousePos.x - oc.x;
                    const float dy_mouse = io.MousePos.y - oc.y;
                    // Accept if the mouse cursor is close.
                    if (dx_mouse * dx_mouse + dy_mouse * dy_mouse <= hit_radius * hit_radius) {
                        // Complete the road to the destination post office.
                        g_road_builder.commit_to_destination(offices[id].pos, id);

                        PostOfficeId src = g_road_builder.src;
                        PostOfficeId dst = g_road_builder.dst;
                        world.add_road(src, dst, g_road_builder.into_road().vertices);
                        g_road_build_state = RoadBuildState::Idle;
                        g_road_builder = RoadBuilder{};
                        SelectOffice(id);
                        hit_office = true;
                        break;
                    }
                }
                if (!hit_office) {
                    // Place a vertex at the snapped point.
                    g_road_builder.commit_middle_and_head();
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
            g_road_menu_road = RoadKey{road.src, road.dst};
            g_road_menu_pos = io.MousePos;
            g_road_menu_open_requested = true;
            g_road_menu_confirm = false;
        }
    }

    DrawRoadMenu(world);
}

// ---------------------------------------------------------------------------
// Transport bar (play/pause, slow-down/speed-up, day/time) — drawn into the
// background draw list, on top of the map but underneath ImGui windows.
// ---------------------------------------------------------------------------

void DrawTransportBar(World& world) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const TransportLayout l = ComputeTransportLayout(world);
    const float speed = world.get_speed_multiplier();
    const bool fast_forward = speed > 1.0f;
    const bool can_slow = speed > kSpeedSteps[0];
    const bool can_speed_up = speed < kSpeedSteps[kSpeedStepCount - 1];
    const float b = l.button_size;
    const float rounding = b * 0.2f;

    // Panel.
    theme::DrawShadowedPanel(draw, l.bar_min, l.bar_max, rounding * 1.5f);

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

namespace {

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
    ImVec2 area_min = {0.0f, 0.0f};
    ImVec2 area_size = io.DisplaySize;
    if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(MainDockSpaceId())) {
        area_min = central->Pos;
        area_size = central->Size;
    }
    const float margin = ImGui::GetFrameHeight() * 0.4f;
    const float right = area_min.x + area_size.x - margin;
    const TransportLayout bar = ComputeTransportLayout(world);
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
        ui::ToggleStatsWindow();
    ImGui::End();

    if (show_imgui_demo)
        ImGui::ShowDemoWindow(&show_imgui_demo);
    if (show_implot_demo)
        ImPlot::ShowDemoWindow(&show_implot_demo);
}

// ---------------------------------------------------------------------------
// Inspector: one docked window showing whichever entity is selected. Its
// title changes with the selection, but its ID ("###Inspector") stays fixed,
// so it keeps its dock slot.
// ---------------------------------------------------------------------------

// Text pushed to the right edge of the current table cell (numbers).
static void TextRightAligned(const char* text, ImVec4 color = theme::kInk) {
    const float w = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
    ImGui::TextColored(color, "%s", text);
}

// Letters as a To / Here for / Deadline / Value / Max bonus table; overdue
// deadlines get a red LATE tag. "Here for" is the time since the letter got where it is
// (this office, or this truck). Hovering a row shows the letter's trail and
// highlights its journey on the map.
static void DrawLetterTable(const char* table_id, const std::vector<Letter>& letters,
                            const std::vector<PostOffice>& offices, Tick now) {
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (letters.empty()) {
        ImGui::TextDisabled("No letters.");
        return;
    }
    if (ImGui::BeginTable(table_id, 5, flags, ImVec2(0.0f, 200.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);  // keep the header visible while scrolling
        ImGui::TableSetupColumn("To");
        ImGui::TableSetupColumn("Here for");
        ImGui::TableSetupColumn("Deadline", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Max bonus");
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < letters.size(); ++i) {
            const Letter& letter = letters[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Selectable(offices[letter.dst].name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
            if (ImGui::IsItemHovered()) {
                ui::HighlightLetter(letter);
                ImGui::BeginTooltip();
                ui::DrawLetterTrail(letter, offices, now);
                ImGui::EndTooltip();
            }
            ImGui::TableSetColumnIndex(1);
            char here_for[16];
            FormatDuration(now - std::min(now, letter.waiting_since()), here_for, sizeof(here_for));
            ImGui::TextUnformatted(here_for);
            ImGui::TableSetColumnIndex(2);
            char due[32];
            FormatDayTime(letter.deadline, due, sizeof(due));
            if (letter.deadline >= now) {
                ImGui::TextUnformatted(due);
            } else {
                theme::Pill("LATE", theme::kBad, theme::kPaperLight);
                ImGui::SameLine();
                ImGui::TextDisabled("%s", due);
            }
            char money[16];
            ImGui::TableSetColumnIndex(3);
            std::snprintf(money, sizeof(money), "$%d", letter.value);
            TextRightAligned(money);
            ImGui::TableSetColumnIndex(4);
            std::snprintf(money, sizeof(money), "$%d", letter.max_bonus);
            TextRightAligned(money, theme::kGood);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// A truck in flight: route, timing and cargo. The World still owns the
// truck, so it outlives a "Go to" click that drops the selection.
static void DrawTruckPanel(const World& world, const Truck& truck) {
    const std::vector<PostOffice>& offices = world.get_post_offices();
    const PostOfficeId src = truck.get_src();
    const PostOfficeId dst = truck.get_dst();

    theme::Pill("TRUCK IN TRANSIT", theme::kRed, theme::kPaperLight);
    ImGui::PushFont(nullptr, theme::kFontTitle);
    ImGui::Text("%s  ->  %s", offices[src].name.c_str(), offices[dst].name.c_str());
    ImGui::PopFont();

    char go_label[160];
    std::snprintf(go_label, sizeof(go_label), "Go to %s##go_src", offices[src].name.c_str());
    if (ImGui::SmallButton(go_label))
        SelectOffice(src);
    ImGui::SameLine();
    std::snprintf(go_label, sizeof(go_label), "Go to %s##go_dst", offices[dst].name.c_str());
    if (ImGui::SmallButton(go_label))
        SelectOffice(dst);
    ImGui::Spacing();

    const Tick now = world.get_tick();
    const Tick departure = truck.get_departure_tick();
    const Tick arrival = truck.get_arrival_tick();
    char departed[32], eta[32], left[16], overlay[32];
    FormatDayTime(departure, departed, sizeof(departed));
    FormatDayTime(arrival, eta, sizeof(eta));
    FormatDuration(arrival > now ? arrival - now : 0, left, sizeof(left));
    std::snprintf(overlay, sizeof(overlay), "%s to go", left);
    if (ImGui::BeginTable("truck_times", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("Departed");
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(departed);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("Arrives");
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(eta);
        ImGui::EndTable();
    }
    const Tick elapsed = now > departure ? now - departure : 0;
    const float progress = arrival > departure
        ? std::clamp(static_cast<float>(elapsed) / static_cast<float>(arrival - departure), 0.0f, 1.0f)
        : 1.0f;
    // A tan fill rather than navy, so the ink overlay text stays readable.
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kInk);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme::kPaper4);
    ImGui::ProgressBar(progress, ImVec2(-FLT_MIN, 0.0f), overlay);
    ImGui::PopStyleColor(2);

    char cargo_title[64];
    std::snprintf(cargo_title, sizeof(cargo_title), "Cargo  (%zu / %zu letters)", truck.get_num_letters(),
                  kTruckCapacity);
    theme::SectionHeading(cargo_title);
    DrawLetterTable("truck_cargo", truck.get_letters(), offices, now);
}

// An HH:MM cell: two narrow drag fields (drag, or double-click to type).
// `max_hours` bounds the hour field; the caller clamps the combined value.
// Returns true and writes `ticks` when the player changed either field.
static bool EditHoursMinutes(const char* id, Tick& ticks, int max_hours) {
    const ClockTime t = duration_to_hours_minutes(ticks);
    int hours = t.hour;
    int minutes = t.minute;
    const float field_width = ImGui::CalcTextSize("00").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(field_width);
    bool changed = ImGui::DragInt("##h", &hours, 0.05f, 0, max_hours, "%02d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::TextUnformatted(":");
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::SetNextItemWidth(field_width);
    changed |= ImGui::DragInt("##m", &minutes, 0.2f, 0, 59, "%02d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopID();
    if (changed)
        ticks = hours_minutes_to_ticks(hours, minutes);
    return changed;
}

// A PostalPrefix rule's prefix list: one field per prefix, stacked
// vertically, with an "OR" label in a left gutter halfway between each pair
// of consecutive fields. Each field has a remove button while more than one
// prefix is left, and "+ Or..." appends a prefix in use on the map (a real
// one, so the list stays valid and an edit applied straight to the World is
// accepted unless the player typed an empty or repeated prefix). `width` spans gutter, fields and buttons; 0 fills the remaining
// width. Returns true and writes `prefixes` when the player changed the list.
static bool EditPrefixList(const char* id, std::vector<std::string>& prefixes,
                           const std::vector<PostOffice>& offices, float width) {
    // The prefix being typed. A field's buffer is otherwise rebuilt from
    // `prefixes` every frame, but a keystroke the caller refuses (an empty or
    // duplicate prefix) must not snap the text back mid-edit, so the active
    // field keeps its text here; it reverts to the caller's value once
    // deactivated. Only one field is ever active, so one buffer serves all.
    static char active_prefix[256];

    const ImGuiStyle& style = ImGui::GetStyle();
    const float or_width = ImGui::CalcTextSize("OR").x;
    const float gutter = or_width + style.ItemSpacing.x;
    const float total_width = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    const bool removable = prefixes.size() > 1;
    const float remove_width = ImGui::CalcTextSize("x").x + style.FramePadding.x * 2.0f;
    const float field_width = total_width - gutter - (removable ? remove_width + style.ItemSpacing.x : 0.0f);

    bool changed = false;
    std::optional<std::size_t> to_remove;
    std::vector<float> row_centers;  // screen y of each field's vertical center

    ImGui::PushID(id);
    ImGui::BeginGroup();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    for (std::size_t k = 0; k < prefixes.size(); ++k) {
        ImGui::PushID(static_cast<int>(k));
        ImGui::SetCursorScreenPos({origin.x + gutter, ImGui::GetCursorScreenPos().y});
        const ImGuiID field_id = ImGui::GetID("##prefix");
        const bool editing = ImGui::GetActiveID() == field_id;
        char row_prefix[256];
        std::snprintf(row_prefix, sizeof(row_prefix), "%s", prefixes[k].c_str());
        char* buf = editing ? active_prefix : row_prefix;
        // Red while the typed text is not (yet) an accepted prefix.
        const bool refused = editing && prefixes[k] != active_prefix;
        if (refused)
            ImGui::PushStyleColor(ImGuiCol_Text, kColorErrorText);
        ImGui::SetNextItemWidth(field_width);
        if (ImGui::InputTextWithHint("##prefix", "e.g. NJ/North", buf, sizeof(active_prefix))) {
            prefixes[k] = buf;
            changed = true;
        }
        if (refused)
            ImGui::PopStyleColor();
        if (ImGui::IsItemActivated())
            std::snprintf(active_prefix, sizeof(active_prefix), "%s", row_prefix);
        row_centers.push_back((ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
        if (removable) {
            ImGui::SameLine();
            if (ImGui::Button("x"))
                to_remove = k;
            ImGui::SetItemTooltip("Remove this prefix");
        }
        ImGui::PopID();
    }

    // Append: every prefix in use on the map that is not listed yet.
    ImGui::SetCursorScreenPos({origin.x + gutter, ImGui::GetCursorScreenPos().y});
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("+ Or...").x + ImGui::GetFrameHeight() +
                            style.FramePadding.x * 2.0f);
    if (ImGui::BeginCombo("##add_prefix", "+ Or...")) {
        bool any = false;
        for (const std::string& suggestion : CollectPostalPrefixes(offices)) {
            if (std::find(prefixes.begin(), prefixes.end(), suggestion) != prefixes.end())
                continue;
            any = true;
            if (ImGui::Selectable(suggestion.c_str())) {
                prefixes.push_back(suggestion);
                changed = true;
            }
        }
        if (!any)
            ImGui::TextDisabled("Every prefix on the map is listed.");
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Also pick up letters addressed to another prefix");
    ImGui::EndGroup();

    // "OR" between each pair of fields, drawn once their positions are known
    // so the labels sit exactly halfway without taking part in the layout.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float text_height = ImGui::GetTextLineHeight();
    for (std::size_t k = 0; k + 1 < row_centers.size(); ++k) {
        const float y = (row_centers[k] + row_centers[k + 1]) * 0.5f - text_height * 0.5f;
        draw->AddText({origin.x, y}, ImGui::GetColorU32(ImGuiCol_TextDisabled), "OR");
    }
    ImGui::PopID();

    if (to_remove.has_value()) {
        prefixes.erase(prefixes.begin() + static_cast<std::ptrdiff_t>(*to_remove));
        changed = true;
    }
    return changed;
}

// A schedule row's drag handle: a 2x3 grip of dots and the row number, as a
// selectable so it can be grabbed for drag-and-drop.
static void DrawDragHandle(const char* number) {
    const float dot_area = ImGui::GetFontSize() * 0.6f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    char id[32];
    std::snprintf(id, sizeof(id), "##handle%s", number);
    ImGui::Selectable(id, false, 0, ImVec2(dot_area + ImGui::CalcTextSize(number).x + 4.0f, 0.0f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 dot_color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float line_h = ImGui::GetTextLineHeight();
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 2; ++col)
            draw->AddCircleFilled({pos.x + 2.5f + static_cast<float>(col) * 4.0f,
                                   pos.y + line_h * 0.5f + static_cast<float>(row - 1) * 4.0f},
                                  1.3f, dot_color);
    draw->AddText({pos.x + dot_area + 2.0f, pos.y}, ImGui::GetColorU32(ImGuiCol_Text), number);
}

// The office's truck schedules as an editable table: the rule and timing
// cells edit their schedule in place through World::update_schedule, which validates it the
// same way add_schedule does. Rows can be dragged to reorder them (list
// order is pick-up priority) and removed.
static void DrawScheduleTable(World& world, PostOfficeId selected) {
    const std::vector<PostOffice>& offices = world.get_post_offices();
    const PostOffice& office = offices[selected];

    // Why the last edit was refused, shown under the table until the next
    // accepted edit or a change of office.
    static std::string edit_error;
    static PostOfficeId edit_error_owner = kNoPostOffice;
    if (edit_error_owner != selected) {
        edit_error.clear();
        edit_error_owner = selected;
    }

    if (office.outbound_schedules.empty()) {
        ImGui::TextDisabled("No schedules yet. Add one below.");
        return;
    }
    ImGui::TextDisabled("Drag a row's grip to reorder.");
    ImGui::SameLine();
    theme::HelpMarker("Departures run top to bottom: when several schedules depart at the same time, "
                      "the first one whose rule matches a letter picks it up.");

    // Record edits, removals and reorders and apply them after the loop: the
    // World mutates the very vector we are iterating, which would invalidate
    // `schedule` and shift the indices of every later row (and of a pending
    // drag-and-drop payload).
    std::optional<std::pair<std::size_t, TruckSchedule>> schedule_to_update;
    std::optional<std::size_t> schedule_to_remove;
    std::optional<std::pair<std::size_t, std::size_t>> schedule_to_move;  // from, to

    const ImGuiTableFlags flags = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable;
    // Nine columns need more room than a narrow inspector has: the table
    // keeps a minimum width and scrolls sideways inside an auto-height child
    // rather than crushing its stretch columns ("To", "Picks up").
    const float table_w = std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 40.0f);
    ImGui::BeginChild("schedules_scroll", ImVec2(-FLT_MIN, 0.0f), ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (ImGui::BeginTable("schedules", 9, flags, ImVec2(table_w, 0.0f))) {
        // Wide enough for DrawDragHandle's grip plus a two-digit row number.
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 0.6f + ImGui::CalcTextSize("00").x + 6.0f);
        ImGui::TableSetupColumn("To", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Picks up", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Every", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Starting at", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Next", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Waiting", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < office.outbound_schedules.size(); ++i) {
            const TruckSchedule& schedule = office.outbound_schedules[i];
            TruckSchedule edited = schedule;  // this row's pending edit, if any
            bool row_changed = false;

            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            // #: the drag handle. Dropping it on another row reorders the
            // schedule to that position.
            ImGui::TableSetColumnIndex(0);
            char handle[16];
            std::snprintf(handle, sizeof(handle), "%zu", i + 1);
            DrawDragHandle(handle);
            ImGui::SetItemTooltip("Drag to reorder");
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("SCHEDULE_REORDER", &i, sizeof(i));
                ImGui::Text("#%zu  to %s", i + 1, offices[schedule.dst].name.c_str());  // drag preview
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCHEDULE_REORDER")) {
                    const std::size_t from = *static_cast<const std::size_t*>(payload->Data);
                    if (from != i)
                        schedule_to_move = {from, i};
                }
                ImGui::EndDragDropTarget();
            }

            // To: fixed once created (remove and re-add to change it).
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(offices[schedule.dst].name.c_str());

            // Picks up: the rule type, plus the prefix list for PostalPrefix.
            ImGui::TableSetColumnIndex(2);
            const bool is_prefix = schedule.rule.type == RoutingRuleType::PostalPrefix;
            char exact_label[160];
            std::snprintf(exact_label, sizeof(exact_label), "%s mail", offices[schedule.dst].name.c_str());
            const char* rule_labels[] = {exact_label, "Postal prefix", "All mail"};
            const RoutingRuleType rule_types[] = {RoutingRuleType::ExactDestination,
                                                  RoutingRuleType::PostalPrefix,
                                                  RoutingRuleType::AllLetters};
            const int current_rule = static_cast<int>(schedule.rule.type);
            ImGui::SetNextItemWidth(is_prefix ? ImGui::GetContentRegionAvail().x * 0.45f : -FLT_MIN);
            if (ImGui::BeginCombo("##rule", is_prefix ? "Prefix" : rule_labels[current_rule])) {
                for (int r = 0; r < 3; ++r) {
                    if (ImGui::Selectable(rule_labels[r], r == current_rule) && r != current_rule) {
                        edited.rule.type = rule_types[r];
                        // A new prefix rule starts from the destination's own
                        // postal code, the most likely thing to collect.
                        if (edited.rule.type == RoutingRuleType::PostalPrefix)
                            edited.rule.prefixes = {offices[edited.dst].postal_code};
                        row_changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (is_prefix) {
                ImGui::SameLine();
                if (EditPrefixList("##prefixes", edited.rule.prefixes, offices, 0.0f))
                    row_changed = true;
            }

            // Every / Starting at: HH:MM, converted to ticks right away.
            ImGui::TableSetColumnIndex(3);
            if (EditHoursMinutes("every", edited.period, 24)) {
                edited.period = std::clamp(edited.period, kMinSchedulePeriod, kTicksPerDay);
                row_changed = true;
            }
            ImGui::TableSetColumnIndex(4);
            if (EditHoursMinutes("start", edited.start_offset, 23))
                row_changed = true;

            ImGui::TableSetColumnIndex(5);
            char next[32];
            FormatDayTime(schedule.next_departure, next, sizeof(next));
            ImGui::TextUnformatted(next);

            // Waiting: how much of the current buffer this rule would load.
            ImGui::TableSetColumnIndex(6);
            std::size_t matched = 0;
            for (const Letter& letter : office.outbound_letters)
                if (schedule.rule.matches(letter.dst, offices[letter.dst].postal_code, schedule.dst))
                    ++matched;
            ImGui::Text("%zu", matched);
            ImGui::SetItemTooltip("Outbound letters this rule would pick up right now");

            // Load: how full this schedule's trucks have been leaving.
            ImGui::TableSetColumnIndex(7);
            ui::DrawScheduleLoadCell(schedule, world.get_tick());

            ImGui::TableSetColumnIndex(8);
            if (theme::DangerSmallButton(" x "))
                schedule_to_remove = i;
            ImGui::SetItemTooltip("Remove this schedule");

            if (row_changed)
                schedule_to_update = {i, edited};
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    if (schedule_to_update.has_value()) {
        const auto& [index, s] = *schedule_to_update;
        if (world.update_schedule(selected, index, s.period, s.start_offset, s.rule))
            edit_error.clear();
        else if (!s.rule.is_well_formed())
            edit_error = "Every prefix must be non-empty and listed once.";
        else
            edit_error = "Another schedule already goes there with that rule.";
    } else if (schedule_to_remove.has_value()) {
        world.remove_schedule(selected, *schedule_to_remove);
    } else if (schedule_to_move.has_value()) {
        world.move_schedule(selected, schedule_to_move->first, schedule_to_move->second);
    }
    if (!edit_error.empty())
        ImGui::TextColored(kColorErrorText, "Not applied: %s", edit_error.c_str());
}

// The office panel's header: name, postal-code tag and an office switcher
// (map clicking is the other way to change selection).
static void DrawOfficeHeader(const std::vector<PostOffice>& offices, PostOfficeId selected) {
    const PostOffice& office = offices[selected];
    ImGui::PushFont(nullptr, theme::kFontTitle);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(office.name.c_str());
    ImGui::PopFont();

    const float combo_w = ImGui::GetFontSize() * 8.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - combo_w));
    ImGui::SetNextItemWidth(combo_w);
    if (ImGui::BeginCombo("##office", "Switch office", ImGuiComboFlags_HeightLarge)) {
        for (PostOfficeId id = 0; id < offices.size(); ++id) {
            if (ImGui::Selectable(offices[id].name.c_str(), id == selected))
                SelectOffice(id);
        }
        ImGui::EndCombo();
    }

    if (!office.postal_code.empty()) {
        theme::Pill(office.postal_code.c_str(), theme::kPaper2, theme::kInkDim);
        ImGui::SameLine();
    }
    if (office.built)
        theme::Pill("OPEN", theme::kGood, theme::kPaperLight);
    else
        theme::Pill("NOT BUILT", theme::kPaper3, theme::kInkDim);
    ImGui::Spacing();
}

// How full the outbound buffer is: "Outbound ... n / max" over a thin bar
// that turns warn / bad at the same thresholds as the office on the map.
static void DrawCapacityBar(const PostOffice& office) {
    const float fill = office.max_outbound_letters == 0 ? 0.0f
        : std::min(1.0f, static_cast<float>(office.outbound_letters.size())
                             / static_cast<float>(office.max_outbound_letters));
    ImGui::TextDisabled("Outbound letters");
    ImGui::SameLine();
    char count[48];
    std::snprintf(count, sizeof(count), "%zu / %zu", office.outbound_letters.size(), office.max_outbound_letters);
    TextRightAligned(count, fill >= theme::kWarnFraction ? theme::FillColor(fill) : theme::kInk);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme::FillColor(fill));
    ImGui::ProgressBar(fill, ImVec2(-FLT_MIN, ImGui::GetFontSize() * 0.45f), "");
    ImGui::PopStyleColor();
    ImGui::TextDisabled("About %zu new letters posted per day.", office.letters_per_day);
}

// A label cell of the add-route form, aligned with the widget beside it.
static void FormLabel(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
}

// The "New schedule" form under the schedule table.
static void DrawAddScheduleForm(World& world, PostOfficeId selected) {
    const std::vector<PostOffice>& offices = world.get_post_offices();
    const PostOffice& office = offices[selected];

    theme::SectionHeading("New schedule");

    // Add-route widget state. Statics are reset whenever the inspected
    // office changes (same pattern as the postal-code buffer).
    static PostOfficeId add_owner = kNoPostOffice;
    static PostOfficeId add_dst = kNoPostOffice;
    static int add_rule_type = 0;  // 0 = exact dst, 1 = postal prefix, 2 = all letters
    static std::vector<std::string> add_prefixes = {""};
    static ImGuiTextFilter dst_filter;
    if (add_owner != selected) {
        add_owner = selected;
        add_dst = kNoPostOffice;
        add_rule_type = 0;
        add_prefixes = {""};
        dst_filter.Clear();
    }
    // Frequency and offset, in ticks (edited as HH:MM). Defaults spell out
    // the canonical example: every 3 hours starting at 00:30.
    static Tick add_period = hours_minutes_to_ticks(3, 0);
    static Tick add_start = hours_minutes_to_ticks(0, 30);

    const bool dst_ok = add_dst != kNoPostOffice && add_dst < offices.size() && world.has_road(selected, add_dst);
    bool prefix_ok = true;

    if (ImGui::BeginTable("add_schedule_form", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch);

        // Destination: a dropdown that narrows as you type, matching both
        // office names and postal codes.
        FormLabel("To");
        const char* dst_preview = dst_ok ? offices[add_dst].name.c_str() : "Choose a destination...";
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##add_dst", dst_preview, ImGuiComboFlags_HeightLarge)) {
            if (ImGui::IsWindowAppearing()) {
                dst_filter.Clear();
                ImGui::SetKeyboardFocusHere();  // typing filters immediately
            }
            dst_filter.Draw("##add_dst_filter", -FLT_MIN);
            bool any = false;
            for (PostOfficeId id = 0; id < offices.size(); ++id) {
                if (id == selected || !world.has_road(selected, id))
                    continue;
                const PostOffice& other = offices[id];
                if (!dst_filter.PassFilter(other.name.c_str()) &&
                    !dst_filter.PassFilter(other.postal_code.c_str()))
                    continue;
                any = true;
                ImGui::PushID(static_cast<int>(id));
                if (ImGui::Selectable(other.name.c_str(), id == add_dst))
                    add_dst = id;
                if (id == add_dst)
                    ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                ImGui::TextDisabled("%s", other.postal_code.c_str());
                ImGui::PopID();
            }
            if (!any)
                ImGui::TextDisabled("No office reachable by road matches.");
            ImGui::EndCombo();
        }

        // Routing rule: which of this office's letters each departure takes.
        FormLabel("Picks up");
        const char* rule_names[] = {"Destination", "Postal prefixes", "All mail"};
        for (int r = 0; r < 3; ++r) {
            if (r > 0)
                ImGui::SameLine(0.0f, 2.0f);
            if (theme::SegmentedButton(rule_names[r], add_rule_type == r))
                add_rule_type = r;
        }
        char rule_help[160];
        if (add_rule_type == 0)
            std::snprintf(rule_help, sizeof(rule_help), "Only letters addressed to %s.",
                          dst_ok ? offices[add_dst].name.c_str() : "the destination");
        else if (add_rule_type == 1)
            std::snprintf(rule_help, sizeof(rule_help), "Letters addressed to any of these postal-code prefixes.");
        else
            std::snprintf(rule_help, sizeof(rule_help), "Every letter; the rest forward onward from the destination.");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", rule_help);
        ImGui::PopTextWrapPos();
        if (add_rule_type == 1) {
            EditPrefixList("##add_prefixes", add_prefixes, offices, std::min(300.0f, ImGui::GetContentRegionAvail().x));
            prefix_ok = RoutingRule{RoutingRuleType::PostalPrefix, add_prefixes}.is_well_formed();
            if (!prefix_ok)
                ImGui::TextColored(kColorErrorText, "Every prefix must be non-empty and listed once.");
        }

        FormLabel("Every");
        if (EditHoursMinutes("add_every", add_period, 24))
            add_period = std::clamp(add_period, kMinSchedulePeriod, kTicksPerDay);
        ImGui::SameLine();
        ImGui::TextDisabled("hh:mm");

        FormLabel("Starting at");
        EditHoursMinutes("add_start", add_start, 23);
        ImGui::SameLine();
        ImGui::TextDisabled("hh:mm");

        ImGui::EndTable();
    }

    // The fully configured rule, used for the live match count, the
    // duplicate check and the World call.
    RoutingRule rule;
    if (add_rule_type == 1) {
        rule.type = RoutingRuleType::PostalPrefix;
        rule.prefixes = add_prefixes;
    } else if (add_rule_type == 2) {
        rule.type = RoutingRuleType::AllLetters;
    }

    // Live feedback: how much of the current buffer this rule would load.
    // Creating a schedule is free; each truck is paid for as it departs.
    ImGui::Spacing();
    if (dst_ok && prefix_ok) {
        std::size_t matched = 0;
        for (const Letter& letter : office.outbound_letters)
            if (rule.matches(letter.dst, offices[letter.dst].postal_code, add_dst))
                ++matched;
        ImGui::TextDisabled("Matches %zu of %zu waiting letters right now.", matched, office.outbound_letters.size());
    }
    ImGui::TextDisabled("Each truck costs $%d when it departs and carries up to %zu letters.",
                        kTruckDepartureCost, kTruckCapacity);

    const bool duplicate = dst_ok && std::any_of(
        office.outbound_schedules.begin(), office.outbound_schedules.end(),
        [&](const TruckSchedule& s) { return s.dst == add_dst && s.rule == rule; });

    ImGui::BeginDisabled(!dst_ok || !prefix_ok || duplicate);
    if (theme::PrimaryButton("Add schedule"))
        world.add_schedule(selected, add_dst, add_period, add_start, rule);
    ImGui::EndDisabled();
    if (dst_ok && duplicate) {
        ImGui::SameLine();
        ImGui::TextColored(kColorErrorText, "An identical schedule already exists.");
    }
}

// A post office: header, outbound letters, statistics and schedule editing.
static void DrawOfficePanel(World& world, PostOfficeId selected) {
    const std::vector<PostOffice>& offices = world.get_post_offices();
    const PostOffice& office = offices[selected];

    DrawOfficeHeader(offices, selected);

    // An unbuilt office has no letters or schedules yet: offer to build it.
    if (!office.built) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::Text("Once built, this office takes in about %zu letters per day and holds up to %zu "
                    "outbound letters.", office.letters_per_day, office.max_outbound_letters);
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        const bool affordable = world.get_money() >= office.build_cost;
        ImGui::BeginDisabled(!affordable);
        char build_label[64];
        std::snprintf(build_label, sizeof(build_label), "Build post office  -  $%d", office.build_cost);
        if (theme::PrimaryButton(build_label, ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 1.3f)))
            world.build_office(selected);
        ImGui::EndDisabled();
        if (!affordable)
            ImGui::TextColored(kColorErrorText, "You only have $%d.", world.get_money());
    } else {
        DrawCapacityBar(office);
        ImGui::Spacing();

        if (ImGui::CollapsingHeader("Outbound letters", ImGuiTreeNodeFlags_DefaultOpen))
            DrawLetterTable("outbound_letters", office.outbound_letters, offices, world.get_tick());

        if (ImGui::CollapsingHeader("Statistics"))
            ui::DrawOfficeStats(world, selected);

        if (ImGui::CollapsingHeader("Truck schedules", ImGuiTreeNodeFlags_DefaultOpen)) {
            DrawScheduleTable(world, selected);
            DrawAddScheduleForm(world, selected);
        }
    }

    // Postal code, editable. InputText needs a mutable buffer, so keep one
    // static buffer that is reloaded whenever the selection changes, and
    // commit every keystroke to the World — the map label updates live.
    if (ImGui::CollapsingHeader("Settings")) {
        static char postal_buf[128];
        static PostOfficeId postal_buf_owner = kNoPostOffice;
        if (postal_buf_owner != selected) {
            std::snprintf(postal_buf, sizeof(postal_buf), "%s", office.postal_code.c_str());
            postal_buf_owner = selected;
        }
        if (ImGui::InputText("Postal code", postal_buf, sizeof(postal_buf)))
            world.set_postal_code(selected, postal_buf);
    }
}

// Road-building controls, shown under either panel.
static void DrawRoadBuildSection(const World& world) {
    const std::vector<PostOffice>& offices = world.get_post_offices();

    theme::SectionHeading("Roads");

    if (g_road_build_state == RoadBuildState::Idle) {
        if (ImGui::Button("Build a road...")) {
            g_road_build_state = RoadBuildState::SelectSource;
            g_road_builder = RoadBuilder{};
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Click a road on the map to remove it.");
        return;
    }

    // Road-building mode: a warn-tinted banner walking through the steps.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Rgb(214, 158, 46, 45));
    ImGui::BeginChild("road_banner", ImVec2(-FLT_MIN, 0.0f),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(nullptr, theme::kFontSmall);
    ImGui::TextColored(theme::kWarnText, "ROAD-BUILDING MODE");
    ImGui::PopFont();

    if (g_road_build_state == RoadBuildState::SelectSource) {
        ImGui::TextUnformatted("1. Click the office the road starts from.");
    } else {
        ImGui::Text("2. From %s: click to place bends, then click the office it ends at.",
                    g_road_builder.src != kNoPostOffice ? offices[g_road_builder.src].name.c_str() : "?");
        const std::size_t bends = g_road_builder.vertices.empty() ? 0
                                : g_road_builder.vertices.size() - 1;  // exclude source office vertex
        ImGui::TextDisabled("%zu bends placed, %.0f units so far.", bends,
                            polyline_length(g_road_builder.vertices));
    }

    if (ImGui::Button("Cancel  (Esc)")) {
        g_road_build_state = RoadBuildState::Idle;
        g_road_builder = RoadBuilder{};
    }
    ImGui::SameLine();
    ImGui::TextDisabled("or right-click the map");
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void DrawInspector(World& world) {
    if (world.get_post_offices().empty())
        return;
    EnsureOfficeSelected(world);

    // A live truck selection wins; once it has expired (the truck arrived)
    // the backup office shows instead. Local copies keep the panel drawing
    // what it started with even if a click inside it changes the selection.
    const std::shared_ptr<const Truck> truck = g_selected_truck.lock();
    const PostOfficeId office = g_selected_office;

    // Pinned into the left dock node: re-docked every frame, and its node
    // can't be dragged out, docked over, or undocked via the window menu.
    // The lone tab is hidden too: the panel draws its own header.
    ImGuiWindowClass inspector_class;
    inspector_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoUndocking |
                                               (int)ImGuiDockNodeFlags_NoWindowMenuButton |
                                               (int)ImGuiDockNodeFlags_NoDockingOverMe |
                                               (int)ImGuiDockNodeFlags_AutoHideTabBar;
    ImGui::SetNextWindowClass(&inspector_class);
    ImGui::SetNextWindowDockID(g_inspector_dock_id, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    ImGui::Begin(truck != nullptr ? "Truck###Inspector" : "Post office###Inspector");
    ImGui::PopStyleVar();
    if (truck != nullptr)
        DrawTruckPanel(world, *truck);
    else
        DrawOfficePanel(world, office);
    DrawRoadBuildSection(world);
    ImGui::End();
}
