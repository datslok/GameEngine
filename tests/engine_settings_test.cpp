#include "core/build_info.h"
#include "engine/engine_settings.h"

#include <cassert>

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

    // SDL_GPU draws until the Vulkan renderer passes every golden image (phase 6b), and the choice is read once.
    void testGraphicsApiDefaultsToSdlGpu() {
        const EngineSettings settings;
        assert(settings.graphicsApi == GraphicsApi::SdlGpu);
    }
}

void testEngineSettings() {
    testBuildIsKnown();
    testGpuValidationFollowsTheBuild();
    testGraphicsApiDefaultsToSdlGpu();
}
