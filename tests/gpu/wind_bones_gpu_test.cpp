#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/wind_bones.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("wind_bones_test");

    // A random unit quaternion: a turn of up to a radian about a random axis.
    Quat randomTurn(CaseRandom& random) {
        return Quat::fromRotationVector(random.direction() * random.between(0.0f, 1.0f));
    }

    Vec4 asVec4(const Quat& turn) {
        return Vec4{turn.x, turn.y, turn.z, turn.w};
    }

    void checkQuat(GpuCheckReport& report, const std::string& name, const Quat& expected, const GpuVec4& actual) {
        report.check(name + " xyz", arithmeticTolerance, Vec3{expected.x, expected.y, expected.z}, vec3From(actual));
        report.check(name + " w", arithmeticTolerance, expected.w, actual[3]);
    }

    // Random unit quaternions, vectors and rotation vectors (a zero one included).
    void checkQuaternions(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{81};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            inputs.push_back(gpuVec4(asVec4(randomTurn(random))));
            inputs.push_back(gpuVec4(asVec4(randomTurn(random))));
            inputs.push_back(gpuVec4(random.direction() * random.between(0.0f, 20.0f)));
            inputs.push_back(gpuVec4(i == 0 ? Vec3{} : random.direction() * random.between(0.0f, 3.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_QUAT, inputs, 4, 3);

        // Products and cross products: rounding only; the rotation vector's sine and cosine.
        for (std::size_t i = 0; i < inputs.size() / 4; ++i) {
            const Quat a{inputs[4 * i][0], inputs[4 * i][1], inputs[4 * i][2], inputs[4 * i][3]};
            const Quat b{inputs[4 * i + 1][0], inputs[4 * i + 1][1], inputs[4 * i + 1][2], inputs[4 * i + 1][3]};
            checkQuat(report, "quatMultiply", a * b, outputs[3 * i]);
            report.check("quatRotate", arithmeticTolerance, a.rotate(vec3From(inputs[4 * i + 2])), vec3From(outputs[3 * i + 1]));
            const Quat fromRotation = Quat::fromRotationVector(vec3From(inputs[4 * i + 3]));
            report.check("quatFromRotationVector xyz", transcendentalTolerance, Vec3{fromRotation.x, fromRotation.y, fromRotation.z},
                         vec3From(outputs[3 * i + 2]));
            report.check("quatFromRotationVector w", transcendentalTolerance, fromRotation.w, outputs[3 * i + 2][3]);
        }
    }

    // Bones a few cm to a metre thick, trees 2-40 m, exponents 0-4: shares clamped and not; branches' modes and phases.
    void checkSharesAndBranches(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{82};
        std::vector<GpuVec4> shares;
        std::vector<GpuVec4> branches;

        for (int i = 0; i < 400; ++i) {
            shares.push_back(gpuVec4(random.between(0.2f, 3.0f), random.between(2.0f, 40.0f), random.between(0.0f, 0.5f), random.between(0.05f, 1.0f)));
            shares.push_back(gpuVec4(random.between(0.0f, 4.0f), random.between(1.0f, 30.0f)));
            const float slowest = random.between(0.05f, 1.0f);
            branches.push_back(gpuVec4(random.between(0.2f, 40.0f), random.between(0.002f, 0.6f), random.between(1000.0f, 5000.0f), slowest));
            branches.push_back(gpuVec4(slowest + random.between(0.0f, 5.0f), static_cast<float>(i * 7919 % 100000), static_cast<float>(i * 13 % 4000)));
        }

        const std::vector<GpuVec4> shared = runner.run(shader, GPU_TEST_WIND_BONE_SHARE, shares, 2, 1);
        const std::vector<GpuVec4> swung = runner.run(shader, GPU_TEST_WIND_BRANCH, branches, 2, 1);

        // A pow (share); a product and divide (hertz); the phase's hash is exact, its scale one multiply.
        for (std::size_t i = 0; i < shared.size(); ++i) {
            const GpuVec4& s = shares[2 * i];
            const GpuVec4& t = shares[2 * i + 1];
            report.check("windBoneShare", transcendentalTolerance, windBoneShare(s[0], s[1], s[2], s[3], t[0], t[1]), shared[i][0]);
            const GpuVec4& f = branches[2 * i];
            const GpuVec4& g = branches[2 * i + 1];
            report.check("windBranchHertz", arithmeticTolerance, windBranchHertz(f[0], f[1], f[2], f[3], g[0]), swung[i][0]);
            const float phase = windBranchPhase(static_cast<std::uint32_t>(g[1]), static_cast<std::uint32_t>(g[2]));
            report.check("windBranchPhase", arithmeticTolerance, phase, swung[i][1]);
        }
    }

    // Unit directions, shares to 2, leans to 0.5 rad, branches to 3 Hz over 100 s, swing shares to 1.
    void checkTurns(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{83};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            inputs.push_back(gpuVec4(random.direction(), random.between(0.0f, 2.0f)));
            inputs.push_back(gpuVec4(random.between(-0.5f, 0.5f), random.between(-0.5f, 0.5f), random.between(0.1f, 3.0f), random.between(0.0f, 6.28f)));
            inputs.push_back(gpuVec4(random.between(0.0f, 100.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_WIND_BONE_TURN, inputs, 3, 1);

        // A sine and cosine of a phase up to about 1900 radians: their arguments round to about 1e-4 at that size.
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& a = inputs[3 * i];
            const GpuVec4& b = inputs[3 * i + 1];
            const GpuVec4& c = inputs[3 * i + 2];
            const Vec3 expected = windBoneTurn(vec3From(a), a[3], Vec2{b[0], b[1]}, b[2], b[3], c[0], Vec2{c[1], c[2]});
            report.check("windBoneTurn", FloatTolerance{.absolute = 2e-4f, .relative = 1e-3f}, expected, vec3From(outputs[i]));
        }
    }

    // Random parent poses (turns to a radian, shifts to 10 m), own turns to 0.5 rad, pivots and places within 20 m.
    void checkPoses(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{84};
        std::vector<GpuVec4> inputs;
        std::vector<GpuVec4> spheres;

        for (int i = 0; i < 300; ++i) {
            inputs.push_back(gpuVec4(asVec4(randomTurn(random))));
            inputs.push_back(gpuVec4(random.direction() * random.between(0.0f, 10.0f)));
            inputs.push_back(gpuVec4(random.direction() * random.between(0.0f, 0.5f)));
            inputs.push_back(gpuVec4(random.direction() * random.between(0.0f, 20.0f)));
            inputs.push_back(gpuVec4(random.direction() * random.between(0.0f, 20.0f), i % 10 == 0 ? 0.0f : (i % 10 == 1 ? 1.0f : random.between(0.0f, 1.0f))));
            inputs.push_back(gpuVec4(random.direction()));

            spheres.push_back(gpuVec4(asVec4(randomTurn(random))));
            spheres.push_back(gpuVec4(random.direction() * random.between(0.0f, 10.0f)));
            spheres.push_back(gpuVec4(random.direction() * random.between(0.0f, 10.0f), random.between(0.0f, 3.0f)));
            // Every fourth second sphere inside the first, so both of the union's early answers are reached.
            const float radius = i % 4 == 0 ? 0.1f : random.between(0.0f, 8.0f);
            spheres.push_back(gpuVec4(i % 4 == 0 ? vec3From(spheres.back()) : random.direction() * random.between(0.0f, 10.0f), radius));
        }

        const std::vector<GpuVec4> posed = runner.run(shader, GPU_TEST_WIND_BONE_POSE, inputs, 6, 4);
        const std::vector<GpuVec4> merged = runner.run(shader, GPU_TEST_WIND_SPHERES, spheres, 4, 2);

        // Rotations of places up to 30 m: rounding a few ulps of 30 m.
        const FloatTolerance places{.absolute = 2e-5f, .relative = 1e-5f};

        for (std::size_t i = 0; i < inputs.size() / 6; ++i) {
            const GpuVec4* in = &inputs[6 * i];
            const WindBonePose parent{.turn = Quat{in[0][0], in[0][1], in[0][2], in[0][3]}, .shift = vec3From(in[1])};
            const WindBonePose pose = windBonePose(parent, vec3From(in[2]), vec3From(in[3]));
            checkQuat(report, "windBonePose turn", pose.turn, posed[4 * i]);
            report.check("windBonePose shift", places, pose.shift, vec3From(posed[4 * i + 1]));
            report.check("windBlendedPlace", places, windBlendedPlace(pose, parent, in[4][3], vec3From(in[4])), vec3From(posed[4 * i + 2]));
            report.check("windBlendedDirection", FloatTolerance{.absolute = 1e-5f, .relative = 1e-5f},
                         windBlendedDirection(pose, parent, in[4][3], vec3From(in[5])), vec3From(posed[4 * i + 3]));

            const GpuVec4* s = &spheres[4 * i];
            const WindBonePose carried{.turn = Quat{s[0][0], s[0][1], s[0][2], s[0][3]}, .shift = vec3From(s[1])};
            const Vec4 first{s[2][0], s[2][1], s[2][2], s[2][3]};
            const Vec4 movedSphere = windPosedSphere(carried, first);
            const Vec4 union_ = windSphereUnion(first, Vec4{s[3][0], s[3][1], s[3][2], s[3][3]});
            report.check("windPosedSphere", places, Vec3{movedSphere.x, movedSphere.y, movedSphere.z}, vec3From(merged[2 * i]));
            report.check("windSphereUnion centre", places, Vec3{union_.x, union_.y, union_.z}, vec3From(merged[2 * i + 1]));
            report.check("windSphereUnion radius", places, union_.w, merged[2 * i + 1][3]);
        }
    }
}

void gpuTestWindBones(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("wind_bones.glsl, quat.glsl");
    checkQuaternions(runner, report);
    checkSharesAndBranches(runner, report);
    checkTurns(runner, report);
    checkPoses(runner, report);
}
