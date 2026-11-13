#include "render/gpu/depth_of_field.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    bool rejected(const CameraSettings& settings) {
        try {
            validateCamera(settings);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    }

    // Off by default (the user's choice), with an everyday f/2.8 lens focusing on the crosshair when on.
    void testSettings() {
        const CameraSettings defaults;
        assert(!defaults.depthOfField && defaults.fNumber == 2.8f && defaults.autofocus && !defaults.showFocus);
        assert(defaults.focusDistance == 5.0f && defaults.focusSeconds == 0.2f);
        validateCamera(defaults);

        CameraSettings wrong;
        wrong.fNumber = 0.5f;
        assert(rejected(wrong));
        wrong = CameraSettings{};
        wrong.focusDistance = 0.05f;
        assert(rejected(wrong));
        wrong = CameraSettings{};
        wrong.focusSeconds = -1.0f;
        assert(rejected(wrong));
        wrong = CameraSettings{};
        wrong.fNumber = std::nanf("");
        assert(rejected(wrong));
    }

    // A lens's field of view on a 24 mm sensor: half the sensor over the focal length is tan(fov / 2).
    void testFocalLength() {
        assert(near(lensFocalLength(std::numbers::pi_v<float> * 0.5f), 0.012f, 1e-7f));
        assert(near(lensFocalLength(70.0f * std::numbers::pi_v<float> / 180.0f), 0.012f / std::tan(35.0f * std::numbers::pi_v<float> / 180.0f), 1e-7f));
        // A narrower view is a longer lens.
        assert(lensFocalLength(0.3f) > lensFocalLength(0.6f));
    }

    /*
    * The thin lens: nothing at the focus blurs, the far field's blur grows towards a limit at infinity and the near
    * field's without bound; the shader's form in inverse distances, A f (u - w) / (1 - f u) as a radius in blur pixels,
    * is the textbook formula's.
    */
    void testCircleOfConfusion() {
        const float focal = 0.05f;
        const float fNumber = 2.0f;
        const float aperture = focal / fNumber;
        assert(circleOfConfusion(3.0f, 3.0f, focal, fNumber) == 0.0f);
        assert(circleOfConfusion(10.0f, 3.0f, focal, fNumber) > 0.0f && circleOfConfusion(1.0f, 3.0f, focal, fNumber) < 0.0f);

        const float farLimit = aperture * focal / (3.0f - focal);
        assert(near(circleOfConfusion(1e9f, 3.0f, focal, fNumber), farLimit, 1e-9f));
        assert(circleOfConfusion(100.0f, 3.0f, focal, fNumber) < farLimit);
        assert(-circleOfConfusion(0.2f, 3.0f, focal, fNumber) > 5.0f * farLimit);

        // The radius in blur pixels: half the diameter as a share of the sensor, times half the output's height.
        const float outputHeight = 1440.0f;
        const float scale = depthOfFieldLensScale(focal, fNumber, outputHeight);
        for (const float distance : {0.5f, 1.0f, 2.0f, 2.9f, 3.0f, 4.0f, 9.0f, 80.0f}) {
            const float expected = 0.5f * circleOfConfusion(distance, 3.0f, focal, fNumber) / static_cast<float>(DEPTH_OF_FIELD_SENSOR_HEIGHT) * outputHeight * 0.5f;
            const float radius = blurRadius(1.0f / distance, 1.0f / 3.0f, focal, scale);
            const float largest = static_cast<float>(DEPTH_OF_FIELD_LARGEST_RADIUS);
            assert(near(radius, std::clamp(expected, -largest, largest), 1e-3f * std::max(1.0f, std::abs(expected))));
        }

        // The sky (inverse distance 0) is the far limit.
        assert(near(blurRadius(0.0f, 1.0f / 3.0f, focal, scale), 0.5f * farLimit / static_cast<float>(DEPTH_OF_FIELD_SENSOR_HEIGHT) * 720.0f, 1e-3f));

        // A game's wide lens blurs little, as a real one does: 70 degrees at f/2.8 focused at 2 m, the background at
        // infinity a radius under 2 blur pixels (under 4 output pixels across) at 1440p.
        const float wide = lensFocalLength(70.0f * std::numbers::pi_v<float> / 180.0f);
        const float background = blurRadius(0.0f, 0.5f, wide, depthOfFieldLensScale(wide, 2.8f, outputHeight));
        assert(background > 0.5f && background < 2.0f);

        // Never focused closer than 0.1 m, nor than twice the focal length (a long lens's).
        assert(near(nearestFocusInverse(0.017f), 10.0f, 1e-5f));
        assert(near(nearestFocusInverse(1.0f), 0.5f, 1e-6f));
    }

    // The focus eases exponentially: a time constant takes 1 - 1/e of the way, two steps make one of their sum, and
    // no time constant goes straight there.
    void testEaseFocus() {
        assert(easeFocus(0.5f, 2.0f, 0.016f, 0.0f) == 2.0f);
        assert(near(easeFocus(0.0f, 1.0f, 0.2f, 0.2f), 1.0f - std::exp(-1.0f), 1e-6f));
        const float twice = easeFocus(easeFocus(0.3f, 1.5f, 0.01f, 0.2f), 1.5f, 0.01f, 0.2f);
        assert(near(twice, easeFocus(0.3f, 1.5f, 0.02f, 0.2f), 1e-6f));
        assert(easeFocus(1.0f, 1.0f, 0.5f, 0.2f) == 1.0f);
    }

    // The share of a pixel a disc covers, whichever field it is in: all of it half a pixel inside the edge, half on it.
    void testCoverage() {
        assert(blurCoverage(3.0f, 2.0f) == 1.0f && blurCoverage(3.0f, 2.5f) == 1.0f);
        assert(blurCoverage(3.0f, 4.0f) == 0.0f && blurCoverage(3.0f, 3.5f) == 0.0f);
        assert(near(blurCoverage(3.0f, 3.0f), 0.5f, 1e-6f));
        assert(near(blurCoverage(-3.0f, 3.25f), 0.25f, 1e-6f));
    }

    /*
    * Ring k: 8 k steps evenly round the middle of its annulus, (k - 1/2) / RINGS out, odd rings half a step on so the
    * rings' samples interleave; each sample stands for its share of the disc's area (ring 1's shared with the pixel's
    * own sample), so the shares add up to 1.
    */
    void testRings() {
        static_assert(DEPTH_OF_FIELD_SAMPLES == 1 + 4 * DEPTH_OF_FIELD_RINGS * (DEPTH_OF_FIELD_RINGS + 1));
        float shares = blurRingShare(0);
        for (int ring = 1; ring <= DEPTH_OF_FIELD_RINGS; ++ring) {
            Vec2 sum{};
            for (int step = 0; step < 8 * ring; ++step) {
                const Vec2 offset = blurRingOffset(ring, step);
                assert(near(offset.length(), (static_cast<float>(ring) - 0.5f) / DEPTH_OF_FIELD_RINGS, 1e-6f));
                sum = sum + offset;
                shares += blurRingShare(ring);
            }
            assert(sum.length() < 1e-5f);
        }
        assert(near(shares, 1.0f, 1e-6f));
        assert(blurRingShare(0) == blurRingShare(1));
        // The outer rings are sparser: each sample there stands for more of the disc.
        assert(blurRingShare(DEPTH_OF_FIELD_RINGS) > blurRingShare(2));
        const Vec2 first = blurRingOffset(1, 0);
        assert(near(std::atan2(first.y, first.x), std::numbers::pi_v<float> / 8.0f, 1e-6f));
        const Vec2 second = blurRingOffset(2, 0);
        assert(near(second.y, 0.0f, 1e-7f) && second.x > 0.0f);
    }

    /*
    * The layers' weights: the far layer takes no near-field sample and lets no sample reach farther than the pixel's own
    * blur (but a near-field pixel's background may come from anywhere behind); the near layer takes near-field samples
    * covering the pixel, each as its light's share landing there.
    */
    void testWeights() {
        assert(farSampleWeight(-4.0f, 5.0f, 1.0f) == 0.0f);
        assert(farSampleWeight(6.0f, 5.0f, 3.0f) == 1.0f);
        // An in-focus pixel takes nothing from the blurred background round it (no halo over a sharp edge).
        assert(farSampleWeight(8.0f, 0.1f, 3.0f) == 0.0f);
        assert(farSampleWeight(8.0f, 2.0f, 3.0f) == 0.0f && farSampleWeight(8.0f, 2.0f, 2.2f) > 0.0f);
        // A near-field pixel's background: whatever behind reaches it.
        assert(farSampleWeight(8.0f, -6.0f, 7.0f) == 1.0f);
        // The pixel itself always counts unless it is in the near field.
        assert(farSampleWeight(0.2f, 0.2f, 0.0f) > 0.5f && farSampleWeight(3.0f, 3.0f, 0.0f) == 1.0f);

        assert(nearSampleWeight(4.0f, 1.0f, 8.0f) == 0.0f);
        assert(nearSampleWeight(-0.3f, 0.0f, 8.0f) == 0.0f);
        assert(nearSampleWeight(-6.0f, 7.5f, 8.0f) == 0.0f);
        // A disc as wide as the reach lands on the pixel whole, per unit of the sample's share of the reach's area.
        assert(near(nearSampleWeight(-8.0f, 2.0f, 8.0f), 1.0f, 1e-6f));
        // A smaller disc's light is more concentrated.
        assert(near(nearSampleWeight(-4.0f, 2.0f, 8.0f), 4.0f, 1e-6f));
    }

    void testMedian() {
        assert(medianOfNine({9.0f, 1.0f, 8.0f, 2.0f, 7.0f, 3.0f, 6.0f, 4.0f, 5.0f}) == 5.0f);
        // A lone bright sample is gone.
        assert(medianOfNine({1.0f, 1.0f, 1.0f, 1.0f, 500.0f, 1.0f, 1.0f, 1.0f, 1.0f}) == 1.0f);
        assert(medianOfNine({2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f}) == 2.0f);
    }

    // The blur pixel: the four output pixels' mean light and the nearest one's radius.
    void testPrepare() {
        const DepthOfFieldTexel texel = prepareBlurTexel({Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec3{1.0f, 1.0f, 1.0f}},
                                                         {6.0f, -2.0f, 0.1f, 6.0f});
        assert(near(texel.colour.x, 0.5f, 1e-7f) && near(texel.colour.y, 0.5f, 1e-7f) && near(texel.colour.z, 0.5f, 1e-7f));
        assert(texel.radius == -2.0f);
    }

    // The sharp pixel in focus, the far layer once blurred by twice the in-focus radius, the near layer over either.
    void testComposite() {
        const Vec3 sharp{1.0f, 0.0f, 0.0f};
        const Vec3 far{0.0f, 1.0f, 0.0f};
        const Vec3 nearColour{0.0f, 0.0f, 1.0f};
        const Vec3 focused = depthOfFieldComposite(sharp, 0.3f, far, nearColour, 0.0f);
        assert(focused.x == 1.0f && focused.y == 0.0f);
        const Vec3 blurred = depthOfFieldComposite(sharp, -3.0f, far, nearColour, 0.0f);
        assert(blurred.y == 1.0f && blurred.x == 0.0f);
        const Vec3 between = depthOfFieldComposite(sharp, 0.75f, far, nearColour, 0.0f);
        assert(near(between.x, 0.5f, 1e-6f) && near(between.y, 0.5f, 1e-6f));
        const Vec3 covered = depthOfFieldComposite(sharp, 0.3f, far, nearColour, 0.25f);
        assert(near(covered.x, 0.75f, 1e-6f) && near(covered.z, 0.25f, 1e-6f));
    }

    // A grid of blur pixels for the gather: a left part and a right part split at a column.
    struct SplitGrid {
        int width = 64;
        int height = 64;
        int split = 32;
        DepthOfFieldTexel left;
        DepthOfFieldTexel right;

        DepthOfFieldTexel at(int x, int y) const {
            assert(x >= 0 && x < width && y >= 0 && y < height);
            return x < split ? left : right;
        }

        float nearReach() const {
            return std::max({0.0f, -left.radius, -right.radius});
        }

        DepthOfFieldLayers gather(int x) const {
            return depthOfFieldAt(x, height / 2, width, height, nearReach(), [this](int sx, int sy) { return at(sx, sy); });
        }
    };

    /*
    * The two ways a plain gather fails, which scatter-as-gather mends: a sharp thing in front of a blurred background
    * throws no halo over it, and a blurred thing in front spreads over the sharp background behind its edge, as much as
    * the share of its discs reaching there (a half-plane of radius 8 discs, 3.5 pixels off its edge: about 23%).
    */
    void testGather() {
        const Vec3 white{1.0f, 1.0f, 1.0f};
        const Vec3 black{0.0f, 0.0f, 0.0f};

        // All alike: the far layer is the light, no near layer.
        {
            SplitGrid grid;
            grid.left = grid.right = DepthOfFieldTexel{Vec3{0.2f, 0.4f, 0.6f}, 5.0f};
            const DepthOfFieldLayers layers = grid.gather(20);
            assert(near(layers.far.x, 0.2f, 1e-6f) && near(layers.far.z, 0.6f, 1e-6f) && layers.nearAlpha == 0.0f);
        }

        // All in focus: copied.
        {
            SplitGrid grid;
            grid.left = DepthOfFieldTexel{white, 0.2f};
            grid.right = DepthOfFieldTexel{black, -0.3f};
            const DepthOfFieldLayers layers = grid.gather(32);
            assert(layers.far.x == 0.0f && layers.nearAlpha == 0.0f);
        }

        // A sharp white thing (left) in front of a blurred black background (right): no white on the background.
        {
            SplitGrid grid;
            grid.left = DepthOfFieldTexel{white, 0.1f};
            grid.right = DepthOfFieldTexel{black, 8.0f};
            for (const int x : {32, 33, 35, 40}) {
                const DepthOfFieldLayers layers = grid.gather(x);
                assert(layers.far.x < 1e-6f && layers.nearAlpha == 0.0f);
            }
            // And the sharp thing keeps its own light.
            assert(grid.gather(31).far.x == 1.0f);
        }

        // A blurred white thing in front (left, the near field) over a sharp black background (right).
        {
            SplitGrid grid;
            grid.left = DepthOfFieldTexel{white, -8.0f};
            grid.right = DepthOfFieldTexel{black, 0.1f};
            const DepthOfFieldLayers edge = grid.gather(35);
            assert(edge.far.x == 0.0f);
            assert(edge.nearAlpha > 0.15f && edge.nearAlpha < 0.32f && near(edge.nearColour.x, 1.0f, 1e-6f));
            const Vec3 shown = depthOfFieldComposite(black, 0.1f, edge.far, edge.nearColour, edge.nearAlpha);
            assert(shown.x > 0.15f);
            // Out of the discs' reach: untouched.
            assert(grid.gather(45).nearAlpha == 0.0f);
            // Deep inside the blurred thing: wholly covered.
            assert(grid.gather(10).nearAlpha > 0.99f);
            // Spreading wider the nearer to the edge.
            assert(grid.gather(33).nearAlpha > edge.nearAlpha && edge.nearAlpha > grid.gather(38).nearAlpha);
        }
    }

    void testGridAndPacking() {
        const RenderSize grid = depthOfFieldGrid(RenderSize{1921, 1081});
        assert(grid.width == 961 && grid.height == 541);
        const RenderSize tiles = depthOfFieldTiles(RenderSize{1921, 1081});
        assert(tiles.width == 61 && tiles.height == 34);

        CameraSettings camera;
        camera.depthOfField = true;
        camera.fNumber = 4.0f;
        camera.focusDistance = 2.0f;
        camera.autofocus = false;
        camera.showFocus = true;
        const float fov = 1.0f;
        const GpuDepthOfFieldPass pass = packDepthOfFieldPass(Mat4::identity(), RenderSize{1280, 720}, RenderSize{2560, 1440}, fov, camera, 0.02f, true);
        const float focal = lensFocalLength(fov);
        assert(pass.renderSize[0] == 1280.0f && near(pass.renderSize[3], 1.0f / 720.0f, 1e-9f));
        assert(pass.outputSize[1] == 1440.0f);
        assert(pass.lens[0] == focal && near(pass.lens[1], depthOfFieldLensScale(focal, 4.0f, 1440.0f), 1e-6f));
        assert(pass.lens[2] == nearestFocusInverse(focal) && pass.lens[3] == 1.0f);
        assert(pass.focus[0] == 0.5f && pass.focus[1] == 0.0f && pass.focus[2] == 0.02f && pass.focus[3] == camera.focusSeconds);
        assert(pass.options[0] == 1.0f);
    }
}

void testDepthOfField() {
    testSettings();
    testFocalLength();
    testCircleOfConfusion();
    testEaseFocus();
    testCoverage();
    testRings();
    testWeights();
    testMedian();
    testPrepare();
    testComposite();
    testGather();
    testGridAndPacking();
}
