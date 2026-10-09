#include "render/gpu/shadow_map.h"
#include "render/gpu/soft_shadow.h"
#include "scene/camera.h"
#include "scene/lighting.h"

#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(float actual, float expected, float tolerance = 0.001f) {
        return std::abs(actual - expected) < tolerance;
    }

    // The shadow map stores depth squashed into 0..1 (finely spaced near the light, coarsely far away). Turned back into
    // distance along the view, a point 3 units in front of the light reads 3 again.
    void testDepthTurnsBackIntoDistance() {
        const float range = 20.0f;
        const Mat4 face = pointShadowFaceMatrix(Vec3{1.0f, 2.0f, 3.0f}, 0, range, 512);

        for (float distance : {0.5f, 3.0f, 12.0f, 19.0f}) {
            const Vec4 clip = face * Vec4{1.0f + distance, 2.0f, 3.0f, 1.0f};
            const float depth = clip.z / clip.w;
            assert(nearlyEqual(linearShadowDepth(depth, shadowNearPlane, range), distance, 0.01f));
        }

        assert(nearlyEqual(linearShadowDepth(0.0f, 0.1f, 20.0f), 0.1f));
        assert(nearlyEqual(linearShadowDepth(1.0f, 0.1f, 20.0f), 20.0f, 0.01f));
    }

    /*
    * The penumbra is similar triangles, as in a solar eclipse: rays from the two edges of the glowing source cross at the
    * blocker's edge and spread again behind it, so its width at the receiver is emitter size * (receiver - blocker) / blocker.
    * Measured in texels of the shadow map at the receiver's distance.
    */
    void testPenumbraFollowsTheGeometry() {
        // An emitter 0.1 in radius, a blocker halfway: 0.1 * (4 - 2) / 2 = 0.1 units, where a texel covers 4 * 0.01 = 0.04.
        assert(nearlyEqual(penumbraTexels(0.1f, 4.0f, 2.0f, 0.01f), 2.5f));

        // A blocker nearer the light throws a softer shadow; one touching the receiver a sharp one.
        assert(penumbraTexels(0.1f, 4.0f, 1.0f, 0.01f) > penumbraTexels(0.1f, 4.0f, 3.0f, 0.01f));
        assert(nearlyEqual(penumbraTexels(0.1f, 4.0f, 4.0f, 0.01f), minShadowFilterTexels));

        // A point source still gets the old soft edge of about one texel, and huge ones are capped.
        assert(nearlyEqual(penumbraTexels(0.0f, 4.0f, 1.0f, 0.01f), minShadowFilterTexels));
        assert(nearlyEqual(penumbraTexels(5.0f, 4.0f, 0.5f, 0.01f), maxShadowFilterTexels));
    }

    // Blockers can hide part of the emitter from anywhere in a cone around the point, widest near the light; the search
    // covers it, within the cap.
    void testBlockerSearchCoversTheCone() {
        assert(nearlyEqual(blockerSearchTexels(0.0f, 4.0f, 0.1f, 0.01f), minShadowFilterTexels));
        assert(nearlyEqual(blockerSearchTexels(1.0f, 4.0f, 0.1f, 0.01f), maxBlockerSearchTexels));

        // A tiny emitter: 0.0001 * (4 - 0.1) / (0.1 * 4) / 0.01 = 0.0975 texels, so the minimum.
        assert(nearlyEqual(blockerSearchTexels(0.0001f, 4.0f, 0.1f, 0.01f), minShadowFilterTexels));
        const float medium = blockerSearchTexels(0.0004f, 4.0f, 0.1f, 0.001f);
        assert(medium > minShadowFilterTexels && medium < maxBlockerSearchTexels);
        assert(nearlyEqual(medium, 0.0004f * 3.9f / 0.4f / 0.001f));
    }

    // Lights have a real glowing size for their shadows, separate from the falloff's sourceRadius.
    void testEmittersAreSmallByDefault() {
        assert(nearlyEqual(PointLight{}.emitterRadius, 0.05f));
        assert(nearlyEqual(SpotLight{}.emitterRadius, 0.025f));
    }

    // The shader learns each perspective tile's near and far planes and how much world one texel covers per unit of
    // distance, and each shadowed light's emitter size. Directional boxes keep plain filtering (all zeros).
    void testTilesCarryWhatSoftShadowsNeed() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{});

        PlacedPointLight point{Vec3{0.0f, 1.0f, 0.0f}, PointLight{}};
        point.light.range = 5.0f;
        point.light.emitterRadius = 0.2f;
        lighting.pointLights.push_back(point);

        PlacedSpotLight spot{Vec3{0.0f, 3.0f, 0.0f}, SpotLight{}};
        spot.light.emitterRadius = 0.03f;
        lighting.spotLights.push_back(spot);

        const Camera camera{Vec3{0.0f, 40.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, 1.5f, 1.5f, 0.1f, 200.0f};
        const ShadowPlan plan = planShadows(lighting, camera);

        const ShadowTileData& box = plan.tileData[static_cast<std::size_t>(plan.uniforms.directionalTiles[0])];
        assert(box.lens[0] == 0.0f && box.lens[1] == 0.0f && box.lens[2] == 0.0f);

        const std::size_t firstFace = static_cast<std::size_t>(plan.uniforms.pointTiles[0][0]);
        const ShadowTileData& face = plan.tileData[firstFace];
        const float size = static_cast<float>(plan.tiles[firstFace].size);
        assert(nearlyEqual(face.lens[0], shadowNearPlane));
        assert(nearlyEqual(face.lens[1], 5.0f));

        // A face is a little wider than 90 degrees (tan 1 plus the edge margin), split into size texels.
        assert(face.lens[2] > 2.0f / size && face.lens[2] < 2.1f / size);

        const ShadowTileData& beam = plan.tileData[static_cast<std::size_t>(plan.uniforms.spotTiles[0][0])];
        assert(nearlyEqual(beam.lens[1], spot.light.range));

        assert(nearlyEqual(plan.uniforms.pointEmitterRadii[0][0], 0.2f));
        assert(nearlyEqual(plan.uniforms.spotEmitterRadii[0][0], 0.03f));
    }
}

void testSoftShadow() {
    testDepthTurnsBackIntoDistance();
    testPenumbraFollowsTheGeometry();
    testBlockerSearchCoversTheCone();
    testEmittersAreSmallByDefault();
    testTilesCarryWhatSoftShadowsNeed();
}
