// ui_inspector.cpp — the inspector docked on the left: the selected post
// office (letters, statistics, schedule table and its "+ New schedule"
// button, postal code) or truck (route, timing, cargo), and the road-building
// controls.
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
#include "imgui_internal.h"  // GetActiveID (EditPrefixList), dock node flags

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using ui::FormatDayTime;
using ui::FormatDuration;
using ui::g_game;
using ui::RoadBuildState;
using ui::SelectOffice;
namespace theme = ui::theme;

constexpr ImVec4 kColorErrorText = theme::kBad;  // red inline messages

// What "+ New schedule" creates; the player tunes it in the schedule table.
// The rule is ExactDestination: it picks up mail for the chosen office.
constexpr Tick kNewSchedulePeriod = hours_minutes_to_ticks(6, 0);
constexpr Tick kNewScheduleStart = hours_minutes_to_ticks(0, 0);

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
// Inspector: one docked window showing whichever entity is selected. Its
// title changes with the selection, but its ID ("###Inspector") stays fixed,
// so it keeps its dock slot.
// ---------------------------------------------------------------------------

// Text pushed to the right edge of the current table cell (numbers).
void TextRightAligned(const char* text, ImVec4 color = theme::kInk) {
    const float w = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
    ImGui::TextColored(color, "%s", text);
}

// Letters as a To / Here for / Deadline / Value / Max bonus table; overdue
// deadlines get a red LATE tag. "Here for" is the time since the letter got where it is
// (this office, or this truck). Hovering a row shows the letter's trail and
// highlights its journey on the map.
void DrawLetterTable(const char* table_id, const std::vector<Letter>& letters,
                            const std::vector<PostOffice>& offices, Tick now) {
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
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
void DrawTruckPanel(const World& world, const Truck& truck) {
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
bool EditHoursMinutes(const char* id, Tick& ticks, int max_hours) {
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
bool EditPrefixList(const char* id, std::vector<std::string>& prefixes,
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

// Why a schedule-table edit was refused, for the player.
const char* ScheduleErrorText(ScheduleError error) {
    switch (error) {
    case ScheduleError::None:          return "";
    case ScheduleError::UnknownOffice: return "That office or schedule no longer exists.";
    case ScheduleError::SameOffice:    return "A schedule can't go to its own office.";
    case ScheduleError::ZeroPeriod:    return "Trucks need time between departures.";
    case ScheduleError::MalformedRule: return "Every prefix must be non-empty and listed once.";
    case ScheduleError::NoRoad:        return "No road connects these offices.";
    }
    return "";
}

// What a rule type picks up, for the table's "Picks up" dropdown.
std::string RuleHelp(RoutingRuleType type, const std::string& dst_name) {
    switch (type) {
    case RoutingRuleType::ExactDestination: return "Only letters addressed to " + dst_name + ".";
    case RoutingRuleType::PostalPrefix:     return "Letters whose postal code starts with any of the listed prefixes.";
    case RoutingRuleType::AllLetters:       return "Every letter; the rest forward onward from the destination.";
    }
    return "";
}

// A schedule row's drag handle: a 2x3 grip of dots and the row number, as a
// selectable so it can be grabbed for drag-and-drop.
void DrawDragHandle(const char* number) {
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
void DrawScheduleTable(World& world, PostOfficeId selected) {
    const std::vector<PostOffice>& offices = world.get_post_offices();
    const PostOffice& office = offices[selected];

    // Why the last edit was refused (reset with the rest of the forms when
    // the inspected office changes, see DrawOfficePanel).
    std::string& edit_error = g_game.forms.edit_error;

    if (office.outbound_schedules.empty()) {
        ImGui::TextDisabled("No schedules yet.");
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
            const auto rule_label = [&](RoutingRuleType type) {
                return type == RoutingRuleType::PostalPrefix ? std::string("Postal prefix")
                                                             : ui::RuleSummary({type, {}}, offices, schedule.dst);
            };
            ImGui::SetNextItemWidth(is_prefix ? ImGui::GetContentRegionAvail().x * 0.45f : -FLT_MIN);
            if (ImGui::BeginCombo("##rule", is_prefix ? "Prefix" : rule_label(schedule.rule.type).c_str())) {
                for (const RoutingRuleType type : ui::kRuleTypes) {
                    const bool current = type == schedule.rule.type;
                    const bool clicked = ImGui::Selectable(rule_label(type).c_str(), current);
                    ImGui::SetItemTooltip("%s", RuleHelp(type, offices[schedule.dst].name).c_str());
                    if (clicked && !current) {
                        edited.rule.type = type;
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
    ui::NoteItemAnchor(TutorialAnchor::ScheduleTable);

    if (schedule_to_update.has_value()) {
        const auto& [index, s] = *schedule_to_update;
        const ScheduleError error = world.check_update_schedule(selected, index, s.period, s.rule);
        if (error == ScheduleError::None && world.update_schedule(selected, index, s.period, s.start_offset, s.rule))
            edit_error.clear();
        else
            edit_error = ScheduleErrorText(error);
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
void DrawOfficeHeader(const std::vector<PostOffice>& offices, PostOfficeId selected) {
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

    if (!office.postal_code.empty())
        theme::Pill(office.postal_code.c_str(), theme::kPaper2, theme::kInkDim);
    else
        ImGui::TextDisabled("No postal code (set one under Settings)");
    ImGui::Spacing();
}

// How full the outbound buffer is: "Outbound ... n / max" over a thin bar
// that turns warn / bad at the same thresholds as the office on the map,
// and, while its overflow timer is running, how long until the game ends.
void DrawCapacityBar(const PostOffice& office) {
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

    if (office.overflow_ticks > 0) {
        const bool full = office.outbound_letters.size() >= office.max_outbound_letters;
        char left[32];
        FormatDuration(kOverflowLimit - std::min(kOverflowLimit, office.overflow_ticks), left, sizeof(left));
        ImGui::PushTextWrapPos(0.0f);
        if (office.overflow_ticks >= kOverflowLimit)
            ImGui::TextColored(kColorErrorText, "Overflowed: this office ended the game.");
        else if (full)
            ImGui::TextColored(kColorErrorText, "Overflowing! Game over in %s if it stays full.", left);
        else
            ImGui::TextColored(theme::kWarnText, "Recovering: %s left on the overflow timer.", left);
        ImGui::PopTextWrapPos();
    }
}

// The "+ New schedule" button under the schedule table: it opens a list of
// the offices this one reaches by road, narrowed as you type (by name or
// postal code), and picking one adds a schedule there with the defaults
// above, to be tuned in the table.
void DrawAddScheduleButton(World& world, PostOfficeId selected) {
    const std::vector<PostOffice>& offices = world.get_post_offices();
    bool any_reachable = false;
    for (PostOfficeId id = 0; id < offices.size() && !any_reachable; ++id)
        any_reachable = id != selected && world.has_road(selected, id);

    ImGui::BeginDisabled(!any_reachable);
    if (ImGui::Button("+ New schedule"))
        ImGui::OpenPopup("new_schedule_dst");
    ImGui::SetItemTooltip("Each truck costs $%d when it departs and carries up to %zu letters.",
                          kTruckDepartureCost, kTruckCapacity);
    ImGui::EndDisabled();
    ui::NoteItemAnchor(TutorialAnchor::AddScheduleButton);
    if (!any_reachable) {
        ImGui::SameLine();
        ImGui::TextDisabled("Build a road from this office first.");
    }

    // The search box. Widget scratch rather than game state: it is cleared
    // whenever the list opens. (Not part of InspectorForms either:
    // ImGuiTextFilter cannot be copy-assigned outside imgui.cpp.)
    static ImGuiTextFilter dst_filter;
    std::optional<PostOfficeId> picked;  // added after the loop over `offices`
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, ImGui::GetFontSize() * 20.0f));
    if (ImGui::BeginPopup("new_schedule_dst")) {
        if (ImGui::IsWindowAppearing()) {
            dst_filter.Clear();
            ImGui::SetKeyboardFocusHere();  // typing filters immediately
        }
        dst_filter.Draw("##new_schedule_filter", ImGui::GetFontSize() * 14.0f);
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
            if (ImGui::Selectable(other.name.c_str()))  // also closes the popup
                picked = id;
            ImGui::SameLine();
            ImGui::TextDisabled("%s", other.postal_code.c_str());
            ImGui::PopID();
        }
        if (!any)
            ImGui::TextDisabled("No office reachable by road matches.");
        ImGui::EndPopup();
    }
    if (picked.has_value())
        world.add_schedule(selected, *picked, kNewSchedulePeriod, kNewScheduleStart,
                           RoutingRule{RoutingRuleType::ExactDestination, {}});
}

// A post office: header, outbound letters, statistics and schedule editing.
void DrawOfficePanel(World& world, PostOfficeId selected) {
    const std::vector<PostOffice>& offices = world.get_post_offices();
    const PostOffice& office = offices[selected];

    // Another office is shown: its forms start over, from its own values.
    if (g_game.forms.office != selected) {
        g_game.forms = ui::InspectorForms{};
        g_game.forms.office = selected;
        std::snprintf(g_game.forms.postal_code, sizeof(g_game.forms.postal_code), "%s",
                      office.postal_code.c_str());
    }

    DrawOfficeHeader(offices, selected);

    DrawCapacityBar(office);
    ImGui::Spacing();

    ImGui::BeginGroup();  // header and table, as one tutorial anchor
    if (ImGui::CollapsingHeader("Outbound letters", ImGuiTreeNodeFlags_DefaultOpen))
        DrawLetterTable("outbound_letters", office.outbound_letters, offices, world.get_tick());
    ImGui::EndGroup();
    ui::NoteItemAnchor(TutorialAnchor::OutboundLetters);

    if (ImGui::CollapsingHeader("Statistics"))
        ui::DrawOfficeStats(world, selected);

    if (ImGui::CollapsingHeader("Truck schedules", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawScheduleTable(world, selected);
        DrawAddScheduleButton(world, selected);
    }

    // Postal code, editable: every keystroke is committed to the World, so
    // the map label updates live.
    const bool settings_open = ImGui::CollapsingHeader("Settings");
    ui::NoteItemAnchor(TutorialAnchor::SettingsHeader);
    if (settings_open) {
        char* postal_buf = g_game.forms.postal_code;
        if (ImGui::InputText("Postal code", postal_buf, sizeof(g_game.forms.postal_code)))
            world.set_postal_code(selected, postal_buf);
        ui::NoteItemAnchor(TutorialAnchor::PostalCodeInput);
    }
}

// Road-building controls, shown under either panel.
void DrawRoadBuildSection(const World& world) {
    const std::vector<PostOffice>& offices = world.get_post_offices();

    theme::SectionHeading("Roads");

    if (g_game.road_build_state == RoadBuildState::Idle) {
        if (ImGui::Button("Build a road...")) {
            g_game.road_build_state = RoadBuildState::SelectSource;
            g_game.road_builder = RoadBuilder{};
        }
        ui::NoteItemAnchor(TutorialAnchor::BuildRoadButton);
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

    if (g_game.road_build_state == RoadBuildState::SelectSource) {
        ImGui::TextUnformatted("1. Click the office the road starts from.");
    } else {
        ImGui::Text("2. From %s: click to place bends, then click the office it ends at.",
                    g_game.road_builder.src != kNoPostOffice ? offices[g_game.road_builder.src].name.c_str() : "?");
        const std::size_t bends = g_game.road_builder.vertices.empty() ? 0
                                : g_game.road_builder.vertices.size() - 1;  // exclude source office vertex
        ImGui::TextDisabled("%zu bends placed, %.0f units so far.", bends,
                            polyline_length(g_game.road_builder.vertices));
    }

    if (ImGui::Button("Cancel  (Esc)")) {
        g_game.road_build_state = RoadBuildState::Idle;
        g_game.road_builder = RoadBuilder{};
    }
    ImGui::SameLine();
    ImGui::TextDisabled("or right-click the map");
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

}  // namespace

void DrawInspector(World& world) {
    if (world.get_post_offices().empty())
        return;
    ui::EnsureOfficeSelected(world);

    // A live truck selection wins; once it has expired (the truck arrived)
    // the backup office shows instead. Local copies keep the panel drawing
    // what it started with even if a click inside it changes the selection.
    const std::shared_ptr<const Truck> truck = g_game.selected_truck.lock();
    const PostOfficeId office = g_game.selected_office;

    // Pinned into the left dock node: re-docked every frame, and its node
    // can't be dragged out, docked over, or undocked via the window menu.
    // The lone tab is hidden too: the panel draws its own header.
    ImGuiWindowClass inspector_class;
    inspector_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoUndocking |
                                               (int)ImGuiDockNodeFlags_NoWindowMenuButton |
                                               (int)ImGuiDockNodeFlags_NoDockingOverMe |
                                               (int)ImGuiDockNodeFlags_AutoHideTabBar;
    ImGui::SetNextWindowClass(&inspector_class);
    ImGui::SetNextWindowDockID(ui::InspectorDockId(), ImGuiCond_Always);
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
