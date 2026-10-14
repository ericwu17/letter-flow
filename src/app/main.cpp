// main.cpp — application bootstrap and frame loop (SDL2 + OpenGL3).
//
// Structure copied from imgui/examples/example_sdl2_opengl3, with the game
// simulation and UI calls plugged into the frame loop:
//   1. poll events          (SDL -> ImGui backends)
//   2. advance simulation   (fixed timestep: exactly kTicksPerSecond per second)
//   3. build UI             (immediate mode: re-describe every frame)
//   4. render               (ImGui draw data -> OpenGL)
//
// This file is shared by all platforms (macOS, Linux, Windows). The only
// platform-specific piece is the macOS key press-and-hold tweak, which lives
// in platform_macos.mm behind ApplyMacOSTweaks().

#ifdef __APPLE__
#include "app/platform_macos.h"
#endif

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include "app/app_icon.h"
#include "sim/game_types.h"
#include "persistence/save_library.h"
#include "sim/tutorial.h"
#include "sim/world.h"
#include "ui/ui.h"
#include "ui/ui_theme.h"

#include <cstdio>
#include <optional>
#include <string>

#include <SDL.h>
#include <SDL_opengl.h>

#ifdef _WIN32
#include <windows.h>  // SetProcessDPIAware()
#endif

// Advance the simulation at a FIXED timestep: the game ticks 60 times per
// second no matter what the display refresh rate is (60/120/144 Hz all work).
// Real time since the last frame goes into an accumulator, scaled by the
// world's fast-forward multiplier; whole ticks are drained from it.
static void step_world(World& world, float delta_time, float& tick_accumulator)
{
    constexpr float kSecondsPerTick = 1.0f / (float)kTicksPerSecond;
    if (world.is_paused())
    {
        tick_accumulator = 0.0f;
        return;
    }
    tick_accumulator += delta_time * world.get_speed_multiplier();
    // After a long hitch, don't try to catch up forever. The cap is a quarter
    // second of *real* time, so it scales with the multiplier — a fixed cap
    // would silently throttle high speeds (one 60 Hz frame at 32x is already
    // over half a second of game time).
    const float max_accumulated = 0.25f * world.get_speed_multiplier();
    if (tick_accumulator > max_accumulated)
        tick_accumulator = max_accumulated;
    while (tick_accumulator >= kSecondsPerTick)
    {
        world.advance_tick();
        tick_accumulator -= kSecondsPerTick;
    }
}

// Writes the game back into the save it was launched from. `why` (quit,
// new day, ...) only goes into the log line.
static bool save_game(SaveLibrary& library, SaveId id, const World& world, const std::string& dir, const char* why)
{
    const bool saved = library.store(id, world);
    if (saved)
        printf("Saved game (%s) to %s\n", why, dir.c_str());
    else
        printf("Warning: failed to save game (%s) to %s\n", why, dir.c_str());
    return saved;
}

// The game takes no command-line arguments, but main keeps this signature:
// on Windows SDL_main.h renames main to SDL_main(int, char*[]), which
// SDL2main calls, so a parameterless main would not build there.
int main(int, char**)
{
    // Setup SDL
#ifdef _WIN32
    ::SetProcessDPIAware();
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
    {
        printf("Error: SDL_Init(): %s\n", SDL_GetError());
        return 1;
    }
#ifdef SDL_HINT_IME_SHOW_UI
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
#endif

    // Select GL version + let the backend select a GLSL version
#if defined(__APPLE__)
    // GL 3.2 Core + GLSL 150
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);  // always required on Mac
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    // GL 3.0 + GLSL 130
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

    // Create window with graphics context
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    float main_scale = ImGui_ImplSDL2_GetContentScaleForDisplay(0);
    const Uint32 window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    SDL_Window* window = SDL_CreateWindow("Letter Flow", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)(1280 * main_scale), (int)(800 * main_scale), window_flags);
    if (window == nullptr)
    {
        printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return 1;
    }
    SetAppIcon(window);

#ifdef __APPLE__
    ApplyMacOSTweaks();  // disable the press-and-hold accent popup
#endif

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (gl_context == nullptr)
    {
        printf("Error: SDL_GL_CreateContext(): %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);  // enable vsync

    // Persistence: the saves and the window layout (imgui.ini) live in SDL's
    // per-user app-data directory (SDL_GetPrefPath takes an org and an app
    // name — created if needed):
    //   macOS:   ~/Library/Application Support/LetterFlow/LetterFlow/
    //   Linux:   ~/.local/share/LetterFlow/LetterFlow/
    //   Windows: %APPDATA%\LetterFlow\LetterFlow
    // ImGui's default IniFilename is relative to the working directory, which we don't want.
    char* pref_path = SDL_GetPrefPath("LetterFlow", "LetterFlow");
    const std::string pref_dir = pref_path ? pref_path : "";
    SDL_free(pref_path);
    const std::string ini_path = pref_dir + "imgui.ini";  // must outlive the ImGui context

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();  // charts in the stats window
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = ini_path.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Setup the font, Dear ImGui / ImPlot styles and DPI scaling (ui_theme.h)
    ui::theme::ApplyTheme(main_scale);

    // Setup Platform/Renderer backends
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(nullptr);

    // Every saved game is a file of its own in persistent_saves/
    // (save_library.h).
    const std::string saves_dir = pref_dir + "persistent_saves";
    SaveLibrary library(saves_dir);

    // Our state: the whole game lives here once the player launches a save
    // from the menu (empty until then). The UI layer reads/mutates it, and
    // the frame loop below steps it forward at a fixed tick rate (frozen
    // while the pause menu is open). It is written back into the save it came
    // from at the start of every game day, when the player returns to the
    // main menu, and on quit. The tutorial runs in a world of its own with no
    // save behind it (save_id empty): it is never written anywhere, and
    // leaving it drops everything and returns to the menu.
    std::optional<World> world;
    std::optional<SaveId> save_id;
    std::optional<Tutorial> tutorial;
    Tick autosaved_day = 0;  // the game day last autosaved (or launched on)
    float tick_accumulator = 0.0f;
    const ImVec4 clear_color = ui::theme::kMap;  // the map's parchment background

    // Back to the main menu: the game (or the tutorial) is dropped. Callers
    // save first.
    auto leave_game = [&]() {
        world.reset();
        save_id.reset();
        tutorial.reset();
        tick_accumulator = 0.0f;
    };

    // Main loop
    bool done = false;
    while (!done)
    {
        // 1. Poll and handle events (inputs, window resize, etc.)
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                done = true;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window))
                done = true;
        }

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        if (world) {
            // 2. Advance the simulation (fixed timestep, see step_world),
            //    unless the pause menu is open, and autosave once each time
            //    the clock enters a new day.
            if (PauseMenuOpen())
                tick_accumulator = 0.0f;
            else
                step_world(*world, io.DeltaTime, tick_accumulator);
            const Tick day = world->get_tick() / kTicksPerDay;
            if (save_id && day != autosaved_day) {
                save_game(library, *save_id, *world, saves_dir, "new day");
                autosaved_day = day;
            }

            // 3. Build this frame's UI. Order matters: the dockspace hosts the
            //    windows, the map goes into the background draw list (under the
            //    windows), the transport bar is drawn on top of the map, then
            //    the HUD/inspector windows, and the pause menu over it all.
            DrawDockspace();
            DrawWorld(*world);
            DrawTransportBar(*world);
            DrawHUD(*world);
            DrawInspector(*world);
            DrawStatsWindow(*world);
            const bool left_tutorial = tutorial && DrawTutorial(*tutorial, *world);
            switch (DrawPauseMenu(*world, library, save_id)) {
            case PauseAction::None:
                if (left_tutorial)
                    leave_game();
                break;
            case PauseAction::MainMenu:
                if (save_id)
                    save_game(library, *save_id, *world, saves_dir, "main menu");
                leave_game();
                break;
            case PauseAction::Quit:
                done = true;  // saved below, after the loop
                break;
            }
        } else if (const std::optional<MenuLaunch> launched = DrawMainMenu(library, world)) {
            // 2-3. No game yet: the main menu, until the player launches a
            // save or the tutorial.
            save_id = launched->save;
            autosaved_day = world->get_tick() / kTicksPerDay;
            if (save_id)
                printf("Loaded save %u\n", static_cast<unsigned>(*save_id));
            else
                tutorial.emplace();
        }

        // 4. Rendering
        ImGui::Render();
        int display_w, display_h;
        SDL_GetWindowSizeInPixels(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    // Autosave on quit (the window's close button or the pause menu's Quit)
    // into the save that was launched, so the next launch can continue it.
    // Quitting from the menu or the tutorial has nothing to save.
    if (world && save_id)
        save_game(library, *save_id, *world, saves_dir, "quit");

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
