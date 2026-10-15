// events.h — pure simulation layer: what network events (NetworkEvent,
// entities.h) are called and how they are described. The World picks an
// event's name by index when it announces it; these turn that index and the
// event's offices into text for the UI and the tests.
#pragma once

#include "sim/entities.h"

#include <cstddef>
#include <string>
#include <vector>

// How many names events of this type can go by (NetworkEvent::name is less).
std::size_t event_name_count(EventType type);

// The event's name with its offices filled in, e.g. "Briarwood County Fair"
// or "Election ballots: Ashford to Dunmore".
std::string event_title(const NetworkEvent& event, const std::vector<PostOffice>& offices);

// What the event does, e.g. "Lots of mail to Briarwood" or "Lots of mail
// from Ashford to Dunmore".
std::string event_summary(const NetworkEvent& event, const std::vector<PostOffice>& offices);
