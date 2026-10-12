// truck.cpp — truck motion and cargo implementation. Pure logic, no
// rendering, no input.
#include "truck.h"

#include <cmath>
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
    return distance_travelled(now) >= route_length();
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
