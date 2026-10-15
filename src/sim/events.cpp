// events.cpp — network event names and descriptions (see events.h).
#include "sim/events.h"

#include <array>
#include <span>
#include <string_view>

namespace {

// Name templates: "{src}" and "{dst}" stand for the event's offices.
constexpr std::array<std::string_view, 6> kSurgeNames = {
    "{dst} County Fair",
    "Founders' Day in {dst}",
    "Holiday cards to {dst}",
    "{dst} Music Festival",
    "Tax season in {dst}",
    "The {dst} Marathon",
};

constexpr std::array<std::string_view, 5> kCorridorNames = {
    "Election ballots: {src} to {dst}",
    "{src}-{dst} trade show",
    "{src} students write home to {dst}",
    "Catalog season: {src} to {dst}",
    "{src} vs {dst} pen-pal week",
};

const std::string& office_name(PostOfficeId id, const std::vector<PostOffice>& offices) {
    static const std::string kUnknown = "?";
    return id < offices.size() ? offices[id].name : kUnknown;
}

// `text` with every "{src}" / "{dst}" replaced by those offices' names.
std::string fill(std::string_view text, const NetworkEvent& event, const std::vector<PostOffice>& offices) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (text.substr(i).starts_with("{src}")) {
            out += office_name(event.src, offices);
            i += 5;
        } else if (text.substr(i).starts_with("{dst}")) {
            out += office_name(event.dst, offices);
            i += 5;
        } else {
            out += text[i++];
        }
    }
    return out;
}

}  // namespace

std::size_t event_name_count(EventType type) {
    return type == EventType::Surge ? kSurgeNames.size() : kCorridorNames.size();
}

std::string event_title(const NetworkEvent& event, const std::vector<PostOffice>& offices) {
    const auto& names = event.type == EventType::Surge ? std::span<const std::string_view>(kSurgeNames)
                                                       : std::span<const std::string_view>(kCorridorNames);
    return fill(names[event.name % names.size()], event, offices);
}

std::string event_summary(const NetworkEvent& event, const std::vector<PostOffice>& offices) {
    return fill(event.type == EventType::Surge ? "Lots of mail to {dst}" : "Lots of mail from {src} to {dst}",
                event, offices);
}
