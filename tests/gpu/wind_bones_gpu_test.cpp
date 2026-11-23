#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/bone_poses.h"
#include "render/gpu/wind_bones.h"

#include <algorithm>
#include <bit>
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

        // A twig's rigid pose (windRigidPose): turns from any pair (half of them across the double cover), feet up a
        // tree, every weight. Inputs: parent's turn and shift, own's turn and shift, the foot and weight.
        std::vector<GpuVec4> rigid;

        for (int i = 0; i < 300; ++i) {
            rigid.push_back(gpuVec4(asVec4(randomTurn(random))));
            rigid.push_back(gpuVec4(random.direction() * random.between(0.0f, 10.0f)));
            const Quat turn = randomTurn(random);
            rigid.push_back(gpuVec4(asVec4(i % 2 == 0 ? turn : -turn)));
            rigid.push_back(gpuVec4(random.direction() * random.between(0.0f, 10.0f)));
            rigid.push_back(gpuVec4(random.direction() * random.between(0.0f, 20.0f), i % 10 == 0 ? 0.0f : (i % 10 == 1 ? 1.0f : random.between(0.0f, 1.0f))));
        }

        const std::vector<GpuVec4> carried = runner.run(shader, GPU_TEST_WIND_RIGID_POSE, rigid, 5, 2);

        for (std::size_t i = 0; i < carried.size() / 2; ++i) {
            const GpuVec4* in = &rigid[5 * i];
            const WindBonePose parent{.turn = Quat{in[0][0], in[0][1], in[0][2], in[0][3]}, .shift = vec3From(in[1])};
            const WindBonePose own{.turn = Quat{in[2][0], in[2][1], in[2][2], in[2][3]}, .shift = vec3From(in[3])};
            const WindBonePose expected = windRigidPose(own, parent, in[4][3], vec3From(in[4]));
            checkQuat(report, "windRigidPose turn", expected.turn, carried[2 * i]);
            // The shift: a foot up to 20 m turned, as windBonePose's (a few ulps of 30 m).
            report.check("windRigidPose shift", FloatTolerance{.absolute = 2e-5f, .relative = 1e-5f}, expected.shift, vec3From(carried[2 * i + 1]));
        }
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

namespace {
    /*
    * The bones pass's maths: a bone's own turn in its tree (random bones, trees, scales, seeds, leans, times, motions),
    * a lean into a plant's frame, and a claim's reach, first pose (fitting, exactly filling, not fitting, near the
    * 32-bit edge) and seed.
    */
    void checkBonePass(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{84};
        std::vector<GpuVec4> turns;
        std::vector<GpuVec4> leans;
        std::vector<GpuVec4> claims;

        for (int i = 0; i < 400; ++i) {
            const Vec3 pivot = random.direction() * random.between(0.0f, 10.0f);
            const Vec3 tip = pivot + random.direction() * random.between(0.0f, 3.0f);
            const auto chainStart = static_cast<std::uint32_t>(i * 37 % 500);
            const auto seed = static_cast<std::uint32_t>(i * 7919 % 100000);
            turns.push_back(gpuVec4(pivot, random.between(0.01f, 0.5f)));
            turns.push_back(gpuVec4(tip, static_cast<float>(chainStart)));
            turns.push_back(gpuVec4(random.between(0.5f, 20.0f), random.between(0.01f, 0.4f), random.between(-0.3f, 0.3f), random.between(-0.3f, 0.3f)));
            turns.push_back(gpuVec4(Vec3{random.between(2.0f, 40.0f), random.between(0.05f, 1.0f), random.between(0.3f, 3.0f)}, static_cast<float>(seed)));
            turns.push_back(gpuVec4(random.between(0.0f, 100.0f)));
            const float slowest = random.between(0.05f, 1.0f);
            turns.push_back(gpuVec4(random.between(0.0f, 4.0f), random.between(1.0f, 20.0f), random.between(1000.0f, 5000.0f), slowest));
            turns.push_back(gpuVec4(slowest + random.between(0.0f, 5.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)));
            leans.push_back(gpuVec4(random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f), random.between(-7.0f, 7.0f)));

            // Integers ride as their bits: what was asked before, the tree's bones, the pool.
            const std::uint32_t pool = i % 4 == 3 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(i * 13 % 3000);
            const std::uint32_t asked = i % 4 == 3 ? 0xFFFFFF00u : static_cast<std::uint32_t>(i * 29 % 3000);
            const std::uint32_t bones = i % 4 == 2 ? pool - std::min(pool, asked) : static_cast<std::uint32_t>(i * 11 % 700);
            claims.push_back(gpuVec4(random.direction() * random.between(0.0f, 600.0f), random.between(1.0f, 500.0f)));
            claims.push_back(gpuVec4(std::bit_cast<float>(asked), std::bit_cast<float>(bones), std::bit_cast<float>(pool), random.between(0.0f, 1.0f)));
            claims.push_back(gpuVec4(random.direction() * random.between(0.0f, 100.0f)));
        }

        const std::vector<GpuVec4> turned = runner.run(shader, GPU_TEST_WIND_BONE_OWN_TURN, turns, 7, 1);
        const std::vector<GpuVec4> leaned = runner.run(shader, GPU_TEST_BONE_LEAN_IN_PLANT, leans, 1, 1);
        const std::vector<GpuVec4> claimed = runner.run(shader, GPU_TEST_BONE_CLAIM, claims, 3, 1);

        // The turn: a pow, a sine and cosine of up to 2 pi times a few hundred radians (100 s at up to 6 Hz).
        constexpr FloatTolerance turnTolerance{.absolute = 1e-4f, .relative = 1e-3f};

        for (std::size_t i = 0; i < turned.size(); ++i) {
            const GpuVec4* t = &turns[7 * i];
            const WindBoneShape bone{.pivot = vec3From(t[0]), .tip = vec3From(t[1]), .radius = t[0][3], .chainStart = static_cast<std::uint32_t>(t[1][3]),
                                     .chainLength = t[2][0], .chainRadius = t[2][1]};
            const WindTreeShape tree{.height = t[3][0], .footRadius = t[3][1], .metresPerUnit = t[3][2], .seed = static_cast<std::uint32_t>(t[3][3])};
            const WindBoneMotion motion{.stiffnessExponent = t[5][0], .mostStiffness = t[5][1], .woodSpeed = t[5][2], .slowestHertz = t[5][3],
                                        .fastestHertz = t[6][0], .swingShare = t[6][1], .sideShare = t[6][2]};
            report.check("windBoneOwnTurn", turnTolerance, windBoneOwnTurn(bone, tree, Vec2{t[2][2], t[2][3]}, t[4][0], motion), vec3From(turned[i]));
            const Vec2 own = windLeanInPlant(Vec2{leans[i][0], leans[i][1]}, leans[i][2]);
            report.check("windLeanInPlant", transcendentalTolerance, own.x, leaned[i][0]);
            report.check("windLeanInPlant", transcendentalTolerance, own.y, leaned[i][1]);

            const GpuVec4* c = &claims[3 * i];
            const bool wanted = boneTreeWanted(vec3From(c[0]), vec3From(c[2]), c[0][3]);
            const std::uint32_t firstPose =
                boneTreeFirstPose(std::bit_cast<std::uint32_t>(c[1][0]), std::bit_cast<std::uint32_t>(c[1][1]), std::bit_cast<std::uint32_t>(c[1][2]));
            report.check("boneTreeWanted", arithmeticTolerance, wanted ? 1.0f : 0.0f, claimed[i][0]);
            report.check("boneTreeFirstPose", arithmeticTolerance, static_cast<float>(firstPose == boneNoPose),
                         static_cast<float>(std::bit_cast<std::uint32_t>(claimed[i][1]) == boneNoPose));
            report.check("boneTreeFirstPose", arithmeticTolerance, static_cast<float>(firstPose), static_cast<float>(std::bit_cast<std::uint32_t>(claimed[i][1])));
            GpuPlacedPlant picked{};
            picked.pick = c[1][3];
            report.check("boneTreeSeed", arithmeticTolerance, static_cast<float>(boneTreeSeed(picked) == std::bit_cast<std::uint32_t>(claimed[i][2])), 1.0f);
        }

        // What a draw reads (boneClaimedTree, bonePoseSlot): stamps matching or not, 0 (no pass), wrapped; integers as
        // their bits. Inputs: the claim's stamp and tree, the frame's stamp, the first pose; the bone.
        std::vector<GpuVec4> reads;

        for (std::uint32_t i = 0; i < 200; ++i) {
            const std::uint32_t frameStamp = i % 5 == 0 ? 0u : i % 5 == 1 ? 0xFFFFFFFFu : i * 7919u;
            const std::uint32_t claimStamp = i % 3 == 0 ? frameStamp : frameStamp + 1u;
            const std::uint32_t tree = i % 7 == 0 ? boneNoTree : i * 31u;
            reads.push_back(gpuVec4(std::bit_cast<float>(claimStamp), std::bit_cast<float>(tree), std::bit_cast<float>(frameStamp),
                                    std::bit_cast<float>(i * 1024u)));
            reads.push_back(gpuVec4(std::bit_cast<float>(i * 3u % 400u)));
        }

        const std::vector<GpuVec4> read = runner.run(shader, GPU_TEST_BONE_CLAIMED_TREE, reads, 2, 1);

        for (std::size_t i = 0; i < read.size(); ++i) {
            const auto word = [&](std::size_t input, int lane) { return std::bit_cast<std::uint32_t>(reads[2 * i + input][lane]); };
            const auto same = [](std::uint32_t expected, float actual) { return static_cast<float>(expected == std::bit_cast<std::uint32_t>(actual)); };
            report.check("boneClaimedTree", arithmeticTolerance, same(boneClaimedTree(word(0, 0), word(0, 1), word(0, 2)), read[i][0]), 1.0f);
            report.check("bonePoseSlot", arithmeticTolerance, same(bonePoseSlot(word(0, 3), word(1, 0), false), read[i][1]), 1.0f);
            report.check("bonePoseSlot", arithmeticTolerance, same(bonePoseSlot(word(0, 3), word(1, 0), true), read[i][2]), 1.0f);
        }
    }
}

void gpuTestWindBones(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("wind_bones.glsl, quat.glsl, bone_poses.glsl");
    checkQuaternions(runner, report);
    checkSharesAndBranches(runner, report);
    checkTurns(runner, report);
    checkPoses(runner, report);
    checkBonePass(runner, report);
}
