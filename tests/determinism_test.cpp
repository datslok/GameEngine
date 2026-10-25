#include "cloth/cloth.h"
#include "core/hash.h"
#include "engine/state_hash.h"
#include "games/lightlab/light_lab_game.h"
#include "math/transform.h"
#include "scene/deforming_mesh.h"

#include "demo_helpers.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {
    // FNV-1a as published (Fowler, Noll and Vo): the hash of nothing is the offset basis, and of "a" a known number.
    void testHashMatchesThePublishedValues() {
        Fnv1aHash empty;
        assert(empty.value == 0xcbf29ce484222325ull);

        Fnv1aHash letter;
        letter.addBytes("a", 1);
        assert(letter.value == 0xaf63dc4c8601ec8cull);

        Fnv1aHash word;
        word.addBytes("foobar", 6);
        assert(word.value == 0x85944171f73967e8ull);
    }

    // Numbers are hashed by their exact bits: the order counts, and even 0 and -0 (equal as numbers) differ.
    void testHashIsBitExact() {
        Fnv1aHash first;
        first.add(1.0f);
        first.add(2.0f);

        Fnv1aHash swapped;
        swapped.add(2.0f);
        swapped.add(1.0f);
        assert(first.value != swapped.value);

        Fnv1aHash zero;
        zero.add(0.0f);
        Fnv1aHash negativeZero;
        negativeZero.add(-0.0f);
        assert(zero.value != negativeZero.value);

        Fnv1aHash again;
        again.add(1.0f);
        again.add(2.0f);
        assert(first.value == again.value);
    }

    /*
    * Consecutive numbers mixed give top bits spread evenly: their top 4 bits fill 16 bins about equally. (FNV-1a alone
    * of small consecutive numbers changes the top bits too little, which once laid a random forest out along curves.)
    */
    void testMixSpreadsConsecutiveNumbers() {
        constexpr int samples = 16000;
        int bins[16] = {};

        for (std::uint64_t value = 0; value < static_cast<std::uint64_t>(samples); ++value) {
            ++bins[mixBits(value) >> 60];
        }

        for (const int count : bins) {
            assert(count > 900 && count < 1100);
        }

        assert(mixBits(1) != mixBits(2));
    }

    // The world hash sees a change in the last bit of one position, and an entity that only differs by handle.
    void testStateHashSeesTheSmallestChange() {
        World first;
        World second;

        for (World* world : {&first, &second}) {
            const Entity entity = world->create();
            Transform transform;
            transform.position = Vec3{1.0f, 2.0f, 3.0f};
            world->add(entity, transform);
        }

        assert(hashSimulationState(first) == hashSimulationState(second));

        second.each<Transform>([](Entity, Transform& transform) {
            transform.position.y = std::nextafter(transform.position.y, 10.0f);
        });
        assert(hashSimulationState(first) != hashSimulationState(second));

        // Same component values, different entity (a destroyed and recreated slot has a new generation).
        World recycled;
        recycled.destroy(recycled.create());
        Transform transform;
        transform.position = Vec3{1.0f, 2.0f, 3.0f};
        recycled.add(recycled.create(), transform);
        assert(hashSimulationState(first) != hashSimulationState(recycled));
    }

    /*
    * A cloth on the GPU holds the GPU's points read back late, repeatable only on one GPU and driver, so the hash leaves
    * them out (and its mesh's shape) but still sees what the simulation decides: its pins.
    */
    void testStateHashLeavesOutGpuCloth() {
        World first;
        World second;
        for (World* world : {&first, &second}) {
            const Entity sheet = world->create();
            Cloth cloth = makeCloth(Mesh::grid(2, 2, 1.0f, 1.0f), Mat4::identity(), ClothSettings{});
            addClothPin(cloth, 6);
            cloth.onGpu = true;
            world->add(sheet, DeformingMesh{MeshHandle{0}, cloth.particles.positions, cloth.particles.positions});
            world->add(sheet, std::move(cloth));
        }
        assert(hashSimulationState(first) == hashSimulationState(second));

        second.each<Cloth>([](Entity, Cloth& cloth) {
            cloth.particles.positions[0].x += 0.01f;
            cloth.particles.velocities[1].y += 0.01f;
        });
        second.each<DeformingMesh>([](Entity, DeformingMesh& mesh) {
            mesh.positions[0].x += 0.01f;
        });
        assert(hashSimulationState(first) == hashSimulationState(second));

        second.each<Cloth>([](Entity, Cloth& cloth) {
            cloth.pins[0].position.x += 0.01f;
        });
        assert(hashSimulationState(first) != hashSimulationState(second));

        // On the CPU its points count again.
        first.each<Cloth>([](Entity, Cloth& cloth) {
            cloth.onGpu = false;
        });
        const std::uint64_t cpuHash = hashSimulationState(first);
        first.each<Cloth>([](Entity, Cloth& cloth) {
            cloth.particles.positions[0].x += 0.01f;
        });
        assert(hashSimulationState(first) != cpuHash);
    }

    /*
    * One scripted play-through of the demo: walking, turning, sprinting, crouching, jumping, throwing grenades that
    * bounce and expire, switching the lamp field on (100 fireflies), then switching to MOBA mode and sending the duck
    * across the ground with a right-click. The input persists between frames as the engine's does, so held keys stay
    * held and presses are fresh only on their frame. leaveOutOneThrow changes a single press.
    */
    class ScriptedDemo {
    public:
        explicit ScriptedDemo(bool leaveOutOneThrow): game(ControlMode::FirstPerson, true), leaveOutOneThrow(leaveOutOneThrow) {
            game.onInit(world, assets);
        }

        void playFrame(int frame) {
            input.beginFrame();
            const bool moba = frame >= mobaFrame;
            input.setWindowSize(Vec2{1600.0f, 900.0f});
            input.setWindowState(true, !moba, moba);
            input.setCursor(Vec2{800.0f, 450.0f}, true);

            hold(Key::W, frame < 90);
            hold(Key::LeftShift, frame >= 60 && frame < 90);
            hold(Key::LeftCtrl, frame >= 70 && frame < 80);
            tap(Key::Space, frame == 30);
            tap(Key::G, frame == 40 || (frame == 100 && !leaveOutOneThrow) || frame == 160);
            tap(Key::L, frame == 120);
            tap(Key::F2, frame == mobaFrame);

            if (frame < 90) {
                input.addMouseMotion(5.0f, frame < 45 ? 1.0f : -1.0f);
            }

            if (frame == mobaFrame + 10) {
                input.pressMouseButton(MouseButtonPress{MouseButton::Right, Vec2{1000.0f, 520.0f}, 0});
            } else if (frame == mobaFrame + 11) {
                input.releaseMouseButton(MouseButton::Right);
            }

            runFrame(game, world, input, ticks);
        }

        std::uint64_t hash() const {
            return hashSimulationState(world);
        }

    private:
        static constexpr int mobaFrame = 200;

        World world;
        AssetManager assets;
        DemoGame game;
        Input input;
        int ticks = 0;
        bool leaveOutOneThrow;

        void hold(Key key, bool held) {
            if (held) {
                input.pressKey(key);
            } else {
                input.releaseKey(key);
            }
        }

        // Pressed on its frame, released on the next.
        void tap(Key key, bool pressNow) {
            hold(key, pressNow);
        }
    };

    /*
    * The determinism promise: the same build, given the same commands, gives bit-identical results. Two games play
    * the same script side by side and must agree after every frame; a third, one grenade throw short, must not (so
    * the test is not passing because the hash ignores what happens).
    */
    void testDemoIsDeterministic() {
        ScriptedDemo first{false};
        ScriptedDemo second{false};
        ScriptedDemo different{true};
        constexpr int frames = 720;

        for (int frame = 0; frame < frames; ++frame) {
            first.playFrame(frame);
            second.playFrame(frame);
            different.playFrame(frame);
            assert(first.hash() == second.hash());
        }

        assert(first.hash() != different.hash());
    }

    /*
    * The light lab played the same way twice: walking towards the building in first person with the sun's clock running,
    * jumping, turning, then dropping in again from the free camera's start. leaveOutOneJump changes a single press.
    */
    class ScriptedLightLab {
    public:
        explicit ScriptedLightLab(bool leaveOutOneJump): game(ControlMode::FirstPerson, true), leaveOutOneJump(leaveOutOneJump) {
            game.getSunSettings().hoursPerSecond = 0.5f;
            game.onInit(world, assets);
        }

        void playFrame(int frame) {
            Input input = firstPersonInput({});
            if (frame < 150) {
                input.pressKey(Key::S);
                input.pressKey(Key::A);
                input.addMouseMotion(frame < 75 ? 3.0f : -2.0f, 0.0f);
            }
            if (frame == 20 || (frame == 130 && !leaveOutOneJump)) {
                input.pressKey(Key::Space);
            }
            if (frame == 200) {
                input.pressKey(Key::F3);
            }
            if (frame == 230) {
                input.pressKey(Key::F1);
            }

            runFrame(game, world, input, ticks);
        }

        std::uint64_t hash() const {
            return hashSimulationState(world);
        }

    private:
        World world;
        AssetManager assets;
        LightLabGame game;
        int ticks = 0;
        bool leaveOutOneJump;
    };

    /*
    * As for the demo; the one-jump-short run is compared just before the free camera, since dropping in from it puts
    * every run's body in the same place.
    */
    void testLightLabIsDeterministic() {
        ScriptedLightLab first{false};
        ScriptedLightLab second{false};
        ScriptedLightLab different{true};

        for (int frame = 0; frame < 360; ++frame) {
            first.playFrame(frame);
            second.playFrame(frame);
            different.playFrame(frame);
            assert(first.hash() == second.hash());

            if (frame == 199) {
                assert(first.hash() != different.hash());
            }
        }
    }
}

void testDeterminism() {
    testHashMatchesThePublishedValues();
    testHashIsBitExact();
    testMixSpreadsConsecutiveNumbers();
    testStateHashSeesTheSmallestChange();
    testStateHashLeavesOutGpuCloth();
    testDemoIsDeterministic();
    testLightLabIsDeterministic();
}
