// truck.cpp — truck motion and cargo implementation. Pure logic, no
// rendering, no input.
#include "sim/truck.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

double Truck::route_length() const {
    double sum = 0;
    for (double d : state.distances) {
        sum += d;
    }
    return sum;
}

double Truck::distance_travelled(Tick now) const {
    const Tick elapsed = (now > state.departure_tick) ? (now - state.departure_tick) : 0;
    return static_cast<double>(elapsed) * static_cast<double>(state.speed);
}

Position Truck::get_position(Tick now) const {
    double dist = distance_travelled(now);

    for (std::size_t i = 0; i < state.distances.size(); ++i) {
        const double segment = state.distances[i];
        if (dist >= segment) {
            dist -= segment;  // also skips zero-length segments (old saves)
            continue;
        }
        // 0 <= dist < segment, so the division is well defined.
        const double fraction = dist / segment;
        const Position from = state.vertices[i];
        const Position step = state.vertices[i + 1] - from;
        return {
            from.x + std::llround(static_cast<double>(step.x) * fraction),
            from.y + std::llround(static_cast<double>(step.y) * fraction),
        };
    }
    return state.vertices.back();
}

bool Truck::has_arrived(Tick now) const {
    // distance_travelled() is 0 until departure and grows with every tick
    // after it, so the truck has arrived exactly from arrival_tick on, with
    // times before departure counting as departure itself.
    return std::max(now, state.departure_tick) >= arrival_tick;
}

Tick Truck::compute_arrival_tick() const {
    // Smallest elapsed tick count with elapsed * speed >= route_length.
    // ceil() gives the estimate; the loops correct for floating-point
    // rounding in either direction.
    const double length = route_length();
    const double speed = static_cast<double>(state.speed);
    if (speed <= 0.0)  // never moves: only an empty route is already done
        return length <= 0.0 ? state.departure_tick : std::numeric_limits<Tick>::max();
    Tick travel = static_cast<Tick>(std::ceil(length / speed));
    while (travel > 0 && static_cast<double>(travel - 1) * speed >= length)
        --travel;
    while (static_cast<double>(travel) * speed < length)
        ++travel;
    return state.departure_tick + travel;
}

void Truck::push_back_letter(Letter letter) {
    state.carried_letters.push_back(std::move(letter));
}

std::size_t Truck::get_num_letters() const {
    return state.carried_letters.size();
}

const std::vector<Letter>& Truck::get_letters() const {
    return state.carried_letters;
}

std::vector<Letter> Truck::take_letters() {
    std::vector<Letter> taken;
    taken.swap(state.carried_letters);
    return taken;
}
