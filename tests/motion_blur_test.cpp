#include "render/gpu/motion_blur.h"
#include "render/gpu/taa.h"

#include <cassert>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    // Off by default (the user's choice), with a film camera's half-frame shutter and the camera's own motion when on.
    void testSettings() {
        const CameraSettings defaults;
        assert(!defaults.motionBlur && defaults.shutterAngle == 180.0f && defaults.cameraMotionBlur);
        validateCamera(defaults);

        CameraSettings wrong;
        wrong.shutterAngle = 0.0f;
        bool threw = false;
        try {
            validateCamera(wrong);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
        wrong.shutterAngle = 400.0f;
        threw = false;
        try {
            validateCamera(wrong);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    /*
    * Which motion a pixel blurs by: the velocity image holds a moving object's whole motion on screen, or TAA_NO_MOTION
    * where the camera's own motion stands (worked out from the depth). With the camera's blur off, only what the
    * objects add to the camera's motion counts, so the world stays sharp while turning.
    */
    void testPixelMotion() {
        const Vec2 none{static_cast<float>(TAA_NO_MOTION), static_cast<float>(TAA_NO_MOTION)};
        const Vec2 camera{0.02f, -0.01f};
        const Vec2 object{0.05f, 0.03f};
        const Vec2 still = pixelMotion(none, camera, true);
        assert(still.x == camera.x && still.y == camera.y);
        const Vec2 objectsOnly = pixelMotion(none, camera, false);
        assert(objectsOnly.x == 0.0f && objectsOnly.y == 0.0f);
        const Vec2 moving = pixelMotion(object, camera, true);
        assert(moving.x == object.x && moving.y == object.y);
        const Vec2 ownMotion = pixelMotion(object, camera, false);
        assert(near(ownMotion.x, 0.03f, 1e-7f) && near(ownMotion.y, 0.04f, 1e-7f));
    }

    /*
    * The blur's half-length in output pixels: the motion's NDC across half the screen per unit, times the shutter's
    * share of the frame, halved (the blur reaches that far either way), y flipped (NDC counts up, pixels down), and
    * never longer than a tile.
    */
    void testHalfVector() {
        const Vec2 size{1000.0f, 500.0f};
        const Vec2 half = motionBlurHalfVector(Vec2{0.1f, 0.0f}, size, 0.5f);
        assert(near(half.x, 12.5f, 1e-5f) && half.y == 0.0f);
        const Vec2 up = motionBlurHalfVector(Vec2{0.0f, 0.04f}, size, 1.0f);
        assert(near(up.y, -5.0f, 1e-5f));
        const Vec2 shorter = motionBlurHalfVector(Vec2{0.1f, 0.0f}, size, 0.25f);
        assert(near(shorter.x, half.x * 0.5f, 1e-5f));

        const Vec2 fast = motionBlurHalfVector(Vec2{1.0f, 1.0f}, size, 1.0f);
        assert(near(fast.length(), static_cast<float>(MOTION_BLUR_TILE), 1e-3f));
        assert(near(fast.x / fast.y, -2.0f, 1e-4f)); // the direction kept: 500 px across for 250 up
    }

    // The tiles keep the longest motion; the weights are McGuire et al. 2012's.
    void testWeights() {
        const Vec2 longer = largerMotion(Vec2{3.0f, 0.0f}, Vec2{0.0f, -4.0f});
        assert(longer.y == -4.0f);
        const Vec2 tie = largerMotion(Vec2{3.0f, 4.0f}, Vec2{5.0f, 0.0f});
        assert(tie.x == 3.0f);

        assert(motionBlurCone(0.0f, 8.0f) == 1.0f && near(motionBlurCone(4.0f, 8.0f), 0.5f, 1e-6f) && motionBlurCone(9.0f, 8.0f) == 0.0f);
        assert(motionBlurCylinder(5.0f, 8.0f) == 1.0f && motionBlurCylinder(9.0f, 8.0f) == 0.0f);

        assert(motionBlurNearer(2.0f, 10.0f) == 1.0f);  // well in front
        assert(motionBlurNearer(10.0f, 2.0f) == 0.0f);  // well behind
        assert(motionBlurNearer(5.0f, 5.0f) == 1.0f);   // the same surface counts as in front either way
        const float justBehind = motionBlurNearer(5.0f + 0.5f * static_cast<float>(MOTION_BLUR_DEPTH_SOFTNESS) * 5.0f, 5.0f);
        assert(near(justBehind, 0.5f, 1e-5f));

        // A still pixel in front of a moving one never takes its light; a moving one in front covers what is behind.
        assert(motionBlurSampleWeight(4.0f, 2.0f, 10.0f, static_cast<float>(MOTION_BLUR_LEAST_MOTION), 8.0f) == 0.0f);
        assert(motionBlurSampleWeight(4.0f, 10.0f, 2.0f, static_cast<float>(MOTION_BLUR_LEAST_MOTION), 8.0f) > 0.0f);
    }

    // A picture and its prepared motion and depth, a row of the same values down each column.
    struct Scene {
        int width = 128;
        int height = 8;
        std::vector<Vec3> picture;
        std::vector<MotionBlurTexel> prepared;

        Scene() : picture(static_cast<std::size_t>(width * height)), prepared(static_cast<std::size_t>(width * height)) {}

        void setColumn(int x, float light, Vec2 half, float depth) {
            for (int y = 0; y < height; ++y) {
                picture[static_cast<std::size_t>(y * width + x)] = Vec3{light, light, light};
                prepared[static_cast<std::size_t>(y * width + x)] = MotionBlurTexel{half, depth};
            }
        }

        // The tiles' largest motion and its 3 x 3 neighbours', as the passes find them.
        Vec2 neighbourMax(int x) const {
            Vec2 largest;
            const int tile = x / MOTION_BLUR_TILE;
            for (int column = std::max(0, (tile - 1) * MOTION_BLUR_TILE); column < std::min(width, (tile + 2) * MOTION_BLUR_TILE); ++column) {
                largest = largerMotion(largest, prepared[static_cast<std::size_t>(column)].half);
            }
            return largest;
        }

        float blurred(int x, float noise = 0.5f) const {
            const MotionBlurPicture pictureAt = [&](int px, int py) { return picture[static_cast<std::size_t>(py * width + px)]; };
            const MotionBlurPrepared preparedAt = [&](int px, int py) { return prepared[static_cast<std::size_t>(py * width + px)]; };
            return motionBlurAt(x, 4, width, height, neighbourMax(x), noise, pictureAt, preparedAt).x;
        }
    };

    // Nothing moving: every pixel exactly as it was.
    void testStillPictureUnchanged() {
        Scene scene;
        for (int x = 0; x < scene.width; ++x) {
            scene.setColumn(x, static_cast<float>(x % 7) * 0.1f, Vec2{}, 5.0f);
        }
        for (int x = 0; x < scene.width; ++x) {
            assert(scene.blurred(x) == static_cast<float>(x % 7) * 0.1f);
        }
    }

    // Everything sliding sideways (a turning camera): an edge becomes a smooth ramp across the blur, rising throughout,
    // past the edge's middle on the bright side only, and untouched beyond the blur's reach.
    void testSlidingEdgeRamps() {
        Scene scene;
        const float reach = 8.0f;
        for (int x = 0; x < scene.width; ++x) {
            scene.setColumn(x, x < 64 ? 0.0f : 1.0f, Vec2{reach, 0.0f}, 5.0f);
        }

        float last = -1.0f;
        for (int x = 40; x < 90; ++x) {
            const float value = scene.blurred(x);
            assert(value >= last - 1e-6f);
            last = value;
        }
        assert(scene.blurred(64 - 9) == 0.0f && scene.blurred(64 + 9) == 1.0f);
        assert(scene.blurred(63) > 0.1f && scene.blurred(63) < 0.5f);
        assert(scene.blurred(64) > 0.5f && scene.blurred(64) < 0.9f);
    }

    // A still post in front of a moving background keeps its edge: none of the background's blur crosses it.
    void testStillForegroundStaysSharp() {
        Scene scene;
        for (int x = 0; x < scene.width; ++x) {
            if (x < 64) {
                scene.setColumn(x, 1.0f, Vec2{}, 2.0f);
            } else {
                scene.setColumn(x, (x / 3) % 2 == 0 ? 0.0f : 0.5f, Vec2{8.0f, 0.0f}, 10.0f);
            }
        }
        for (int x = 40; x < 64; ++x) {
            assert(scene.blurred(x) == 1.0f);
        }
    }

    /*
    * A thing moving over a still background smears over it as far as its blur reaches and no further; inside, it keeps
    * its own light. A shorter shutter (half the reach) smears half as far.
    */
    void testMovingObjectSmears() {
        for (const float reach : {8.0f, 4.0f}) {
            Scene scene;
            for (int x = 0; x < scene.width; ++x) {
                const bool object = x >= 40 && x < 60;
                scene.setColumn(x, object ? 1.0f : 0.0f, object ? Vec2{reach, 0.0f} : Vec2{}, object ? 2.0f : 10.0f);
            }
            assert(scene.blurred(50) == 1.0f);
            assert(scene.blurred(59 + static_cast<int>(reach) + 2) == 0.0f);
            assert(scene.blurred(59 + static_cast<int>(reach) / 2) > 0.0f);
        }

        Scene shortShutter;
        for (int x = 0; x < shortShutter.width; ++x) {
            const bool object = x >= 40 && x < 60;
            shortShutter.setColumn(x, object ? 1.0f : 0.0f, object ? Vec2{4.0f, 0.0f} : Vec2{}, object ? 2.0f : 10.0f);
        }
        assert(shortShutter.blurred(66) == 0.0f);
    }

    // The sample places: symmetric about the pixel without noise, the middle one left out (the pixel itself is counted).
    void testSampleOffsets() {
        for (int index = 0; index < MOTION_BLUR_SAMPLES; ++index) {
            const float t = motionBlurOffset(index, 0.5f);
            assert(t > -1.0f && t < 1.0f);
            assert(near(t, -motionBlurOffset(MOTION_BLUR_SAMPLES - 1 - index, 0.5f), 1e-6f));
        }
        assert(near(motionBlurOffset(0, 0.0f), motionBlurOffset(0, 0.5f) - 1.0f / (MOTION_BLUR_SAMPLES + 1), 1e-6f));
    }

    // The passes' record and the tiles' grid.
    void testPacking() {
        CameraSettings camera;
        camera.shutterAngle = 90.0f;
        camera.cameraMotionBlur = false;
        const Mat4 inverseViewProjection = Mat4::translation(1.0f, 2.0f, 3.0f);
        const Mat4 reprojection = Mat4::translation(4.0f, 5.0f, 6.0f);
        const GpuMotionBlurPass pass = packMotionBlurPass(inverseViewProjection, reprojection, RenderSize{960, 540}, RenderSize{1920, 1080}, camera);
        assert(pass.inverseViewProjection[12] == 1.0f && pass.reprojection[12] == 4.0f);
        assert(pass.renderSize[0] == 960.0f && pass.renderSize[1] == 540.0f && pass.renderSize[2] == 1.0f / 960.0f);
        assert(pass.outputSize[0] == 1920.0f && pass.outputSize[1] == 1080.0f && pass.outputSize[3] == 1.0f / 1080.0f);
        assert(pass.options[0] == 0.25f && pass.options[1] == 0.0f);

        const RenderSize tiles = motionBlurTiles(RenderSize{1920, 1080});
        assert(tiles.width == 60 && tiles.height == 34);
    }
}

void testMotionBlur() {
    testSettings();
    testPixelMotion();
    testHalfVector();
    testWeights();
    testStillPictureUnchanged();
    testSlidingEdgeRamps();
    testStillForegroundStaysSharp();
    testMovingObjectSmears();
    testSampleOffsets();
    testPacking();
}
