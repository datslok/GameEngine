#include "core/startup_clock.h"

#include <cstdio>
#include <utility>

namespace {
    // Seconds to two places, with the unit.
    std::string secondsText(double seconds) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f s", seconds);
        return text;
    }
}

StartupClock::StartupClock(double startSeconds):
    start(startSeconds),
    last(startSeconds)
{
}

void StartupClock::lap(std::string name, double nowSeconds) {
    recorded.push_back(Lap{std::move(name), nowSeconds - last});
    last = nowSeconds;
}

const std::vector<StartupClock::Lap>& StartupClock::laps() const {
    return recorded;
}

double StartupClock::total() const {
    return last - start;
}

std::string StartupClock::describe(std::string_view title) const {
    std::string line = std::string(title) + " " + secondsText(total());

    for (std::size_t index = 0; index < recorded.size(); ++index) {
        line += (index == 0 ? ": " : ", ") + recorded[index].name + " " + secondsText(recorded[index].seconds);
    }

    return line;
}
