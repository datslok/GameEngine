#pragma once

#include <cstddef>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/*
* The test run's progress bar: a line as each test starts, scrolling down, filled by the share of the expected time done
* (each test expected to take as long as last run, kept in a times file beside tests.exe), with the time left and the
* test running. It is written to the console itself (Windows' CONOUT$), not to stdout, so it shows even while make holds
* the run's output back until it ends (--output-sync), and ends on a line with the whole run's time; with no console it is
* not shown. A failing test's line is the last before its message.
*/

// Last run's times file: a line per test, its seconds then its name. Lines that are not that are skipped.
std::map<std::string, double> parseTestTimes(std::string_view text);
std::string formatTestTimes(const std::vector<std::pair<double, const char*>>& times);

// How long each test is expected to take: last run's time, else the mean of the known ones (1 s with none).
std::vector<double> expectedTestSeconds(const std::vector<std::string>& names, const std::map<std::string, double>& last);

struct ProgressState {
    std::size_t done = 0;
    std::size_t total = 0;
    double expectedDone = 0.0;  // the expected seconds of the tests done
    double expectedTotal = 0.0; // and of all of them
    double elapsed = 0.0;       // the seconds the run has taken so far
    std::string current;        // the test running
};

// The line: the bar barWidth characters wide, the share done, the count, the time left at the pace so far, the test.
std::string progressLine(const ProgressState& state, int barWidth);

// The bar on the console, if there is one.
class ConsoleProgress {
public:
    ConsoleProgress();
    ~ConsoleProgress();
    ConsoleProgress(const ConsoleProgress&) = delete;
    ConsoleProgress& operator=(const ConsoleProgress&) = delete;

    void show(const ProgressState& state);

    // The last line: everything done, in seconds.
    void finish(const ProgressState& state);

private:
    std::FILE* console = nullptr;
};
