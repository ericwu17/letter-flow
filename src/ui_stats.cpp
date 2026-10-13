// ui_stats.cpp — Dear ImGui / ImPlot presentation of the simulation's
// statistics (stats.h): the Factorio-style statistics window, the inspector's
// per-office and per-schedule stat widgets, and letter-trail tooltips.
// Read-only towards the World, apart from moving the selection.
#include "ui.h"
#include "ui_internal.h"

#include "balance.h"
#include "entities.h"
#include "game_types.h"
#include "stats.h"
#include "world.h"
#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Time ranges
// ---------------------------------------------------------------------------

// The range buttons. Each shows the newest `buckets` buckets of one stat
// level (stats.h), with x-axis ticks every `tick_step`.
struct StatRange {
    const char* label;
    std::size_t level;
    std::size_t buckets;
    Tick tick_step;
    bool day_labels;  // label ticks "Day N" rather than with the time of day
};

constexpr StatRange kStatRanges[] = {
    {"6 h", 0, 30, kTicksPerHour, false},
    {"1 day", 0, kStatBuckets, 3 * kTicksPerHour, false},
    {"10 days", 1, kStatBuckets, kTicksPerDay, true},
    {"30 days", 2, kStatBuckets, 5 * kTicksPerDay, true},
};
constexpr int kStatRangeCount = static_cast<int>(sizeof(kStatRanges) / sizeof(kStatRanges[0]));

int g_range = 1;  // index into kStatRanges, shared by every stats view
bool g_show_stats = false;
PostOfficeId g_stats_office = kNoPostOffice;  // picked in the Offices tab
ScheduleId g_stats_schedule = kNoSchedule;    // picked in the Schedules tab

const StatRange& CurrentRange() { return kStatRanges[g_range]; }
Tick RangeTicks(const StatRange& r) { return kStatBucketTicks[r.level] * r.buckets; }
double TicksToHours(Tick ticks) { return static_cast<double>(ticks) / static_cast<double>(kTicksPerHour); }

// A series' total over the current range, the bucket in progress included.
int64_t RangeSum(const StatSeries& series, Tick now) {
    const StatRange& r = CurrentRange();
    return series.sum(r.level, now, r.buckets);
}

// A gauge's peak over the current range.
int64_t RangePeak(const StatSeries& series, Tick now) {
    const StatRange& r = CurrentRange();
    std::vector<int64_t> values(r.buckets);
    series.read(r.level, now, r.buckets, values.data());
    return *std::max_element(values.begin(), values.end());
}

void DrawRangeButtons() {
    for (int i = 0; i < kStatRangeCount; ++i) {
        if (i > 0)
            ImGui::SameLine();
        if (ImGui::RadioButton(kStatRanges[i].label, g_range == i))
            g_range = i;
    }
}

// ---------------------------------------------------------------------------
// Value formatting (literal format strings only: -Wformat=2)
// ---------------------------------------------------------------------------

enum class Unit { Count, PerHour, Money, MoneyPerHour, Hours, Percent };

void FormatValue(Unit unit, double v, char* buf, std::size_t len) {
    switch (unit) {
    case Unit::Count:        std::snprintf(buf, len, "%.0f", v); return;
    case Unit::PerHour:      std::snprintf(buf, len, "%.1f /h", v); return;
    case Unit::Money:        std::snprintf(buf, len, "$%.0f", v); return;
    case Unit::MoneyPerHour: std::snprintf(buf, len, "$%.0f /h", v); return;
    case Unit::Hours:        std::snprintf(buf, len, "%.1f h", v); return;
    case Unit::Percent:      std::snprintf(buf, len, "%.0f%%", v); return;
    }
}

// "H:MM" for a duration given in fractional hours.
void FormatHours(double hours, char* buf, std::size_t len) {
    ui::FormatDuration(static_cast<Tick>(std::llround(std::max(hours, 0.0) * static_cast<double>(kTicksPerHour))),
                       buf, len);
}

// ---------------------------------------------------------------------------
// Time charts
// ---------------------------------------------------------------------------

// The buckets one series shows in a chart of the current range.
struct ChartWindow {
    std::size_t level = 0;
    std::size_t count = 0;
    Tick end = 0;     // a tick inside the newest bucket shown
    Tick bucket = 0;  // bucket length in ticks
};

// Flows stop at the last completed bucket, so the bucket still filling up
// never reads as a sudden drop; gauges include it (their value is a peak).
ChartWindow WindowFor(StatKind kind, Tick now) {
    const StatRange& r = CurrentRange();
    const Tick bucket = kStatBucketTicks[r.level];
    const Tick end = kind == StatKind::Flow && now >= bucket ? now - bucket : now;
    return {r.level, r.buckets, end, bucket};
}

// Each bucket is plotted at the middle of its time span, in hours since the
// start of the game (the x unit of every time chart).
std::vector<double> BucketXs(const ChartWindow& w) {
    std::vector<double> xs(w.count);
    const double newest = static_cast<double>(w.end / w.bucket);
    for (std::size_t i = 0; i < w.count; ++i) {
        const double bucket = newest - static_cast<double>(w.count - 1 - i);
        xs[i] = (bucket + 0.5) * TicksToHours(w.bucket);
    }
    return xs;
}

std::vector<double> BucketValues(const StatSeries& series, const ChartWindow& w) {
    std::vector<int64_t> raw(w.count);
    series.read(w.level, w.end, w.count, raw.data());
    return std::vector<double>(raw.begin(), raw.end());
}

// Everything plotted in the current chart, for the hover readout.
struct ReadoutSeries {
    std::string label;
    Unit unit;
    std::vector<double> xs, ys;
};
std::vector<ReadoutSeries> g_readout;

void PlotAndRecord(const char* label, Unit unit, std::vector<double> xs, std::vector<double> ys,
                   const ImPlotSpec& spec) {
    ImPlot::PlotLine(label, xs.data(), ys.data(), static_cast<int>(xs.size()), spec);
    g_readout.push_back({label, unit, std::move(xs), std::move(ys)});
}

ImPlotSpec LineSpec(std::optional<ImVec4> color = std::nullopt) {
    ImPlotSpec spec;
    spec.LineWeight = 2.0f;
    spec.Flags = ImPlotLineFlags_SkipNaN;
    if (color.has_value())
        spec.LineColor = *color;
    return spec;
}

// Flows, summed, as a rate per in-game hour: the rate reads the same
// whichever range (and so bucket length) is shown.
void PlotRate(const char* label, std::initializer_list<const StatSeries*> series, Tick now,
              Unit unit = Unit::PerHour, std::optional<ImVec4> color = std::nullopt) {
    const ChartWindow w = WindowFor(StatKind::Flow, now);
    std::vector<double> ys(w.count, 0.0);
    for (const StatSeries* s : series) {
        const std::vector<double> values = BucketValues(*s, w);
        for (std::size_t i = 0; i < w.count; ++i)
            ys[i] += values[i];
    }
    const double per_hour = 1.0 / TicksToHours(w.bucket);
    for (double& y : ys)
        y *= per_hour;
    PlotAndRecord(label, unit, BucketXs(w), std::move(ys), LineSpec(color));
}

// A gauge's per-bucket peak, times `scale`.
void PlotLevel(const char* label, const StatSeries& series, Tick now, Unit unit,
               double scale = 1.0, std::optional<ImVec4> color = std::nullopt) {
    const ChartWindow w = WindowFor(StatKind::Gauge, now);
    std::vector<double> ys = BucketValues(series, w);
    for (double& y : ys)
        y *= scale;
    PlotAndRecord(label, unit, BucketXs(w), std::move(ys), LineSpec(color));
}

// Per bucket, sum(numerator) / sum(denominators) * scale: e.g. total delivery
// time over letters delivered. Buckets with a zero denominator are gaps.
void PlotAverage(const char* label, const StatSeries& numerator,
                 std::initializer_list<const StatSeries*> denominators, Tick now, Unit unit,
                 double scale, std::optional<ImVec4> color = std::nullopt) {
    const ChartWindow w = WindowFor(StatKind::Flow, now);
    std::vector<double> ys = BucketValues(numerator, w);
    std::vector<double> counts(w.count, 0.0);
    for (const StatSeries* s : denominators) {
        const std::vector<double> values = BucketValues(*s, w);
        for (std::size_t i = 0; i < w.count; ++i)
            counts[i] += values[i];
    }
    for (std::size_t i = 0; i < w.count; ++i)
        ys[i] = counts[i] > 0.0 ? ys[i] / counts[i] * scale : std::numeric_limits<double>::quiet_NaN();
    PlotAndRecord(label, unit, BucketXs(w), std::move(ys), LineSpec(color));
}

// X-axis tick labels: hours since the start of the game, shown as a time of
// day ("06:00"), or "Day N" at midnight and on the multi-day ranges.
int FormatAxisTime(double hours, char* buf, int size, void* /*user_data*/) {
    const StatRange* range = &CurrentRange();
    const Tick tick = static_cast<Tick>(std::llround(std::max(hours, 0.0) * static_cast<double>(kTicksPerHour)));
    const ClockTime t = tick_to_time_of_day(tick);
    const auto day = static_cast<unsigned long long>(tick / kTicksPerDay) + 1;
    const auto len = static_cast<std::size_t>(size);
    if (range->day_labels || (t.hour == 0 && t.minute == 0))
        return std::snprintf(buf, len, "Day %llu", day);
    return std::snprintf(buf, len, "%02d:%02d", t.hour, t.minute);
}

// Opens a chart over the current range, scrolling with the game clock (the x
// axis always ends at `now`). Plot series with PlotRate / PlotLevel /
// PlotAverage, then close with EndTimeChart, which adds the hover readout.
bool BeginTimeChart(const char* title, Tick now, const char* y_label, float height) {
    g_readout.clear();
    const float chart_width = ImGui::GetContentRegionAvail().x;
    if (!ImPlot::BeginPlot(title, ImVec2(-1.0f, height), ImPlotFlags_NoMouseText | ImPlotFlags_NoBoxSelect))
        return false;
    const StatRange& r = CurrentRange();
    const Tick span = RangeTicks(r);
    const Tick start = now > span ? now - span : 0;
    const double x_min = TicksToHours(start);
    const double x_max = std::max(TicksToHours(now), x_min + 0.5);

    ImPlot::SetupAxes(nullptr, y_label, ImPlotAxisFlags_None, ImPlotAxisFlags_AutoFit);
    ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max, ImPlotCond_Always);
    ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0.0, INFINITY);
    ImPlot::SetupAxisFormat(ImAxis_X1, FormatAxisTime);
    // Ticks on round times: every tick_step since the start of the game,
    // thinned to every 2nd, 3rd... step until the labels fit the chart.
    const float label_width = ImGui::CalcTextSize("Day 00  ").x;
    const auto max_labels = static_cast<Tick>(std::max(2.0f, chart_width / label_width * 0.8f));
    Tick step = r.tick_step;
    while (span / step > max_labels)
        step += r.tick_step;
    std::vector<double> ticks;
    for (Tick t = (start + step - 1) / step * step; t <= now; t += step)
        ticks.push_back(TicksToHours(t));
    if (!ticks.empty())
        ImPlot::SetupAxisTicks(ImAxis_X1, ticks.data(), static_cast<int>(ticks.size()));
    ImPlot::SetupLegend(ImPlotLocation_NorthWest);
    return true;
}

// Vertical markers at the given ticks (e.g. schedule edits). Call between
// BeginTimeChart and EndTimeChart.
void PlotTickMarkers(const char* label, const std::vector<Tick>& ticks, ImVec4 color) {
    std::vector<double> xs;
    for (Tick t : ticks)
        xs.push_back(TicksToHours(t));
    ImPlotSpec spec;
    spec.LineColor = color;
    spec.LineWeight = 1.5f;
    ImPlot::PlotInfLines(label, xs.data(), static_cast<int>(xs.size()), spec);
}

// Hovering a chart shows every series' value in the bucket under the mouse.
void EndTimeChart() {
    if (ImPlot::IsPlotHovered() && !g_readout.empty() && !g_readout.front().xs.empty()) {
        const double mouse_x = ImPlot::GetPlotMousePos().x;
        const std::vector<double>& xs = g_readout.front().xs;
        std::size_t nearest = 0;
        for (std::size_t i = 1; i < xs.size(); ++i)
            if (std::abs(xs[i] - mouse_x) < std::abs(xs[nearest] - mouse_x))
                nearest = i;
        ImGui::BeginTooltip();
        char when[32];
        ui::FormatDayTime(static_cast<Tick>(std::max(xs[nearest], 0.0) * static_cast<double>(kTicksPerHour)),
                          when, sizeof(when));
        ImGui::TextDisabled("Around %s", when);
        for (const ReadoutSeries& s : g_readout) {
            if (nearest >= s.ys.size())
                continue;
            char value[48];
            if (std::isnan(s.ys[nearest]))
                std::snprintf(value, sizeof(value), "-");
            else
                FormatValue(s.unit, s.ys[nearest], value, sizeof(value));
            ImGui::Text("%s: %s", s.label.c_str(), value);
        }
        ImGui::EndTooltip();
    }
    ImPlot::EndPlot();
}

// A small, axis-less line of `values` for table cells.
void Sparkline(const char* id, const std::vector<double>& values, double y_max, ImVec2 size, ImVec4 color) {
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(0.0f, 0.0f));
    if (ImPlot::BeginPlot(id, size, ImPlotFlags_CanvasOnly | ImPlotFlags_NoInputs | ImPlotFlags_NoFrame)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_NoDecorations);
        // 15% headroom, so a line pinned at y_max (a full truck, a full
        // buffer) stays visible instead of merging into the top edge.
        ImPlot::SetupAxesLimits(0.0, std::max(1.0, static_cast<double>(values.size()) - 1.0), 0.0,
                                std::max(y_max, 1.0) * 1.15, ImPlotCond_Always);
        ImPlotSpec spec;
        spec.LineColor = color;
        spec.FillColor = color;
        spec.FillAlpha = 0.25f;
        ImPlot::PlotShaded(id, values.data(), static_cast<int>(values.size()), 0.0, 1.0, 0.0, spec);
        ImPlot::PlotLine(id, values.data(), static_cast<int>(values.size()), 1.0, 0.0, spec);
        ImPlot::EndPlot();
    }
    ImPlot::PopStyleVar();
}

// Semantic series colors (the rest come from ImPlot's default colormap).
constexpr ImVec4 kGood{0.36f, 0.82f, 0.45f, 1.0f};      // income, on-time deliveries
constexpr ImVec4 kBad{0.94f, 0.36f, 0.33f, 1.0f};       // losses, late deliveries
constexpr ImVec4 kWarn{0.93f, 0.76f, 0.27f, 1.0f};      // bonuses, left behind
constexpr ImVec4 kNeutral{0.40f, 0.62f, 0.95f, 1.0f};   // counts, levels
constexpr ImVec4 kSpend{0.94f, 0.59f, 0.24f, 1.0f};     // money spent
constexpr ImVec4 kMuted{0.65f, 0.67f, 0.75f, 1.0f};     // secondary lines, markers

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

// A schedule's position (1-based, as the inspector's # column shows it) in
// its office's list, or 0 if no schedule there has that id any more.
std::size_t ScheduleNumber(const std::vector<PostOffice>& offices, PostOfficeId office, ScheduleId id) {
    if (office >= offices.size())
        return 0;
    const std::vector<TruckSchedule>& schedules = offices[office].outbound_schedules;
    for (std::size_t i = 0; i < schedules.size(); ++i)
        if (schedules[i].id == id)
            return i + 1;
    return 0;
}

// "Princeton mail" / "Prefix NJ/Mercer, PA" / "All mail".
std::string RuleSummary(const RoutingRule& rule, const std::vector<PostOffice>& offices, PostOfficeId dst) {
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

// Sorts table rows by the active column of a sortable ImGui table; `key`
// maps (row, column index) to a comparable number.
template <typename Row, typename Key>
void SortRows(std::vector<Row>& rows, Key key) {
    const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
    if (specs == nullptr || specs->SpecsCount == 0)
        return;
    const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
    const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
    std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        const double ka = key(a, spec.ColumnIndex);
        const double kb = key(b, spec.ColumnIndex);
        return ascending ? ka < kb : ka > kb;
    });
}

// ---------------------------------------------------------------------------
// Global tab
// ---------------------------------------------------------------------------

void DrawGlobalTab(const World& world) {
    const GlobalStats& g = world.get_stats();
    const Tick now = world.get_tick();
    auto total = [&](GlobalStat m) { return RangeSum(g[m], now); };

    const int64_t income_value = total(GlobalStat::IncomeValue);
    const int64_t income_bonus = total(GlobalStat::IncomeBonus);
    const int64_t spent_trucks = total(GlobalStat::SpentTrucks);
    const int64_t spent_build = total(GlobalStat::SpentRoads) + total(GlobalStat::SpentOffices);
    const int64_t net = income_value + income_bonus - spent_trucks - spent_build;
    const int64_t on_time = total(GlobalStat::DeliveredOnTime);
    const int64_t late = total(GlobalStat::DeliveredLate);
    const int64_t delivered = on_time + late;
    const int64_t lost = total(GlobalStat::LettersLost);
    const int64_t skipped = total(GlobalStat::DeparturesSkipped);

    // Summary for the range.
    if (ImGui::BeginTable("summary", 4, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Income");
        ImGui::Text("$%lld", static_cast<long long>(income_value + income_bonus));
        ImGui::TextDisabled("incl. $%lld early bonus", static_cast<long long>(income_bonus));

        ImGui::TableNextColumn();
        ImGui::TextDisabled("Spent");
        ImGui::Text("$%lld", static_cast<long long>(spent_trucks + spent_build));
        ImGui::TextDisabled("$%lld trucks, $%lld building", static_cast<long long>(spent_trucks),
                            static_cast<long long>(spent_build));

        ImGui::TableNextColumn();
        ImGui::TextDisabled("Net");
        ImGui::TextColored(net >= 0 ? kGood : kBad, "%s$%lld", net < 0 ? "-" : "+",
                           static_cast<long long>(std::llabs(net)));

        ImGui::TableNextColumn();
        ImGui::TextDisabled("Delivered");
        if (delivered > 0)
            ImGui::Text("%lld  (%.0f%% on time)", static_cast<long long>(delivered),
                        100.0 * static_cast<double>(on_time) / static_cast<double>(delivered));
        else
            ImGui::Text("0");
        if (lost > 0)
            ImGui::TextColored(kBad, "%lld lost (buffers full)", static_cast<long long>(lost));
        else
            ImGui::TextDisabled("none lost");
        ImGui::EndTable();
    }
    if (delivered > 0) {
        const double avg_total = TicksToHours(static_cast<Tick>(total(GlobalStat::DeliveryTicks))) / static_cast<double>(delivered);
        const double avg_transit = TicksToHours(static_cast<Tick>(total(GlobalStat::TransitTicks))) / static_cast<double>(delivered);
        char total_text[16], transit_text[16];
        FormatHours(avg_total, total_text, sizeof(total_text));
        FormatHours(avg_transit, transit_text, sizeof(transit_text));
        ImGui::Text("Average delivery takes %s, of which %s on trucks and %.0f%% waiting in offices.",
                    total_text, transit_text, avg_total > 0.0 ? 100.0 * (1.0 - avg_transit / avg_total) : 0.0);
    }
    if (skipped > 0)
        ImGui::TextColored(kBad, "%lld departures skipped: not enough money to send the truck.",
                           static_cast<long long>(skipped));

    // Charts, two per row.
    const float chart_h = 190.0f;
    if (!ImGui::BeginTable("charts", 2, ImGuiTableFlags_SizingStretchSame))
        return;
    ImGui::TableNextColumn();
    if (BeginTimeChart("Money", now, "$ per hour", chart_h)) {
        PlotRate("Letter values", {&g[GlobalStat::IncomeValue]}, now, Unit::MoneyPerHour, kGood);
        PlotRate("Early bonuses", {&g[GlobalStat::IncomeBonus]}, now, Unit::MoneyPerHour, kWarn);
        PlotRate("Trucks", {&g[GlobalStat::SpentTrucks]}, now, Unit::MoneyPerHour, kSpend);
        PlotRate("Building", {&g[GlobalStat::SpentRoads], &g[GlobalStat::SpentOffices]}, now,
                 Unit::MoneyPerHour, kMuted);
        EndTimeChart();
    }
    ImGui::TableNextColumn();
    if (BeginTimeChart("Letters", now, "per hour", chart_h)) {
        PlotRate("Posted", {&g[GlobalStat::LettersSpawned]}, now, Unit::PerHour, kNeutral);
        PlotRate("On time", {&g[GlobalStat::DeliveredOnTime]}, now, Unit::PerHour, kGood);
        PlotRate("Late", {&g[GlobalStat::DeliveredLate]}, now, Unit::PerHour, kWarn);
        PlotRate("Lost", {&g[GlobalStat::LettersLost]}, now, Unit::PerHour, kBad);
        EndTimeChart();
    }
    ImGui::TableNextColumn();
    if (BeginTimeChart("Letters in the network", now, "letters", chart_h)) {
        PlotLevel("Waiting in offices", g[GlobalStat::LettersWaiting], now, Unit::Count, 1.0, kWarn);
        PlotLevel("On trucks", g[GlobalStat::LettersInTransit], now, Unit::Count, 1.0, kNeutral);
        EndTimeChart();
    }
    ImGui::TableNextColumn();
    if (BeginTimeChart("Delivery time", now, "hours", chart_h)) {
        const double ticks_to_hours = 1.0 / static_cast<double>(kTicksPerHour);
        PlotAverage("Total", g[GlobalStat::DeliveryTicks],
                    {&g[GlobalStat::DeliveredOnTime], &g[GlobalStat::DeliveredLate]}, now, Unit::Hours,
                    ticks_to_hours, kWarn);
        PlotAverage("On trucks", g[GlobalStat::TransitTicks],
                    {&g[GlobalStat::DeliveredOnTime], &g[GlobalStat::DeliveredLate]}, now, Unit::Hours,
                    ticks_to_hours, kNeutral);
        EndTimeChart();
    }
    ImGui::TableNextColumn();
    if (BeginTimeChart("Balance", now, "$", chart_h)) {
        PlotLevel("Money", g[GlobalStat::Money], now, Unit::Money, 1.0, kGood);
        EndTimeChart();
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------------------
// Offices
// ---------------------------------------------------------------------------

// Charts for one office: its mail flows, its backlog against capacity, and
// how long letters wait there. Stacked, or side by side in three columns.
void DrawOfficeCharts(const World& world, PostOfficeId id, float height, bool side_by_side) {
    const PostOffice& office = world.get_post_offices()[id];
    const OfficeStats& s = office.stats;
    const Tick now = world.get_tick();
    if (side_by_side && !ImGui::BeginTable("office_charts", 3, ImGuiTableFlags_SizingStretchSame))
        return;
    auto next_cell = [&] {
        if (side_by_side)
            ImGui::TableNextColumn();
    };
    next_cell();
    if (BeginTimeChart("Mail", now, "per hour", height)) {
        PlotRate("Posted", {&s[OfficeStat::Spawned]}, now, Unit::PerHour, kNeutral);
        PlotRate("Delivered here", {&s[OfficeStat::Delivered]}, now, Unit::PerHour, kGood);
        PlotRate("Forwarded", {&s[OfficeStat::Forwarded]}, now, Unit::PerHour, kMuted);
        PlotRate("Lost", {&s[OfficeStat::Lost]}, now, Unit::PerHour, kBad);
        EndTimeChart();
    }
    next_cell();
    if (BeginTimeChart("Backlog", now, "letters", height)) {
        PlotLevel("Waiting", s[OfficeStat::Backlog], now, Unit::Count, 1.0, kWarn);
        const double capacity = static_cast<double>(office.max_outbound_letters);
        ImPlotSpec cap_spec;
        cap_spec.LineColor = kBad;
        cap_spec.Flags = ImPlotInfLinesFlags_Horizontal;
        ImPlot::PlotInfLines("Capacity", &capacity, 1, cap_spec);
        EndTimeChart();
    }
    next_cell();
    if (BeginTimeChart("Waiting time", now, "hours", height)) {
        PlotAverage("Average wait", s[OfficeStat::WaitTicks], {&s[OfficeStat::LettersDeparted]},
                    now, Unit::Hours, 1.0 / static_cast<double>(kTicksPerHour), kNeutral);
        PlotLevel("Oldest letter", s[OfficeStat::OldestWaiting], now, Unit::Hours,
                  1.0 / static_cast<double>(kTicksPerHour), kWarn);
        EndTimeChart();
    }
    if (side_by_side)
        ImGui::EndTable();
}

void DrawOfficesTab(const World& world) {
    const std::vector<PostOffice>& offices = world.get_post_offices();
    const Tick now = world.get_tick();

    struct Row {
        PostOfficeId id;
        int64_t spawned, lost, delivered, forwarded, peak_backlog;
        double avg_wait_hours;  // NaN if nothing departed
        double fill;            // peak backlog / capacity
    };
    std::vector<Row> rows;
    for (PostOfficeId id = 0; id < offices.size(); ++id) {
        const PostOffice& o = offices[id];
        if (!o.built)
            continue;
        const int64_t departed = RangeSum(o.stats[OfficeStat::LettersDeparted], now);
        const int64_t peak = RangePeak(o.stats[OfficeStat::Backlog], now);
        rows.push_back({id, RangeSum(o.stats[OfficeStat::Spawned], now), RangeSum(o.stats[OfficeStat::Lost], now),
                        RangeSum(o.stats[OfficeStat::Delivered], now), RangeSum(o.stats[OfficeStat::Forwarded], now),
                        peak,
                        departed > 0 ? TicksToHours(static_cast<Tick>(RangeSum(o.stats[OfficeStat::WaitTicks], now)))
                                           / static_cast<double>(departed)
                                     : std::numeric_limits<double>::quiet_NaN(),
                        o.max_outbound_letters > 0 ? static_cast<double>(peak) / static_cast<double>(o.max_outbound_letters) : 0.0});
    }
    if (rows.empty()) {
        ImGui::TextDisabled("No post offices built yet.");
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    const float table_h = std::min(260.0f, ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(rows.size() + 2) + 8.0f);
    if (ImGui::BeginTable("offices", 8, flags, ImVec2(0.0f, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Office", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn("Posted");
        ImGui::TableSetupColumn("Lost");
        ImGui::TableSetupColumn("Delivered");
        ImGui::TableSetupColumn("Forwarded");
        ImGui::TableSetupColumn("Avg wait");
        ImGui::TableSetupColumn("Peak backlog");
        ImGui::TableSetupColumn("Backlog", ImGuiTableColumnFlags_NoSort, 120.0f);
        ImGui::TableHeadersRow();
        SortRows(rows, [&](const Row& r, int column) -> double {
            switch (column) {
            case 0: return static_cast<double>(r.id);
            case 1: return static_cast<double>(r.spawned);
            case 2: return static_cast<double>(r.lost);
            case 3: return static_cast<double>(r.delivered);
            case 4: return static_cast<double>(r.forwarded);
            case 5: return std::isnan(r.avg_wait_hours) ? -1.0 : r.avg_wait_hours;
            case 6: return r.fill;
            default: return 0.0;
            }
        });

        for (const Row& r : rows) {
            const PostOffice& o = offices[r.id];
            ImGui::PushID(static_cast<int>(r.id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(o.name.c_str(), g_stats_office == r.id, ImGuiSelectableFlags_SpanAllColumns)) {
                g_stats_office = r.id;
                ui::SelectOffice(r.id);
            }
            ImGui::TableNextColumn();
            ImGui::Text("%lld", static_cast<long long>(r.spawned));
            ImGui::TableNextColumn();
            if (r.lost > 0)
                ImGui::TextColored(kBad, "%lld", static_cast<long long>(r.lost));
            else
                ImGui::TextDisabled("0");
            ImGui::TableNextColumn();
            ImGui::Text("%lld", static_cast<long long>(r.delivered));
            ImGui::TableNextColumn();
            ImGui::Text("%lld", static_cast<long long>(r.forwarded));
            ImGui::TableNextColumn();
            if (std::isnan(r.avg_wait_hours)) {
                ImGui::TextDisabled("-");
            } else {
                char wait[16];
                FormatHours(r.avg_wait_hours, wait, sizeof(wait));
                ImGui::TextUnformatted(wait);
            }
            ImGui::TableNextColumn();
            ImGui::TextColored(r.fill >= 1.0 ? kBad : r.fill >= 0.75 ? kWarn : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                               "%lld / %zu", static_cast<long long>(r.peak_backlog), o.max_outbound_letters);
            ImGui::TableNextColumn();
            const ChartWindow w = WindowFor(StatKind::Gauge, now);
            Sparkline("##backlog", BucketValues(o.stats[OfficeStat::Backlog], w),
                      static_cast<double>(o.max_outbound_letters),
                      ImVec2(-1.0f, ImGui::GetTextLineHeight()), kWarn);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (g_stats_office >= offices.size() || !offices[g_stats_office].built) {
        ImGui::TextDisabled("Click an office for its charts.");
        return;
    }
    ImGui::SeparatorText(offices[g_stats_office].name.c_str());
    DrawOfficeCharts(world, g_stats_office, 200.0f, /*side_by_side=*/true);
}

// ---------------------------------------------------------------------------
// Schedules
// ---------------------------------------------------------------------------

// A schedule's utilization over the current range.
struct Utilization {
    int64_t departures = 0, loaded = 0, left_behind = 0, empty = 0, skipped = 0, income = 0;
    double load = 0.0;  // average fraction of kTruckCapacity used per departure
    int64_t net() const { return income - departures * kTruckDepartureCost; }
};

Utilization UtilizationOf(const TruckSchedule& schedule, Tick now) {
    const ScheduleStats& s = schedule.stats;
    Utilization u;
    u.departures = RangeSum(s[ScheduleStat::Departures], now);
    u.loaded = RangeSum(s[ScheduleStat::LettersLoaded], now);
    u.left_behind = RangeSum(s[ScheduleStat::LeftBehind], now);
    u.empty = RangeSum(s[ScheduleStat::EmptyDepartures], now);
    u.skipped = RangeSum(s[ScheduleStat::Skipped], now);
    u.income = RangeSum(s[ScheduleStat::Income], now);
    if (u.departures > 0)
        u.load = static_cast<double>(u.loaded) / static_cast<double>(u.departures * static_cast<int64_t>(kTruckCapacity));
    return u;
}

void UtilizationTooltipBody(const Utilization& u) {
    ImGui::TextDisabled("Over the last %s:", CurrentRange().label);
    ImGui::Text("%lld departures, %.0f%% full on average", static_cast<long long>(u.departures), 100.0 * u.load);
    ImGui::Text("%lld letters carried, %lld left behind (no room)", static_cast<long long>(u.loaded),
                static_cast<long long>(u.left_behind));
    ImGui::Text("%lld left empty, %lld skipped (no money)", static_cast<long long>(u.empty),
                static_cast<long long>(u.skipped));
    ImGui::Text("Earned $%lld on final legs, net $%lld after truck costs", static_cast<long long>(u.income),
                static_cast<long long>(u.net()));
}

// The departure log as bars: letters loaded, with the letters left behind
// stacked on top in red, against the truck capacity.
void DrawDepartureLogChart(const TruckSchedule& schedule, float height) {
    const std::vector<DepartureRecord>& log = schedule.departure_log;
    if (!ImPlot::BeginPlot("Recent departures", ImVec2(-1.0f, height), ImPlotFlags_NoMouseText | ImPlotFlags_NoBoxSelect))
        return;
    ImPlot::SetupAxes("departure", "letters", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
    ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0.0, INFINITY);
    ImPlot::SetupLegend(ImPlotLocation_NorthWest);
    std::vector<double> xs, loaded, demand;
    for (std::size_t i = 0; i < log.size(); ++i) {
        xs.push_back(static_cast<double>(i) - static_cast<double>(log.size()));  // -n .. -1: newest is -1
        loaded.push_back(static_cast<double>(log[i].loaded));
        demand.push_back(static_cast<double>(log[i].loaded + log[i].left_behind));
    }
    const int n = static_cast<int>(xs.size());
    ImPlotSpec demand_spec;
    demand_spec.FillColor = kBad;
    ImPlot::PlotBars("Left behind", xs.data(), demand.data(), n, 0.7, demand_spec);
    ImPlotSpec loaded_spec;
    loaded_spec.FillColor = kNeutral;
    ImPlot::PlotBars("Loaded", xs.data(), loaded.data(), n, 0.7, loaded_spec);
    const double capacity = static_cast<double>(kTruckCapacity);
    ImPlotSpec cap_spec;
    cap_spec.LineColor = kMuted;
    cap_spec.Flags = ImPlotInfLinesFlags_Horizontal;
    ImPlot::PlotInfLines("Capacity", &capacity, 1, cap_spec);

    if (ImPlot::IsPlotHovered() && n > 0) {
        const double mx = ImPlot::GetPlotMousePos().x;
        const long index = std::lround(mx) + static_cast<long>(log.size());
        if (index >= 0 && index < static_cast<long>(log.size())) {
            const DepartureRecord& r = log[static_cast<std::size_t>(index)];
            char when[32];
            ui::FormatDayTime(r.tick, when, sizeof(when));
            ImGui::SetTooltip("%s\n%u loaded, %u left behind", when, r.loaded, r.left_behind);
        }
    }
    ImPlot::EndPlot();
}

void DrawSchedulesTab(const World& world) {
    const std::vector<PostOffice>& offices = world.get_post_offices();
    const Tick now = world.get_tick();

    struct Row {
        PostOfficeId src;
        std::size_t index;
        const TruckSchedule* schedule;
        Utilization u;
    };
    std::vector<Row> rows;
    for (PostOfficeId id = 0; id < offices.size(); ++id)
        for (std::size_t i = 0; i < offices[id].outbound_schedules.size(); ++i) {
            const TruckSchedule& s = offices[id].outbound_schedules[i];
            rows.push_back({id, i, &s, UtilizationOf(s, now)});
        }
    if (rows.empty()) {
        ImGui::TextDisabled("No truck schedules yet.");
        return;
    }
    ImGui::TextDisabled("Load is how full trucks left on average; \"left behind\" counts matching letters that did not fit.");

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    const float table_h = std::min(280.0f, ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(rows.size() + 2) + 8.0f);
    if (ImGui::BeginTable("schedules_stats", 10, flags, ImVec2(0.0f, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Route", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 1.6f);
        ImGui::TableSetupColumn("Picks up", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Departures");
        ImGui::TableSetupColumn("Load");
        ImGui::TableSetupColumn("Left behind");
        ImGui::TableSetupColumn("Empty");
        ImGui::TableSetupColumn("Skipped");
        ImGui::TableSetupColumn("Income");
        ImGui::TableSetupColumn("Net");
        ImGui::TableSetupColumn("Recent loads", ImGuiTableColumnFlags_NoSort, 110.0f);
        ImGui::TableHeadersRow();
        SortRows(rows, [](const Row& r, int column) -> double {
            switch (column) {
            case 0: return static_cast<double>(r.src) * 1000.0 + static_cast<double>(r.index);
            case 2: return static_cast<double>(r.u.departures);
            case 3: return r.u.load;
            case 4: return static_cast<double>(r.u.left_behind);
            case 5: return static_cast<double>(r.u.empty);
            case 6: return static_cast<double>(r.u.skipped);
            case 7: return static_cast<double>(r.u.income);
            case 8: return static_cast<double>(r.u.net());
            default: return 0.0;
            }
        });

        for (const Row& r : rows) {
            const TruckSchedule& s = *r.schedule;
            ImGui::PushID(static_cast<int>(s.id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char route[256];
            std::snprintf(route, sizeof(route), "%s #%zu  ->  %s", offices[r.src].name.c_str(), r.index + 1,
                          offices[s.dst].name.c_str());
            if (ImGui::Selectable(route, g_stats_schedule == s.id, ImGuiSelectableFlags_SpanAllColumns)) {
                g_stats_schedule = s.id;
                ui::SelectOffice(r.src);
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(RuleSummary(s.rule, offices, s.dst).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%lld", static_cast<long long>(r.u.departures));
            ImGui::TableNextColumn();
            if (r.u.departures > 0)
                ImGui::Text("%.0f%%", 100.0 * r.u.load);
            else
                ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (r.u.left_behind > 0)
                ImGui::TextColored(kWarn, "%lld", static_cast<long long>(r.u.left_behind));
            else
                ImGui::TextDisabled("0");
            ImGui::TableNextColumn();
            ImGui::Text("%lld", static_cast<long long>(r.u.empty));
            ImGui::TableNextColumn();
            if (r.u.skipped > 0)
                ImGui::TextColored(kBad, "%lld", static_cast<long long>(r.u.skipped));
            else
                ImGui::TextDisabled("0");
            ImGui::TableNextColumn();
            ImGui::Text("$%lld", static_cast<long long>(r.u.income));
            ImGui::TableNextColumn();
            ImGui::TextColored(r.u.net() >= 0 ? kGood : kBad, "%s$%lld", r.u.net() < 0 ? "-" : "",
                               static_cast<long long>(std::llabs(r.u.net())));
            ImGui::TableNextColumn();
            std::vector<double> loads;
            for (const DepartureRecord& d : s.departure_log)
                loads.push_back(static_cast<double>(d.loaded));
            if (!loads.empty())
                Sparkline("##loads", loads, static_cast<double>(kTruckCapacity),
                          ImVec2(-1.0f, ImGui::GetTextLineHeight()), kNeutral);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Details of the picked schedule.
    const TruckSchedule* picked = nullptr;
    PostOfficeId picked_src = kNoPostOffice;
    for (const Row& r : rows)
        if (r.schedule->id == g_stats_schedule) {
            picked = r.schedule;
            picked_src = r.src;
        }
    if (picked == nullptr) {
        ImGui::TextDisabled("Click a schedule for its departure history.");
        return;
    }
    char title[256];
    std::snprintf(title, sizeof(title), "%s #%zu -> %s", offices[picked_src].name.c_str(),
                  ScheduleNumber(offices, picked_src, picked->id), offices[picked->dst].name.c_str());
    ImGui::SeparatorText(title);
    if (ImGui::BeginTable("schedule_charts", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        DrawDepartureLogChart(*picked, 210.0f);
        ImGui::TableNextColumn();
        const ScheduleStats& s = picked->stats;
        if (BeginTimeChart("Load over time", now, "% of capacity", 210.0f)) {
            PlotAverage("Load", s[ScheduleStat::LettersLoaded], {&s[ScheduleStat::Departures]}, now,
                        Unit::Percent, 100.0 / static_cast<double>(kTruckCapacity), kNeutral);
            PlotAverage("Left behind", s[ScheduleStat::LeftBehind], {&s[ScheduleStat::Departures]}, now,
                        Unit::Percent, 100.0 / static_cast<double>(kTruckCapacity), kBad);
            PlotTickMarkers("Edited", picked->edits, kMuted);
            EndTimeChart();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("\"Left behind\" over time is per departure, as a share of a truckload. Vertical lines mark edits.");
}

// ---------------------------------------------------------------------------
// Recent deliveries
// ---------------------------------------------------------------------------

void DrawDeliveriesTab(const World& world) {
    const std::vector<PostOffice>& offices = world.get_post_offices();
    const std::vector<DeliveredLetter>& deliveries = world.get_recent_deliveries();
    if (deliveries.empty()) {
        ImGui::TextDisabled("No letters delivered yet.");
        return;
    }
    ImGui::TextDisabled("The last %zu letters delivered, newest first. Hover one for its journey.", deliveries.size());
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("deliveries", 7, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Delivered");
    ImGui::TableSetupColumn("From -> To", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Took");
    ImGui::TableSetupColumn("Waiting");
    ImGui::TableSetupColumn("Hops");
    ImGui::TableSetupColumn("Result");
    ImGui::TableSetupColumn("Earned");
    ImGui::TableHeadersRow();
    for (std::size_t k = deliveries.size(); k-- > 0;) {
        const DeliveredLetter& d = deliveries[k];
        const Letter& letter = d.letter;
        ImGui::PushID(static_cast<int>(k));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char when[32];
        ui::FormatDayTime(d.delivered, when, sizeof(when));
        ImGui::Selectable(when, false, ImGuiSelectableFlags_SpanAllColumns);
        if (ImGui::IsItemHovered()) {
            ui::HighlightLetter(letter);
            ImGui::BeginTooltip();
            ui::DrawLetterTrail(letter, offices, world.get_tick());
            ImGui::EndTooltip();
        }
        ImGui::TableNextColumn();
        ImGui::Text("%s -> %s", offices[letter.src].name.c_str(), offices[letter.dst].name.c_str());

        Tick transit = 0;
        std::size_t hops = 0;
        for (std::size_t i = 1; i < letter.trail.size(); ++i)
            if (letter.trail[i - 1].event == TrailEvent::Departed && letter.trail[i].event == TrailEvent::Arrived) {
                transit += letter.trail[i].tick - letter.trail[i - 1].tick;
                ++hops;
            }
        const Tick took = d.delivered - letter.created;
        char took_text[16], waiting_text[16];
        ui::FormatDuration(took, took_text, sizeof(took_text));
        ui::FormatDuration(took - std::min(transit, took), waiting_text, sizeof(waiting_text));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(took_text);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(waiting_text);
        ImGui::TableNextColumn();
        ImGui::Text("%zu", hops);
        ImGui::TableNextColumn();
        if (d.delivered <= letter.deadline) {
            ImGui::TextColored(kGood, "on time");
        } else {
            char late[16];
            ui::FormatDuration(d.delivered - letter.deadline, late, sizeof(late));
            ImGui::TextColored(kBad, "late by %s", late);
        }
        ImGui::TableNextColumn();
        ImGui::Text("$%d", d.earned);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

void DrawStatsWindow(const World& world) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_P, false))
        g_show_stats = !g_show_stats;
    if (!g_show_stats)
        return;

    ImGui::SetNextWindowSize(ImVec2(900.0f, 640.0f), ImGuiCond_FirstUseEver);
    // Floating only, like the HUD (the dockspace takes no other windows).
    if (!ImGui::Begin("Statistics (P)###Statistics", &g_show_stats, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    DrawRangeButtons();
    ImGui::SameLine();
    ImGui::TextDisabled("  of in-game time");
    if (ImGui::BeginTabBar("stats_tabs")) {
        if (ImGui::BeginTabItem("Network")) {
            DrawGlobalTab(world);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Offices")) {
            DrawOfficesTab(world);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Schedules")) {
            DrawSchedulesTab(world);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Recent deliveries")) {
            DrawDeliveriesTab(world);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

namespace ui {

void ToggleStatsWindow() { g_show_stats = !g_show_stats; }

void DrawOfficeStats(const World& world, PostOfficeId id) {
    const PostOffice& office = world.get_post_offices()[id];
    const OfficeStats& s = office.stats;
    const Tick now = world.get_tick();
    DrawRangeButtons();

    const int64_t lost = RangeSum(s[OfficeStat::Lost], now);
    ImGui::Text("Posted %lld, delivered here %lld, forwarded %lld",
                static_cast<long long>(RangeSum(s[OfficeStat::Spawned], now)),
                static_cast<long long>(RangeSum(s[OfficeStat::Delivered], now)),
                static_cast<long long>(RangeSum(s[OfficeStat::Forwarded], now)));
    if (lost > 0)
        ImGui::TextColored(kBad, "%lld letters lost: the outbound buffer was full.", static_cast<long long>(lost));
    DrawOfficeCharts(world, id, 150.0f, /*side_by_side=*/false);
}

void DrawScheduleLoadCell(const TruckSchedule& schedule, Tick now) {
    const Utilization u = UtilizationOf(schedule, now);
    ImGui::BeginGroup();
    if (u.departures == 0) {
        ImGui::TextDisabled("-");
    } else if (u.left_behind > 0) {
        ImGui::TextColored(kWarn, "%.0f%%+", 100.0 * u.load);  // "+": more mail than fit
    } else {
        ImGui::Text("%.0f%%", 100.0 * u.load);
    }
    // Last few departures as a tiny histogram.
    constexpr std::size_t kBars = 12;
    const std::vector<DepartureRecord>& log = schedule.departure_log;
    if (!log.empty()) {
        std::vector<float> bars;
        for (std::size_t i = log.size() > kBars ? log.size() - kBars : 0; i < log.size(); ++i)
            bars.push_back(static_cast<float>(log[i].loaded));
        ImGui::SameLine();
        ImGui::PlotHistogram("##loads", bars.data(), static_cast<int>(bars.size()), 0, nullptr, 0.0f,
                             static_cast<float>(kTruckCapacity),
                             ImVec2(static_cast<float>(kBars) * 4.0f, ImGui::GetTextLineHeight()));
    }
    ImGui::EndGroup();
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        UtilizationTooltipBody(u);
        ImGui::EndTooltip();
    }
}

void DrawLetterTrail(const Letter& letter, const std::vector<PostOffice>& offices, Tick now) {
    char when[32], span[16];
    ImGui::Text("%s -> %s", offices[letter.src].name.c_str(), offices[letter.dst].name.c_str());
    FormatDayTime(letter.deadline, when, sizeof(when));
    ImGui::TextDisabled("Due %s, worth $%d (+ up to $%d early)", when, letter.value, letter.max_bonus);
    ImGui::Separator();
    if (letter.trail.empty()) {
        ImGui::TextDisabled("No journey recorded (posted before trails existed).");
        return;
    }

    for (std::size_t i = 0; i < letter.trail.size(); ++i) {
        const TrailStop& stop = letter.trail[i];
        // The gap since the previous stop: on a truck, or waiting in an office.
        if (i > 0) {
            const TrailStop& prev = letter.trail[i - 1];
            FormatDuration(stop.tick - prev.tick, span, sizeof(span));
            if (prev.event == TrailEvent::Departed)
                ImGui::TextDisabled("      %s on the road", span);
            else
                ImGui::TextDisabled("      waited %s", span);
        }
        FormatDayTime(stop.tick, when, sizeof(when));
        const char* office = stop.office < offices.size() ? offices[stop.office].name.c_str() : "?";
        switch (stop.event) {
        case TrailEvent::Spawned:
            ImGui::Text("%s  Posted at %s", when, office);
            break;
        case TrailEvent::Departed: {
            const std::size_t number = ScheduleNumber(offices, stop.office, stop.schedule);
            if (number > 0)
                ImGui::Text("%s  Left %s on schedule #%zu", when, office, number);
            else
                ImGui::Text("%s  Left %s", when, office);
            break;
        }
        case TrailEvent::Arrived:
            if (stop.office == letter.dst)
                ImGui::TextColored(stop.tick <= letter.deadline ? kGood : kBad, "%s  Delivered at %s", when, office);
            else
                ImGui::Text("%s  Arrived at %s, forwarded", when, office);
            break;
        }
    }

    // Where it is now, unless delivered.
    const TrailStop& last = letter.trail.back();
    if (last.event == TrailEvent::Arrived && last.office == letter.dst)
        return;
    FormatDuration(now >= last.tick ? now - last.tick : 0, span, sizeof(span));
    if (last.event == TrailEvent::Departed)
        ImGui::TextDisabled("      on the road for %s so far", span);
    else
        ImGui::TextColored(kWarn, "      waiting at %s for %s so far",
                           last.office < offices.size() ? offices[last.office].name.c_str() : "?", span);
}

}  // namespace ui
