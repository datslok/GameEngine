#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "math/mat4.h"
#include "render/gpu/camera_relative.h"
#include "render/gpu/screen_reflections.h"
#include "scene/camera.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("screen_reflections_test");

    void pushMatrix(std::vector<GpuVec4>& inputs, const Mat4& matrix) {
        for (int column = 0; column < 4; ++column) {
            inputs.push_back(GpuVec4{matrix.values[0][column], matrix.values[1][column], matrix.values[2][column], matrix.values[3][column]});
        }
    }

    // A camera looking any way, with a lens from narrow to wide, as the main pass draws with it (measured from the camera).
    Mat4 randomViewProjection(CaseRandom& random, Vec2 size) {
        const Camera camera{Vec3{}, random.direction(), Vec3{0.0f, 1.0f, 0.0f}, random.between(0.6f, 1.6f), size.x / size.y, 0.1f, 500.0f};
        return cameraRelativeViewProjection(camera);
    }

    // Pyramid cells at every level of screens up to 4K, odd and even, and the nearer of two depths.
    void checkPyramid(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{89};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const float fineSize = std::floor(random.between(1.0f, 4000.0f));
            const float coarseSize = std::max(std::floor(fineSize / 2.0f), 1.0f);
            const float level = std::floor(random.between(0.0f, 12.0f));
            const float levelSize = std::max(std::floor(fineSize / std::exp2(level)), 1.0f);
            inputs.push_back(GpuVec4{std::floor(random.between(0.0f, coarseSize)), coarseSize, fineSize, level});
            inputs.push_back(GpuVec4{std::floor(random.between(-2.0f, fineSize + 2.0f)), levelSize, random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_SSR_PYRAMID, inputs, 2, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4* in = &inputs[i * 2];
            const int lastFine = pyramidLastFine(static_cast<int>(in[0][0]), static_cast<int>(in[0][1]), static_cast<int>(in[0][2]));
            const int cell = pyramidCell(static_cast<int>(in[1][0]), static_cast<std::uint32_t>(in[0][3]), static_cast<int>(in[1][1]));
            report.check("pyramidLastFine", exactTolerance, static_cast<float>(lastFine), outputs[i][0]);
            report.check("pyramidCell", exactTolerance, static_cast<float>(cell), outputs[i][1]);
            report.check("nearerDepth", exactTolerance, nearerDepth(in[1][2], in[1][3]), outputs[i][2]);
        }
    }

    // Pixels all over the screen at depths from the near plane to the far one, back through the inverse.
    void checkPosition(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{97};
        std::vector<GpuVec4> inputs;
        std::vector<Mat4> inverses;
        const Vec2 size{1920.0f, 1080.0f};

        for (int i = 0; i < 256; ++i) {
            const Mat4 inverseViewProjection = *inverse(randomViewProjection(random, size));
            inverses.push_back(inverseViewProjection);
            pushMatrix(inputs, inverseViewProjection);
            inputs.push_back(GpuVec4{random.between(0.0f, size.x), random.between(0.0f, size.y), random.between(0.0002f, 1.0f), 0.0f});
            inputs.push_back(GpuVec4{size.x, size.y, 0.0f, 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_SSR_POSITION, inputs, 6, 1);
        const FloatTolerance tolerance{.absolute = 1e-4f, .relative = 1e-4f};

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4* in = &inputs[i * 6];
            const Vec2 pixel{in[4][0], in[4][1]};
            report.check("positionAtPixel", tolerance, positionAtPixel(inverses[i], pixel, in[4][2], size), vec3From(outputs[i]));
            report.check("viewDistanceAtPixel", tolerance, viewDistanceAtPixel(inverses[i], pixel, in[4][2], size), outputs[i][3]);
        }
    }

    /*
    * Rays from points on screen in every direction, short and long, many leaving through an edge, the near plane or
    * the far depth. Inputs per case: the view-projection's four columns, (position, reach), (direction, 0), (size, 0, 0).
    */
    void checkScreenRay(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{101};
        std::vector<GpuVec4> inputs;
        std::vector<Mat4> viewProjections;
        const Vec2 size{1920.0f, 1080.0f};

        for (int i = 0; i < 256; ++i) {
            const Mat4 viewProjection = randomViewProjection(random, size);
            const Vec3 position = positionAtPixel(*inverse(viewProjection), Vec2{random.between(1.0f, size.x - 1.0f), random.between(1.0f, size.y - 1.0f)},
                                                  random.between(0.001f, 0.9f), size);
            viewProjections.push_back(viewProjection);
            pushMatrix(inputs, viewProjection);
            inputs.push_back(gpuVec4(position, random.between(1.0f, 200.0f)));
            inputs.push_back(gpuVec4(random.direction()));
            inputs.push_back(GpuVec4{size.x, size.y, 0.0f, 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_SSR_SCREEN_RAY, inputs, 7, 2);
        const FloatTolerance pixelTolerance{.absolute = 1e-2f, .relative = 1e-4f};
        const FloatTolerance endTolerance{.absolute = 1e-4f, .relative = 1e-3f};

        for (std::size_t i = 0; i < outputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 7];
            const GpuVec4* out = &outputs[i * 2];
            const ScreenRay ray = screenRay(viewProjections[i], vec3From(in[4]), vec3From(in[5]), in[4][3], size);
            report.check("screenRay start", pixelTolerance, ray.start, vec3From(out[0]));
            report.check("screenRay step", pixelTolerance, ray.step, vec3From(out[1]));
            report.check("screenRay end", endTolerance, ray.end, out[0][3]);
        }
    }

    /*
    * The march's moves from random places: rays going deeper and nearer, cells from 1 to 64 texels, levels 0 to 8,
    * nearest depths on either side of the ray. Inputs per case: (ray start, t), (ray step, cell nearest), (position,
    * level), (top level, cell size, ray distance, surface distance), (thickness, 0, 0, 0).
    */
    void checkMarch(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{103};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const Vec3 start{random.between(0.0f, 1920.0f), random.between(0.0f, 1080.0f), random.between(0.0f, 1.0f)};
            const Vec3 step{random.between(-500.0f, 500.0f), random.between(-500.0f, 500.0f), random.between(-0.5f, 0.5f)};
            const float t = random.between(0.0f, 0.5f);
            const Vec3 position = start + step * t;
            inputs.push_back(gpuVec4(start, t));
            inputs.push_back(gpuVec4(step, random.between(0.0f, 1.0f)));
            inputs.push_back(gpuVec4(position, std::floor(random.between(0.0f, 9.0f))));
            inputs.push_back(GpuVec4{10.0f, std::exp2(std::floor(random.between(0.0f, 7.0f))), random.between(1.0f, 50.0f), random.between(1.0f, 50.0f)});
            inputs.push_back(GpuVec4{random.between(0.0f, 2.0f), 0.0f, 0.0f, 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_SSR_MARCH, inputs, 5, 4);
        const FloatTolerance tTolerance{.absolute = 1e-5f, .relative = 1e-4f};
        const FloatTolerance pixelTolerance{.absolute = 1e-2f, .relative = 1e-4f};

        for (std::size_t i = 0; i < outputs.size() / 4; ++i) {
            const GpuVec4* in = &inputs[i * 5];
            const GpuVec4* out = &outputs[i * 4];
            const ScreenRay ray{vec3From(in[0]), vec3From(in[1]), 1.0f};
            const HizTrace trace{vec3From(in[2]), in[0][3], static_cast<int>(in[2][3])};
            const HizTrace stepped = hizStep(trace, ray, in[1][3], static_cast<int>(in[3][0]));
            const HizTrace passed = passTexel(trace, ray);

            report.check("cellExit", tTolerance, cellExit(Vec2{in[2][0], in[2][1]}, Vec2{in[1][0], in[1][1]}, in[3][1]), out[0][0]);
            report.check("withinThickness", exactTolerance, withinThickness(in[3][2], in[3][3], in[4][0]) ? 1.0f : 0.0f, out[0][1]);
            report.check("hizStep level", exactTolerance, static_cast<float>(stepped.level), out[2][0]);
            report.check("hizStep t", tTolerance, stepped.t, out[1][3]);
            report.check("hizStep position", pixelTolerance, stepped.position, vec3From(out[1]));
            report.check("passTexel level", exactTolerance, static_cast<float>(passed.level), out[2][2]);
            report.check("passTexel t", tTolerance, passed.t, out[2][1]);
            report.check("passTexel position", pixelTolerance, passed.position, vec3From(out[3]));
        }
    }

    /*
    * The confidence over roughness, the whole screen (edges included), travel to past the reach and every facing; cone
    * levels from a mirror to the roughest; the swap from untrusted to trusted, dark and bright. Inputs per case:
    * (roughness, pixel, travelled), (facing, size, sharp roughness), (max roughness, edge fade, reach, travelled
    * pixels), (scene, confidence), (stand-in), (specular albedo), (hit light).
    */
    void checkShading(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{107};
        std::vector<GpuVec4> inputs;
        const auto colour = [&](float scale) { return Vec3{random.between(0.0f, scale), random.between(0.0f, scale), random.between(0.0f, scale)}; };

        for (int i = 0; i < 256; ++i) {
            const Vec2 size{random.between(320.0f, 3840.0f), random.between(240.0f, 2160.0f)};
            const float sharp = random.between(0.0f, 0.5f);
            const float reach = random.between(10.0f, 300.0f);
            inputs.push_back(GpuVec4{random.between(0.0f, 1.0f), random.between(0.0f, size.x), random.between(0.0f, size.y), random.between(0.0f, 1.2f * reach)});
            inputs.push_back(GpuVec4{random.between(-1.0f, 1.0f), size.x, size.y, sharp});
            inputs.push_back(GpuVec4{random.between(sharp + 0.01f, 1.0f), random.between(0.01f, 1.0f), reach, random.between(0.0f, 2000.0f)});
            inputs.push_back(gpuVec4(colour(4.0f), i % 8 == 0 ? 0.0f : random.between(0.0f, 1.0f)));
            inputs.push_back(gpuVec4(colour(2.0f)));
            inputs.push_back(gpuVec4(colour(1.0f)));
            inputs.push_back(gpuVec4(colour(50.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_SSR_SHADING, inputs, 7, 2);

        for (std::size_t i = 0; i < outputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 7];
            const GpuVec4* out = &outputs[i * 2];
            const ReflectionSettings settings{.sharpRoughness = in[1][3], .maxRoughness = in[2][0], .reach = in[2][2], .edgeFade = in[2][1]};
            const ReflectionHit hit{in[0][0], Vec2{in[0][1], in[0][2]}, in[0][3], in[1][0]};
            report.check("reflectionConfidence", transcendentalTolerance, reflectionConfidence(hit, Vec2{in[1][1], in[1][2]}, settings), out[0][0]);
            report.check("reflectionConeLevel", transcendentalTolerance, reflectionConeLevel(in[0][0], in[2][3]), out[0][1]);
            report.check("reflectedScene", arithmeticTolerance,
                         reflectedScene(vec3From(in[3]), vec3From(in[4]), vec3From(in[5]), vec3From(in[6]), in[3][3]), vec3From(out[1]));
        }
    }
}

void gpuTestScreenReflections(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("screen_reflections.glsl");
    checkPyramid(runner, report);
    checkPosition(runner, report);
    checkScreenRay(runner, report);
    checkMarch(runner, report);
    checkShading(runner, report);
}
