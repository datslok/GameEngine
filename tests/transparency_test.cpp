#include "render/gpu/transparency.h"
#include "scene/frame_description.h"

#include <cassert>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {
    bool near(float actual, float expected, float tolerance = 1e-6f) {
        return std::abs(actual - expected) <= tolerance;
    }

    bool nearVec(const Vec3& actual, const Vec3& expected, float tolerance = 1e-6f) {
        return near(actual.x, expected.x, tolerance) && near(actual.y, expected.y, tolerance) && near(actual.z, expected.z, tolerance);
    }

    const Vec3 diffuseLight{0.4f, 0.3f, 0.2f};
    const Vec3 specularLight{0.05f, 0.06f, 0.07f};
    const Vec3 glow{0.1f, 0.0f, 0.0f};
    const Vec3 tint{0.9f, 0.5f, 0.2f};
    const Vec3 specularAlbedo{0.04f, 0.04f, 0.04f};

    // Fully covering and not transmitting: an opaque surface, nothing seen through it.
    void testOpaqueSurface() {
        const TransparentLight light = transparentBlend(1.0f, 0.0f, 0.0f, diffuseLight, specularLight, glow, tint, specularAlbedo);
        assert(nearVec(light.added, diffuseLight + specularLight + glow));
        assert(nearVec(light.transmittance, Vec3{0.0f, 0.0f, 0.0f}));
    }

    // glTF's coverage: alpha of the pixel is the surface, the rest what lies behind, everything about the surface scaled.
    void testCoverage() {
        const TransparentLight light = transparentBlend(0.25f, 0.0f, 0.0f, diffuseLight, specularLight, glow, tint, specularAlbedo);
        assert(nearVec(light.added, (diffuseLight + specularLight + glow) * 0.25f));
        assert(nearVec(light.transmittance, Vec3{0.75f, 0.75f, 0.75f}));

        const TransparentLight none = transparentBlend(0.0f, 0.0f, 0.0f, diffuseLight, specularLight, glow, tint, specularAlbedo);
        assert(nearVec(none.added, Vec3{0.0f, 0.0f, 0.0f}) && nearVec(none.transmittance, Vec3{1.0f, 1.0f, 1.0f}));
    }

    /*
    * Thin glass (KHR_materials_transmission): the whole pixel is glass. Its reflection stays whole; its diffuse share
    * goes; what is behind comes through tinted by its colour, less what the surface reflected (the specular albedo).
    */
    void testTransmission() {
        const TransparentLight glass = transparentBlend(1.0f, 1.0f, 0.0f, diffuseLight, specularLight, glow, tint, specularAlbedo);
        assert(nearVec(glass.added, specularLight + glow));
        assert(nearVec(glass.transmittance, Vec3{0.9f * 0.96f, 0.5f * 0.96f, 0.2f * 0.96f}));

        const TransparentLight half = transparentBlend(1.0f, 0.5f, 0.0f, diffuseLight, specularLight, glow, tint, specularAlbedo);
        assert(nearVec(half.added, diffuseLight * 0.5f + specularLight + glow));
        assert(nearVec(half.transmittance, glass.transmittance * 0.5f));

        // Seen at a grazing angle glass reflects nearly everything, so little comes through.
        const TransparentLight grazing = transparentBlend(1.0f, 1.0f, 0.0f, diffuseLight, specularLight, glow, tint, Vec3{0.9f, 0.9f, 0.9f});
        assert(grazing.transmittance.x < 0.1f);
    }

    // Metals pass no light (glTF: transmission belongs to the non-metal part); coverage applies on top of transmission.
    void testMetalsAndCoverageWithTransmission() {
        const TransparentLight metal = transparentBlend(1.0f, 1.0f, 1.0f, diffuseLight, specularLight, glow, tint, specularAlbedo);
        assert(nearVec(metal.added, diffuseLight + specularLight + glow));
        assert(nearVec(metal.transmittance, Vec3{0.0f, 0.0f, 0.0f}));

        const TransparentLight glass = transparentBlend(1.0f, 1.0f, 0.0f, diffuseLight, specularLight, glow, tint, specularAlbedo);
        const TransparentLight faded = transparentBlend(0.5f, 1.0f, 0.0f, diffuseLight, specularLight, glow, tint, specularAlbedo);
        assert(nearVec(faded.added, glass.added * 0.5f));
        assert(nearVec(faded.transmittance, glass.transmittance * 0.5f + Vec3{0.5f, 0.5f, 0.5f}));
    }

    DrawItem drawAt(std::uint32_t mesh, const Vec3& centre, bool doubleSided = false, bool transparent = true) {
        DrawItem draw;
        draw.mesh = MeshHandle{mesh};
        draw.model = Mat4::translation(centre.x, centre.y, centre.z);
        draw.bounds = BoundingSphere{centre, 1.0f};
        draw.material.doubleSided = doubleSided;
        draw.material.alphaMode = transparent ? AlphaMode::Blend : AlphaMode::Opaque;
        return draw;
    }

    // Glass and blended surfaces are drawn after the opaque scene; transmission alone makes a material transparent too.
    void testTransparentDrawsAreSplitOff() {
        DrawItem window = drawAt(0, Vec3{0.0f, 0.0f, -5.0f}, false, false);
        window.material.transmission = 1.0f;
        const std::vector<DrawItem> draws{drawAt(0, {}, false, false), drawAt(1, {}), window, drawAt(2, {}, false, false)};

        assert(!isTransparent(draws[0].material) && isTransparent(draws[1].material) && isTransparent(draws[2].material));
        assert((opaqueDraws(draws, {3, 1, 2, 0}) == std::vector<std::size_t>{3, 0}));
        assert((transparentDraws(draws, {3, 1, 2, 0}) == std::vector<std::size_t>{1, 2}));
    }

    /*
    * Blending is done far to near, by each object's centre from the camera, one draw list entry each. A double-sided
    * object is drawn twice from its entry: its back faces first, then its front ones, so its far side shows through its
    * near side. Equal distances keep the order they were chosen in.
    */
    void testSortedFarToNear() {
        const Vec3 camera{0.0f, 1.0f, 0.0f};
        const std::vector<DrawItem> draws{
            drawAt(4, Vec3{0.0f, 1.0f, -3.0f}),
            drawAt(5, Vec3{0.0f, 1.0f, -9.0f}, true),
            drawAt(6, Vec3{3.0f, 1.0f, 0.0f}),
            drawAt(7, Vec3{0.0f, 1.0f, 3.0f})
        };

        const SortedTransparentDraws sorted = sortTransparentDraws(draws, {0, 1, 2, 3}, camera, 20);
        assert((sorted.order == std::vector<std::size_t>{1, 0, 2, 3}));
        assert(sorted.batches.size() == 5);
        assert(sorted.batches[0].mesh == MeshHandle{5} && sorted.batches[0].firstInstance == 20 && sorted.batches[0].backFaces);
        assert(sorted.batches[1].mesh == MeshHandle{5} && sorted.batches[1].firstInstance == 20 && !sorted.batches[1].backFaces);
        assert(sorted.batches[2].mesh == MeshHandle{4} && sorted.batches[2].firstInstance == 21 && !sorted.batches[2].backFaces);
        assert(sorted.batches[3].firstInstance == 22 && sorted.batches[4].firstInstance == 23);
    }

    // A draw without bounds is placed by its model matrix's translation.
    void testUnboundedDrawsSortByTheirOrigin() {
        DrawItem closer = drawAt(0, Vec3{0.0f, 0.0f, -2.0f});
        closer.bounds = BoundingSphere{};
        const std::vector<DrawItem> draws{closer, drawAt(1, Vec3{0.0f, 0.0f, -4.0f})};
        assert((sortTransparentDraws(draws, {0, 1}, Vec3{}, 0).order == std::vector<std::size_t>{1, 0}));
    }
    // The glass pass reads the reflection probe only when there is one, at the level its roughness picks.
    void testTransparentView() {
        const GpuTransparentView view = packTransparentView(true, 9);
        assert(view.probe[0] == 1.0f && view.probe[1] == 9.0f && view.probe[2] == 0.0f && view.probe[3] == 0.0f);
        assert(packTransparentView(false, 0).probe[0] == 0.0f);
    }

    /*
    * The reactive mask: the share of a pixel's light the transparent surfaces changed, from the picture before and after
    * them, in its most changed channel. Untouched pixels are 0; a pixel the glass made wholly its own reaches the limit.
    */
    void testReactiveShare() {
        const Vec3 behind{0.5f, 0.4f, 0.3f};
        assert(reactiveShare(behind, behind, 1.0f, 0.9f) == 0.0f);
        assert(near(reactiveShare(behind, Vec3{0.0f, 0.0f, 0.0f}, 1.0f, 0.9f), 0.9f));
        assert(near(reactiveShare(behind, Vec3{0.0f, 0.0f, 0.0f}, 1.0f, 1.0f), 1.0f));

        // Half the light gone, or the light doubled: half of the brighter of the two is the glass's.
        assert(near(reactiveShare(behind, behind * 0.5f, 1.0f, 1.0f), 0.5f));
        assert(near(reactiveShare(behind, behind * 2.0f, 1.0f, 1.0f), 0.5f));

        // The most changed channel counts: only red halved.
        assert(near(reactiveShare(behind, Vec3{0.25f, 0.4f, 0.3f}, 1.0f, 1.0f), 0.5f));

        // A share of the light, so the same at any brightness (the exposure is not known yet when it is measured).
        assert(near(reactiveShare(behind * 1000.0f, behind * 500.0f, 1.0f, 1.0f), 0.5f, 1e-5f));
        assert(near(reactiveShare(behind * 1e-3f, behind * 5e-4f, 1.0f, 1.0f), 0.5f, 1e-5f));

        // Black stays black: nothing to share. The scale strengthens it up to the limit; negative light counts as none.
        assert(reactiveShare(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}, 1.0f, 0.9f) == 0.0f);
        assert(near(reactiveShare(behind, behind * 0.75f, 2.0f, 1.0f), 0.5f));
        assert(near(reactiveShare(behind, behind * 0.25f, 2.0f, 0.9f), 0.9f));
        assert(reactiveShare(behind, behind * 0.25f, 0.0f, 0.9f) == 0.0f);
        assert(near(reactiveShare(Vec3{-1.0f, 0.4f, 0.3f}, Vec3{0.0f, 0.4f, 0.3f}, 1.0f, 1.0f), 0.0f));
    }

    // On by default; a negative scale or a limit outside 0..1 is refused.
    void testTransparencySettings() {
        const TransparencySettings defaults;
        assert(defaults.reactiveMask && defaults.reactiveScale == 1.0f && defaults.reactiveLimit > 0.0f && defaults.reactiveLimit <= 1.0f);
        validateTransparency(defaults);

        const auto refused = [](TransparencySettings settings) {
            try {
                validateTransparency(settings);
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };

        TransparencySettings bad;
        bad.reactiveScale = -0.5f;
        assert(refused(bad));
        bad = TransparencySettings{};
        bad.reactiveLimit = 1.5f;
        assert(refused(bad));
        bad.reactiveLimit = -0.1f;
        assert(refused(bad));
    }

    // The mask pass is told the render screen it walks and the settings.
    void testReactivePass() {
        TransparencySettings settings;
        settings.reactiveScale = 1.5f;
        settings.reactiveLimit = 0.8f;
        const GpuReactivePass pass = packReactivePass(RenderSize{320, 180}, settings);
        assert(pass.size[0] == 320.0f && pass.size[1] == 180.0f && pass.options[0] == 1.5f && pass.options[1] == 0.8f);
    }
}

void testTransparency() {
    testOpaqueSurface();
    testCoverage();
    testTransmission();
    testMetalsAndCoverageWithTransmission();
    testTransparentDrawsAreSplitOff();
    testSortedFarToNear();
    testUnboundedDrawsSortByTheirOrigin();
    testTransparentView();
    testReactiveShare();
    testTransparencySettings();
    testReactivePass();
}
