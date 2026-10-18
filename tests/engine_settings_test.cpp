#include "core/build_info.h"
#include "engine/engine_settings.h"
#include "engine/settings_file.h"
#include "render/frames_in_flight.h"

#include <cassert>
#include <string>
#include <vector>

namespace {
    /*
    * The makefile tells the code which build it is. GCC itself defines __OPTIMIZE__ whenever optimisation is on, so
    * the two must agree: the debug build is unoptimised, the release build is not.
    */
    void testBuildIsKnown() {
#ifdef __OPTIMIZE__
        assert(!isDebugBuild);
#else
        assert(isDebugBuild);
#endif
    }

    // GPU validation (checking every graphics call, slow) is on by default in debug builds only.
    void testGpuValidationFollowsTheBuild() {
        const EngineSettings settings;
        assert(settings.gpuValidation == isDebugBuild);
    }

    SettingsWalk walkOf(EngineSettings& settings) {
        return [&settings](SettingsVisitor& visitor) {
            visit(settings, visitor);
        };
    }

    // Two frames in flight by default, so the CPU records the next frame while the GPU draws this one.
    void testTwoFramesInFlightByDefault() {
        const EngineSettings settings;
        assert(settings.framesInFlight == 2);
        assert(settings.framesInFlight <= maxFramesInFlight);
    }

    // It changes while the game runs (the debug UI keeps start-up fields as they are, and this one still changes),
    // and a value outside 1 to maxFramesInFlight is clamped with a warning.
    void testFramesInFlightAppliesLiveWithinLimits() {
        EngineSettings settings;
        const std::vector<std::string> none = applySettings({{"framesInFlight", "1"}}, walkOf(settings), StartupFields::Keep);
        assert(none.empty());
        assert(settings.framesInFlight == 1);

        const std::vector<std::string> tooMany = applySettings({{"framesInFlight", "5"}}, walkOf(settings), StartupFields::Keep);
        assert(!tooMany.empty());
        assert(settings.framesInFlight == maxFramesInFlight);

        const std::vector<std::string> tooFew = applySettings({{"framesInFlight", "0"}}, walkOf(settings), StartupFields::Keep);
        assert(!tooFew.empty());
        assert(settings.framesInFlight == 1);
    }

    // The renderer clamps what it is given the same way, so a value set in code cannot break it either.
    void testRendererFramesInFlightAreClamped() {
        assert(validFramesInFlight(0) == 1);
        assert(validFramesInFlight(1) == 1);
        assert(validFramesInFlight(2) == 2);
        assert(validFramesInFlight(7) == maxFramesInFlight);
    }
}

void testEngineSettings() {
    testBuildIsKnown();
    testGpuValidationFollowsTheBuild();
    testTwoFramesInFlightByDefault();
    testFramesInFlightAppliesLiveWithinLimits();
    testRendererFramesInFlightAreClamped();
}
