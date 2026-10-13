// platform_macos.h — macOS-specific startup tweaks.
//
// main.cpp is shared by all platforms; anything that needs Cocoa lives here
// instead, implemented in platform_macos.mm (Objective-C++) and compiled
// only into the macOS build.
#pragma once

// Applies macOS-specific settings. Called once at startup from main.cpp.
void ApplyMacOSTweaks();
