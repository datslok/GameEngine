#include "core/settings_visitor.h"
#include "engine/settings_file.h"
#include "gameplay/first_person_body.h"
#include "gameplay/player_controls.h"
#include "math/transform.h"
#include "physics/character_controller.h"

#include "demo_helpers.h"
#include "test_levels.h"

#include <cassert>
#include <set>
#include <string>

namespace {
    // A setup unlike the defaults, so the tests see that the controls follow it.
    PlayerControlsSetup testSetup() {
        PlayerControlsSetup setup;
        setup.freeCameraPosition = Vec3{10.0f, 4.0f, 3.0f};
        setup.freeCameraTarget = Vec3{10.0f, 0.0f, -20.0f};
        setup.mobaLookPoint = Vec3{-5.0f, 0.0f, 7.0f};
        setup.firstPersonFeet = Vec3{1.0f, 0.0f, 2.0f};
        setup.farPlane = 900.0f;
        return setup;
    }

    // A focused window with no mouse capture, pressing one key.
    Input pressing(Key key, bool keyboardFocus = true) {
        Input input;
        input.beginFrame();
        input.setWindowState(keyboardFocus, false, false);
        input.pressKey(key);
        return input;
    }

    bool samePoint(const Vec3& a, const Vec3& b) {
        return near(a.x, b.x, 1e-4f) && near(a.y, b.y, 1e-4f) && near(a.z, b.z, 1e-4f);
    }

    // Each mode starts where the setup says, with the lens it gives.
    void testModesStartFromTheSetup() {
        World world;
        const PlayerControlsSetup setup = testSetup();
        PlayerControls controls{setup, ControlMode::FreeCamera, true};
        controls.spawnBody(world);
        assert(world.isAlive(controls.getFirstPersonBody()));
        assert(samePoint(world.get<Transform>(controls.getFirstPersonBody()).position, setup.firstPersonFeet));
        assert(controls.getCamera().getFarPlane() == setup.farPlane);

        controls.setControlMode(world, ControlMode::FreeCamera);
        assert(controls.getMode() == ControlMode::FreeCamera);
        assert(samePoint(controls.getCamera().getPosition(), setup.freeCameraPosition));
        assert(controls.wantsMouseLook());
        assert(samePoint(controls.getLightFocus(), setup.freeCameraPosition));

        controls.setControlMode(world, ControlMode::Moba);
        assert(!controls.wantsMouseLook());
        assert(samePoint(controls.getLightFocus(), setup.mobaLookPoint));

        controls.setControlMode(world, ControlMode::FirstPerson);
        assert(controls.wantsMouseLook());
        assert(samePoint(controls.getCamera().getPosition(), firstPersonEyePosition(world, controls.getFirstPersonBody(), 1.0f)));
    }

    // F1-F3 switch modes only while debug switching is on and the window has the keyboard; a switch is reported.
    void testModeSwitchingKeys() {
        World world;
        PlayerControls controls{testSetup(), ControlMode::FreeCamera, true};
        controls.spawnBody(world);
        controls.setControlMode(world, ControlMode::FreeCamera);

        assert(controls.updateModeSwitching(world, pressing(Key::F2)));
        assert(controls.getMode() == ControlMode::Moba);
        assert(!controls.updateModeSwitching(world, pressing(Key::F1, false)));
        assert(controls.getMode() == ControlMode::Moba);
        assert(controls.updateModeSwitching(world, pressing(Key::F3)));
        assert(controls.getMode() == ControlMode::FreeCamera);
        assert(!controls.updateModeSwitching(world, pressing(Key::W)));

        PlayerControls locked{testSetup(), ControlMode::FirstPerson, false};
        locked.spawnBody(world);
        locked.setControlMode(world, ControlMode::FirstPerson);
        assert(!locked.updateModeSwitching(world, pressing(Key::F2)));
        assert(locked.getMode() == ControlMode::FirstPerson);
    }

    // First person from the free camera drops the body where the camera is: its eyes at the camera.
    void testDropFromFreeCamera() {
        World world;
        PlayerControls controls{testSetup(), ControlMode::FreeCamera, true};
        controls.spawnBody(world);
        controls.setControlMode(world, ControlMode::FreeCamera);
        controls.poseFreeCamera(world, Vec3{30.0f, 7.0f, -2.0f}, Vec3{30.0f, 7.0f, -12.0f});

        controls.setControlMode(world, ControlMode::FirstPerson);
        const Entity body = controls.getFirstPersonBody();
        const float eyeHeight = world.get<FirstPersonBody>(body).settings.standingEyeHeight;
        assert(samePoint(world.get<Transform>(body).position, Vec3{30.0f, 7.0f - eyeHeight, -2.0f}));
    }

    // The movement keys become a wish velocity only in first person with the mouse captured.
    void testFirstPersonCommands() {
        World world;
        PlayerControls controls{testSetup(), ControlMode::FirstPerson, true};
        controls.spawnBody(world);
        controls.setControlMode(world, ControlMode::FirstPerson);
        const Entity body = controls.getFirstPersonBody();

        assert(controls.updateFirstPersonCommands(world, firstPersonInput({Key::W})));
        assert(world.get<CharacterController>(body).wishVelocity.z < -1.0f);

        Input released = firstPersonInput({Key::W});
        released.setWindowState(true, false, false);
        assert(!controls.updateFirstPersonCommands(world, released));
        assert(world.get<CharacterController>(body).wishVelocity.lengthSquared() == 0.0f);

        controls.setControlMode(world, ControlMode::Moba);
        assert(!controls.updateFirstPersonCommands(world, firstPersonInput({Key::W})));
        assert(world.get<CharacterController>(body).wishVelocity.lengthSquared() == 0.0f);
    }

    // Each camera's settings in a group of its own, so two games holding controls save them under the same keys.
    void testSettingsGroups() {
        PlayerControls controls{testSetup(), ControlMode::FreeCamera, true};
        const SettingsWalk walk = [&controls](SettingsVisitor& visitor) { controls.visitSettings(visitor); };
        std::set<std::string> groups;

        for (const SettingEntry& entry : captureSettings(walk)) {
            groups.insert(entry.key.substr(0, entry.key.find('.')));
        }

        assert(groups == (std::set<std::string>{"firstPersonCamera", "mobaCamera", "freeCamera"}));
    }
}

void testPlayerControls() {
    testModesStartFromTheSetup();
    testModeSwitchingKeys();
    testDropFromFreeCamera();
    testFirstPersonCommands();
    testSettingsGroups();
}
