#pragma once

#include <string>
#include <string_view>
#include <vector>

/*
* The laps of a one-off start (the engine's, the renderer's): each step's name and how long it took, written to the log
* as one line, so a slower start shows where. The times are passed in (profilerSeconds), so tests need no clock. Only
* for starting: never inside a tick.
*/
class StartupClock {
public:
    struct Lap {
        std::string name;
        double seconds = 0.0;
    };

    explicit StartupClock(double startSeconds);

    // A step finished at nowSeconds: it took the time since the last lap (or the start).
    void lap(std::string name, double nowSeconds);

    const std::vector<Lap>& laps() const;

    // From the start to the last lap.
    double total() const;

    // "title 9.87 s: window 0.12 s, renderer 2.30 s", seconds to two places.
    std::string describe(std::string_view title) const;

private:
    double start;
    double last;
    std::vector<Lap> recorded;
};
