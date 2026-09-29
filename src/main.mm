// main.mm — application bootstrap and frame loop (Objective-C++: SDL2 + Metal).
//
// Structure copied from imgui/examples/example_sdl2_metal, with the game
// simulation and UI calls plugged into the frame loop:
//   1. poll events          (SDL -> ImGui backends)
//   2. advance simulation   (fixed timestep: exactly kTicksPerSecond per second)
//   3. build UI             (immediate mode: re-describe every frame)
//   4. render               (ImGui draw data -> Metal)

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_metal.h"

#include "game_types.h"
#include "scenario.h"
#include "world.h"
#include "ui.h"

#include <cstdio>
#include <string>
#include <utility>

#include <SDL.h>

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

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
    if (tick_accumulator > 0.25f)
        tick_accumulator = 0.25f;  // after a long hitch, don't try to catch up forever
    while (tick_accumulator >= kSecondsPerTick)
    {
        world.advance_tick();
        tick_accumulator -= kSecondsPerTick;
    }
}

int main(int, char**)
{
    // Setup SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
    {
        printf("Error: SDL_Init(): %s\n", SDL_GetError());
        return 1;
    }
#ifdef SDL_HINT_IME_SHOW_UI
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
#endif

    // Create window
    float main_scale = ImGui_ImplSDL2_GetContentScaleForDisplay(0);
    int window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    SDL_Window* window = SDL_CreateWindow("Letter Flow", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)(1280 * main_scale), (int)(800 * main_scale), window_flags);
    if (window == nullptr)
    {
        printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return 1;
    }

    // Create Metal device _before_ creating the view/layer
    id<MTLDevice> metalDevice = MTLCreateSystemDefaultDevice();
    if (!metalDevice)
    {
        printf("Error: failed to create Metal device.\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_MetalView view = SDL_Metal_CreateView(window);
    CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
    layer.device = metalDevice;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;

    id<MTLCommandQueue> commandQueue = [layer.device newCommandQueue];
    MTLRenderPassDescriptor* renderPassDescriptor = [MTLRenderPassDescriptor new];

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;  // requires the imgui 'docking' branch

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup scaling
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    // Setup Platform/Renderer backends
    ImGui_ImplMetal_Init(layer.device);
    ImGui_ImplSDL2_InitForMetal(window);

    // Our state: the whole game lives here. The UI layer reads/mutates it,
    // and the frame loop below steps it forward at a fixed tick rate.
    //
    // Persistence: the save lives in SDL's per-user app-data directory
    // (~/Library/Application Support/LetterFlow/LetterFlow/ on macOS —
    // SDL_GetPrefPath takes an org and an app name — created if needed).
    // Startup continues the previous session if a save exists; the autosave
    // after the main loop writes it back on quit.
    char* pref_path = SDL_GetPrefPath("LetterFlow", "LetterFlow");
    const std::string save_path = std::string(pref_path ? pref_path : "") + "save.json";
    SDL_free(pref_path);

    World world = create_default_world();
    if (auto saved = World::load_from_file(save_path))
    {
        world = std::move(*saved);
        printf("Loaded saved game from %s\n", save_path.c_str());
    }
    float tick_accumulator = 0.0f;
    const double clear_color[4] = {0.07, 0.08, 0.10, 1.00};  // MTLClearColorMake takes doubles

    // Main loop
    bool done = false;
    while (!done)
    {
        @autoreleasepool
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

            int width, height;
            SDL_GetWindowSizeInPixels(window, &width, &height);
            layer.drawableSize = CGSizeMake(width, height);
            id<CAMetalDrawable> drawable = [layer nextDrawable];

            id<MTLCommandBuffer> commandBuffer = [commandQueue commandBuffer];
            renderPassDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(clear_color[0] * clear_color[3], clear_color[1] * clear_color[3], clear_color[2] * clear_color[3], clear_color[3]);
            renderPassDescriptor.colorAttachments[0].texture = drawable.texture;
            renderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
            renderPassDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> renderEncoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
            [renderEncoder pushDebugGroup:@"Letter Flow"];

            // Start the Dear ImGui frame
            ImGui_ImplMetal_NewFrame(renderPassDescriptor);
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();

            // 2. Advance the simulation (fixed timestep, see step_world).
            step_world(world, io.DeltaTime, tick_accumulator);

            // 3. Build this frame's UI. Order matters: the dockspace hosts the
            //    windows, the map goes into the background draw list (under the
            //    windows), the transport bar is drawn on top of the map, then
            //    the HUD/inspector windows.
            DrawDockspace();
            DrawWorld(world);
            DrawTransportBar(world);
            DrawHUD(world);
            DrawInspector(world);

            // 4. Rendering
            ImGui::Render();
            ImDrawData* draw_data = ImGui::GetDrawData();
            ImGui_ImplMetal_RenderDrawData(draw_data, commandBuffer, renderEncoder);

            [renderEncoder popDebugGroup];
            [renderEncoder endEncoding];

            [commandBuffer presentDrawable:drawable];
            [commandBuffer commit];
        }
    }

    // Autosave on quit so the next launch continues this session.
    if (world.save_to_file(save_path))
        printf("Saved game to %s\n", save_path.c_str());
    else
        printf("Warning: failed to save game to %s\n", save_path.c_str());

    // Cleanup
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
