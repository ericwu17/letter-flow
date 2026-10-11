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

Position Truck::get_position(Tick now) const {
    const float length = route_length();
    if (length < 1e-6f) {
        return state.vertices.back();
    }
    const Tick elapsed = (now > state.departure_tick) ? (now - state.departure_tick) : 0;
    double dist = static_cast<double>(elapsed) * state.speed;
    


    for (size_t i = 0; i + 1 < state.vertices.size(); i++) {
        if (dist > state.distances[i]) {
            dist -= state.distances[i];
        } else {
            float fraction = dist / state.distances[i];
            return {
                (int64_t)(state.vertices[i].x + (state.vertices[i+1].x - state.vertices[i].x) * fraction),
                (int64_t)(state.vertices[i].y + (state.vertices[i+1].y - state.vertices[i].y) * fraction),
            };
        }
    }
    return state.vertices.back();
}

bool Truck::has_arrived(Tick now) const {
    const Tick elapsed = (now > state.departure_tick) ? (now - state.departure_tick) : 0;
    return static_cast<float>(elapsed) * state.speed >= route_length();
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
