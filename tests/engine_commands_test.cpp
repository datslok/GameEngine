#include "core/command_registry.h"
#include "engine/engine_commands.h"
#include "engine/engine_settings.h"
#include "engine/settings_file.h"
#include "engine/state_hash.h"
#include "games/demo/demo_game.h"

#include "demo_helpers.h"

#include <cassert>
#include <cmath>
#include <string>
#include <vector>

namespace {
    bool contains(const std::string& text, const std::string& part) {
        return text.find(part) != std::string::npos;
    }

    // A registry with the settings commands over the given settings, their defaults those of a fresh EngineSettings.
    struct SettingsSetup {
        EngineSettings settings;
        CommandRegistry registry;

        SettingsSetup() {
            EngineSettings defaults;
            addSettingsCommands(registry, engineSettings(settings), captureSettings(engineSettings(defaults)));
        }
    };

    // set changes a field of every kind, live, and replies with the value it now has.
    void testSetChangesSettings() {
        SettingsSetup setup;

        const CommandReply fps = setup.registry.execute("set engine.targetFPS 60");
        assert(fps.ok);
        assert(setup.settings.targetFPS == 60);
        assert(fps.text == "engine.targetFPS = 60");

        assert(setup.registry.execute("set engine.render.bloom.strength 0.1").ok);
        assert(setup.settings.render.bloom.strength == 0.1f);

        assert(setup.registry.execute("set engine.clothOnGPU false").ok);
        assert(!setup.settings.clothOnGpu);

        assert(setup.registry.execute("set engine.presentMode Vsync").ok);
        assert(setup.settings.presentMode == PresentMode::Vsync);

        // key=value works too, as at launch.
        assert(setup.registry.execute("set engine.targetFPS=90").ok);
        assert(setup.settings.targetFPS == 90);
    }

    // A value past its limits is clamped, and the reply says so; one that does not read is refused, leaving the field.
    void testSetReportsProblems() {
        SettingsSetup setup;

        const CommandReply clamped = setup.registry.execute("set engine.render.bloom.strength 5");
        assert(clamped.ok);
        assert(setup.settings.render.bloom.strength == maxBloomStrength);
        assert(contains(clamped.text, "limit"));

        const CommandReply word = setup.registry.execute("set engine.targetFPS fast");
        assert(!word.ok);
        assert(setup.settings.targetFPS == EngineSettings{}.targetFPS);

        const CommandReply choice = setup.registry.execute("set engine.presentMode Sometimes");
        assert(!choice.ok);
        assert(contains(choice.text, "Mailbox"));

        assert(!setup.registry.execute("set engine.targetFPS").ok);
        assert(!setup.registry.execute("set").ok);
    }

    // An unknown key is refused with the nearest keys; a group's name is not a field.
    void testUnknownKeysSuggestNearOnes() {
        SettingsSetup setup;

        const CommandReply typo = setup.registry.execute("set engine.targetFSP 60");
        assert(!typo.ok);
        assert(contains(typo.text, "engine.targetFPS"));

        const CommandReply partial = setup.registry.execute("set bloom.strength 0.1");
        assert(!partial.ok);
        assert(contains(partial.text, "engine.render.bloom.strength"));

        assert(!setup.registry.execute("set engine.render.bloom 1").ok);
    }

    // Start-up fields were read once already, so set refuses them and says how to change them instead.
    void testStartupFieldsAreRefusedLive() {
        SettingsSetup setup;

        const CommandReply reply = setup.registry.execute("set engine.ticksPerSecond 60");
        assert(!reply.ok);
        assert(contains(reply.text, "start-up"));
        assert(contains(reply.text, "--set"));
        assert(setup.settings.ticksPerSecond == EngineSettings{}.ticksPerSecond);
    }

    // get shows one field, or every field under a group; nothing matching is an error with near keys.
    void testGetShowsValues() {
        SettingsSetup setup;
        setup.settings.targetFPS = 75;

        const CommandReply one = setup.registry.execute("get engine.targetFPS");
        assert(one.ok);
        assert(one.text == "engine.targetFPS = 75");

        const CommandReply group = setup.registry.execute("get engine.render.bloom");
        assert(group.ok);
        assert(contains(group.text, "engine.render.bloom.strength = "));
        assert(contains(group.text, "engine.render.bloom.maxLevels = "));
        assert(!contains(group.text, "engine.targetFPS"));

        // A group's name must end at a dot: "engine.render.bloo" is not the bloom group.
        assert(!setup.registry.execute("get engine.render.bloo").ok);

        const CommandReply everything = setup.registry.execute("get");
        assert(everything.ok);
        assert(contains(everything.text, "engine.targetFPS = 75"));
        assert(contains(everything.text, "engine.render.bloom.strength = "));
    }

    // list adds what each field takes and whether it is read only at start-up.
    void testListDescribesFields() {
        SettingsSetup setup;

        const CommandReply bloom = setup.registry.execute("list engine.render.bloom");
        assert(bloom.ok);
        assert(contains(bloom.text, "engine.render.bloom.strength = "));
        assert(contains(bloom.text, "number 0 to "));
        assert(contains(bloom.text, "true or false"));

        const CommandReply present = setup.registry.execute("list engine.presentMode");
        assert(contains(present.text, "one of Vsync, Mailbox, Immediate"));

        const CommandReply ticks = setup.registry.execute("list engine.ticksPerSecond");
        assert(contains(ticks.text, "start-up"));
    }

    // reset returns one field, a group or everything to the defaults, leaving start-up fields alone.
    void testResetRestoresDefaults() {
        SettingsSetup setup;
        const EngineSettings defaults;

        setup.settings.targetFPS = 30;
        setup.settings.render.bloom.strength = 0.15f;
        setup.settings.render.bloom.maxLevels = 2;
        setup.settings.clothOnGpu = !defaults.clothOnGpu;

        assert(setup.registry.execute("reset engine.targetFPS").ok);
        assert(setup.settings.targetFPS == defaults.targetFPS);
        assert(setup.settings.render.bloom.strength == 0.15f);

        assert(setup.registry.execute("reset engine.render.bloom").ok);
        assert(setup.settings.render.bloom.strength == defaults.render.bloom.strength);
        assert(setup.settings.render.bloom.maxLevels == defaults.render.bloom.maxLevels);
        assert(setup.settings.clothOnGpu != defaults.clothOnGpu);

        setup.settings.ticksPerSecond = 60.0;
        assert(setup.registry.execute("reset all").ok);
        assert(setup.settings.clothOnGpu == defaults.clothOnGpu);
        assert(setup.settings.ticksPerSecond == 60.0);

        assert(!setup.registry.execute("reset nonsense.key").ok);
        assert(!setup.registry.execute("reset").ok);
    }

    // A paused frame runs no ticks, and their time is dropped, so resuming does not race to catch up.
    void testPausedFramesRunNoTicks() {
        SimulationControl control;
        assert(ticksThisFrame(control, 0) == 0);
        assert(ticksThisFrame(control, 3) == 3);

        control.paused = true;
        assert(ticksThisFrame(control, 3) == 0);
    }

    // pause and resume switch the clock; step pauses first, then runs exactly that many ticks (one without a count).
    void testPauseAndStep() {
        SimulationControl control;
        CommandRegistry registry;
        int ticksRun = 0;
        addSimulationCommands(registry, control, [&] {
            ++ticksRun;
        });

        assert(registry.execute("pause").ok);
        assert(control.paused);
        assert(registry.execute("resume").ok);
        assert(!control.paused);

        const CommandReply five = registry.execute("step 5");
        assert(five.ok);
        assert(control.paused);
        assert(ticksRun == 5);

        assert(registry.execute("step").ok);
        assert(ticksRun == 6);

        assert(!registry.execute("step 0").ok);
        assert(!registry.execute("step -2").ok);
        assert(!registry.execute("step lots").ok);
        assert(!registry.execute("step " + std::to_string(maxStepTicks + 1)).ok);
        assert(ticksRun == 6);
    }

    /*
    * Stepping runs the same ticks the loop would: the demo stepped 120 ticks by the command reaches the very state of
    * the demo ticked 120 times, bit for bit (hashSimulationState), so commands never cost determinism.
    */
    void testSteppingIsDeterministic() {
        World ticked;
        AssetManager tickedAssets;
        DemoGame tickedGame;
        tickedGame.onInit(ticked, tickedAssets);
        int tickedCount = 0;
        runTicks(tickedGame, ticked, 120, tickedCount);

        World stepped;
        AssetManager steppedAssets;
        DemoGame steppedGame;
        steppedGame.onInit(stepped, steppedAssets);
        int steppedCount = 0;

        SimulationControl control;
        CommandRegistry registry;
        addSimulationCommands(registry, control, [&] {
            runTicks(steppedGame, stepped, 1, steppedCount);
        });
        assert(registry.execute("step 100").ok);
        assert(registry.execute("step 20").ok);

        assert(steppedCount == tickedCount);
        assert(hashSimulationState(stepped) == hashSimulationState(ticked));
    }

    bool near(const Vec3& a, const Vec3& b) {
        return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f && std::fabs(a.z - b.z) < 1e-4f;
    }

    // camera shows the pose; with a place it moves there, looking at the target or the same way as before.
    void testCameraCommands() {
        World world;
        AssetManager assets;
        DemoGame game;
        game.onInit(world, assets);

        CommandRegistry registry;
        addCameraCommands(registry, game, world);

        const CommandReply shown = registry.execute("camera");
        assert(shown.ok);
        assert(contains(shown.text, "CAMERA"));

        assert(registry.execute("camera 1 2 3 1 2 -7").ok);
        assert(near(game.getCamera().getPosition(), Vec3{1.0f, 2.0f, 3.0f}));
        assert(near(game.getCamera().getForward(), Vec3{0.0f, 0.0f, -1.0f}));

        assert(registry.execute("camera 4 5 6").ok);
        assert(near(game.getCamera().getPosition(), Vec3{4.0f, 5.0f, 6.0f}));
        assert(near(game.getCamera().getForward(), Vec3{0.0f, 0.0f, -1.0f}));

        // The pose holds through a frame: the free camera takes it over rather than snapping back.
        game.onUpdate(world, firstPersonInput({}), tickSeconds, 1.0f);
        assert(near(game.getCamera().getPosition(), Vec3{4.0f, 5.0f, 6.0f}));

        assert(!registry.execute("camera 1 2").ok);
        assert(!registry.execute("camera 1 2 3 4").ok);
        assert(!registry.execute("camera 1 2 x").ok);
        assert(!registry.execute("camera 1 2 3 1 2 3").ok);
    }

    // A game that cannot place its camera says so, rather than failing silently.
    void testCameraOfAGameThatCannotBePlaced() {
        class FixedCameraGame final : public Game {
        public:
            const char* title() const override {
                return "Fixed";
            }
            void onInit(World&, AssetManager&) override {
            }
            void onInput(World&, const Input&) override {
            }
            void onFixedUpdate(World&, float, double) override {
            }
            void onUpdate(World&, const Input&, float, float) override {
            }
            Camera& getCamera() override {
                return camera;
            }
            bool wantsMouseLook() const override {
                return false;
            }

        private:
            Camera camera{Vec3{0.0f, 2.0f, 5.0f}, Vec3{0.0f, 2.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 16.0f / 9.0f, 0.1f, 100.0f};
        };

        World world;
        FixedCameraGame game;
        CommandRegistry registry;
        addCameraCommands(registry, game, world);

        assert(registry.execute("camera").ok);
        const CommandReply refused = registry.execute("camera 1 2 3");
        assert(!refused.ok);
        assert(contains(refused.text, "Fixed"));
    }
}

void testEngineCommands() {
    testSetChangesSettings();
    testSetReportsProblems();
    testUnknownKeysSuggestNearOnes();
    testStartupFieldsAreRefusedLive();
    testGetShowsValues();
    testListDescribesFields();
    testResetRestoresDefaults();
    testPausedFramesRunNoTicks();
    testPauseAndStep();
    testSteppingIsDeterministic();
    testCameraCommands();
    testCameraOfAGameThatCannotBePlaced();
}
