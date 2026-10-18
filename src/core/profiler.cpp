#include "core/profiler.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

Profiler::Profiler(double windowSeconds)
    : windowSeconds(windowSeconds) {
    if (!(windowSeconds > 0.0)) {
        throw std::invalid_argument("Profiler: the window must be longer than zero");
    }
}

int Profiler::addSection(std::string name) {
    names.push_back(std::move(name));
    collecting.emplace_back();
    shown.emplace_back();
    return static_cast<int>(names.size()) - 1;
}

void Profiler::record(int section, double seconds) {
    Collecting& sofar = collecting.at(section);
    sofar.totalSeconds += seconds;
    sofar.worstSeconds = std::max(sofar.worstSeconds, seconds);
    ++sofar.calls;
}

/*
* A section that did not run in a window shows zeros rather than its old numbers, so a system that stopped running does
* not look as if it still costs something.
*/
void Profiler::update(double nowSeconds) {
    if (!started) {
        started = true;
        windowStart = nowSeconds;
        return;
    }
    if (nowSeconds - windowStart < windowSeconds) {
        return;
    }

    for (std::size_t section = 0; section < collecting.size(); ++section) {
        const Collecting& sofar = collecting[section];
        SectionTiming& timing = shown[section];
        timing.calls = sofar.calls;
        timing.worstSeconds = sofar.worstSeconds;
        timing.averageSeconds = sofar.calls > 0 ? sofar.totalSeconds / sofar.calls : 0.0;
        collecting[section] = Collecting{};
    }
    windowStart = nowSeconds;
}

const SectionTiming& Profiler::getTiming(int section) const {
    return shown.at(section);
}

const std::string& Profiler::getName(int section) const {
    return names.at(section);
}

int Profiler::getSectionCount() const {
    return static_cast<int>(names.size());
}

/*
* steady_clock rather than system_clock, which jumps when the computer corrects its time. On Windows it reads the
* high-resolution performance counter (about 100 ns steps, a few tens of nanoseconds to read), far finer and cheaper
* than anything worth timing here.
*/
double profilerSeconds() {
    const auto sinceStart = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(sinceStart).count();
}

ScopedTimer::ScopedTimer(Profiler& profiler, int section)
    : profiler(profiler), section(section), start(profilerSeconds()) {
}

ScopedTimer::~ScopedTimer() {
    profiler.record(section, profilerSeconds() - start);
}
