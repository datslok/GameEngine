#pragma once

#include "core/float_tolerance.h"
#include "math/vec3.h"
#include "math/vec4.h"
#include "gpu_function_runner.h"

#include <cstdint>
#include <string>
#include <vector>

/*
* Shared by the GPU tests: building inputs, and a report of every comparison of a shader's results with its C++ mirror.
*/

/*
* The tolerances the tests choose from, sized by what the GPU may do differently from the CPU:
* - arithmetic: only + - * / and sqrt. The GPU may fuse a multiply and an add into one rounding, and Vulkan allows
*   division 2.5 units in the last place (ULP: the gap to the next float, about 1.2e-7 of the value) off and sqrt
*   about as much, so a few operations in a row stay within some 1e-6 of the value; 1e-5 leaves room for ten times that.
* - transcendental: pow, exp2 and log2 too. pow(x, y) is exp2(y log2 x), and exp2's allowance grows with its argument
*   (3 + 2|x| ULP), so a high power can lose more; 1e-4 relative.
* - exact: results that are whole numbers (a cube face), or copies, must match exactly.
* A real bug (a wrong constant, swapped arguments, a missing pi) is off by percent or more, orders of magnitude beyond
* either, so the tests can be this tight and still tell rounding from mistakes.
*/
inline constexpr FloatTolerance arithmeticTolerance{.absolute = 1e-6f, .relative = 1e-5f};
inline constexpr FloatTolerance transcendentalTolerance{.absolute = 1e-6f, .relative = 1e-4f};
inline constexpr FloatTolerance exactTolerance{};

// The compiled test shaders (tests/gpu/shaders, built by make gputest), from the repo root.
inline std::string gpuTestShader(const char* name) {
    return std::string{"tests/gpu/shaders/"} + name + ".comp.spv";
}

inline GpuVec4 gpuVec4(float x,float y = 0.0f, float z = 0.0f, float w = 0.0f) {
    return GpuVec4{x, y, z, w};
}

inline GpuVec4 gpuVec4(const Vec3& xyz, float w = 0.0f) {
    return GpuVec4{xyz.x, xyz.y, xyz.z, w};
}

inline GpuVec4 gpuVec4(const Vec4& value) {
    return GpuVec4{value.x, value.y, value.z, value.w};
}

inline Vec3 vec3From(const GpuVec4& value) {
    return Vec3{value[0], value[1], value[2]};
}

/*
* Inputs that look random but are the same every run (xorshift32), so a failure always comes back the same way.
* std::uniform_real_distribution would do, but its output differs between standard libraries.
*/
class CaseRandom {
public:
    explicit CaseRandom(std::uint32_t seed) : state(seed != 0 ? seed : 1u) {}

    // Uniform in lowest..highest.
    float between(float lowest, float highest);

    // A direction with every orientation equally likely, of unit length.
    Vec3 direction();

private:
    std::uint32_t state;

    std::uint32_t next();
};

/*
* Every comparison, grouped by name (a function, or one of its outputs), each with its tolerance (usually the same for
* a whole group, but it may follow the case, as for sharp specular highlights). print()
* shows how many values each group compared and how much of its tolerance the worst one used, so the margin is visible
* as well as the verdict: a group that passes using 90% of its allowance is a warning sign.
*/
class GpuCheckReport {
public:
    void check(const std::string& name, const FloatTolerance& tolerance, float expected, float actual);
    void check(const std::string& name, const FloatTolerance& tolerance, const Vec3& expected, const Vec3& actual);

    // Starts a section (a shader library): groups checked from here on are listed under it.
    void section(const std::string& title);

    // Every group, section by section, with its verdict and worst comparison.
    void print() const;

    std::size_t failedGroups() const;

private:
    struct Group {
        std::string section;
        std::string name;
        ToleranceTally tally;
    };

    std::string currentSection;
    std::vector<Group> groups;

    Group& find(const std::string& name, const FloatTolerance& tolerance);
};

// One function per shader library, each comparing the shader's functions with their C++ mirrors.
void gpuTestBrdf(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestNormalMapping(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestLighting(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestSoftShadow(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestToneMapping(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestBloom(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestTaa(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestScreenReflections(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestReflectionProbe(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestMirroredLight(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestAmbientOcclusion(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestAlphaMask(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestTransparency(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestDither(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestOverlay(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestStars(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestClusters(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestCascades(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestDdgiSampling(GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestProbeRelocation(GpuFunctionRunner& runner, GpuCheckReport& report);

class VulkanDevice;

// The cascades' depth reduction, run as the renderer runs it, on depth images of known values.
void gpuTestDepthRange(const VulkanDevice& device, GpuCheckReport& report);

// The sky's library, then its passes (the tables and the flat ambient) against the CPU's marches.
void gpuTestAtmosphere(const VulkanDevice& device, GpuFunctionRunner& runner, GpuCheckReport& report);
void gpuTestEyeAdaptation(const VulkanDevice& device, GpuFunctionRunner& runner, GpuCheckReport& report);

// Ray tracing's library, then real acceleration structures traced on the GPU against the CPU's raycast (skipped on a
// GPU without ray queries).
void gpuTestRayTracing(const VulkanDevice& device, GpuFunctionRunner& runner, GpuCheckReport& report);

// The light probes' library, then the real probe passes on scenes the CPU works out ray by ray (skipped on a GPU without
// ray queries).
void gpuTestDdgi(const VulkanDevice& device, GpuFunctionRunner& runner, GpuCheckReport& report);
