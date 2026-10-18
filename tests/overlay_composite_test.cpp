#include "core/srgb.h"
#include "math/vec3.h"
#include "math/vec4.h"
#include "render/gpu/overlay_composite.h"

#include <cassert>
#include <cmath>

namespace {
    bool near(float a, float b, float tolerance = 1e-5f) {
        return std::fabs(a - b) <= tolerance;
    }

    // Nothing drawn (alpha 0) leaves the screen exactly as it was.
    void testEmptyOverlayLeavesTheScreen() {
        const Vec3 screen{0.3f, 2.0f, 0.01f};
        const Vec3 result = blendOverlay(screen, overlayOutput(Vec4{0.0f, 0.0f, 0.0f, 0.0f}, 1.0f));
        assert(result.x == screen.x && result.y == screen.y && result.z == screen.z);
    }

    // An opaque texel replaces the screen with its colour decoded from sRGB, so on an SDR screen (white level 1,
    // whose swapchain encodes to sRGB again) it shows the exact bytes the UI asked for.
    void testOpaqueTexelShowsItsColour() {
        const Vec4 texel{0.5f, 0.25f, 1.0f, 1.0f};
        const Vec3 result = blendOverlay(Vec3{9.0f, 9.0f, 9.0f}, overlayOutput(texel, 1.0f));

        assert(near(result.x, srgbToLinear(0.5f)));
        assert(near(result.y, srgbToLinear(0.25f)));
        assert(near(result.z, 1.0f));
        assert(near(linearToSrgb(result.x), 0.5f));
    }

    // On an HDR screen the UI's white sits at the white level, like the desktop's, not at scRGB's dim 80 nits.
    void testHdrScalesToTheWhiteLevel() {
        const Vec3 result = blendOverlay(Vec3{0.0f, 0.0f, 0.0f}, overlayOutput(Vec4{1.0f, 1.0f, 1.0f, 1.0f}, 2.5f));
        assert(near(result.x, 2.5f) && near(result.y, 2.5f) && near(result.z, 2.5f));
    }

    // The overlay is premultiplied (ImGui's blending onto a cleared image leaves colour times alpha), so a half
    // covered texel is decoded from its own colour and mixed half and half with the screen.
    void testPartialCoverageMixes() {
        const float alpha = 0.5f;
        const float colour = 0.8f;
        const Vec4 texel{colour * alpha, colour * alpha, colour * alpha, alpha};
        const Vec3 screen{0.2f, 0.2f, 0.2f};

        const Vec3 result = blendOverlay(screen, overlayOutput(texel, 1.0f));
        const float expected = srgbToLinear(colour) * alpha + screen.x * (1.0f - alpha);
        assert(near(result.x, expected));
    }
}

void testOverlayComposite() {
    testEmptyOverlayLeavesTheScreen();
    testOpaqueTexelShowsItsColour();
    testHdrScalesToTheWhiteLevel();
    testPartialCoverageMixes();
}
