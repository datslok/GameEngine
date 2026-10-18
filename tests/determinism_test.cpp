#include "core/hash.h"
#include "engine/state_hash.h"
#include "math/transform.h"

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
}

void testDeterminism() {
    testHashMatchesThePublishedValues();
    testHashIsBitExact();
    testStateHashSeesTheSmallestChange();
    testDemoIsDeterministic();
}
