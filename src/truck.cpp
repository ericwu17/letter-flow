// truck.cpp — truck motion and cargo implementation. Pure logic, no
// rendering, no input.
#include "truck.h"

#include <cmath>
#include <utility>

float Truck::route_length() const {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    return std::sqrt(dx * dx + dy * dy);
}

Position Truck::get_position(Tick now) const {
    const float length = route_length();
    if (length < 1e-6f)
        return to;
    const Tick elapsed = (now > departure_tick) ? (now - departure_tick) : 0;
    float fraction = (static_cast<float>(elapsed) * speed) / length;
    if (fraction > 1.0f)
        fraction = 1.0f;
    return {
        from.x + (to.x - from.x) * fraction,
        from.y + (to.y - from.y) * fraction,
    };
}

bool Truck::has_arrived(Tick now) const {
    const Tick elapsed = (now > departure_tick) ? (now - departure_tick) : 0;
    return static_cast<float>(elapsed) * speed >= route_length();
}

void Truck::push_back_letter(Letter letter) {
    carried_letters.push_back(std::move(letter));
}

std::size_t Truck::get_num_letters() const {
    return carried_letters.size();
}

const std::vector<Letter>& Truck::get_letters() const {
    return carried_letters;
}
std::vector<Letter> Truck::take_letters() {
    std::vector<Letter> taken;
    taken.swap(carried_letters);
    return taken;
}
