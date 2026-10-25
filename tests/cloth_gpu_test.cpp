#include "render/gpu/cloth_gpu.h"
#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <stdexcept>

namespace {
    Cloth smallSheet() {
        Cloth cloth = makeCloth(Mesh::grid(3, 2, 3.0f, 2.0f), Mat4::identity(), ClothSettings{});
        addClothPin(cloth, 8);
        addClothPin(cloth, 11);
        return cloth;
    }

    // Each point's position with its inverse mass beside it; pinned points weigh nothing.
    void testPoints() {
        const Cloth cloth = smallSheet();
        const std::vector<Vec4> points = packClothPoints(cloth.particles);
        assert(points.size() == cloth.particles.size());
        for (std::size_t index = 0; index < points.size(); ++index) {
            const Vec3& position = cloth.particles.positions[index];
            assert(points[index].x == position.x && points[index].y == position.y && points[index].z == position.z);
            assert(points[index].w == cloth.particles.inverseMasses[index]);
        }
        assert(points[8].w == 0.0f && points[11].w == 0.0f && points[0].w > 0.0f);
    }

    // The constraints keep their order (the colours') and every number.
    void testConstraints() {
        const Cloth cloth = smallSheet();
        const std::vector<GpuStretch> stretch = packStretch(cloth.stretch);
        assert(stretch.size() == cloth.stretch.size());
        for (std::size_t index = 0; index < stretch.size(); ++index) {
            assert(stretch[index].first == cloth.stretch[index].first && stretch[index].second == cloth.stretch[index].second);
            assert(stretch[index].restLength == cloth.stretch[index].restLength);
        }

        const std::vector<GpuBend> bend = packBend(cloth.bend);
        assert(bend.size() == cloth.bend.size() && !bend.empty());
        for (std::size_t index = 0; index < bend.size(); ++index) {
            const BendConstraint& hinge = cloth.bend[index];
            assert(bend[index].wing0 == hinge.wing0 && bend[index].wing1 == hinge.wing1);
            assert(bend[index].edgeStart == hinge.edgeStart && bend[index].edgeEnd == hinge.edgeEnd);
            assert(bend[index].restAngle == hinge.restAngle && bend[index].stiffnessShare == hinge.stiffnessShare);
            for (int weight = 0; weight < 4; ++weight) {
                assert(bend[index].isometricWeights[weight] == hinge.isometricWeights[weight]);
            }
        }
    }

    void testColliders() {
        const GpuCollider sphere = packCollider(Sphere{Vec3{1.0f, 2.0f, 3.0f}, 0.5f});
        assert(sphere.kind == CLOTH_COLLIDER_SPHERE && sphere.radius == 0.5f && sphere.a.x == 1.0f && sphere.a.z == 3.0f);

        const GpuCollider capsule = packCollider(Capsule{Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 2.0f, 0.0f}, 0.25f});
        assert(capsule.kind == CLOTH_COLLIDER_CAPSULE && capsule.radius == 0.25f && capsule.a.y == 1.0f && capsule.b.y == 2.0f);

        const GpuCollider box = packCollider(Aabb{Vec3{-1.0f, 0.0f, -2.0f}, Vec3{1.0f, 0.5f, 2.0f}});
        assert(box.kind == CLOTH_COLLIDER_BOX && box.a.z == -2.0f && box.b.y == 0.5f);
    }

    // Only the pins' points have a slot, their place in the cloth's list.
    void testPinSlots() {
        const std::vector<std::uint32_t> slots = clothPinSlots(smallSheet());
        assert(slots.size() == 12);
        assert(slots[8] == 0 && slots[11] == 1);
        for (std::size_t index = 0; index < slots.size(); ++index) {
            if (index != 8 && index != 11) {
                assert(slots[index] == clothNoPin);
            }
        }
    }

    // A tick's numbers worked out as the CPU solver works them out, so both start from the same bits.
    void testTick() {
        Cloth cloth = smallSheet();
        cloth.settings.substeps = 12;
        cloth.settings.bendingModel = BendingModel::Hinge;
        cloth.pins[1].position = Vec3{2.0f, 1.0f, 0.5f};
        cloth.grab = ClothGrab{3, Vec3{0.0f, -1.0f, 0.25f}};
        const std::vector<Shape> bodies{Sphere{Vec3{}, 1.0f}, Aabb{Vec3{}, Vec3{1.0f, 1.0f, 1.0f}}};

        const ClothTickCommands commands = clothTickCommands(cloth, 1.0f / 120.0f, bodies);
        assert(commands.pinTargets.size() == 2 && commands.pinTargets[1].x == 2.0f);
        assert(commands.grab && commands.grab->particle == 3);
        assert(commands.colliders.size() == 2);

        const GpuClothTick tick = packClothTick(commands, 4, 7);
        const ClothSettings& settings = cloth.settings;
        const float substep = (1.0f / 120.0f) / 12.0f;
        assert(tick.substepSeconds == substep && tick.substeps == 12);
        assert(tick.gravity == settings.gravity);
        assert(tick.stretchCompliance == settings.stretchCompliance / (substep * substep));
        assert(tick.grabCompliance == settings.grabCompliance / (substep * substep));
        assert(tick.bendPerSecondSquared == 1.0f / (substep * substep));
        assert(tick.bendingStiffness == settings.bendingStiffness);
        assert(tick.kept == std::exp(-settings.drag * substep));
        assert(tick.thickness == settings.thickness);
        assert(tick.staticFriction == settings.staticFriction && tick.kineticFriction == settings.kineticFriction);
        assert(tick.bendingModel == CLOTH_BEND_HINGE);
        assert(tick.pinFirst == 4 && tick.colliderFirst == 7 && tick.colliderCount == 2);
        assert(tick.grabParticle == 3 && tick.grabTarget.y == -1.0f && tick.grabTarget.z == 0.25f);

        cloth.grab.reset();
        assert(packClothTick(clothTickCommands(cloth, 1.0f / 120.0f), 0, 0).grabParticle == clothNoGrab);

        ClothTickCommands none = commands;
        none.settings.substeps = 0;
        bool threw = false;
        try {
            packClothTick(none, 0, 0);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    // Per substep: the prediction, each colour of each kind, the end; no bending passes without bending.
    void testDispatches() {
        const Cloth cloth = smallSheet();
        const std::uint32_t stretchColours = static_cast<std::uint32_t>(cloth.stretchColours.size() - 1);
        const std::uint32_t bendColours = static_cast<std::uint32_t>(cloth.bendColours.size() - 1);
        ClothSettings settings = cloth.settings;
        assert(clothDispatchesPerTick(cloth, settings) == 15 * (2 + stretchColours + bendColours));

        settings.bendingStiffness = 0.0f;
        settings.substeps = 4;
        assert(clothDispatchesPerTick(cloth, settings) == 4 * (2 + stretchColours));
    }
}

void testClothGpu() {
    testPoints();
    testConstraints();
    testColliders();
    testPinSlots();
    testTick();
    testDispatches();
}
