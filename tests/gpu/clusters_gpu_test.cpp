#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "math/angles.h"
#include "render/gpu/clusters.h"
#include "scene/camera.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("clusters_test");

    // Each case: the grid's six vec4s, then three of the function's own.
    constexpr std::uint32_t inputsPerCase = 9;

    ClusterGrid testGrid() {
        const Camera camera{Vec3{3.0f, 2.0f, -1.0f}, Vec3{-2.0f, 0.5f, -9.0f}, Vec3{0.0f, 1.0f, 0.0f}, radiansFromDegrees(70.0f), 16.0f / 9.0f, 0.1f, 100.0f};
        return makeClusterGrid(camera, 2560, 1440);
    }

    // The grid as clusters.glsl's ClusterGrid, the tile counts as floats.
    void addCase(std::vector<GpuVec4>& inputs, const ClusterGrid& grid, const GpuVec4& a, const GpuVec4& b = {}, const GpuVec4& c = {}) {
        inputs.push_back(gpuVec4(static_cast<float>(grid.tilesX), static_cast<float>(grid.tilesY), static_cast<float>(grid.slices)));
        inputs.push_back(gpuVec4(static_cast<float>(grid.screenWidth), static_cast<float>(grid.screenHeight), grid.tanHalfWidth, grid.tanHalfHeight));
        inputs.push_back(gpuVec4(grid.sliceScale, grid.sliceBias));
        inputs.push_back(gpuVec4(grid.right));
        inputs.push_back(gpuVec4(grid.up));
        inputs.push_back(gpuVec4(grid.forward));
        inputs.push_back(a);
        inputs.push_back(b);
        inputs.push_back(c);
    }

    GpuVec4 caseInput(const std::vector<GpuVec4>& inputs, std::size_t caseIndex, std::size_t input) {
        return inputs[caseIndex * inputsPerCase + 6 + input];
    }

    std::size_t caseCount(const std::vector<GpuVec4>& inputs) {
        return inputs.size() / inputsPerCase;
    }

    // A depth between near and far, spread evenly over the logarithm so every slice gets its share.
    float randomDepth(CaseRandom& random, const ClusterGrid& grid) {
        return grid.nearDepth * std::pow(grid.farDepth / grid.nearDepth, random.between(0.0f, 1.0f));
    }

    // Whether a depth is clear of the slice boundaries, where the GPU's log may round to the neighbouring slice (the
    // bounds' margin is there for exactly that, so it is not an error, but the comparison must be exact elsewhere).
    bool clearOfSliceBoundaries(const ClusterGrid& grid, float depth) {
        const int slice = clusterSlice(depth, grid.sliceScale, grid.sliceBias, grid.slices);
        return clusterSlice(depth * 0.9999f, grid.sliceScale, grid.sliceBias, grid.slices) == slice &&
               clusterSlice(depth * 1.0001f, grid.sliceScale, grid.sliceBias, grid.slices) == slice;
    }

    void checkToClusterView(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const ClusterGrid grid = testGrid();
        CaseRandom random{11};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 200; ++i) {
            addCase(inputs, grid, gpuVec4(random.direction() * random.between(0.1f, 100.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_TO_CLUSTER_VIEW, inputs, inputsPerCase, 1);

        for (std::size_t i = 0; i < caseCount(inputs); ++i) {
            report.check("toClusterView", arithmeticTolerance, toClusterView(grid, vec3From(caseInput(inputs, i, 0))), vec3From(outputs[i]));
        }
    }

    // Slices of depths all over the view, clear of the boundaries (exact), and every slice's start depth.
    void checkSlices(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const ClusterGrid grid = testGrid();
        CaseRandom random{12};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            const float depth = randomDepth(random, grid);

            if (clearOfSliceBoundaries(grid, depth)) {
                addCase(inputs, grid, gpuVec4(depth, static_cast<float>(i % (grid.slices + 1))));
            }
        }

        addCase(inputs, grid, gpuVec4(0.0f));
        addCase(inputs, grid, gpuVec4(-1.0f));
        addCase(inputs, grid, gpuVec4(500.0f));

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CLUSTER_SLICE, inputs, inputsPerCase, 1);

        for (std::size_t i = 0; i < caseCount(inputs); ++i) {
            const GpuVec4 a = caseInput(inputs, i, 0);
            const int slice = clusterSlice(a[0], grid.sliceScale, grid.sliceBias, grid.slices);
            report.check("clusterSlice", exactTolerance, static_cast<float>(slice), outputs[i][0]);
            report.check("sliceStartDepth", transcendentalTolerance, sliceStartDepth(static_cast<int>(a[1]), grid.sliceScale, grid.sliceBias), outputs[i][1]);
        }
    }

    // Pixel centres all over the screen, including the corners and the last, cut-off row of tiles, at depths clear of
    // the slice boundaries: the cluster number must match exactly.
    void checkClusterOfPixel(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const ClusterGrid grid = testGrid();
        CaseRandom random{13};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            const float depth = randomDepth(random, grid);
            const float pixelX = std::floor(random.between(0.0f, 2560.0f)) + 0.5f;
            const float pixelY = std::floor(random.between(0.0f, 1440.0f)) + 0.5f;

            if (clearOfSliceBoundaries(grid, depth)) {
                addCase(inputs, grid, gpuVec4(pixelX, pixelY, depth));
            }
        }

        addCase(inputs, grid, gpuVec4(0.5f, 0.5f, 1.0f));
        addCase(inputs, grid, gpuVec4(2559.5f, 1439.5f, 1.0f));

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CLUSTER_OF_PIXEL, inputs, inputsPerCase, 1);

        for (std::size_t i = 0; i < caseCount(inputs); ++i) {
            const GpuVec4 a = caseInput(inputs, i, 0);
            report.check("clusterOfPixel", exactTolerance, static_cast<float>(clusterOfPixel(grid, a[0], a[1], a[2])), outputs[i][0]);
        }
    }

    // Boxes of tiles across the screen (the first, the last cut-off ones, random ones) in every slice.
    void checkClusterBounds(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const ClusterGrid grid = testGrid();
        CaseRandom random{14};
        std::vector<GpuVec4> inputs;

        for (int slice = 0; slice < grid.slices; ++slice) {
            addCase(inputs, grid, gpuVec4(0.0f, 0.0f, static_cast<float>(slice)));
            addCase(inputs, grid, gpuVec4(static_cast<float>(grid.tilesX - 1), static_cast<float>(grid.tilesY - 1), static_cast<float>(slice)));

            for (int i = 0; i < 8; ++i) {
                const float tileX = std::floor(random.between(0.0f, static_cast<float>(grid.tilesX)));
                const float tileY = std::floor(random.between(0.0f, static_cast<float>(grid.tilesY)));
                addCase(inputs, grid, gpuVec4(tileX, tileY, static_cast<float>(slice)));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CLUSTER_BOUNDS, inputs, inputsPerCase, 2);

        for (std::size_t i = 0; i < caseCount(inputs); ++i) {
            const GpuVec4 a = caseInput(inputs, i, 0);
            const Aabb box = clusterBounds(grid, static_cast<int>(a[0]), static_cast<int>(a[1]), static_cast<int>(a[2]));
            report.check("clusterBounds", transcendentalTolerance, box.minimum, vec3From(outputs[2 * i]));
            report.check("clusterBounds", transcendentalTolerance, box.maximum, vec3From(outputs[2 * i + 1]));
        }
    }

    /*
    * Random spheres against random boxes and cones, kept only where the answer is clear: the same with the radius 0.1%
    * smaller and larger. At the exact boundary the GPU's rounding may decide either way, which the bounds' margin
    * covers; everywhere else the answers must be the same.
    */
    void checkTouchTests(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const ClusterGrid grid = testGrid();
        CaseRandom random{15};
        std::vector<GpuVec4> boxCases;
        std::vector<GpuVec4> coneCases;

        while (caseCount(boxCases) < 400) {
            const Vec3 minimum{random.between(-5.0f, 5.0f), random.between(-5.0f, 5.0f), random.between(0.0f, 50.0f)};
            const Vec3 maximum = minimum + Vec3{random.between(0.1f, 4.0f), random.between(0.1f, 4.0f), random.between(0.1f, 10.0f)};
            const Vec3 centre = minimum + Vec3{random.between(-8.0f, 8.0f), random.between(-8.0f, 8.0f), random.between(-15.0f, 15.0f)};
            const float radius = random.between(0.1f, 8.0f);
            const Aabb box{minimum, maximum};
            const bool touches = sphereTouchesBox(centre, radius, box);

            if (sphereTouchesBox(centre, radius * 0.999f, box) == touches && sphereTouchesBox(centre, radius * 1.001f, box) == touches) {
                addCase(boxCases, grid, gpuVec4(centre, radius), gpuVec4(minimum), gpuVec4(maximum));
            }
        }

        while (caseCount(coneCases) < 400) {
            const Vec3 apex{random.between(-5.0f, 5.0f), random.between(-5.0f, 5.0f), random.between(0.0f, 30.0f)};
            const Vec3 axis = random.direction();
            const float cosOuter = random.between(-0.2f, 0.99f);
            const float range = random.between(1.0f, 20.0f);
            const Vec3 centre = apex + random.direction() * random.between(0.0f, 25.0f);
            const float radius = random.between(0.1f, 4.0f);
            const bool touches = coneTouchesSphere(apex, axis, cosOuter, range, centre, radius);

            if (coneTouchesSphere(apex, axis, cosOuter, range, centre, radius * 0.999f) == touches &&
                coneTouchesSphere(apex, axis, cosOuter, range, centre, radius * 1.001f) == touches) {
                addCase(coneCases, grid, gpuVec4(apex, cosOuter), gpuVec4(axis, range), gpuVec4(centre, radius));
            }
        }

        const std::vector<GpuVec4> boxOutputs = runner.run(shader, GPU_TEST_SPHERE_TOUCHES_BOX, boxCases, inputsPerCase, 1);
        const std::vector<GpuVec4> coneOutputs = runner.run(shader, GPU_TEST_CONE_TOUCHES_SPHERE, coneCases, inputsPerCase, 1);

        for (std::size_t i = 0; i < caseCount(boxCases); ++i) {
            const GpuVec4 a = caseInput(boxCases, i, 0);
            const Aabb box{vec3From(caseInput(boxCases, i, 1)), vec3From(caseInput(boxCases, i, 2))};
            report.check("sphereTouchesBox", exactTolerance, sphereTouchesBox(vec3From(a), a[3], box) ? 1.0f : 0.0f, boxOutputs[i][0]);
        }

        for (std::size_t i = 0; i < caseCount(coneCases); ++i) {
            const GpuVec4 a = caseInput(coneCases, i, 0);
            const GpuVec4 b = caseInput(coneCases, i, 1);
            const GpuVec4 c = caseInput(coneCases, i, 2);
            const bool touches = coneTouchesSphere(vec3From(a), vec3From(b), a[3], b[3], vec3From(c), c[3]);
            report.check("coneTouchesSphere", exactTolerance, touches ? 1.0f : 0.0f, coneOutputs[i][0]);
        }
    }

    // Every light count from none to past the full scale.
    void checkHeatColour(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const ClusterGrid grid = testGrid();
        std::vector<GpuVec4> inputs;

        for (int lights = 0; lights <= 2 * clusterHeatmapLights; ++lights) {
            addCase(inputs, grid, gpuVec4(static_cast<float>(lights)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CLUSTER_HEAT_COLOUR, inputs, inputsPerCase, 1);

        for (std::size_t i = 0; i < caseCount(inputs); ++i) {
            report.check("clusterHeatColour", arithmeticTolerance, clusterHeatColour(static_cast<int>(i)), vec3From(outputs[i]));
        }
    }
}

void gpuTestClusters(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("clusters.glsl");
    checkToClusterView(runner, report);
    checkSlices(runner, report);
    checkClusterOfPixel(runner, report);
    checkClusterBounds(runner, report);
    checkTouchTests(runner, report);
    checkHeatColour(runner, report);
}
