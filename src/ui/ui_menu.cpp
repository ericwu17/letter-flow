// ui_menu.cpp — the screens before a game starts: the main menu, the credits
// page and the save list (new, rename, copy, delete, play). Drawn as one
// fullscreen window over the map's parchment clear color, with each screen
// on a paper card. Once a save is launched, main.cpp never calls
// DrawMainMenu again; leaving the tutorial comes back here.
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "ui/ui_theme.h"

#include "persistence/save_library.h"
#include "sim/scenario.h"
#include "sim/world.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <utility>

namespace {

namespace theme = ui::theme;

enum class Screen { Main, Credits, Saves };

// The name popup either names a new save or renames an existing one.
enum class NamePurpose { Create, Rename };

Screen g_screen = Screen::Main;

// The last action that failed, shown above the save list until the next one.
std::string g_error;

// Popups are opened through latches: the buttons that open them sit inside
// the table's per-row ID scope, but OpenPopup and BeginPopupModal must agree
// on the ID, so both happen at the card's level.
bool g_name_popup_requested = false;
NamePurpose g_name_purpose = NamePurpose::Create;
SaveId g_name_target = 0;  // the save being renamed
char g_name_buf[64] = "";

bool g_delete_popup_requested = false;
SaveId g_delete_target = 0;
std::string g_delete_name;

std::string Trimmed(const char* text) {
    std::string s = text;
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string::npos)
        return "";
    const auto last = s.find_last_not_of(" \t");
    return s.substr(first, last - first + 1);
}

void SetNameBuffer(const std::string& name) {
    std::snprintf(g_name_buf, sizeof(g_name_buf), "%s", name.c_str());
}

// "Game N" for the first N not already taken.
std::string DefaultSaveName(const SaveLibrary& library) {
    for (std::size_t n = library.saves().size() + 1;; ++n) {
        const std::string name = "Game " + std::to_string(n);
        const bool taken = std::any_of(library.saves().begin(), library.saves().end(),
                                       [&](const SaveSummary& s) { return s.name == name; });
        if (!taken)
            return name;
    }
}

// Unix seconds -> local "YYYY-MM-DD HH:MM".
void FormatLastPlayed(std::int64_t unix_seconds, char* buf, std::size_t buf_len) {
    const std::time_t time = static_cast<std::time_t>(unix_seconds);
    const std::tm* local = std::localtime(&time);
    if (local == nullptr || std::strftime(buf, buf_len, "%Y-%m-%d %H:%M", local) == 0)
        std::snprintf(buf, buf_len, "-");
}

void TextCentered(const char* text) {
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - width) * 0.5f));
    ImGui::TextUnformatted(text);
}

// A paper card centered horizontally in the fullscreen window, `top` below
// its top edge. A height of 0 fits the card to its contents.
void BeginCard(const char* id, float width, float top, float height) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    width = std::min(width, avail.x);
    ImGui::SetCursorPos(ImVec2((avail.x - width) * 0.5f, top));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kPaper);
    ImGuiChildFlags flags = ImGuiChildFlags_Borders;
    if (height <= 0.0f)
        flags |= ImGuiChildFlags_AutoResizeY;
    ImGui::BeginChild(id, ImVec2(width, height), flags);
}

void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// Draws the title card. Returns true when the player starts the tutorial.
bool DrawMainScreen() {
    const float em = ImGui::GetFontSize();
    BeginCard("main_card", em * 18.0f, ImGui::GetContentRegionAvail().y * 0.28f, 0.0f);
    ImGui::Dummy(ImVec2(0.0f, em * 0.5f));
    ImGui::PushFont(nullptr, theme::kFontHuge);
    TextCentered("Letter Flow");
    ImGui::PopFont();
    ImGui::PushFont(nullptr, theme::kFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kInkDim);
    TextCentered("a small logistics toy");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, em));

    if (theme::PrimaryButton("Play", ImVec2(-FLT_MIN, em * 2.2f))) {
        g_error.clear();
        g_screen = Screen::Saves;
    }
    const bool tutorial = ImGui::Button("Tutorial", ImVec2(-FLT_MIN, 0.0f));
    if (ImGui::Button("Credits", ImVec2(-FLT_MIN, 0.0f)))
        g_screen = Screen::Credits;
    ImGui::Dummy(ImVec2(0.0f, em * 0.25f));
    EndCard();
    return tutorial;
}

void DrawCreditsScreen() {
    const float em = ImGui::GetFontSize();
    BeginCard("credits_card", em * 30.0f, em * 3.0f, 0.0f);
    theme::SectionHeading("Credits");
    ImGui::Dummy(ImVec2(0.0f, em * 6.0f));
    if (ImGui::Button("Back"))
        g_screen = Screen::Main;
    EndCard();
}

// The name popup for "New game" and "Rename".
void DrawNamePopup(SaveLibrary& library) {
    if (g_name_popup_requested) {
        ImGui::OpenPopup("###save_name");
        g_name_popup_requested = false;
    }
    const bool create = g_name_purpose == NamePurpose::Create;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(create ? "New game###save_name" : "Rename save###save_name", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;

    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    const bool entered = ImGui::InputText("##name", g_name_buf, sizeof(g_name_buf),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string name = Trimmed(g_name_buf);

    ImGui::Spacing();
    ImGui::BeginDisabled(name.empty());
    const bool confirmed = theme::PrimaryButton(create ? "Create" : "Rename");
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool cancelled = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);

    if ((confirmed || entered) && !name.empty()) {
        g_error.clear();
        if (create) {
            if (!library.create(name))
                g_error = "Couldn't create the save: the save file could not be written.";
        } else if (!library.rename(g_name_target, name)) {
            g_error = "Couldn't rename the save: the save file could not be written.";
        }
        ImGui::CloseCurrentPopup();
    } else if (cancelled) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawDeletePopup(SaveLibrary& library) {
    if (g_delete_popup_requested) {
        ImGui::OpenPopup("Delete save");
        g_delete_popup_requested = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Delete save", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;

    ImGui::Text("Delete \"%s\"?", g_delete_name.c_str());
    ImGui::TextColored(theme::kInkDim, "This can't be undone.");
    ImGui::Spacing();
    if (theme::PrimaryButton("Delete")) {
        g_error.clear();
        if (!library.remove(g_delete_target))
            g_error = "Couldn't delete the save: the save file could not be removed.";
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// The save list. Returns the save the player chose to play, if any. Actions
// that change the library are applied after the table, never while it is
// iterating library.saves().
std::optional<SaveId> DrawSavesScreen(SaveLibrary& library) {
    const float em = ImGui::GetFontSize();
    const float margin = em * 2.0f;
    BeginCard("saves_card", em * 52.0f, margin, ImGui::GetContentRegionAvail().y - 2.0f * margin);

    theme::SectionHeading("Saves");
    if (ImGui::Button("Back"))
        g_screen = Screen::Main;
    ImGui::SameLine();
    const char* new_label = "New game";
    const float new_width = ImGui::CalcTextSize(new_label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - new_width);
    if (theme::PrimaryButton(new_label)) {
        g_name_purpose = NamePurpose::Create;
        SetNameBuffer(DefaultSaveName(library));
        g_name_popup_requested = true;
    }

    if (!library.warning().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kWarnText);
        ImGui::TextWrapped("%s", library.warning().c_str());
        ImGui::PopStyleColor();
    }
    if (!g_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kBad);
        ImGui::TextWrapped("%s", g_error.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();

    std::optional<SaveId> play;
    std::optional<std::pair<SaveId, std::string>> copy;  // source id, name for the copy

    if (library.saves().empty()) {
        ImGui::Dummy(ImVec2(0.0f, em));
        TextCentered("No saves yet. Start a new game.");
    } else {
        const ImGuiTableFlags flags = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("saves", 5, flags)) {
            ImGui::TableSetupScrollFreeze(0, 1);  // keep the header visible while scrolling
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Day");
            ImGui::TableSetupColumn("Money");
            ImGui::TableSetupColumn("Last played");
            ImGui::TableSetupColumn("##actions");
            ImGui::TableHeadersRow();

            for (const SaveSummary& save : library.saves()) {
                ImGui::PushID(static_cast<int>(save.id));
                ImGui::TableNextRow();

                // The whole row is a selectable, double-click to play; the
                // buttons in the last column overlap it.
                ImGui::TableSetColumnIndex(0);
                ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
                ImGui::Selectable(save.name.c_str(), false,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap |
                                      ImGuiSelectableFlags_AllowDoubleClick,
                                  ImVec2(0.0f, ImGui::GetFrameHeight()));
                ImGui::PopStyleVar();
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    play = save.id;

                char text[64];
                ImGui::TableSetColumnIndex(1);
                ImGui::AlignTextToFramePadding();
                ui::FormatDayTime(save.tick, text, sizeof(text));
                ImGui::TextUnformatted(text);

                ImGui::TableSetColumnIndex(2);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(save.money < 0 ? theme::kBad : theme::kInk, "%s$%d",
                                   save.money < 0 ? "-" : "", std::abs(save.money));

                ImGui::TableSetColumnIndex(3);
                ImGui::AlignTextToFramePadding();
                FormatLastPlayed(save.last_played, text, sizeof(text));
                ImGui::TextColored(theme::kInkDim, "%s", text);

                ImGui::TableSetColumnIndex(4);
                if (theme::PrimaryButton("Play"))
                    play = save.id;
                ImGui::SameLine();
                if (ImGui::Button("Rename")) {
                    g_name_purpose = NamePurpose::Rename;
                    g_name_target = save.id;
                    SetNameBuffer(save.name);
                    g_name_popup_requested = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Copy"))
                    copy.emplace(save.id, save.name + " (copy)");
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, theme::kBad);
                if (ImGui::Button("Delete")) {
                    g_delete_target = save.id;
                    g_delete_name = save.name;
                    g_delete_popup_requested = true;
                }
                ImGui::PopStyleColor();

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    if (copy) {
        g_error.clear();
        if (!library.copy(copy->first, copy->second))
            g_error = "Couldn't copy the save: the save file could not be read or written.";
    }

    DrawNamePopup(library);
    DrawDeletePopup(library);
    EndCard();
    return play;
}

}  // namespace

std::optional<MenuLaunch> DrawMainMenu(SaveLibrary& library, std::optional<World>& game) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground;
    ImGui::Begin("##main_menu", nullptr, flags);

    std::optional<SaveId> play;
    bool tutorial = false;
    switch (g_screen) {
    case Screen::Main:
        tutorial = DrawMainScreen();
        break;
    case Screen::Credits:
        DrawCreditsScreen();
        break;
    case Screen::Saves:
        play = DrawSavesScreen(library);
        break;
    }
    ImGui::End();

    if (tutorial) {
        game = create_tutorial_world();
        ResetGameUi();
        return MenuLaunch{};
    }
    if (!play)
        return std::nullopt;
    std::string name;
    for (const SaveSummary& save : library.saves())
        if (save.id == *play)
            name = save.name;
    std::optional<World> world = library.load(*play);
    if (!world) {
        g_error = "Couldn't load \"" + name + "\": the save is damaged or from an incompatible version.";
        return std::nullopt;
    }
    game = std::move(world);
    ResetGameUi();
    return MenuLaunch{play};
}
