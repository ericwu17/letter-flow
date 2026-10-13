// tutorial.h — the tutorial's steps and progress. Pure simulation layer: no
// imgui/SDL/OpenGL headers (see game_types.h); ui_tutorial.cpp draws it.
//
// The tutorial plays in its own world (create_tutorial_world, scenario.h) and
// never touches the save library: nothing about it outlives the session, not
// even whether it was finished.
//
// Each step either waits for "Next" (an info card) or checks the World and
// a few facts about the UI every frame and completes on its own once the
// player has done what it asks. Nothing is ever locked: the player may act
// out of order, and steps whose goal is already met are skipped over.
#pragma once

#include "entities.h"
#include "world.h"

#include <array>
#include <cstddef>

// UI state the steps can depend on, reported by the presentation layer.
struct TutorialUiFacts {
    PostOfficeId selected_office = kNoPostOffice;  // shown in the inspector
    bool truck_selected = false;
    bool building_road = false;  // road-building mode is active
    bool stats_open = false;     // the statistics window is open
};

// On-screen elements a step can point at. The UI records where each one was
// drawn this frame (ui::NoteAnchor); the tutorial spotlights the step's.
enum class TutorialAnchor {
    None,
    BuildOfficeButton,  // "Build post office" (an unbuilt office's inspector)
    BuildRoadButton,    // "Build a road..."
    TransportBar,       // play/pause and speed
    OutboundLetters,    // a built office's outbound letter list
    ScheduleTable,      // a built office's schedule table
    AddScheduleForm,    // the "New schedule" form
    SettingsHeader,     // the office's collapsed "Settings" section...
    PostalCodeInput,    // ...and the postal-code field inside it
    StatsButton,        // the HUD's statistics button
    Count,
};

struct TutorialStep {
    const char* title;
    const char* body;
    // The office the step happens at. Until it is selected, the map rings it
    // (the step's text says to select it); once it is, `anchor` is spotlit.
    PostOfficeId office = kNoPostOffice;
    TutorialAnchor anchor = TutorialAnchor::None;
    // Further offices to ring on the map while the step is current (a road's
    // two ends, the offices left to build), regardless of the selection.
    std::array<PostOfficeId, 3> rings = {kNoPostOffice, kNoPostOffice, kNoPostOffice};
    // True once the player has done what the step asks. nullptr makes it an
    // info card, which only "Next" moves past.
    bool (*done)(const World&, const TutorialUiFacts&) = nullptr;
};

class Tutorial {
public:
    // Moves past the current step, and every step after it, whose goal is
    // already met. Stops at the first info card or unmet step. Call once a
    // frame.
    void update(const World& world, const TutorialUiFacts& facts);
    // "Next" on an info card, "Skip" on any other.
    void next();

    bool finished() const { return step >= count(); }
    // The current step. Only valid while !finished().
    const TutorialStep& current() const;
    std::size_t index() const { return step; }
    static std::size_t count();

private:
    std::size_t step = 0;
};
