#include "math/mat4.h"
#include "render/gpu/camera_relative.h"
#include "render/gpu/taa.h"
#include "scene/camera.h"

#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {
    Camera makeCamera(const Vec3& position, const Vec3& target) {
        return Camera{position, target, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 16.0f / 9.0f, 0.1f, 500.0f};
    }

    // A point measured from origin through a view-projection, to its pixel and depth.
    Vec3 pixelAndDepth(const Mat4& viewProjection, const Vec3& point, const Vec3& origin, Vec2 size) {
        const Vec3 relative = point - origin;
        const Vec4 clip = viewProjection * Vec4{relative.x, relative.y, relative.z, 1.0f};
        const Vec2 pixel = ndcToPixel(Vec2{clip.x / clip.w, clip.y / clip.w}, size);
        return Vec3{pixel.x, pixel.y, clip.z / clip.w};
    }

    // The radical inverse: the index's digits in the base, mirrored behind the point.
    void testHalton() {
        assert(halton(1, 2) == 0.5f && halton(2, 2) == 0.25f && halton(3, 2) == 0.75f && halton(4, 2) == 0.125f);
        assert(near(halton(1, 3), 1.0f / 3.0f, 1e-6f) && near(halton(2, 3), 2.0f / 3.0f, 1e-6f) && near(halton(3, 3), 1.0f / 9.0f, 1e-6f) && near(halton(4, 3), 4.0f / 9.0f, 1e-6f));
        assert(halton(0, 2) == 0.0f);
    }

    /*
    * The jitter: inside the pixel (within half a pixel of its centre), every frame of a cycle at a different place, the
    * cycle repeating, and spread evenly enough that it averages close to the centre.
    */
    void testJitter() {
        Vec2 sum;

        for (std::uint64_t frame = 0; frame < taaJitterFrames; ++frame) {
            const Vec2 offset = taaJitter(frame);
            assert(std::abs(offset.x) < 0.5f && std::abs(offset.y) < 0.5f);
            sum = sum + offset;

            for (std::uint64_t other = 0; other < frame; ++other) {
                const Vec2 earlier = taaJitter(other);
                assert(earlier.x != offset.x || earlier.y != offset.y);
            }
        }

        assert(taaJitter(taaJitterFrames).x == taaJitter(0).x && taaJitter(taaJitterFrames + 3).y == taaJitter(3).y);
        assert(std::abs(sum.x / taaJitterFrames) < 0.05f && std::abs(sum.y / taaJitterFrames) < 0.05f);

        // A cycle of another length: the same Halton points, repeating after it, never the corner (index 0).
        for (std::uint64_t frame = 0; frame < 40; ++frame) {
            const Vec2 longer = taaJitter(frame, 32);
            assert(longer.x == taaJitter(frame % 32, 32).x && longer.y == taaJitter(frame + 32, 32).y);
            assert(longer.x != -0.5f || longer.y != -0.5f);
        }

        assert(taaJitter(5, taaJitterFrames).x == taaJitter(5).x && taaJitter(20, 32).x != taaJitter(20).x);
    }

    // Pixels count from the top-left corner, as the flipped viewport draws: NDC (-1, 1) is that corner, y down the screen.
    void testPixelsAndNdc() {
        const Vec2 size{800.0f, 600.0f};
        const Vec2 corner = ndcToPixel(Vec2{-1.0f, 1.0f}, size);
        assert(corner.x == 0.0f && corner.y == 0.0f);
        const Vec2 far = ndcToPixel(Vec2{1.0f, -1.0f}, size);
        assert(far.x == 800.0f && far.y == 600.0f);
        const Vec2 back = pixelToNdc(ndcToPixel(Vec2{0.3f, -0.7f}, size), size);
        assert(near(back.x, 0.3f, 1e-6f) && near(back.y, -0.7f, 1e-6f));
    }

    // Jittering moves every point on screen by the same sub-pixel offset, near or far, and leaves its depth alone.
    void testJitteredProjection() {
        const Vec2 size{800.0f, 600.0f};
        const Camera camera = makeCamera(Vec3{0.0f, 2.0f, 5.0f}, Vec3{0.0f, 0.0f, 0.0f});
        const Mat4 plain = cameraRelativeViewProjection(camera);
        const Mat4 jittered = jitteredClip(plain, Vec2{0.25f, -0.5f}, size.x, size.y);

        for (const Vec3& point : {Vec3{0.0f, 0.0f, 0.0f}, Vec3{3.0f, 1.0f, -40.0f}, Vec3{-1.0f, 2.5f, 4.0f}}) {
            const Vec3 before = pixelAndDepth(plain, point, camera.getPosition(), size);
            const Vec3 after = pixelAndDepth(jittered, point, camera.getPosition(), size);
            assert(near(after.x - before.x, 0.25f, 1e-3f) && near(after.y - before.y, -0.5f, 1e-3f));
            assert(near(after.z, before.z, 1e-7f));
        }
    }

    /*
    * Reprojection: a still camera maps every pixel to itself exactly; a camera that moved and turned maps the pixel a
    * point covers now to the pixel it covered last frame, the sky included (depth 0, the far plane); a point that was
    * behind last frame's camera has no pixel there.
    */
    void testReprojection() {
        const Vec2 size{1280.0f, 720.0f};
        const Camera still = makeCamera(Vec3{2.0f, 1.7f, 9.0f}, Vec3{0.0f, 1.0f, 0.0f});
        const Mat4 stillMatrix = cameraRelativeViewProjection(still);
        const Mat4 unmoved = taaReprojection(stillMatrix, previousRelativeViewProjection(stillMatrix, still.getPosition(), still.getPosition()));

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                assert(unmoved.values[row][column] == (row == column ? 1.0f : 0.0f));
            }
        }

        const Camera previous = makeCamera(Vec3{1000.0f, 1.7f, 9.0f}, Vec3{1000.0f, 1.0f, 0.0f});
        const Camera current = makeCamera(Vec3{1000.3f, 1.75f, 8.6f}, Vec3{999.6f, 1.0f, 0.0f});
        const Mat4 previousMatrix = cameraRelativeViewProjection(previous);
        const Mat4 currentMatrix = cameraRelativeViewProjection(current);
        const Mat4 reprojection = taaReprojection(currentMatrix, previousRelativeViewProjection(previousMatrix, previous.getPosition(), current.getPosition()));

        for (const Vec3& point : {Vec3{1000.0f, 1.0f, 0.0f}, Vec3{1003.0f, 0.0f, -20.0f}, Vec3{998.5f, 2.0f, 4.0f}}) {
            const Vec3 now = pixelAndDepth(currentMatrix, point, current.getPosition(), size);
            const Vec3 before = pixelAndDepth(previousMatrix, point, previous.getPosition(), size);
            const Reprojected found = reprojectPixel(reprojection, Vec2{now.x, now.y}, now.z, size);
            assert(found.valid && near(found.pixel.x, before.x, 0.01f) && near(found.pixel.y, before.y, 0.01f));
        }

        // The far plane straight ahead: only the turn moves it.
        const Vec3 skyNow = pixelAndDepth(currentMatrix, current.getPosition() + Vec3{-0.4f, -0.7f, -8.6f}.normalized() * 499.0f, current.getPosition(), size);
        assert(reprojectPixel(reprojection, Vec2{skyNow.x, skyNow.y}, skyNow.z, size).valid);

        // Behind last frame's camera: a point it could not see.
        const Camera turned = makeCamera(Vec3{0.0f, 1.7f, 0.0f}, Vec3{0.0f, 1.7f, 10.0f});
        const Camera ahead = makeCamera(Vec3{0.0f, 1.7f, 0.0f}, Vec3{0.0f, 1.7f, -10.0f});
        const Mat4 backwards = taaReprojection(cameraRelativeViewProjection(ahead),
                                               previousRelativeViewProjection(cameraRelativeViewProjection(turned), turned.getPosition(), ahead.getPosition()));
        const Vec3 seen = pixelAndDepth(cameraRelativeViewProjection(ahead), Vec3{0.0f, 1.7f, -5.0f}, ahead.getPosition(), size);
        assert(!reprojectPixel(backwards, Vec2{seen.x, seen.y}, seen.z, size).valid);

        // The camera's motion at a pixel, as the velocity image holds it (this frame's NDC minus last frame's): none
        // for a still camera, the point's own step for a moved one, none for a point last frame's camera did not see.
        const Vec2 stillMotion = cameraMotion(unmoved, Vec2{100.0f, 200.0f}, 0.3f, size);
        assert(stillMotion.x == 0.0f && stillMotion.y == 0.0f);

        for (const Vec3& point : {Vec3{1000.0f, 1.0f, 0.0f}, Vec3{1003.0f, 0.0f, -20.0f}}) {
            const Vec3 now = pixelAndDepth(currentMatrix, point, current.getPosition(), size);
            const Vec3 before = pixelAndDepth(previousMatrix, point, previous.getPosition(), size);
            const Vec2 motion = cameraMotion(reprojection, Vec2{now.x, now.y}, now.z, size);
            const Vec2 expected = pixelToNdc(Vec2{now.x, now.y}, size) - pixelToNdc(Vec2{before.x, before.y}, size);
            assert(near(motion.x, expected.x, 1e-4f) && near(motion.y, expected.y, 1e-4f));
        }

        const Vec2 unseen = cameraMotion(backwards, Vec2{seen.x, seen.y}, seen.z, size);
        assert(unseen.x == 0.0f && unseen.y == 0.0f);
    }

    // An object's motion (this frame's NDC minus last frame's) turned into the pixel it covered last frame.
    void testMotionToPixels() {
        const Vec2 size{800.0f, 600.0f};
        const Vec2 previous = previousPixelFromMotion(Vec2{400.0f, 300.0f}, Vec2{0.1f, 0.2f}, size);
        assert(near(previous.x, 360.0f, 1e-4f) && near(previous.y, 360.0f, 1e-4f));

        // From the two clip positions: the difference after the division by w; none for a point behind either camera.
        const Vec2 motion = objectMotion(Vec4{1.0f, 0.5f, 0.2f, 2.0f}, Vec4{0.2f, 0.5f, 0.1f, 1.0f});
        assert(near(motion.x, 0.3f, 1e-6f) && near(motion.y, -0.25f, 1e-6f));
        assert(objectMotion(Vec4{1.0f, 0.5f, 0.2f, 2.0f}, Vec4{0.2f, 0.5f, 0.1f, -1.0f}).x > static_cast<float>(TAA_NO_MOTION_TEST));
    }

    /*
    * Upscaling: a render pixel's sample, jittered, lies on the output screen where the unjittered picture shows what it
    * saw (the jitter undone, then scaled to output pixels), checked against a jittered projection drawn at the render
    * size; and an output pixel's place on the render screen.
    */
    void testUpscaledSamples() {
        const Vec2 centre = sampleOutputPixel(Vec2{3.5f, 4.5f}, Vec2{}, Vec2{1.0f, 1.0f});
        assert(centre.x == 3.5f && centre.y == 4.5f);
        const Vec2 doubled = sampleOutputPixel(Vec2{3.5f, 4.5f}, Vec2{0.25f, -0.5f}, Vec2{2.0f, 2.0f});
        assert(near(doubled.x, 6.5f, 1e-6f) && near(doubled.y, 10.0f, 1e-6f));

        const Vec2 render{400.0f, 300.0f};
        const Vec2 output{800.0f, 600.0f};
        const Vec2 jitter{0.3f, -0.2f};
        const Camera camera = makeCamera(Vec3{0.0f, 2.0f, 5.0f}, Vec3{0.0f, 0.0f, 0.0f});
        const Mat4 plain = cameraRelativeViewProjection(camera);
        const Mat4 jittered = jitteredClip(plain, jitter, render.x, render.y);

        for (const Vec3& point : {Vec3{0.0f, 0.0f, 0.0f}, Vec3{3.0f, 1.0f, -40.0f}, Vec3{-1.0f, 2.5f, 4.0f}}) {
            const Vec3 drawn = pixelAndDepth(jittered, point, camera.getPosition(), render);
            const Vec3 shown = pixelAndDepth(plain, point, camera.getPosition(), output);
            const Vec2 found = sampleOutputPixel(Vec2{drawn.x, drawn.y}, jitter, Vec2{output.x / render.x, output.y / render.y});
            assert(near(found.x, shown.x, 2e-3f) && near(found.y, shown.y, 2e-3f));
        }

        const Vec2 onRender = renderPixelOf(Vec2{6.5f, 10.0f}, Vec2{0.5f, 0.5f});
        assert(onRender.x == 3.25f && onRender.y == 5.0f);
    }

    /*
    * The reconstruction's weight for a sample at a squared distance (in output pixels) from the pixel: 1 dead on, falling
    * with distance (Blackman-Harris's Gaussian fit), never quite zero; and the new frame's share of the blend, the
    * setting's for a sample dead on, less the further the nearest sample landed.
    */
    void testSampleWeights() {
        assert(taaSampleWeight(0.0f) == 1.0f);
        assert(near(taaSampleWeight(1.0f), std::exp(-static_cast<float>(TAA_SAMPLE_FALLOFF)), 1e-6f));
        assert(taaSampleWeight(0.25f) > taaSampleWeight(0.5f) && taaSampleWeight(0.5f) > taaSampleWeight(2.0f) && taaSampleWeight(9.0f) > 0.0f);

        assert(taaCurrentShare(0.1f, 1.0f) == 0.1f);
        assert(near(taaCurrentShare(0.1f, taaSampleWeight(0.5f)), 0.1f * taaSampleWeight(0.5f), 1e-7f));
        assert(taaCurrentShare(0.1f, taaSampleWeight(2.0f)) < taaCurrentShare(0.1f, taaSampleWeight(0.5f)));
        assert(taaCurrentShare(1.0f, 1.0f) == 1.0f);
    }

    // YCoCg: luminance and two colour differences. A grey has none; the conversion goes back exactly.
    void testYCoCg() {
        const Vec3 grey = toYCoCg(Vec3{0.5f, 0.5f, 0.5f});
        assert(near(grey.x, 0.5f, 1e-7f) && grey.y == 0.0f && grey.z == 0.0f);

        const Vec3 colour{0.9f, 0.2f, 0.05f};
        const Vec3 back = fromYCoCg(toYCoCg(colour));
        assert(near(back.x, colour.x, 1e-6f) && near(back.y, colour.y, 1e-6f) && near(back.z, colour.z, 1e-6f));
    }

    // The clip: a history inside the box is kept as it is; one outside moves along the line to the centre onto the box.
    void testClipToBox() {
        const Vec3 centre{1.0f, 0.0f, 0.0f};
        const Vec3 extent{0.5f, 0.1f, 0.1f};
        const Vec3 inside{1.2f, 0.05f, -0.02f};
        const Vec3 kept = clipToBox(inside, centre, extent);
        assert(kept.x == inside.x && kept.y == inside.y && kept.z == inside.z);

        const Vec3 clipped = clipToBox(Vec3{1.0f, 0.4f, 0.0f}, centre, extent);
        assert(near(clipped.x, 1.0f, 1e-6f) && near(clipped.y, 0.1f, 1e-6f) && near(clipped.z, 0.0f, 1e-6f));

        const Vec3 corner = clipToBox(Vec3{3.0f, 0.2f, 0.0f}, centre, extent);
        assert(near(corner.x, 1.5f, 1e-6f) && near(corner.y, 0.05f, 1e-6f));

        // A flat box (every neighbour the same colour) collapses the history onto it.
        const Vec3 flat = clipToBox(Vec3{0.7f, 0.3f, 0.1f}, centre, Vec3{});
        assert(near(flat.x, 1.0f, 1e-6f) && near(flat.y, 0.0f, 1e-6f) && near(flat.z, 0.0f, 1e-6f));
    }

    /*
    * The blend: all current at weight 1, all history at 0, the same colour unchanged; and a lone very bright current
    * sample weighs far less than its share would in a plain average, so one sparkle does not flash.
    */
    void testBlend() {
        const Vec3 current{0.8f, 0.4f, 0.2f};
        const Vec3 history{0.1f, 0.3f, 0.5f};
        const Vec3 allCurrent = taaBlend(current, history, 1.0f);
        assert(near(allCurrent.x, current.x, 1e-6f) && near(allCurrent.z, current.z, 1e-6f));
        const Vec3 allHistory = taaBlend(current, history, 0.0f);
        assert(near(allHistory.x, history.x, 1e-6f) && near(allHistory.z, history.z, 1e-6f));
        const Vec3 same = taaBlend(current, current, 0.1f);
        assert(near(same.y, current.y, 1e-6f));

        const Vec3 sparkle = taaBlend(Vec3{1000.0f, 1000.0f, 1000.0f}, Vec3{0.1f, 0.1f, 0.1f}, 0.1f);
        assert(sparkle.x < 2.0f && sparkle.x > 0.1f);
    }

    // Catmull-Rom's weights: they sum to 1, pass through the samples (t 0 and 1) and are symmetric about the middle.
    void testCatmullRom() {
        for (float t : {0.0f, 0.2f, 0.5f, 0.75f, 1.0f}) {
            const Vec4 weights = catmullRomWeights(t);
            assert(near(weights.x + weights.y + weights.z + weights.w, 1.0f, 1e-6f));
        }

        const Vec4 start = catmullRomWeights(0.0f);
        assert(start.x == 0.0f && start.y == 1.0f && start.z == 0.0f && start.w == 0.0f);
        const Vec4 end = catmullRomWeights(1.0f);
        assert(near(end.z, 1.0f, 1e-6f) && near(end.x, 0.0f, 1e-6f) && near(end.y, 0.0f, 1e-6f) && near(end.w, 0.0f, 1e-6f));
        const Vec4 middle = catmullRomWeights(0.5f);
        assert(near(middle.x, middle.w, 1e-6f) && near(middle.y, middle.z, 1e-6f) && middle.x < 0.0f);
    }

    // The settings: on by default, each default within its limits; weights outside 0..1 or a negative clip refused.
    void testSettings() {
        const TaaSettings defaults;
        assert(defaults.enabled && defaults.jitter && !defaults.showMotion);
        assert(defaults.currentWeight > 0.0f && defaults.currentWeight < 1.0f && defaults.clipSigma > 0.0f);
        validateTaa(defaults);

        const auto refused = [](TaaSettings settings) {
            try {
                validateTaa(settings);
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };

        TaaSettings bad;
        bad.currentWeight = 0.0f;
        assert(refused(bad));
        bad.currentWeight = 1.5f;
        assert(refused(bad));
        bad = TaaSettings{};
        bad.clipSigma = -1.0f;
        assert(refused(bad));
    }

    /*
    * What the passes are told: the reprojection, the output screen, the render screen and this frame's jitter, the
    * settings, whether the history may be used.
    */
    void testPacking() {
        TaaSettings settings;
        settings.showMotion = true;
        const Mat4 reprojection = Mat4::translation(1.0f, 2.0f, 3.0f);
        const GpuTaaPass pass = packTaaPass(reprojection, 640, 360, RenderSize{320, 180}, Vec2{0.25f, -0.125f}, settings, true);
        assert(pass.reprojection[12] == 1.0f && pass.reprojection[13] == 2.0f && pass.reprojection[0] == 1.0f); // column-major
        assert(pass.size[0] == 640.0f && pass.size[1] == 360.0f && near(pass.size[2], 1.0f / 640.0f, 1e-9f));
        assert(pass.render[0] == 320.0f && pass.render[1] == 180.0f && pass.render[2] == 0.25f && pass.render[3] == -0.125f);
        assert(pass.options[0] == settings.currentWeight && pass.options[1] == settings.clipSigma);
        assert(pass.options[2] == 1.0f && pass.options[3] == 1.0f);
        assert(packTaaPass(reprojection, 640, 360, RenderSize{640, 360}, Vec2{}, TaaSettings{}, false).options[2] == 0.0f);

        const GpuMotionView view = packMotionView(Mat4::translation(4.0f, 0.0f, 0.0f), Mat4::translation(0.0f, 5.0f, 0.0f));
        assert(view.current[12] == 4.0f && view.previous[13] == 5.0f);

        // The camera motion pass: the reprojection and the render screen it walks.
        const GpuMotionFillPass fill = packMotionFillPass(reprojection, RenderSize{320, 180});
        assert(fill.reprojection[12] == 1.0f && fill.size[0] == 320.0f && fill.size[1] == 180.0f);
    }
}

void testTaa() {
    testHalton();
    testJitter();
    testPixelsAndNdc();
    testJitteredProjection();
    testReprojection();
    testMotionToPixels();
    testUpscaledSamples();
    testSampleWeights();
    testYCoCg();
    testClipToBox();
    testBlend();
    testCatmullRom();
    testSettings();
    testPacking();
}
