// truck.cpp — truck motion and cargo implementation. Pure logic, no
// rendering, no input.
#include "truck.h"

#include <cmath>
#include <utility>

float Truck::route_length() const {
    const float dx = state.to.x - state.from.x;
    const float dy = state.to.y - state.from.y;
    return std::sqrt(dx * dx + dy * dy);
}

Position Truck::get_position(Tick now) const {
    const float length = route_length();
    if (length < 1e-6f)
        return state.to;
    const Tick elapsed = (now > state.departure_tick) ? (now - state.departure_tick) : 0;
    float fraction = (static_cast<float>(elapsed) * state.speed) / length;
    if (fraction > 1.0f)
        fraction = 1.0f;
    return {
        state.from.x + (state.to.x - state.from.x) * fraction,
        state.from.y + (state.to.y - state.from.y) * fraction,
    };
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
