#include <iostream>
#include <cstddef>
#include <string>
#include <vector>

// game runs at 60 ticks per second.
// each "day" in the game is 3*60 seconds = 10800 ticks.

struct Letter;
struct Position;
struct PostOffice;
struct Truck;
struct TruckSchedule;

struct Position {
    float x;
    float y;
};


struct PostOffice {
    Position pos;
    std::string name;
    std::size_t max_outbound_letters;
    std::size_t letters_per_day;
    std::vector<Letter> outbound_letters;
    std::vector<TruckSchedule> outbound_schedules;

    void generate_letters(std::size_t curr_tick);  // this member function adds a bunch of letters to this PostOffice's buffer, if at the end of the day tick;
    void run_schedules(std::size_t curr_tick);      // this member function, called every tick, will generate new trucks carrying outbound letters.
};

// TODO: this is a struct that defines how a truck schedule determines which letters to pick up.
struct RoutingRule {

};

struct TruckSchedule {
    PostOffice* src;
    PostOffice* dst;
    int period;  // this schedule fires once every period ticks.
    RoutingRule routing_rule;
};

struct Truck {
    PostOffice* src;
    PostOffice* dst;
    Position curr_pos;
    float speed; // represented in units per tick
    std::vector<Letter> carried_letters;
    
    // to be called once per tick
    void advance();
};

struct Letter {
    PostOffice* src;
    PostOffice* dst;
    std::size_t deadline;  // timestamp
    int value;
    int fine;
};