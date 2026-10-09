#include "ecs/world.h"
#include "math/transform.h"
#include "scene/frame_description.h"
#include "scene/interpolation.h"
#include "scene/light.h"
#include "scene/model_renderer.h"

#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.00001f;
    }

    // Where a model matrix puts the origin: its translation column.
    bool placesOriginAt(const Mat4& model, float x, float y, float z) {
        return nearlyEqual(model.values[0][3], x) && nearlyEqual(model.values[1][3], y) && nearlyEqual(model.values[2][3], z);
    }

    Camera makeTestCamera() {
        return Camera{Vec3{0.0f, 1.0f, 5.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.5f, 0.1f, 100.0f};
    }

    Transform placedAt(float x, float y, float z) {
        Transform transform;
        transform.position = Vec3{x, y, z};
        return transform;
    }

    // The frame carries its own copy of the camera, so the renderer can adjust it (aspect ratio) without touching the game's.
    void testCameraIsCopied() {
        World world;
        const FrameDescription frame = buildFrame(world, makeTestCamera(), 0.0f);

        assert(nearlyEqual(frame.camera.getPosition().z, 5.0f));
        assert(nearlyEqual(frame.camera.getAspectRatio(), 1.5f));
        assert(frame.draws.empty());

        // Lights are ranked by their brightness at the camera unless the game names a better point.
        assert(nearlyEqual(frame.lightFocus.z, 5.0f) && nearlyEqual(frame.lightFocus.y, 1.0f));
    }

    // The frame's lights are the World's lights, gathered the same way collectLighting does.
    void testLightingIsCollected() {
        World world;
        world.add(world.create(), AmbientLight{Vec3{0.1f, 0.2f, 0.3f}});
        world.add(world.create(), DirectionalLight{Vec3{0.0f, -1.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, 0.5f});

        const FrameDescription frame = buildFrame(world, makeTestCamera(), 0.0f);

        assert(nearlyEqual(frame.lighting.ambient.y, 0.2f));
        assert(frame.lighting.directionalLights.size() == 1);
    }

    // A model with several parts stays one entity, but each part is its own draw, placed by the entity times the part's local transform.
    void testEachPartIsOneDraw() {
        World world;
        const Entity entity = world.create();
        world.add(entity, placedAt(10.0f, 0.0f, 0.0f));

        Material red;
        red.colour = Pixel{255, 0, 0};

        ModelRenderer renderer;
        renderer.parts.push_back(RenderPart{MeshHandle{3}, red, Mat4::identity()});
        renderer.parts.push_back(RenderPart{MeshHandle{4}, Material{}, Mat4::translation(0.0f, 2.0f, 0.0f)});
        world.add(entity, renderer);

        const FrameDescription frame = buildFrame(world, makeTestCamera(), 0.0f);

        assert(frame.draws.size() == 2);
        assert(frame.draws[0].mesh == MeshHandle{3});
        assert(frame.draws[0].material.colour.g == 0);
        assert(placesOriginAt(frame.draws[0].model, 10.0f, 0.0f, 0.0f));
        assert(frame.draws[1].mesh == MeshHandle{4});
        assert(placesOriginAt(frame.draws[1].model, 10.0f, 2.0f, 0.0f));
    }

    // Hidden models and models with nowhere to be drawn (no Transform) are left out.
    void testHiddenAndUnplacedModelsAreSkipped() {
        World world;

        const Entity hidden = world.create();
        world.add(hidden, Transform{});
        ModelRenderer hiddenRenderer = makeMeshRenderer(MeshHandle{0});
        hiddenRenderer.visible = false;
        world.add(hidden, hiddenRenderer);

        const Entity unplaced = world.create();
        world.add(unplaced, makeMeshRenderer(MeshHandle{1}));

        const FrameDescription frame = buildFrame(world, makeTestCamera(), 0.0f);

        assert(frame.draws.empty());
    }

    // Moving entities are drawn alpha of the way from their previous tick to the current one, like before.
    void testMovingEntitiesAreInterpolated() {
        World world;
        const Entity entity = world.create();
        world.add(entity, placedAt(4.0f, 0.0f, 0.0f));
        world.add(entity, PreviousTransform{placedAt(2.0f, 0.0f, 0.0f)});
        world.add(entity, makeMeshRenderer(MeshHandle{0}));

        const FrameDescription frame = buildFrame(world, makeTestCamera(), 0.5f);

        assert(frame.draws.size() == 1);
        assert(placesOriginAt(frame.draws[0].model, 3.0f, 0.0f, 0.0f));
    }
}

namespace {
    // A model can opt out of casting shadows (the moon sphere surrounds its own light); the draw carries the choice to the renderer.
    void testShadowCastingIsCarried() {
        World world;

        const Entity caster = world.create();
        world.add(caster, Transform{});
        world.add(caster, makeMeshRenderer(MeshHandle{0}));

        const Entity glow = world.create();
        world.add(glow, Transform{});
        ModelRenderer glowRenderer = makeMeshRenderer(MeshHandle{1});
        glowRenderer.castsShadows = false;
        world.add(glow, glowRenderer);

        const FrameDescription frame = buildFrame(world, makeTestCamera(), 0.0f);

        assert(frame.draws.size() == 2);

        for (const DrawItem& draw : frame.draws) {
            assert(draw.castsShadows == (draw.mesh == MeshHandle{0}));
        }
    }
}

void testFrameDescription() {
    testShadowCastingIsCarried();
    testCameraIsCopied();
    testLightingIsCollected();
    testEachPartIsOneDraw();
    testHiddenAndUnplacedModelsAreSkipped();
    testMovingEntitiesAreInterpolated();
}
