#include "test_progress.h"

#include <cassert>
#include <map>
#include <string>
#include <vector>

namespace {
    // The times file: a line per test, its seconds then its name; lines that are not that are skipped.
    void testTimesAreReadAndWritten() {
        const std::string text = formatTestTimes({{1.5, "testA"}, {0.25, "testB"}});
        assert(text == "1.500 testA\n0.250 testB\n");

        const std::map<std::string, double> times = parseTestTimes(text + "nonsense\n\n3 \n");
        assert(times.size() == 2 && times.at("testA") == 1.5 && times.at("testB") == 0.25);
        assert(parseTestTimes("").empty());
    }

    // Each test is expected to take as long as last time; one never run before, the known ones' mean (1 s with none).
    void testEachTestIsExpectedToTakeAsLongAsLastTime() {
        const std::map<std::string, double> last{{"testA", 2.0}, {"testB", 4.0}};
        const std::vector<double> expected = expectedTestSeconds({"testA", "testNew", "testB"}, last);
        assert((expected == std::vector<double>{2.0, 3.0, 4.0}));
        assert((expectedTestSeconds({"testX", "testY"}, {}) == std::vector<double>{1.0, 1.0}));
    }

    /*
    * The bar is filled by the share of the expected time done, not of the tests counted, so one long test shows as the
    * long stretch it is; the time left is the expected rest at the pace the run has gone so far.
    */
    void testTheLineShowsTheShareDoneAndTheTimeLeft() {
        const ProgressState state{.done = 3, .total = 10, .expectedDone = 5.0, .expectedTotal = 20.0, .elapsed = 10.0, .current = "testForest"};
        const std::string line = progressLine(state, 10);
        assert(line == "[##--------]  25%  3/10  ~30 s left  testForest");

        // Before anything has finished the pace is unknown: the expected time itself.
        const ProgressState start{.done = 0, .total = 10, .expectedDone = 0.0, .expectedTotal = 20.0, .elapsed = 0.0, .current = "testA"};
        assert(progressLine(start, 4) == "[----]   0%  0/10  ~20 s left  testA");

        // Done: full, nothing left.
        const ProgressState end{.done = 10, .total = 10, .expectedDone = 20.0, .expectedTotal = 20.0, .elapsed = 18.0, .current = ""};
        assert(progressLine(end, 4) == "[####] 100%  10/10  ~0 s left  ");
    }
}

void testTestProgress() {
    testTimesAreReadAndWritten();
    testEachTestIsExpectedToTakeAsLongAsLastTime();
    testTheLineShowsTheShareDoneAndTheTimeLeft();
}
