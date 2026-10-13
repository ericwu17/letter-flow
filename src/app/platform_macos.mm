// platform_macos.mm — macOS-specific startup tweaks (Objective-C++).
// See platform_macos.h; compiled only into the macOS build.

#import <Foundation/Foundation.h>

void ApplyMacOSTweaks()
{
    // Disable the press and hold popup on apple (that shows the different
    // accent characters for the held down key), so holding WASD pans the map
    // with normal key repeat like on other platforms.
    [[NSUserDefaults standardUserDefaults] setBool:NO forKey:@"ApplePressAndHoldEnabled"];
}
