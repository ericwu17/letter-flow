// ui_menu.cpp — the menus: the screens before a game starts (the main menu,
// the credits page and the save list: new, rename, copy, delete, play),
// drawn as one fullscreen window over the map's parchment clear color with
// each screen on a paper card, and the in-game pause menu. main.cpp calls
// DrawMainMenu while no game is running: before the first launch, and after
// the player leaves a game or the tutorial, which lands on the title card.
// The game-over card, which replaces the pause menu once a game has ended,
// lives here too.
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "ui/ui_theme.h"

#include "persistence/save_library.h"
#include "sim/game_types.h"
#include "sim/scenario.h"
#include "sim/stats.h"
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

// Whether an Esc pressed this frame may open the pause menu. Decided at the
// end of the previous frame, because Dear ImGui handles Esc inside NewFrame
// (closing a popup, leaving a text field) and the map cancels road building
// on it before DrawPauseMenu runs: by then whatever Esc cancelled is gone.
bool g_esc_opens_menu = false;

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
                if (save.game_over) {
                    // Ended games still open (to look at the final map), but
                    // can't be played on.
                    ImGui::SameLine();
                    theme::Pill("GAME OVER", theme::kBad, theme::kPaperLight);
                }

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

    // Leaving the game comes back to the title card.
    if (tutorial || play) {
        g_screen = Screen::Main;
        g_error.clear();
    }
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
        g_screen = Screen::Saves;
        g_error = "Couldn't load \"" + name + "\": the save is damaged or from an incompatible version.";
        return std::nullopt;
    }
    game = std::move(world);
    ResetGameUi();
    return MenuLaunch{play};
}

PauseAction DrawPauseMenu(const World& world, SaveLibrary& library, std::optional<SaveId> save) {
    ui::GameUiState& game = ui::g_game;
    if (world.is_game_over()) {
        // The game-over card has taken over (DrawGameOver).
        game.pause_menu_open = false;
        return PauseAction::None;
    }
    if (!game.pause_menu_open && g_esc_opens_menu && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        game.pause_menu_open = true;

    PauseAction action = PauseAction::None;
    if (game.pause_menu_open) {
        // OpenPopup needs a current window, so the popup is hosted in an
        // invisible, input-transparent one (like the tutorial's modal card).
        ImGui::Begin("##pause_menu_host", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
        if (!ImGui::IsPopupOpen("##pause_menu")) {
            ImGui::OpenPopup("##pause_menu");
            game.pause_menu_status.clear();
        }

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 18.0f));
        const bool open = ImGui::BeginPopupModal("##pause_menu", nullptr,
                                                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                                     ImGuiWindowFlags_AlwaysAutoResize |
                                                     ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar();
        if (open) {
            const float em = ImGui::GetFontSize();
            const ImVec2 button_size(em * 14.0f, 0.0f);
            theme::SectionHeading("Paused");

            // The Esc that opened the menu must not close it again.
            bool resume = theme::PrimaryButton("Resume", button_size);
            if (!ImGui::IsWindowAppearing() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                resume = true;

            ImGui::BeginDisabled(!save);
            if (ImGui::Button("Save", button_size) && save) {
                char when[64];
                ui::FormatDayTime(world.get_tick(), when, sizeof(when));
                game.pause_menu_status_failed = !library.store(*save, world);
                game.pause_menu_status = game.pause_menu_status_failed
                                             ? "Couldn't save: the save file could not be written."
                                             : std::string("Saved (") + when + ")";
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Main menu", button_size))
                action = PauseAction::MainMenu;
            if (ImGui::Button("Quit", button_size))
                action = PauseAction::Quit;

            ImGui::Spacing();
            ImGui::PushFont(nullptr, theme::kFontSmall);
            if (!game.pause_menu_status.empty())
                ImGui::TextColored(game.pause_menu_status_failed ? theme::kBad : theme::kGood, "%s",
                                   game.pause_menu_status.c_str());

            ImGui::PopFont();

            if (resume || action != PauseAction::None) {
                game.pause_menu_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::End();
    }

    g_esc_opens_menu = !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !ImGui::IsAnyItemActive() &&
                       game.road_build_state == ui::RoadBuildState::Idle;
    return action;
}

bool PauseMenuOpen() { return ui::g_game.pause_menu_open; }

PauseAction DrawGameOver(const World& world) {
    // Hosted like the pause menu: OpenPopup needs a current window.
    ImGui::Begin("##game_over_host", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
    if (!ImGui::IsPopupOpen("##game_over"))
        ImGui::OpenPopup("##game_over");

    PauseAction action = PauseAction::None;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 18.0f));
    const bool open = ImGui::BeginPopupModal("##game_over", nullptr,
                                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_AlwaysAutoResize |
                                                 ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (open) {
        const float em = ImGui::GetFontSize();
        const ImVec2 button_size(em * 16.0f, 0.0f);
        ImGui::PushFont(nullptr, theme::kFontHuge);
        ImGui::TextColored(theme::kBad, "Game over");
        ImGui::PopFont();

        const std::vector<PostOffice>& offices = world.get_post_offices();
        const PostOfficeId overflowed = world.get_game_over_office();
        char when[64];
        ui::FormatDayTime(world.get_tick(), when, sizeof(when));
        ImGui::Text("%s overflowed (%s).", overflowed < offices.size() ? offices[overflowed].name.c_str() : "An office",
                    when);
        ImGui::Spacing();

        const Tick tick = world.get_tick();
        const Tick days = tick / kTicksPerDay;
        const Tick hours = tick % kTicksPerDay / kTicksPerHour;
        ImGui::TextColored(theme::kInkDim, "Lasted");
        ImGui::SameLine(em * 9.0f);
        ImGui::Text("%llu %s, %llu %s", static_cast<unsigned long long>(days), days == 1 ? "day" : "days",
                    static_cast<unsigned long long>(hours), hours == 1 ? "hour" : "hours");
        ImGui::TextColored(theme::kInkDim, "Post offices");
        ImGui::SameLine(em * 9.0f);
        ImGui::Text("%zu", offices.size());
        ImGui::TextColored(theme::kInkDim, "Delivered");
        ImGui::SameLine(em * 9.0f);
        ImGui::Text("%zu letters (%zu late)", world.get_letters_delivered_on_time() + world.get_letters_delivered_late(),
                    world.get_letters_delivered_late());
        ImGui::TextColored(theme::kInkDim, "Lost");
        ImGui::SameLine(em * 9.0f);
        ImGui::Text("%lld letters", static_cast<long long>(world.get_stats()[GlobalStat::Lost].total));
        ImGui::TextColored(theme::kInkDim, "Events weathered");
        ImGui::SameLine(em * 9.0f);
        ImGui::Text("%zu", world.get_events_so_far());
        ImGui::Spacing();
        ImGui::Spacing();

        if (ImGui::Button("Statistics  (L)", button_size) ||
            (!ImGui::IsPopupOpen("Statistics###game_over_stats") && ImGui::IsKeyPressed(ImGuiKey_L, false)))
            ImGui::OpenPopup("Statistics###game_over_stats");
        if (theme::PrimaryButton("Main menu", button_size))
            action = PauseAction::MainMenu;
        if (ImGui::Button("Quit", button_size))
            action = PauseAction::Quit;

        // The statistics window, as a modal over the card (the floating
        // window would sit behind the card, out of reach). Its title-bar
        // button, Esc or L closes it again.
        ImGui::SetNextWindowSize(ImVec2(900.0f, 640.0f), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        bool stats_open = true;
        if (ImGui::BeginPopupModal("Statistics###game_over_stats", &stats_open, ImGuiWindowFlags_NoSavedSettings)) {
            ui::DrawStatsContents(world);
            const bool typing = ImGui::GetIO().WantTextInput;
            if (!ImGui::IsWindowAppearing() && !typing &&
                (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_L, false)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (action != PauseAction::None)
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
    return action;
}
