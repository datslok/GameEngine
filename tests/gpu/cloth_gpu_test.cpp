#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "cloth/cloth.h"
#include "cloth/cloth_yardstick.h"
#include "math/angles.h"
#include "render/gpu/cloth_gpu.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_cloth.h"
#include "render/vulkan/vulkan_device.h"
#include "render/vulkan/vulkan_mesh.h"
#include "scene/deforming_mesh.h"

#include <array>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <numbers>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("cloth_test");

    // The default fabric's substep: 15 a 120 Hz tick.
    constexpr float substep = (1.0f / 120.0f) / 15.0f;

    /*
    * A hinge's gradients are 1 / a wing's height (about 20 for 5 cm squares), and the edge ends' are differences of two
    * such terms, so their rounding is relative to the terms, not to the result: an absolute allowance of that size.
    */
    constexpr FloatTolerance gradientTolerance{.absolute = 1e-4f, .relative = 1e-4f};

    // Points from vec4s: positions in xyz, inverse masses in w, still, starting where they are.
    ClothParticles particlesOf(const std::vector<Vec4>& points) {
        ClothParticles particles;
        for (const Vec4& point : points) {
            particles.positions.push_back(Vec3{point.x, point.y, point.z});
            particles.previousPositions.push_back(Vec3{point.x, point.y, point.z});
            particles.velocities.push_back(Vec3{});
            particles.inverseMasses.push_back(point.w);
        }
        return particles;
    }

    // An inverse mass: a pinned point a tenth of the time, otherwise a few grams' to a kilogram's.
    float randomWeight(CaseRandom& random) {
        return random.between(0.0f, 1.0f) < 0.1f ? 0.0f : random.between(1.0f, 500.0f);
    }

    Vec3 randomPoint(CaseRandom& random, float size) {
        return Vec3{random.between(-size, size), random.between(-size, size), random.between(-size, size)};
    }

    // A hinge about the size of a cloth's squares: an edge along x and two wings either side, folded and jittered.
    std::vector<Vec4> randomHinge(CaseRandom& random) {
        const float length = random.between(0.02f, 0.08f);
        const float fold = random.between(-2.5f, 2.5f);
        const float height0 = random.between(0.4f, 1.0f) * length;
        const float height1 = random.between(0.4f, 1.0f) * length;
        const Vec3 wing0{random.between(0.1f, 0.9f) * length, height0, 0.0f};
        const Vec3 wing1{random.between(0.1f, 0.9f) * length, -height1 * std::cos(fold), height1 * std::sin(fold)};
        const float jitter = 0.05f * length;
        return {Vec4{wing0.x, wing0.y, wing0.z, randomWeight(random)} , Vec4{wing1.x, wing1.y, wing1.z, randomWeight(random)},
                Vec4{random.between(-jitter, jitter), 0.0f, 0.0f, randomWeight(random)}, Vec4{length, random.between(-jitter, jitter), 0.0f, randomWeight(random)}};
    }

    void checkPredict(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{101};
        const Vec3 gravity{0.0f, -9.81f, 0.0f};
        std::vector<GpuVec4> inputs;
        std::vector<Vec4> points;
        std::vector<Vec3> velocities;
        for (int i = 0; i < 300; ++i) {
            const Vec3 position = randomPoint(random, 2.0f);
            points.push_back(Vec4{position.x, position.y, position.z, randomWeight(random)});
            velocities.push_back(randomPoint(random, 3.0f));
            inputs.push_back(gpuVec4(points.back()));
            inputs.push_back(gpuVec4(velocities.back()));
            inputs.push_back(gpuVec4(gravity * substep, substep));
        }
        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CLOTH_PREDICT, inputs, 3, 3);

        for (std::size_t i = 0; i < points.size(); ++i) {
            ClothParticles particles = particlesOf({points[i]});
            particles.velocities[0] = velocities[i];
            predictCloth(particles, gravity, substep);
            report.check("clothPredict position", arithmeticTolerance, particles.positions[0], vec3From(outputs[i * 3]));
            report.check("clothPredict velocity", arithmeticTolerance, particles.velocities[0], vec3From(outputs[i * 3 + 1]));
            report.check("clothPredict previous", exactTolerance, particles.previousPositions[0], vec3From(outputs[i * 3 + 2]));
        }
    }

    void checkPinPosition(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{103};
        std::vector<GpuVec4> inputs;
        for (int i = 0; i < 300; ++i) {
            const int substeps = 1 + i % 20;
            inputs.push_back(gpuVec4(randomPoint(random, 3.0f)));
            inputs.push_back(gpuVec4(randomPoint(random, 3.0f)));
            inputs.push_back(gpuVec4(static_cast<float>(i % substeps), static_cast<float>(substeps)));
        }
        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CLOTH_PIN_POSITION, inputs, 3, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const Vec3 expected = slidPinPosition(vec3From(inputs[i * 3]), vec3From(inputs[i * 3 + 1]), static_cast<int>(inputs[i * 3 + 2][0]),
                                                  static_cast<int>(inputs[i * 3 + 2][1]));
            report.check("clothPinPosition", arithmeticTolerance, expected, vec3From(outputs[i]));
        }
    }

    // Edges of cloth's size stretched and squashed, with the default fabric's compliance and stiffer and softer ones.
    void checkStretch(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{107};
        std::vector<GpuVec4> inputs;
        std::vector<float> compliances;
        for (int i = 0; i < 400; ++i) {
            const Vec3 first = randomPoint(random, 1.0f);
            const Vec3 second = first + random.direction() * random.between(0.01f, 0.1f);
            const float compliance = i % 4 == 0 ? 0.0f : random.between(1e-5f, 1e-2f);
            compliances.push_back(compliance);
            inputs.push_back(gpuVec4(first, randomWeight(random)));
            inputs.push_back(gpuVec4(second, randomWeight(random)));
            inputs.push_back(gpuVec4(random.between(0.01f, 0.1f), compliance / (substep * substep)));
        }
        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CLOTH_STRETCH, inputs, 3, 2);

        for (std::size_t i = 0; i < compliances.size(); ++i) {
            ClothParticles particles = particlesOf({Vec4{inputs[i * 3][0], inputs[i * 3][1], inputs[i * 3][2], inputs[i * 3][3]},
                                                    Vec4{inputs[i * 3 + 1][0], inputs[i * 3 + 1][1], inputs[i * 3 + 1][2], inputs[i * 3 + 1][3]}});
            solveStretch(particles, {StretchConstraint{0, 1, inputs[i * 3 + 2][0]}}, compliances[i], substep);
            report.check("clothStretch", arithmeticTolerance, particles.positions[0], vec3From(outputs[i * 2]));
            report.check("clothStretch", arithmeticTolerance, particles.positions[1], vec3From(outputs[i * 2 + 1]));
        }
    }

    // Hinges bent from rest, with fabrics from silk to card, through both models' steps.
    void checkBend(GpuFunctionRunner& runner, GpuCheckReport& report, BendingModel model) {
        CaseRandom random{model == BendingModel::Isometric ? 109u : 113u};
        const bool isometric = model == BendingModel::Isometric;
        const float perSecondSquared = 1.0f / (substep * substep);
        std::vector<GpuVec4> inputs;
        std::vector<BendConstraint> hinges;
        std::vector<std::vector<Vec4>> shapes;
        std::vector<float> stiffnesses;
        for (int i = 0; i < 300; ++i) {
            const std::vector<Vec4> rest = randomHinge(random);
            const std::vector<Vec4> bent = randomHinge(random);
            BendConstraint hinge{0, 1, 2, 3};
            hinge.restAngle = hingeAngle(Vec3{rest[0].x, rest[0].y, rest[0].z}, Vec3{rest[1].x, rest[1].y, rest[1].z},
                                         Vec3{rest[2].x, rest[2].y, rest[2].z}, Vec3{rest[3].x, rest[3].y, rest[3].z});
            hinge.stiffnessShare = random.between(2.0f, 12.0f);
            hinge.isometricWeights = isometricBendWeights(Vec3{rest[0].x, rest[0].y, rest[0].z}, Vec3{rest[1].x, rest[1].y, rest[1].z},
                                                          Vec3{rest[2].x, rest[2].y, rest[2].z}, Vec3{rest[3].x, rest[3].y, rest[3].z});
            const float stiffness = random.between(1e-6f, 5e-3f);
            hinges.push_back(hinge);
            shapes.push_back(bent);
            stiffnesses.push_back(stiffness);
            for (const Vec4& point : bent) {
                inputs.push_back(gpuVec4(point));
            }
            if (isometric) {
                const std::array<float, 4>& weights = hinge.isometricWeights;
                inputs.push_back(gpuVec4(weights[0], weights[1], weights[2], weights[3]));
                inputs.push_back(gpuVec4(stiffness, hinge.stiffnessShare, perSecondSquared));
            } else {
                inputs.push_back(gpuVec4(hinge.restAngle, stiffness, hinge.stiffnessShare, perSecondSquared));
            }
        }
        const std::uint32_t inputsPerCase = isometric ? 6 : 5;
        const std::vector<GpuVec4> outputs = runner.run(shader, isometric ? GPU_TEST_CLOTH_ISOMETRIC_BEND : GPU_TEST_CLOTH_HINGE_BEND, inputs,
                                                        inputsPerCase, 4);

        const char* name = isometric ? "clothIsometricBend" : "clothHingeBend";
        const FloatTolerance tolerance = isometric ? arithmeticTolerance : transcendentalTolerance;
        for (std::size_t i = 0; i < hinges.size(); ++i) {
            ClothParticles particles = particlesOf(shapes[i]);
            solveBend(particles, {hinges[i]}, stiffnesses[i], substep, model);
            for (int point = 0; point < 4; ++point) {
                report.check(name, tolerance, particles.positions[point], vec3From(outputs[i * 4 + point]));
            }
        }
    }

    void checkHingeState(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{127};
        std::vector<GpuVec4> inputs;
        for (int i = 0; i < 300; ++i) {
            for (const Vec4& point : randomHinge(random)) {
                inputs.push_back(gpuVec4(point));
            }
        }
        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CLOTH_HINGE_STATE, inputs, 4, 5);

        for (std::size_t i = 0; i < outputs.size() / 5; ++i) {
            const Vec3 wing0 = vec3From(inputs[i * 4]);
            const Vec3 wing1 = vec3From(inputs[i * 4 + 1]);
            const Vec3 edgeStart = vec3From(inputs[i * 4 + 2]);
            const Vec3 edgeEnd = vec3From(inputs[i * 4 + 3]);
            report.check("clothHingeState angle", transcendentalTolerance, hingeAngle(wing0, wing1, edgeStart, edgeEnd), outputs[i * 5][0]);
            const std::array<Vec3, 4> gradient = hingeAngleGradient(wing0, wing1, edgeStart, edgeEnd);
            for (int point = 0; point < 4; ++point) {
                report.check("clothHingeState gradient", gradientTolerance, gradient[point], vec3From(outputs[i * 5 + 1 + point]));
            }
        }
    }

    // Angles over several turns, kept away from the odd multiples of pi where the two sides may wrap either way.
    void checkWrapAngle(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{131};
        std::vector<GpuVec4> inputs;
        while (inputs.size() < 500) {
            const float angle = random.between(-20.0f, 20.0f);
            const float turns = angle / fullTurnRadians;
            if (std::abs(turns - std::round(turns - 0.5f) - 0.5f) > 1e-4f) {
                inputs.push_back(gpuVec4(angle));
            }
        }
        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CLOTH_WRAP_ANGLE, inputs, 1, 1);
        const FloatTolerance wrapTolerance{.absolute = 1e-5f, .relative = 0.0f};
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("clothWrapAngle", wrapTolerance, wrapAngle(inputs[i][0]), outputs[i][0]);
        }
    }

    // A sphere, a capsule or a box about the size of the cloth lab's bodies.
    Shape randomShape(CaseRandom& random, int kind) {
        const Vec3 centre = randomPoint(random, 0.5f);
        if (kind == 0) {
            return Sphere{centre, random.between(0.1f, 0.6f)};
        }
        if (kind == 1) {
            return Capsule{centre, centre + random.direction() * random.between(0.0f, 0.8f), random.between(0.05f, 0.4f)};
        }
        const Vec3 half{random.between(0.05f, 0.6f), random.between(0.05f, 0.6f), random.between(0.05f, 0.6f)};
        return Aabb{centre - half, centre + half};
    }

    void pushShape(std::vector<GpuVec4>& inputs, const Shape& shape) {
        const GpuCollider collider = packCollider(shape);
        inputs.push_back(gpuVec4(static_cast<float>(collider.kind), collider.radius));
        inputs.push_back(gpuVec4(collider.a));
        inputs.push_back(gpuVec4(collider.b));
    }

    void checkSurfacePoint(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{137};
        std::vector<GpuVec4> inputs;
        std::vector<Shape> shapes;
        std::vector<Vec3> points;
        for (int i = 0; i < 600; ++i) {
            shapes.push_back(randomShape(random, i % 3));
            points.push_back(randomPoint(random, 1.2f));
            pushShape(inputs, shapes.back());
            inputs.push_back(gpuVec4(points.back()));
        }
        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CLOTH_SURFACE_POINT, inputs, 4, 1);

        for (std::size_t i = 0; i < shapes.size(); ++i) {
            const SurfacePoint expected = surfacePoint(shapes[i], points[i]);
            report.check("clothSurfacePoint normal", arithmeticTolerance, expected.normal, vec3From(outputs[i]));
            report.check("clothSurfacePoint distance", arithmeticTolerance, expected.distance, outputs[i][3]);
        }
    }

    // Points near a body's surface, inside and out, sliding by up to a few millimetres a substep.
    void checkContact(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{139};
        ClothSettings settings;
        std::vector<GpuVec4> inputs;
        std::vector<Shape> shapes;
        std::vector<Vec3> points;
        std::vector<Vec3> previous;
        for (int i = 0; i < 600; ++i) {
            shapes.push_back(randomShape(random, i % 3));
            const Vec3 centre = std::visit([](const auto& held) {
                using Held = std::decay_t<decltype(held)>;
                if constexpr (std::is_same_v<Held, Sphere>) {
                    return held.centre;
                } else if constexpr (std::is_same_v<Held, Capsule>) {
                    return held.a;
                } else {
                    return (held.minimum + held.maximum) * 0.5f;
                }
            }, shapes.back());
            const Vec3 outward = random.direction();
            Vec3 point = centre + outward;
            point = point - surfacePoint(shapes.back(), point).normal * (surfacePoint(shapes.back(), point).distance + random.between(-0.01f, 0.01f));
            points.push_back(point);
            previous.push_back(point + randomPoint(random, 0.004f));
            pushShape(inputs, shapes.back());
            inputs.push_back(gpuVec4(points.back()));
            inputs.push_back(gpuVec4(previous.back()));
            inputs.push_back(gpuVec4(settings.thickness, settings.staticFriction, settings.kineticFriction));
        }
        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CLOTH_CONTACT, inputs, 6, 1);

        for (std::size_t i = 0; i < shapes.size(); ++i) {
            ClothParticles particles = particlesOf({Vec4{points[i].x, points[i].y, points[i].z, 1.0f}});
            particles.previousPositions[0] = previous[i];
            solveContacts(particles, {shapes[i]}, settings);
            report.check("clothContact", arithmeticTolerance, particles.positions[0], vec3From(outputs[i]));
        }
    }

    void checkGrab(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{149};
        const float compliance = ClothSettings{}.grabCompliance;
        std::vector<GpuVec4> inputs;
        for (int i = 0; i < 300; ++i) {
            const Vec3 point = randomPoint(random, 1.0f);
            inputs.push_back(gpuVec4(point, randomWeight(random)));
            inputs.push_back(gpuVec4(point + randomPoint(random, 0.3f)));
            inputs.push_back(gpuVec4(compliance / (substep * substep)));
        }
        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CLOTH_GRAB, inputs, 3, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            ClothParticles particles = particlesOf({Vec4{inputs[i * 3][0], inputs[i * 3][1], inputs[i * 3][2], inputs[i * 3][3]}});
            solveGrab(particles, ClothGrab{0, vec3From(inputs[i * 3 + 1])}, compliance, substep);
            report.check("clothGrab", arithmeticTolerance, particles.positions[0], vec3From(outputs[i]));
        }
    }

    void checkFinishVelocity(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{151};
        const float kept = std::exp(-ClothSettings{}.drag * substep);
        std::vector<GpuVec4> inputs;
        for (int i = 0; i < 300; ++i) {
            const Vec3 point = randomPoint(random, 1.0f);
            inputs.push_back(gpuVec4(point, randomWeight(random)));
            inputs.push_back(gpuVec4(point + randomPoint(random, 0.005f)));
            inputs.push_back(gpuVec4(substep, kept));
        }
        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CLOTH_FINISH_VELOCITY, inputs, 3, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            ClothParticles particles = particlesOf({Vec4{inputs[i * 3][0], inputs[i * 3][1], inputs[i * 3][2], inputs[i * 3][3]}});
            particles.previousPositions[0] = vec3From(inputs[i * 3 + 1]);
            finishClothStep(particles, substep);
            Vec3 expected = particles.velocities[0];
            if (particles.inverseMasses[0] > 0.0f) {
                expected = expected * kept;
            }
            report.check("clothFinishVelocity", arithmeticTolerance, expected, vec3From(outputs[i]));
        }
    }

    // A 3 x 3 block as the shaders take it: its diagonal (xx, yy, zz), then the rest (xy, xz, yz).
    void pushBlock(std::vector<GpuVec4>& inputs, const ClothStiffnessBlock& block) {
        inputs.push_back(gpuVec4(block.xx, block.yy, block.zz));
        inputs.push_back(gpuVec4(block.xy, block.xz, block.yz));
    }

    ClothStiffnessBlock blockFrom(const GpuVec4& diagonal, const GpuVec4& rest) {
        ClothStiffnessBlock block;
        block.xx = diagonal[0];
        block.yy = diagonal[1];
        block.zz = diagonal[2];
        block.xy = rest[0];
        block.xz = rest[1];
        block.yz = rest[2];
        return block;
    }

    // A block like a point's: its inertia on the diagonal, a few edges' stiffness along random directions.
    ClothStiffnessBlock randomBlock(CaseRandom& random) {
        ClothStiffnessBlock block;
        block.addDiagonal(random.between(500.0f, 5000.0f));
        for (int edge = 0; edge < 4; ++edge) {
            block.addOuter(random.direction(), random.between(0.0f, 4000.0f));
        }
        return block;
    }

    void checkBlock(GpuCheckReport& report, const std::string& name, const FloatTolerance& tolerance, const ClothStiffnessBlock& expected,
                    const GpuVec4& diagonal, const GpuVec4& rest) {
        const ClothStiffnessBlock actual = blockFrom(diagonal, rest);
        report.check(name + " block", tolerance, Vec3{expected.xx, expected.yy, expected.zz}, Vec3{actual.xx, actual.yy, actual.zz});
        report.check(name + " block", tolerance, Vec3{expected.xy, expected.xz, expected.yz}, Vec3{actual.xy, actual.xz, actual.yz});
    }

    /*
    * Each of phi's per-point terms (cloth_energy.h's add*Term) added to a force and block already holding some, with
    * the default fabric's numbers and others round them. An edge's stiffness across it is k (1 - r / L), which near
    * rest is a difference of nearly equal numbers, and its force k (L - r) another: their rounding is relative to k and
    * L, not to the result, so an absolute allowance of a millionth of the stiffnesses used (about 5 N/m in 4,000).
    */
    void checkPointTerms(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const FloatTolerance edgeTolerance{.absolute = 5e-3f, .relative = 1e-5f};
        const std::array<std::uint32_t, 5> functions{GPU_TEST_CLOTH_INERTIA_TERM, GPU_TEST_CLOTH_EDGE_TERM, GPU_TEST_CLOTH_ISOMETRIC_HINGE_TERM,
                                                     GPU_TEST_CLOTH_HINGE_ANGLE_TERM, GPU_TEST_CLOTH_HAND_TERM};
        const std::array<const char*, 5> names{"clothInertiaTerm", "clothEdgeTerm", "clothIsometricHingeTerm", "clothHingeAngleTerm", "clothHandTerm"};
        for (std::size_t kind = 0; kind < functions.size(); ++kind) {
            CaseRandom random{157 + static_cast<std::uint32_t>(kind)};
            std::vector<GpuVec4> inputs;
            std::vector<Vec3> forces;
            std::vector<ClothStiffnessBlock> blocks;
            for (int i = 0; i < 300; ++i) {
                Vec3 force = randomPoint(random, 50.0f);
                ClothStiffnessBlock block = randomBlock(random);
                inputs.push_back(gpuVec4(force));
                pushBlock(inputs, block);

                const Vec3 here = randomPoint(random, 1.0f);
                if (functions[kind] == GPU_TEST_CLOTH_INERTIA_TERM) {
                    const Vec3 predicted = here + randomPoint(random, 0.001f);
                    const float inertia = random.between(100.0f, 1e5f);
                    inputs.push_back(gpuVec4(here));
                    inputs.push_back(gpuVec4(predicted, inertia));
                    inputs.push_back(gpuVec4(0.0f));
                    addInertiaTerm(force, &block, here, predicted, inertia);
                } else if (functions[kind] == GPU_TEST_CLOTH_EDGE_TERM) {
                    const float restLength = random.between(0.01f, 0.1f);
                    const Vec3 other = here + random.direction() * (restLength * random.between(0.9f, 1.1f));
                    const float stiffness = random.between(1000.0f, 4000.0f);
                    inputs.push_back(gpuVec4(here));
                    inputs.push_back(gpuVec4(other, restLength));
                    inputs.push_back(gpuVec4(stiffness));
                    addEdgeTerm(force, &block, here, other, restLength, stiffness);
                } else if (functions[kind] == GPU_TEST_CLOTH_ISOMETRIC_HINGE_TERM) {
                    const Vec3 curvature = randomPoint(random, 0.01f);
                    const float weight = random.between(-2.0f, 2.0f);
                    const float stiffness = random.between(1e-6f, 1e-3f);
                    inputs.push_back(gpuVec4(curvature, weight));
                    inputs.push_back(gpuVec4(stiffness));
                    inputs.push_back(gpuVec4(0.0f));
                    addIsometricHingeTerm(force, &block, curvature, weight, stiffness);
                } else if (functions[kind] == GPU_TEST_CLOTH_HINGE_ANGLE_TERM) {
                    const Vec3 slope = randomPoint(random, 40.0f);
                    const float error = random.between(-3.0f, 3.0f);
                    const float stiffness = random.between(1e-6f, 1e-3f);
                    inputs.push_back(gpuVec4(slope, error));
                    inputs.push_back(gpuVec4(stiffness));
                    inputs.push_back(gpuVec4(0.0f));
                    addHingeAngleTerm(force, &block, slope, error, stiffness);
                } else {
                    const Vec3 target = here + randomPoint(random, 0.3f);
                    const float stiffness = energyStiffness(ClothSettings{}.grabCompliance) * random.between(0.5f, 2.0f);
                    inputs.push_back(gpuVec4(here));
                    inputs.push_back(gpuVec4(target, stiffness));
                    inputs.push_back(gpuVec4(0.0f));
                    addHandTerm(force, &block, here, target, stiffness);
                }
                forces.push_back(force);
                blocks.push_back(block);
            }
            const std::vector<GpuVec4> outputs = runner.run(shader, functions[kind], inputs, 6, 3);

            const FloatTolerance& tolerance = functions[kind] == GPU_TEST_CLOTH_EDGE_TERM ? edgeTolerance : arithmeticTolerance;
            for (std::size_t i = 0; i < forces.size(); ++i) {
                report.check(std::string(names[kind]) + " force", tolerance, forces[i], vec3From(outputs[i * 3]));
                checkBlock(report, names[kind], tolerance, blocks[i], outputs[i * 3 + 1], outputs[i * 3 + 2]);
            }
        }
    }

    /*
    * A point's Newton step: blocks like a point's (its inertia and its edges' stiffness, a condition number of ten or
    * so, which the adjugate's differences of products round by about that many times float's precision), and an empty
    * block, which moves nothing. (A block only nearly singular is no case: rounding decides which side of the threshold
    * its determinant lands, differently on each side.)
    */
    void checkBlockSolve(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const FloatTolerance solveTolerance{.absolute = 1e-7f, .relative = 1e-4f};
        CaseRandom random{167};
        std::vector<GpuVec4> inputs;
        std::vector<ClothStiffnessBlock> blocks;
        std::vector<Vec3> forces;
        for (int i = 0; i < 400; ++i) {
            ClothStiffnessBlock block = randomBlock(random);
            if (i % 50 == 0) {
                block = ClothStiffnessBlock{};
            }
            const Vec3 force = randomPoint(random, 50.0f);
            pushBlock(inputs, block);
            inputs.push_back(gpuVec4(force));
            blocks.push_back(block);
            forces.push_back(force);
        }
        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_CLOTH_BLOCK_SOLVE, inputs, 3, 1);

        for (std::size_t i = 0; i < blocks.size(); ++i) {
            report.check("clothBlockSolve", solveTolerance, blocks[i].solve(forces[i]), vec3From(outputs[i]));
        }
    }

    /*
    * A busy sheet: hung flat from two pins sliding sideways, a hand pulling its near edge for the first half second,
    * falling onto a ball, a capsule and a box. Kept: the cloth before each tick, the tick's commands and the cloth after.
    */
    struct TickCase {
        Cloth before;
        ClothTickCommands commands;
        Cloth after;
    };

    std::vector<TickCase> busySheet(BendingModel model, ClothMethod method) {
        ClothSettings settings;
        settings.bendingModel = model;
        settings.method = method;
        const Mat4 level = Mat4::translation(0.0f, 0.6f, 0.0f) * Mat4::rotationX(-0.5f * std::numbers::pi_v<float>);
        Cloth cloth = makeCloth(Mesh::grid(16, 16, 1.0f, 1.0f), level, settings);
        addClothPin(cloth, 16 * 17);
        addClothPin(cloth, 16 * 17 + 16);
        const std::vector<Shape> bodies{Sphere{Vec3{0.1f, 0.2f, 0.0f}, 0.25f}, Capsule{Vec3{-0.4f, 0.1f, -0.3f}, Vec3{0.4f, 0.1f, -0.3f}, 0.08f},
                                        Aabb{Vec3{-0.6f, -0.2f, -0.6f}, Vec3{0.6f, 0.0f, 0.6f}}};

        std::vector<TickCase> cases;
        const Vec3 pinStart0 = cloth.pins[0].position;
        const Vec3 pinStart1 = cloth.pins[1].position;
        const float tickSeconds = 1.0f / 120.0f;
        for (int tick = 1; tick <= 120; ++tick) {
            const Cloth before = cloth;
            const Vec3 along{0.002f * static_cast<float>(tick), 0.0f, 0.0f};
            cloth.pins[0].position = pinStart0 + along;
            cloth.pins[1].position = pinStart1 + along;
            if (tick <= 60) {
                cloth.grab = ClothGrab{8, Vec3{0.0f, 0.8f + 0.004f * static_cast<float>(tick), 0.7f}};
            } else {
                cloth.grab.reset();
            }
            const ClothTickCommands commands = clothTickCommands(cloth, tickSeconds, bodies);
            stepCloth(cloth, tickSeconds, bodies);
            cases.push_back(TickCase{before, commands, cloth});
        }
        return cases;
    }

    // Each solver the GPU runs, with each bending model.
    struct SolverCase {
        ClothMethod method;
        BendingModel model;
        const char* name;
    };

    constexpr std::array<SolverCase, 4> solverCases{
        SolverCase{ClothMethod::Xpbd, BendingModel::Isometric, "VulkanCloth isometric"},
        SolverCase{ClothMethod::Xpbd, BendingModel::Hinge, "VulkanCloth hinge"},
        SolverCase{ClothMethod::VertexBlockDescent, BendingModel::Isometric, "VulkanCloth block descent isometric"},
        SolverCase{ClothMethod::VertexBlockDescent, BendingModel::Hinge, "VulkanCloth block descent hinge"},
    };

    std::vector<Vec3> runOnGpu(const VulkanDevice& device, const VulkanClothPipelines& pipelines, ImmediateCommands& commands, const Cloth& start,
                               std::span<const ClothTickCommands> ticks) {
        VulkanCloth cloth{device, pipelines, commands, start};
        commands.run([&](VkCommandBuffer buffer) {
            cloth.recordTicks(buffer, ticks, 0);
            cloth.recordReadback(buffer, 0);
        });
        return cloth.readPositions(0);
    }

    /*
    * A busy sheet is chaotic: nudging one point by a micrometre on the CPU alone moves it by tens of centimetres within
    * a second (measured: 0.2 mm after 10 ticks, 10 cm after 30, as far as the GPU's rounding moves it), so whole runs
    * cannot be compared point by point. Instead each of its ticks runs on the GPU from the CPU's state before it, with
    * the CPU's commands, and lands within rounding of the CPU's state after it; a missed pass, a wrong order or a sign
    * moves points by millimetres or more in a tick. The whole run, twice on the GPU, gives the same bits. Both
    * methods: XPBD's constraint passes and vertex block descent's point passes.
    */
    void checkSolverTicks(const VulkanDevice& device, const VulkanClothPipelines& pipelines, ImmediateCommands& commands, GpuCheckReport& report) {
        const FloatTolerance tickTolerance{.absolute = 1e-5f, .relative = 0.0f};
        for (const SolverCase& solver : solverCases) {
            const std::vector<TickCase> cases = busySheet(solver.model, solver.method);
            const std::string name = solver.name;
            for (const TickCase& tick : cases) {
                const std::vector<Vec3> gpu = runOnGpu(device, pipelines, commands, tick.before, std::span<const ClothTickCommands>{&tick.commands, 1});
                for (std::size_t point = 0; point < gpu.size(); ++point) {
                    report.check(name + " tick against stepCloth", tickTolerance, tick.after.particles.positions[point], gpu[point]);
                }
            }

            std::vector<ClothTickCommands> all;
            for (const TickCase& tick : cases) {
                all.push_back(tick.commands);
            }
            const std::vector<Vec3> once = runOnGpu(device, pipelines, commands, cases.front().before, all);
            const std::vector<Vec3> again = runOnGpu(device, pipelines, commands, cases.front().before, all);
            for (std::size_t point = 0; point < once.size(); ++point) {
                report.check(name + " repeated", exactTolerance, once[point], again[point]);
            }
        }
    }

    /*
    * A sheet hanging still is not chaotic: it settles the same way from nearby starts, so after two seconds on the GPU
    * its yardstick measures (cloth/cloth_yardstick.h) match the CPU's to well under the differences the yardstick looks
    * for (a tenth of a millimetre, against centimetres between rungs). With either method.
    */
    void checkSettledSheet(const VulkanDevice& device, const VulkanClothPipelines& pipelines, ImmediateCommands& commands, GpuCheckReport& report) {
        const float tickSeconds = 1.0f / 120.0f;
        for (const ClothMethod method : {ClothMethod::Xpbd, ClothMethod::VertexBlockDescent}) {
            ClothSettings settings;
            settings.method = method;
            Cloth cpu = yardstickSheet(10, settings);
            const std::vector<ClothTickCommands> ticks(240, clothTickCommands(cpu, tickSeconds));
            Cloth gpu = cpu;
            gpu.particles.positions = runOnGpu(device, pipelines, commands, cpu, ticks);
            settleYardstick(cpu, 2.0f, tickSeconds);

            const SheetMeasure expected = measureSheet(cpu, 10);
            const SheetMeasure actual = measureSheet(gpu, 10);
            const FloatTolerance settledTolerance{.absolute = 1e-4f, .relative = 0.0f};
            const std::string name = method == ClothMethod::Xpbd ? "VulkanCloth settled" : "VulkanCloth block descent settled";
            report.check(name + " path stretch", settledTolerance, expected.pathStretch, actual.pathStretch);
            report.check(name + " worst edge", settledTolerance, expected.worstEdgeStrain, actual.worstEdgeStrain);
            report.check(name + " sag", settledTolerance, expected.sag, actual.sag);
        }
    }
}

namespace {
    // deform.glsl's steps against deforming_mesh.cpp's, on triangles of cloth's size, some squashed or with flat UVs.
    void checkDeformSteps(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{163};
        std::vector<GpuVec4> blendInputs;
        std::vector<GpuVec4> triangleInputs;
        std::vector<GpuVec4> tangentInputs;
        std::vector<GpuVec4> normalInputs;
        std::vector<GpuVec4> directionInputs;
        for (int i = 0; i < 300; ++i) {
            const Vec3 first = randomPoint(random, 1.0f);
            const Vec3 second = first + randomPoint(random, 0.05f);
            const Vec3 third = i % 10 == 0 ? second : first + randomPoint(random, 0.05f);
            blendInputs.push_back(gpuVec4(first));
            blendInputs.push_back(gpuVec4(second));
            blendInputs.push_back(gpuVec4(random.between(0.0f, 1.0f)));
            triangleInputs.push_back(gpuVec4(first));
            triangleInputs.push_back(gpuVec4(second));
            triangleInputs.push_back(gpuVec4(third));
            const Vec2 uv0{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)};
            const Vec2 uv1{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)};
            const Vec2 uv2 = i % 7 == 0 ? uv1 : Vec2{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)};
            tangentInputs.push_back(gpuVec4(first));
            tangentInputs.push_back(gpuVec4(second));
            tangentInputs.push_back(gpuVec4(third));
            tangentInputs.push_back(gpuVec4(uv0.x, uv0.y, uv1.x, uv1.y));
            tangentInputs.push_back(gpuVec4(uv2.x, uv2.y));
            normalInputs.push_back(gpuVec4(i % 10 == 0 ? Vec3{} : randomPoint(random, 0.01f)));
            const Vec3 normal = random.direction();
            directionInputs.push_back(gpuVec4(i % 10 == 0 ? normal * 0.5f : randomPoint(random, 0.01f)));
            directionInputs.push_back(gpuVec4(normal));
        }

        const std::vector<GpuVec4> blended = runner.run(shader, GPU_TEST_DEFORM_BLEND, blendInputs, 3, 1);
        const std::vector<GpuVec4> normals = runner.run(shader, GPU_TEST_DEFORM_AREA_NORMAL, triangleInputs, 3, 1);
        const std::vector<GpuVec4> tangents = runner.run(shader, GPU_TEST_DEFORM_TANGENT, tangentInputs, 5, 1);
        const std::vector<GpuVec4> unitNormals = runner.run(shader, GPU_TEST_DEFORM_NORMAL, normalInputs, 1, 1);
        const std::vector<GpuVec4> directions = runner.run(shader, GPU_TEST_DEFORM_TANGENT_DIRECTION, directionInputs, 2, 1);

        for (std::size_t i = 0; i < blended.size(); ++i) {
            DeformingMesh mesh;
            mesh.previousPositions = {vec3From(blendInputs[i * 3])};
            mesh.positions = {vec3From(blendInputs[i * 3 + 1])};
            report.check("deformBlend", arithmeticTolerance, blendedPositions(mesh, blendInputs[i * 3 + 2][0])[0], vec3From(blended[i]));

            const Vec3 first = vec3From(triangleInputs[i * 3]);
            const Vec3 second = vec3From(triangleInputs[i * 3 + 1]);
            const Vec3 third = vec3From(triangleInputs[i * 3 + 2]);
            report.check("deformAreaNormal", arithmeticTolerance, triangleAreaNormal(first, second, third), vec3From(normals[i]));

            const GpuVec4& uvs = tangentInputs[i * 5 + 3];
            const TriangleTangent along = triangleTangent(first, second, third, Vec2{uvs[0], uvs[1]}, Vec2{uvs[2], uvs[3]},
                                                          Vec2{tangentInputs[i * 5 + 4][0], tangentInputs[i * 5 + 4][1]});
            report.check("deformTangent", arithmeticTolerance, along.alongU, vec3From(tangents[i]));
            report.check("deformTangent has UV area", exactTolerance, along.hasUvArea ? 1.0f : 0.0f, tangents[i][3]);

            report.check("deformNormal", arithmeticTolerance, deformedNormal(vec3From(normalInputs[i])), vec3From(unitNormals[i]));
            report.check("deformTangentDirection", arithmeticTolerance,
                         deformedTangent(vec3From(directionInputs[i * 2]), vec3From(directionInputs[i * 2 + 1])), vec3From(directions[i]));
        }
    }

    // A mesh slot's vertices copied out, after everything recorded before it.
    std::vector<MeshVertex> readSlot(const VulkanDevice& device, ImmediateCommands& commands, const VulkanMesh& mesh, std::uint32_t slot) {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(mesh.getVertexCount()) * sizeof(MeshVertex);
        VulkanBuffer readback{device.getAllocator(), bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback};
        commands.run([&](VkCommandBuffer buffer) {
            VkMemoryBarrier2 toCopy{};
            toCopy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            toCopy.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            toCopy.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            toCopy.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            toCopy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &toCopy;
            vkCmdPipelineBarrier2(buffer, &dependency);

            const VkBufferCopy region{0, 0, bytes};
            vkCmdCopyBuffer(buffer, mesh.getSlotBuffer(slot), readback.get(), 1, &region);

            VkMemoryBarrier2 toHost = toCopy;
            toHost.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            dependency.pMemoryBarriers = &toHost;
            vkCmdPipelineBarrier2(buffer, &dependency);
        });
        readback.invalidate();
        std::vector<MeshVertex> vertices(mesh.getVertexCount());
        std::memcpy(vertices.data(), readback.getMapped(), static_cast<std::size_t>(bytes));
        return vertices;
    }

    /*
    * The shape pass against deformVertices: a sheet falls two ticks on the GPU, its points read back after each, and its
    * mesh is shaped a third of the way between them. The CPU shapes the same blend of the GPU's own points, so only the
    * shape pass's arithmetic differs (positions, normals and tangents within rounding; UVs and tangent sides copied).
    */
    void checkShapePass(const VulkanDevice& device, const VulkanClothPipelines& pipelines, ImmediateCommands& commands, GpuCheckReport& report) {
        const Mesh grid = Mesh::grid(8, 6, 1.0f, 0.75f);
        const DeformingTopology topology = buildDeformingTopology(grid);
        VulkanMesh mesh{device, commands, topology.rest, true};
        Cloth cloth = makeCloth(grid, Mat4::rotationX(-0.6f), ClothSettings{});
        addClothPin(cloth, 6 * 9);
        addClothPin(cloth, 6 * 9 + 8);
        VulkanCloth gpu{device, pipelines, commands, cloth, 2};
        gpu.attachShape(commands, topology, mesh);
        const ClothTickCommands tick = clothTickCommands(cloth, 1.0f / 120.0f);

        commands.run([&](VkCommandBuffer buffer) {
            gpu.recordTicks(buffer, std::span<const ClothTickCommands>{&tick, 1}, 0);
            gpu.recordReadback(buffer, 0);
        });
        const std::vector<Vec3> before = gpu.readPositions(0);

        const float alpha = 1.0f / 3.0f;
        const std::uint32_t slot = mesh.advanceGpuShape(1);
        commands.run([&](VkCommandBuffer buffer) {
            gpu.recordTicks(buffer, std::span<const ClothTickCommands>{&tick, 1}, 1);
            gpu.recordShape(buffer, slot, alpha);
            gpu.recordReadback(buffer, 1);
        });
        const std::vector<Vec3> after = gpu.readPositions(1);
        const std::vector<MeshVertex> shaped = readSlot(device, commands, mesh, slot);

        DeformingMesh between;
        between.previousPositions = before;
        between.positions = after;
        std::vector<MeshVertex> expected;
        deformVertices(topology, blendedPositions(between, alpha), expected);
        for (std::size_t vertex = 0; vertex < expected.size(); ++vertex) {
            report.check("cloth_shape position", arithmeticTolerance, expected[vertex].position, shaped[vertex].position);
            report.check("cloth_shape normal", arithmeticTolerance, expected[vertex].normal, shaped[vertex].normal);
            report.check("cloth_shape tangent", arithmeticTolerance, Vec3{expected[vertex].tangent.x, expected[vertex].tangent.y, expected[vertex].tangent.z},
                         Vec3{shaped[vertex].tangent.x, shaped[vertex].tangent.y, shaped[vertex].tangent.z});
            report.check("cloth_shape copied", exactTolerance, Vec3{expected[vertex].uv.x, expected[vertex].uv.y, expected[vertex].tangent.w},
                         Vec3{shaped[vertex].uv.x, shaped[vertex].uv.y, shaped[vertex].tangent.w});
        }
    }
}

void gpuTestCloth(const VulkanDevice& device, GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("cloth.glsl");
    checkPredict(runner, report);
    checkPinPosition(runner, report);
    checkStretch(runner, report);
    checkBend(runner, report, BendingModel::Isometric);
    checkBend(runner, report, BendingModel::Hinge);
    checkHingeState(runner, report);
    checkWrapAngle(runner, report);
    checkSurfacePoint(runner, report);
    checkContact(runner, report);
    checkGrab(runner, report);
    checkFinishVelocity(runner, report);
    checkPointTerms(runner, report);
    checkBlockSolve(runner, report);
    const VulkanClothPipelines pipelines{device};
    ImmediateCommands commands{device};
    checkSolverTicks(device, pipelines, commands, report);
    checkSettledSheet(device, pipelines, commands, report);
    checkDeformSteps(runner, report);
    checkShapePass(device, pipelines, commands, report);
}

