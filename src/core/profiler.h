#pragma once

#include <string>
#include <vector>

// How long the profiler collects before it shows new numbers: long enough to read them, short enough to follow changes.
constexpr double defaultProfileWindowSeconds = 0.5;

// What one section cost during the last finished window.
struct SectionTiming {
    double averageSeconds = 0.0; // per call
    double worstSeconds = 0.0;   // the slowest single call
    int calls = 0;
};

/*
* Times named pieces of work (a system, the renderer, a whole frame). Sections are added once and recorded by number,
* so recording is an addition into an array, with no lookups or allocations per frame.
*
* Single measurements are noisy (the OS pauses the thread, caches are warm or cold) and ticks do not fall one per frame,
* so the profiler counts per call and collects for a fixed window before showing anything: the average and the worst
* call of the window that just ended, which stay put until the next one ends. Readable text, and spikes are not
* averaged away.
*/
class Profiler {
public:
    explicit Profiler(double windowSeconds = defaultProfileWindowSeconds);

    // Add a section and return its number (0 for the first, then 1, 2 ...).
    int addSection(std::string name);

    // One call of a section took this long.
    void record(int section, double seconds);

    // Call once a frame with the current time (profilerSeconds()). The first call starts the first window; when a window has
    // passed, every section's numbers are replaced by that window's and collecting starts again.
    void update(double nowSeconds);

    const SectionTiming& getTiming(int section) const;
    const std::string& getName(int section) const;
    int getSectionCount() const;

private:
    // What a section has collected so far in the current window.
    struct Collecting {
        double totalSeconds = 0.0;
        double worstSeconds = 0.0;
        int calls = 0;
    };

    double windowSeconds;
    double windowStart = 0.0;
    bool started = false;

    std::vector<std::string> names;
    std::vector<Collecting> collecting;
    std::vector<SectionTiming> shown;
};

// A steady clock in seconds: it only goes forwards, at a constant rate, unlike the wall clock. Only differences mean anything.
double profilerSeconds();

/*
* Times its own block: it reads the clock when made and records the difference into the section when the block ends,
* however it ends (an early return or an exception), so the stopwatch can never be left running.
*/
class ScopedTimer {
public:
    ScopedTimer(Profiler& profiler, int section);
    ~ScopedTimer();

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    Profiler& profiler;
    int section;
    double start;
};

// Run work (any callable) and record how long it took into the section: one line per timed system.
template <typename Work>
void timeSection(Profiler& profiler, int section, Work&& work) {
    ScopedTimer timer{profiler, section};
    work();
}
