// app_icon.cpp — see app_icon.h.

#include "app/app_icon.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <SDL.h>

// Only PNG decoding from memory is needed; skip the other formats and stdio.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb/stb_image.h"

// Generated from assets/logo.png by the makefiles: defines kLogoPng[].
#include "logo_png.h"

void SetAppIcon(SDL_Window* window)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(kLogoPng, (int)sizeof(kLogoPng), &width, &height, &channels, 4);
    if (pixels == nullptr)
    {
        printf("Warning: could not decode app icon: %s\n", stbi_failure_reason());
        return;
    }

    // Icons are displayed square: center the logo on a transparent square
    // canvas so a non-square logo isn't stretched.
    const int side = width > height ? width : height;
    const int offset_x = (side - width) / 2;
    const int offset_y = (side - height) / 2;
    std::vector<unsigned char> square((size_t)side * (size_t)side * 4, 0);
    for (int y = 0; y < height; ++y)
    {
        const size_t src = (size_t)y * (size_t)width * 4;
        const size_t dst = ((size_t)(y + offset_y) * (size_t)side + (size_t)offset_x) * 4;
        std::memcpy(&square[dst], &pixels[src], (size_t)width * 4);
    }
    stbi_image_free(pixels);

    // RGBA bytes in memory order, whatever the host endianness.
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(square.data(), side, side, 32, side * 4, SDL_PIXELFORMAT_RGBA32);
    if (surface == nullptr)
    {
        printf("Warning: could not create app icon surface: %s\n", SDL_GetError());
        return;
    }
    SDL_SetWindowIcon(window, surface);  // SDL copies the pixels
    SDL_FreeSurface(surface);
}
