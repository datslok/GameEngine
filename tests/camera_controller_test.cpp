#include "gameplay/camera/first_person_camera_controller.h"
#include "gameplay/camera/free_fly_camera_controller.h"
#include "gameplay/camera/look_controls.h"
#include "gameplay/camera/moba_camera_controller.h"
#include "scene/camera.h"

#include <cassert>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace {
    constexpr float pi = std::numbers::pi_v<float>;

    bool nearlyEqual(float a, float b) {
        return std::abs(a - b) < 0.0001f;
    }

    bool nearlyEqual(const Vec3& a, const Vec3& b) {
        return nearlyEqual(a.x, b.x) && nearlyEqual(a.y, b.y) && nearlyEqual(a.z, b.z);
    }

    Camera makeCamera(const Vec3& position, const Vec3& target) {
        return Camera{position, target, Vec3{0.0f, 1.0f, 0.0f}, pi / 2.0f, 16.0f / 9.0f, 0.1f, 100.0f};
    }

    // A focused window with the mouse captured for mouse look.
    Input capturedInput() {
        Input input;
        input.beginFrame();
        input.setWindowState(true, true, false);
        return input;
    }
}

void testCameraControllers() {
    // View angles are spherical coordinates: yaw 0 looks down -Z, positive yaw turns right, positive pitch looks up.
    {
        assert(nearlyEqual(directionFromAngles(ViewAngles{0.0f, 0.0f}), Vec3{0.0f, 0.0f, -1.0f}));
        assert(nearlyEqual(directionFromAngles(ViewAngles{pi / 2.0f, 0.0f}), Vec3{1.0f, 0.0f, 0.0f}));
        assert(nearlyEqual(directionFromAngles(ViewAngles{0.0f, pi / 2.0f}), Vec3{0.0f, 1.0f, 0.0f}));

        // Converting there and back gives the same angles, in every quadrant.
        for (const ViewAngles original : {ViewAngles{0.3f, 0.2f}, ViewAngles{2.5f, -0.7f}, ViewAngles{-2.0f, 1.2f}}) {
            const ViewAngles roundTrip = anglesFromDirection(directionFromAngles(original));
            assert(nearlyEqual(roundTrip.yaw, original.yaw));
            assert(nearlyEqual(roundTrip.pitch, original.pitch));
        }
    }

    // Mouse look: right turns right, up looks up, and the pitch stops short of vertical.
    {
        ViewAngles angles;
        applyMouseLook(angles, Vec2{100.0f, 0.0f}, 0.01f);
        assert(nearlyEqual(angles.yaw, 1.0f));

        applyMouseLook(angles, Vec2{0.0f, -50.0f}, 0.01f);
        assert(nearlyEqual(angles.pitch, 0.5f));

        applyMouseLook(angles, Vec2{0.0f, -100000.0f}, 0.01f);
        assert(nearlyEqual(angles.pitch, maxPitchRadians));
    }

    // Free fly moves along the view direction, including up and down.
    {
        Camera camera = makeCamera(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, -1.0f});
        FreeFlyCameraController controller{2.0f, 0.001f};
        controller.takeOver(camera);
        assert(nearlyEqual(controller.getAngles().pitch, pi / 4.0f));

        Input input = capturedInput();
        input.pressKey(Key::W);
        controller.update(camera, input, 1.0f);

        const float step = 2.0f / std::sqrt(2.0f);
        assert(nearlyEqual(camera.getPosition(), Vec3{0.0f, step, -step}));

        // Space rises straight up.
        Camera level = makeCamera(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f});
        FreeFlyCameraController rising{2.0f, 0.001f};
        rising.takeOver(level);
        Input space = capturedInput();
        space.pressKey(Key::Space);
        rising.update(level, space, 0.5f);
        assert(nearlyEqual(level.getPosition(), Vec3{0.0f, 1.0f, 0.0f}));

        // Shift flies at double speed, in every direction including up.
        Input fastRise = capturedInput();
        fastRise.pressKey(Key::Space);
        fastRise.pressKey(Key::LeftShift);
        rising.update(level, fastRise, 0.5f);
        assert(nearlyEqual(level.getPosition(), Vec3{0.0f, 3.0f, 0.0f}));

        // After Escape (mouse not captured) the camera neither turns nor moves.
        Input released = capturedInput();
        released.setWindowState(true, false, false);
        released.pressKey(Key::W);
        released.addMouseMotion(500.0f, 0.0f);
        const Vec3 before = camera.getPosition();
        const Vec3 facing = camera.getForward();
        controller.update(camera, released, 1.0f);
        assert(nearlyEqual(camera.getPosition(), before));
        assert(nearlyEqual(camera.getForward(), facing));
    }

    // First person walks on the ground plane: looking up does not change height.
    {
        Camera camera = makeCamera(Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 2.0f, -1.0f});
        FirstPersonCameraController controller{3.0f, 0.001f};
        controller.takeOver(camera);

        Input input = capturedInput();
        input.pressKey(Key::W);
        controller.update(camera, input, 1.0f);
        assert(nearlyEqual(camera.getPosition(), Vec3{0.0f, 1.0f, -3.0f}));

        // Strafing right while facing -Z moves towards +X, and diagonals are not faster.
        Input diagonal = capturedInput();
        diagonal.pressKey(Key::W);
        diagonal.pressKey(Key::D);
        const Vec3 start = camera.getPosition();
        controller.update(camera, diagonal, 1.0f);
        const Vec3 moved = camera.getPosition() - start;
        assert(nearlyEqual(moved.x, 3.0f / std::sqrt(2.0f)));
        assert(nearlyEqual(moved.z, -3.0f / std::sqrt(2.0f)));

        // Holding Shift runs at double speed; either Shift key works.
        Camera runner = makeCamera(Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 1.0f, -1.0f});
        FirstPersonCameraController running{3.0f, 0.001f};
        running.takeOver(runner);
        Input run = capturedInput();
        run.pressKey(Key::W);
        run.pressKey(Key::LeftShift);
        running.update(runner, run, 1.0f);
        assert(nearlyEqual(runner.getPosition(), Vec3{0.0f, 1.0f, -6.0f}));

        Input runRight = capturedInput();
        runRight.pressKey(Key::W);
        runRight.pressKey(Key::RightShift);
        running.update(runner, runRight, 1.0f);
        assert(nearlyEqual(runner.getPosition(), Vec3{0.0f, 1.0f, -12.0f}));

        // Mouse motion turns the camera and changes the walking direction.
        Input turn = capturedInput();
        turn.addMouseMotion(pi / 2.0f / 0.001f, 0.0f);
        controller.update(camera, turn, 1.0f);
        assert(nearlyEqual(controller.getAngles().yaw, pi / 2.0f));
    }

    // The MOBA camera follows a target at its fixed offset, or pans at the window edges.
    {
        const Vec3 offset{0.0f, 12.0f, 10.0f};
        MobaCameraController controller{offset, 9.0f};
        Camera camera = makeCamera(Vec3{0.0f, 5.0f, 5.0f}, Vec3{0.0f, 0.0f, 0.0f});

        Input input;
        input.beginFrame();
        input.setWindowSize(Vec2{800.0f, 600.0f});
        input.setWindowState(true, false, true);
        input.setCursor(Vec2{400.0f, 300.0f}, true);

        const Vec3 target{2.0f, 0.0f, -6.0f};
        controller.update(camera, input, 1.0f, target);
        assert(nearlyEqual(camera.getPosition(), target + offset));
        assert(nearlyEqual(camera.getForward(), (offset * -1.0f).normalized()));

        // Cursor at the left edge: pan left without turning.
        const Vec3 facing = camera.getForward();
        input.setCursor(Vec2{0.0f, 300.0f}, true);
        controller.update(camera, input, 0.5f, std::nullopt);
        assert(nearlyEqual(camera.getPosition(), target + offset + Vec3{-4.5f, 0.0f, 0.0f}));
        assert(nearlyEqual(camera.getForward(), facing));

        // No panning when the cursor is not confined to the window.
        const Vec3 before = camera.getPosition();
        input.setWindowState(true, false, false);
        controller.update(camera, input, 1.0f, std::nullopt);
        assert(nearlyEqual(camera.getPosition(), before));
    }

    // The camera rebuilds its projection from the lens parameters, so the field of view can change.
    {
        Camera camera = makeCamera(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f});
        const float wide = camera.getProjectionMatrix().values[1][1];

        camera.setVerticalFov(pi / 4.0f);
        assert(camera.getProjectionMatrix().values[1][1] > wide);

        camera.setAspectRatio(2.0f);
        const Mat4 projection = camera.getProjectionMatrix();
        assert(nearlyEqual(projection.values[0][0], projection.values[1][1] / 2.0f));

        bool rejected = false;
        try {
            camera.setVerticalFov(0.0f);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
        assert(nearlyEqual(camera.getVerticalFov(), pi / 4.0f));
    }
}
