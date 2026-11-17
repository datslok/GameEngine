#include "test_progress.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

std::map<std::string, double> parseTestTimes(std::string_view text) {
    std::map<std::string, double> times;
    std::istringstream lines{std::string{text}};
    std::string line;

    while (std::getline(lines, line)) {
        std::istringstream fields{line};
        double seconds = 0.0;
        std::string name;

        if (fields >> seconds >> name && seconds >= 0.0) {
            times[name] = seconds;
        }
    }

    return times;
}

std::string formatTestTimes(const std::vector<std::pair<double, const char*>>& times) {
    std::string text;
    char number[32];

    for (const auto& [seconds, name] : times) {
        std::snprintf(number, sizeof(number), "%.3f ", seconds);
        text += number;
        text += name;
        text += '\n';
    }

    return text;
}

std::vector<double> expectedTestSeconds(const std::vector<std::string>& names, const std::map<std::string, double>& last) {
    double known = 0.0;
    std::size_t knownCount = 0;

    for (const std::string& name : names) {
        if (const auto found = last.find(name); found != last.end()) {
            known += found->second;
            ++knownCount;
        }
    }

    const double fallback = knownCount > 0 ? known / static_cast<double>(knownCount) : 1.0;
    std::vector<double> expected;
    expected.reserve(names.size());

    for (const std::string& name : names) {
        const auto found = last.find(name);
        expected.push_back(found != last.end() ? found->second : fallback);
    }

    return expected;
}

// The pace so far scales the expected rest: a run going slower than last time (a busy machine) says so.
std::string progressLine(const ProgressState& state, int barWidth) {
    const double share = state.expectedTotal > 0.0 ? std::clamp(state.expectedDone / state.expectedTotal, 0.0, 1.0) : 0.0;
    const int filled = std::clamp(static_cast<int>(share * barWidth), 0, barWidth);
    const double rest = std::max(state.expectedTotal - state.expectedDone, 0.0);
    const double left = state.expectedDone > 0.0 ? rest * state.elapsed / state.expectedDone : rest;

    char numbers[96];
    std::snprintf(numbers, sizeof(numbers), "] %3d%%  %zu/%zu  ~%ld s left  ", static_cast<int>(share * 100.0), state.done, state.total,
                  std::lround(left));
    return "[" + std::string(static_cast<std::size_t>(filled), '#') + std::string(static_cast<std::size_t>(barWidth - filled), '-') + numbers + state.current;
}

namespace {
    constexpr int progressBarWidth = 30;
}

ConsoleProgress::ConsoleProgress() {
#ifdef _WIN32
    console = std::fopen("CONOUT$", "w");
#endif
}

ConsoleProgress::~ConsoleProgress() {
    if (console != nullptr) {
        std::fclose(console);
    }
}

// A line a test, scrolling: the last line shown is the test running, so a failing one is named just above its message.
void ConsoleProgress::show(const ProgressState& state) {
    if (console == nullptr) {
        return;
    }

    std::fputs((progressLine(state, progressBarWidth) + "\n").c_str(), console);
    std::fflush(console);
}

void ConsoleProgress::finish(const ProgressState& state) {
    if (console == nullptr) {
        return;
    }

    char line[96];
    std::snprintf(line, sizeof(line), "] 100%%  %zu/%zu  done in %ld s\n", state.done, state.total, std::lround(state.elapsed));
    std::fputs(("[" + std::string(static_cast<std::size_t>(progressBarWidth), '#') + line).c_str(), console);
    std::fflush(console);
}
