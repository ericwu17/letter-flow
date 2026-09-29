// entities.h — pure simulation layer: passive data entities (letters, truck
// schedules, post offices). No imgui/SDL/Metal headers (see game_types.h).
#pragma once

#include "game_types.h"

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

// Post offices live in World::post_offices and are referenced everywhere by
// index (a "handle") instead of raw pointer. Indices stay valid no matter how
// the vector grows — pointers would dangle on every reallocation.
using PostOfficeId = std::size_t;
inline constexpr PostOfficeId kNoPostOffice = std::numeric_limits<PostOfficeId>::max();

// Every entity provides full-state member-wise equality, defaulted so fields
// added later are compared automatically. This backs the save/load round-trip
// test (tests/test_serialization.cpp): its fixture gives every serialized
// field a non-default value, so a field forgotten in serialization.cpp comes
// back from a save as its default and the test fails loudly instead of the
// data being quietly lost. Truck's fields live in a plain-data TruckState
// with defaulted comparison, so its operator== is defaulted too; only World
// maintains its operator== by hand — see the notes there.
struct Letter {
    PostOfficeId src = kNoPostOffice;
    PostOfficeId dst = kNoPostOffice;
    Tick deadline = 0;  // delivered by this tick -> earn value; later -> pay fine
    int value = 0;      // earned on on-time delivery
    int fine = 0;       // paid on late delivery

    bool operator==(const Letter&) const = default;
};

// Which letters a departing truck takes from its office's outbound buffer.
// A schedule's rule is evaluated at departure time against the destination
// offices' *current* postal codes, so runtime edits take effect immediately.
enum class RoutingRuleType {
    ExactDestination,  // only letters addressed to the schedule's dst office
    PostalPrefix,      // letters addressed to offices whose postal code starts
                       // with `prefix` — hub-style collection; letters not
                       // addressed to dst are forwarded on arrival
    AllLetters,        // the whole buffer, regardless of address
};

struct RoutingRule {
    RoutingRuleType type = RoutingRuleType::ExactDestination;
    std::string prefix;  // only meaningful (and required non-empty) for PostalPrefix

    // `letter_dst` is the office the letter is addressed to and
    // `dst_postal_code` its current postal code; `schedule_dst` is the office
    // the truck departs for.
    bool matches(PostOfficeId letter_dst, const std::string& dst_postal_code,
                 PostOfficeId schedule_dst) const {
        switch (type) {
        case RoutingRuleType::ExactDestination:
            return letter_dst == schedule_dst;
        case RoutingRuleType::PostalPrefix:
            // An empty prefix would match everything; treat it as matching
            // nothing so a misconfigured rule never silently acts as AllLetters.
            return !prefix.empty() && dst_postal_code.starts_with(prefix);
        case RoutingRuleType::AllLetters:
            return true;
        }
        return false;
    }

    bool operator==(const RoutingRule&) const = default;
};

struct TruckSchedule {
    PostOfficeId dst = kNoPostOffice;  // src is implicitly the office that owns this schedule
    RoutingRule rule;                  // which outbound letters the truck picks up
    Tick period = 0;                   // one departure every `period` ticks (must be > 0)
    Tick start_offset = 0;             // time of day of the first possible departure, in ticks
                                       // since midnight; departures fall on the grid
                                       // start_offset + k * period (k = 0, 1, 2, ...)
    Tick next_departure = 0;           // absolute tick of the next departure

    bool operator==(const TruckSchedule&) const = default;
};

struct PostOffice {
    Position pos;
    std::string name;
    std::string postal_code;               // hierarchical code, e.g. "NJ/Mercer/Princeton";
                                           // PostalPrefix routing rules match on prefixes of it
    std::size_t max_outbound_letters = 0;  // game-rule cap on the buffer (not vector::capacity!)
    std::size_t letters_per_day = 0;       // expected letters per day: each tick spawns
                                           // one with probability letters_per_day / kTicksPerDay
    std::vector<Letter> outbound_letters;
    std::vector<TruckSchedule> outbound_schedules;

    bool operator==(const PostOffice&) const = default;
};
