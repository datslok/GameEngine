#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/mirrored_light.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("mirrored_light_test");

    // A direction within 85 degrees of normal: the receivers and lights in front of a surface.
    Vec3 inFront(CaseRandom& random, const Vec3& normal) {
        const Vec3 direction = random.direction();
        const float along = direction.dot(normal);
        const Vec3 front = along < 0.1f ? direction + normal * (0.1f - along) : direction;
        return front.normalized();
    }

    /*
    * Surfaces facing every way, near and far from their receivers and lights, flat to sharply curved either way, with
    * triangles of every shape; a tenth of the lights behind the surface. Inputs per case: (receiver, range), (light,
    * source radius), (point, path length), (normal), (curvature), (edge 1, normal length), (edge 2), (normal change 1),
    * (normal change 2), (intensity, sphere radius).
    */
    void checkMirror(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{137};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const Vec3 point = random.direction() * random.between(0.0f, 30.0f);
            const Vec3 normal = random.direction();
            const Vec3 receiver = point + inFront(random, normal) * random.between(0.2f, 20.0f);
            const Vec3 towardsLight = i % 10 == 0 ? random.direction() * -1.0f : inFront(random, normal);
            const Vec3 light = point + towardsLight * random.between(0.2f, 20.0f);
            const float twist = random.between(-1.0f, 1.0f);
            const float scale = i % 4 == 0 ? 0.0f : 2.0f;
            inputs.push_back(gpuVec4(receiver, random.between(5.0f, 60.0f)));
            inputs.push_back(gpuVec4(light, random.between(0.05f, 4.5f)));
            inputs.push_back(gpuVec4(point, random.between(0.5f, 70.0f)));
            inputs.push_back(gpuVec4(normal));
            inputs.push_back(GpuVec4{random.between(-scale, scale), twist * scale * 0.5f, twist * scale * 0.5f, random.between(-scale, scale)});
            inputs.push_back(gpuVec4(random.direction() * random.between(0.01f, 2.0f), random.between(0.5f, 1.0f)));
            inputs.push_back(gpuVec4(random.direction() * random.between(0.01f, 2.0f)));
            inputs.push_back(gpuVec4(random.direction() * random.between(0.0f, 0.5f)));
            inputs.push_back(gpuVec4(random.direction() * random.between(0.0f, 0.5f)));
            inputs.push_back(gpuVec4(Vec3{random.between(0.0f, 20.0f), random.between(0.0f, 20.0f), random.between(0.0f, 20.0f)},
                                     random.between(0.1f, 3.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_MIRROR, inputs, 10, 8);
        // Newton's step and the spread area divide by the Jacobian's determinant, which can lose digits to cancellation.
        const FloatTolerance dividedTolerance{.absolute = 1e-4f, .relative = 1e-3f};

        for (std::size_t i = 0; i < outputs.size() / 8; ++i) {
            const GpuVec4* in = &inputs[i * 10];
            const GpuVec4* out = &outputs[i * 8];
            const Vec3 receiver = vec3From(in[0]);
            const Vec3 light = vec3From(in[1]);
            const TangentFrame frame = tangentFrame(vec3From(in[3]));
            const TangentMatrix curvature{in[4][0], in[4][1], in[4][2], in[4][3]};
            const MirrorSurface surface{vec3From(in[2]), frame, curvature};

            report.check("tangentFrame", arithmeticTolerance, frame.tangent, vec3From(out[0]));
            report.check("tangentFrame", arithmeticTolerance, frame.bitangent, vec3From(out[1]));
            report.check("isFlatMirror", exactTolerance, isFlatMirror(curvature) ? 1.0f : 0.0f, out[0][3]);
            report.check("mirrorIncidence", transcendentalTolerance, mirrorIncidence(receiver, surface), out[1][3]);

            const Vec2 constraint = mirrorConstraint(receiver, light, surface);
            report.check("mirrorConstraint", transcendentalTolerance, Vec3{constraint.x, constraint.y, 0.0f}, Vec3{out[2][0], out[2][1], 0.0f});
            report.check("mirrorSpreadArea", dividedTolerance, mirrorSpreadArea(receiver, light, surface), out[2][2]);
            report.check("mirrorPairMayReach", exactTolerance, mirrorPairMayReach(receiver, surface.position, in[9][3], light, in[0][3]) ? 1.0f : 0.0f,
                         out[2][3]);

            const TangentMatrix jacobian = mirrorJacobian(receiver, light, surface);
            report.check("mirrorJacobian", transcendentalTolerance, Vec3{jacobian.xx, jacobian.xy, jacobian.yx}, vec3From(out[3]));
            report.check("mirrorJacobian", transcendentalTolerance, jacobian.yy, out[3][3]);

            const std::optional<Vec3> target = mirrorNewtonTarget(receiver, light, surface);
            report.check("mirrorNewtonTarget", exactTolerance, target ? 1.0f : 0.0f, out[4][3]);

            if (target) {
                report.check("mirrorNewtonTarget", dividedTolerance, *target, vec3From(out[4]));
            }

            const std::optional<Vec3> image = planeMirrorPoint(receiver, light, surface.position, frame.normal);
            report.check("planeMirrorPoint", exactTolerance, image ? 1.0f : 0.0f, out[5][3]);

            if (image) {
                report.check("planeMirrorPoint", arithmeticTolerance, *image, vec3From(out[5]));
            }

            report.check("mirroredIrradiance", transcendentalTolerance,
                         mirroredIrradiance(vec3From(in[9]), Vec3{0.9f, 0.8f, 0.7f}, in[2][3] * in[2][3] * 0.5f, in[2][3], in[0][3], in[1][3]),
                         vec3From(out[6]));

            const TangentMatrix triangle = triangleCurvature(vec3From(in[5]), vec3From(in[6]), vec3From(in[7]), vec3From(in[8]), in[5][3], frame);
            report.check("triangleCurvature", dividedTolerance, Vec3{triangle.xx, triangle.xy, triangle.yy}, vec3From(out[7]));
        }
    }
}

void gpuTestMirroredLight(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("mirrored_light.glsl");
    checkMirror(runner, report);
}
