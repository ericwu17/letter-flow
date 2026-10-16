// entities.h — pure simulation layer: passive data entities (letters, truck
// schedules, post offices). No imgui/SDL/OpenGL headers (see game_types.h).
#pragma once

#include "sim/game_types.h"
#include "sim/stats.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// Post offices live in World::post_offices and are referenced everywhere by
// index (a "handle") instead of raw pointer. Indices stay valid no matter how
// the vector grows — pointers would dangle on every reallocation.
using PostOfficeId = std::size_t;
inline constexpr PostOfficeId kNoPostOffice = std::numeric_limits<PostOfficeId>::max();

// Truck schedules are stored by value in their office's list and move around
// in it (reordering, removal), so anything that must find a schedule again
// later — a truck crediting income on arrival, a letter's trail — names it by
// this id instead. Ids are handed out by the World and never reused.
using ScheduleId = std::uint64_t;
inline constexpr ScheduleId kNoSchedule = std::numeric_limits<ScheduleId>::max();

// One step of a letter's journey (see Letter::trail).
enum class TrailEvent {
    Spawned,   // created at `office`
    Departed,  // left `office` on a truck of schedule `schedule`
    Arrived,   // reached `office`: delivered if it is the letter's dst, else forwarded
};

struct TrailStop {
    Tick tick = 0;
    TrailEvent event = TrailEvent::Spawned;
    PostOfficeId office = kNoPostOffice;
    ScheduleId schedule = kNoSchedule;  // Departed only

    bool operator==(const TrailStop&) const = default;
};

// Every entity provides full-state member-wise equality, defaulted so fields
// added later are compared automatically. This backs the save/load round-trip
// test (tests/test_serialization.cpp): its fixture gives every serialized
// field a non-default value, so a field forgotten in serialization.cpp comes
// back from a save as its default and the test fails loudly instead of the
// data being quietly lost. Truck defaults its own the same way. World's is
// hand-written (it owns trucks through pointers and must compare the trucks,
// not the pointers), so a new World field must also be added there.
struct Letter {
    PostOfficeId src = kNoPostOffice;
    PostOfficeId dst = kNoPostOffice;
    Tick created = 0;   // tick the letter spawned; the early bonus shrinks linearly
                        // from max_bonus at `created` to 0 at `deadline`
    Tick deadline = 0;  // delivered by this tick -> earn an early bonus on top of value
    int value = 0;      // always earned on delivery, on time or late
    int max_bonus = 0;  // extra earned if delivered instantly; scaled by time left
    bool event = false; // posted for a NetworkEvent rather than as everyday mail
    // Where the letter has been, oldest first: Spawned, then a Departed /
    // Arrived pair per truck ride. Empty for letters from saves that predate
    // trails. Bounded by the number of hops, i.e. a handful of entries.
    std::vector<TrailStop> trail;

    // When the letter last arrived at (or spawned in) the office it is in.
    Tick waiting_since() const { return trail.empty() ? created : trail.back().tick; }

    // Truck rides completed so far: every Departed -> Arrived pair in the trail.
    std::size_t hops() const {
        std::size_t count = 0;
        for (std::size_t i = 1; i < trail.size(); ++i)
            if (trail[i - 1].event == TrailEvent::Departed && trail[i].event == TrailEvent::Arrived)
                ++count;
        return count;
    }

    // Total time spent on those rides. Whatever else of its age the letter
    // has spent waiting in offices.
    Tick transit_ticks() const {
        Tick transit = 0;
        for (std::size_t i = 1; i < trail.size(); ++i)
            if (trail[i - 1].event == TrailEvent::Departed && trail[i].event == TrailEvent::Arrived)
                transit += trail[i].tick - trail[i - 1].tick;
        return transit;
    }

    bool operator==(const Letter&) const = default;
};

// A letter that reached its destination, kept (with its trail) in the
// World's recent-deliveries log so finished journeys can still be inspected.
// The delivery tick is the trail's last stop (or `delivered`, for letters
// from saves that predate trails).
struct DeliveredLetter {
    Letter letter;
    Tick delivered = 0;
    int earned = 0;  // value plus early bonus

    bool operator==(const DeliveredLetter&) const = default;
};

// How many delivered letters the World remembers, newest last.
inline constexpr std::size_t kRecentDeliveriesSize = 100;

// A burst of extra mail that strains part of the network for a while (see
// the Events section of balance.h). The World announces it kEventWarning
// before `start`, then posts its `letters` evenly over [start, end). Event
// letters are ordinary letters (flagged Letter::event): same deadlines,
// values, routing and lost-when-full rule.
enum class EventType {
    Surge,     // every other office mails `dst`, each in proportion to its size
    Corridor,  // `src` mails `dst`
};

struct NetworkEvent {
    EventType type = EventType::Surge;
    PostOfficeId src = kNoPostOffice;  // Corridor only
    PostOfficeId dst = kNoPostOffice;
    Tick start = 0;           // first tick letters are posted
    Tick end = 0;             // first tick after the event
    std::size_t letters = 0;  // how many the event posts in all (lost ones included)
    std::size_t posted = 0;   // how many it has posted (or tried to) so far
    std::size_t name = 0;     // which of its type's names it goes by (events.h)

    bool operator==(const NetworkEvent&) const = default;
};

// A change in money at an office: a truck arrival that paid out (`amount` > 0,
// the total earned — values plus early bonuses — across every letter finally
// delivered at `office`) or a truck departure that was paid for (`amount` < 0).
// A pure presentation hint — not part of the saved or compared simulation state.
struct MoneyEvent {
    PostOfficeId office = kNoPostOffice;
    int amount = 0;
};

// Which letters a departing truck takes from its office's outbound buffer.
// A schedule's rule is evaluated at departure time against the destination
// offices' *current* postal codes, so runtime edits take effect immediately.
enum class RoutingRuleType {
    ExactDestination,  // only letters addressed to the schedule's dst office
    PostalPrefix,      // letters addressed to offices whose postal code starts
                       // with any of `prefixes` — hub-style collection; letters not
                       // addressed to dst are forwarded on arrival
    AllLetters,        // the whole buffer, regardless of address
};

struct RoutingRule {
    RoutingRuleType type = RoutingRuleType::ExactDestination;
    // Only meaningful for PostalPrefix, where it is required non-empty with no
    // empty element; a letter matches if its postal code starts with any of them.
    std::vector<std::string> prefixes;

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
            for (const std::string& prefix : prefixes)
                if (!prefix.empty() && dst_postal_code.starts_with(prefix))
                    return true;
            return false;
        case RoutingRuleType::AllLetters:
            return true;
        }
        return false;
    }

    // False for a PostalPrefix rule with no prefix, an empty one (such a rule
    // must be an explicit AllLetters rule instead) or one listed twice.
    bool is_well_formed() const {
        if (type != RoutingRuleType::PostalPrefix)
            return true;
        if (prefixes.empty())
            return false;
        for (std::size_t i = 0; i < prefixes.size(); ++i) {
            if (prefixes[i].empty())
                return false;
            for (std::size_t j = 0; j < i; ++j)
                if (prefixes[j] == prefixes[i])
                    return false;
        }
        return true;
    }

    bool operator==(const RoutingRule&) const = default;
};

// Why World::add_schedule / update_schedule refused a schedule (see
// World::check_add_schedule), in the order they are checked.
enum class ScheduleError {
    None,           // valid
    UnknownOffice,  // src or dst is not an office id (or, for an update, no such schedule)
    SameOffice,     // src == dst
    MalformedRule,  // RoutingRule::is_well_formed() is false
    NoRoad,         // no road connects src and dst
};

// One departure of a schedule, for its utilization log.
struct DepartureRecord {
    Tick tick = 0;
    std::uint32_t loaded = 0;       // letters that boarded
    std::uint32_t left_behind = 0;  // matching letters that did not fit

    bool operator==(const DepartureRecord&) const = default;
};

// Most recent departures (and edits) a schedule remembers.
inline constexpr std::size_t kDepartureLogSize = 50;
inline constexpr std::size_t kScheduleEditLogSize = 20;

// A schedule has no timetable: it sends a truck whenever enough outbound
// letters match its rule to fill one, or a matching letter has waited
// kMaxLetterWait (see World::ready_to_depart).
struct TruckSchedule {
    ScheduleId id = kNoSchedule;       // assigned by World::add_schedule
    PostOfficeId dst = kNoPostOffice;  // src is implicitly the office that owns this schedule
    RoutingRule rule;                  // which outbound letters the truck picks up
    bool enabled = true;               // off: never departs (switched off by the player, or
                                       // by destroying its road; only a schedule whose road
                                       // exists can be switched back on)

    // Utilization history. Kept across update_schedule() edits (it is the same
    // schedule, retuned), with the edit ticks recorded so a chart can mark them.
    ScheduleStats stats;
    std::vector<DepartureRecord> departure_log;  // oldest first, at most kDepartureLogSize
    std::vector<Tick> edits;                     // oldest first, at most kScheduleEditLogSize

    bool operator==(const TruckSchedule&) const = default;
};

struct PostOffice {
    Position pos;
    std::string name;
    std::string postal_code;               // hierarchical code, e.g. "NJ/Mercer/Princeton", set
                                           // by the player (offices appear with none);
                                           // PostalPrefix routing rules match on prefixes of it
    std::size_t max_outbound_letters = 0;  // game-rule cap on the buffer (not vector::capacity!)
    std::size_t letters_per_day = 0;       // expected letters per day: each tick spawns
                                           // one with probability letters_per_day / kTicksPerDay
    Tick overflow_ticks = 0;               // how long the buffer has (net) been full: +1 per
                                           // tick at or over the cap, -1 per tick below it;
                                           // the game ends at kOverflowLimit
    std::vector<Letter> outbound_letters;
    std::vector<TruckSchedule> outbound_schedules;
    OfficeStats stats;

    bool operator==(const PostOffice&) const = default;
};
