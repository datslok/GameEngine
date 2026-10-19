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

    // The shadow map stores depth squashed into 0..1 (1 at the light's near plane, 0 at its reach). Turned back into
    // distance along the view, a point 3 units in front of the light reads 3 again.
    void testDepthTurnsBackIntoDistance() {
        const float range = 20.0f;
        const Mat4 face = pointShadowFaceMatrix(Vec3{1.0f, 2.0f, 3.0f}, 0, range, 512);

        for (float distance : {0.5f, 3.0f, 12.0f, 19.0f}) {
            const Vec4 clip = face * Vec4{1.0f + distance, 2.0f, 3.0f, 1.0f};
            const float depth = clip.z / clip.w;
            assert(nearlyEqual(linearShadowDepth(depth, shadowNearPlane, range), distance, 0.01f));
        }

        assert(nearlyEqual(linearShadowDepth(1.0f, 0.1f, 20.0f), 0.1f));
        assert(nearlyEqual(linearShadowDepth(0.0f, 0.1f, 20.0f), 20.0f, 0.01f));
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

    // A box's depth is a straight ramp from 1 at its near side (towards the light) to 0 at the far side.
    void testBoxDepthTurnsBackIntoDistance() {
        assert(nearlyEqual(boxShadowDistance(1.0f, 40.0f), 0.0f));
        assert(nearlyEqual(boxShadowDistance(0.25f, 40.0f), 30.0f));
        assert(nearlyEqual(boxShadowDistance(0.0f, 40.0f), 40.0f));
    }

    /*
    * The sun's rays are parallel apart from its own size: from a point, its disc spans an angle (its angular radius
    * either side of its centre), so behind a blocker the penumbra widens by tan(angular radius) per metre between them.
    * Measured in world units, then turned into texels of whichever cascade is shadowing the point: a coarser cascade sees
    * the same width as fewer texels, so a shadow keeps its softness across a cascade boundary.
    */
    void testSunPenumbraFollowsItsAngularSize() {
        // 4 m behind the blocker with tan = 0.01: 0.04 m, where a texel covers 0.01 m.
        assert(nearlyEqual(sunPenumbraTexels(0.01f, 10.0f, 6.0f, 0.01f), 4.0f));
        assert(nearlyEqual(sunPenumbraTexels(0.01f, 10.0f, 6.0f, 0.02f), 2.0f));

        // A blocker touching the receiver throws a sharp shadow; far behind it the width is capped.
        assert(nearlyEqual(sunPenumbraTexels(0.01f, 10.0f, 10.0f, 0.01f), minShadowFilterTexels));
        assert(nearlyEqual(sunPenumbraTexels(0.01f, 100.0f, 1.0f, 0.01f), maxShadowFilterTexels));
    }

    // The widest penumbra a blocker could throw comes from one at the box's near side, a receiver's whole distance away.
    void testSunBlockerSearchReachesTheBoxsNearSide() {
        assert(nearlyEqual(sunBlockerSearchTexels(0.01f, 5.0f, 0.01f), 5.0f));
        assert(nearlyEqual(sunBlockerSearchTexels(0.01f, 500.0f, 0.01f), maxBlockerSearchTexels));
        assert(nearlyEqual(sunBlockerSearchTexels(0.0f, 5.0f, 0.01f), minShadowFilterTexels));
    }

    // A directional light is the sun by default: its disc is about half a degree across (0.267 degrees in radius).
    void testDirectionalLightsAreTheSunsSize() {
        assert(nearlyEqual(DirectionalLight{}.angularRadius, 0.00465f, 0.00001f));
    }

    // Lights have a real glowing size for their shadows, separate from the falloff's sourceRadius.
    void testEmittersAreSmallByDefault() {
        assert(nearlyEqual(PointLight{}.emitterRadius, 0.05f));
        assert(nearlyEqual(SpotLight{}.emitterRadius, 0.025f));
    }

    // The shader learns each perspective tile's near and far planes and how much world one texel covers per unit of
    // distance, and each shadowed light's emitter size. A sun's cascade boxes have no near plane (lens x 0, which tells
    // them apart), their depth, one texel's fixed world size and their radius, and the sun's size as tan of its angle.
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

        const ShadowTileData& box = plan.tileData[static_cast<std::size_t>(plan.directional[0].firstTile)];
        assert(box.lens[0] == 0.0f && box.lens[1] > 0.0f && box.lens[2] > 0.0f && box.lens[3] > 0.0f);
        assert(nearlyEqual(box.lens[2], 2.0f * box.lens[3] / static_cast<float>(plan.tiles[static_cast<std::size_t>(plan.directional[0].firstTile)].size), 0.00001f));
        assert(nearlyEqual(plan.directional[0].emitterRadius, std::tan(DirectionalLight{}.angularRadius), 0.000001f));

        const std::size_t firstFace = static_cast<std::size_t>(plan.points[0].firstTile);
        const ShadowTileData& face = plan.tileData[firstFace];
        const float size = static_cast<float>(plan.tiles[firstFace].size);
        assert(nearlyEqual(face.lens[0], shadowNearPlane));
        assert(nearlyEqual(face.lens[1], 5.0f));

        // A face is a little wider than 90 degrees (tan 1 plus the edge margin), split into size texels.
        assert(face.lens[2] > 2.0f / size && face.lens[2] < 2.1f / size);

        const ShadowTileData& beam = plan.tileData[static_cast<std::size_t>(plan.spots[0].firstTile)];
        assert(nearlyEqual(beam.lens[1], spot.light.range));

        assert(nearlyEqual(plan.points[0].emitterRadius, 0.2f));
        assert(nearlyEqual(plan.spots[0].emitterRadius, 0.03f));
    }
}

void testSoftShadow() {
    testDepthTurnsBackIntoDistance();
    testPenumbraFollowsTheGeometry();
    testBlockerSearchCoversTheCone();
    testBoxDepthTurnsBackIntoDistance();
    testSunPenumbraFollowsItsAngularSize();
    testSunBlockerSearchReachesTheBoxsNearSide();
    testDirectionalLightsAreTheSunsSize();
    testEmittersAreSmallByDefault();
    testTilesCarryWhatSoftShadowsNeed();
}
