// app_icon.h — the window / taskbar / Dock icon.
//
// assets/logo.png is embedded into the executable at build time (the
// makefiles generate logo_png.h from it), so the binary needs no assets
// directory at runtime.
#pragma once

struct SDL_Window;

// Decodes the embedded logo and sets it as the window icon. On macOS SDL
// also uses it as the Dock icon. Failure is non-fatal: the window just keeps
// the default icon.
void SetAppIcon(SDL_Window* window);
