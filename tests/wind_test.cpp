#include "ecs/world.h"
#include "engine/debug_views.h"
#include "games/forest/forest_game.h"
#include "math/angles.h"
#include "render/gpu/wind.h"
#include "scene/frame_description.h"
#include "scene/wind.h"

#include "forest_helpers.h"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    // Value noise: between 0 and 1, the same every time, smooth (a small step moves it a little).
    void testTheNoise() {
        float least = 1.0f;
        float most = 0.0f;

        for (int i = 0; i < 400; ++i) {
            const Vec3 place{0.37f * static_cast<float>(i) - 50.0f, 0.11f * static_cast<float>(i), -0.23f * static_cast<float>(i)};
            const float value = windNoise(place);
            assert(value >= 0.0f && value <= 1.0f && value == windNoise(place));
            assert(std::abs(windNoise(place + Vec3{0.01f, 0.0f, 0.0f}) - value) < 0.05f);
            least = std::min(least, value);
            most = std::max(most, value);
        }

        assert(least < 0.2f && most > 0.8f);
    }

    /*
    * The wind: without gusts or turbulence, exactly its speed towards its direction; with gusts its mean over the land is
    * still about that, and the gusts travel with the wind (frozen turbulence): a second later the same gust is the wind's
    * speed further on. Turbulence pushes sideways, at most its share of the speed.
    */
    void testTheWind() {
        Wind steady;
        steady.towardsDegrees = 90.0f;
        steady.speed = 4.0f;
        steady.gustStrength = 0.0f;
        steady.turbulence = 0.0f;
        const Vec2 calm = windVelocity(steady, Vec2{12.0f, -3.0f}, 7.0f);
        assert(near(calm.x, 0.0f, 1e-5f) && near(calm.y, 4.0f, 1e-5f));

        Wind gusty;
        gusty.towardsDegrees = 0.0f;
        gusty.speed = 3.0f;
        gusty.turbulence = 0.0f;
        Vec2 sum{0.0f, 0.0f};
        constexpr int samples = 2000;

        for (int i = 0; i < samples; ++i) {
            sum = sum + windVelocity(gusty, Vec2{7.3f * static_cast<float>(i % 50), 9.1f * static_cast<float>(i / 50)}, 0.0f);
        }

        assert(near(sum.x / samples, 3.0f, 0.3f) && near(sum.y / samples, 0.0f, 1e-4f));

        float strongest = 0.0f;

        for (int i = 0; i < 50; ++i) {
            const Vec2 place{11.0f * static_cast<float>(i), 5.0f};
            const Vec2 now = windVelocity(gusty, place, 2.0f);
            const Vec2 later = windVelocity(gusty, place + Vec2{gusty.speed, 0.0f}, 3.0f);
            assert(std::abs(later.x - now.x) < 0.15f * gusty.speed);
            strongest = std::max(strongest, now.x);
        }

        assert(strongest > gusty.speed * 1.1f);

        Wind turbulent = gusty;
        turbulent.gustStrength = 0.0f;
        turbulent.turbulence = 0.3f;
        float sideways = 0.0f;

        for (int i = 0; i < 100; ++i) {
            const Vec2 velocity = windVelocity(turbulent, Vec2{3.1f * static_cast<float>(i), 0.0f}, 1.0f);
            assert(std::abs(velocity.y) <= turbulent.turbulence * turbulent.speed + 1e-4f);
            sideways = std::max(sideways, std::abs(velocity.y));
        }

        assert(sideways > 0.1f * turbulent.turbulence * turbulent.speed);
    }

    // A spring's rest under a wind: its velocity times its speed times the response (drag grows with the square).
    void testTheTarget() {
        const Vec2 target = windSpringTarget(Vec2{3.0f, -4.0f}, 0.01f);
        assert(near(target.x, 0.15f, 1e-6f) && near(target.y, -0.2f, 1e-6f));
    }

    // The displacement along x of a spring from rest at 0 pushed towards 1, after seconds in steps of step.
    std::vector<float> springPath(float frequency, float damping, float step, float seconds) {
        Vec4 state{0.0f, 0.0f, 0.0f, 0.0f};
        std::vector<float> path;

        for (float time = 0.0f; time < seconds; time += step) {
            state = windSpringStep(state, Vec2{1.0f, 0.0f}, frequency, damping, step);
            path.push_back(state.x);
        }

        return path;
    }

    /*
    * The spring: settles on its target; lightly damped it overshoots and swings at its frequency (the damped frequency
    * f sqrt(1 - zeta^2)), critically damped it never overshoots; it stays stable at the longest step at grass's
    * frequency.
    */
    void testTheSpring() {
        const std::vector<float> light = springPath(1.0f, 0.1f, 1.0f / 120.0f, 40.0f);
        assert(near(light.back(), 1.0f, 1e-3f));
        assert(*std::max_element(light.begin(), light.end()) > 1.5f);

        // Its swings: the times it crosses the target going up, a damped period apart.
        std::vector<float> crossings;

        for (std::size_t i = 1; i < light.size(); ++i) {
            if (light[i - 1] < 1.0f && light[i] >= 1.0f) {
                crossings.push_back(static_cast<float>(i) / 120.0f);
            }
        }

        assert(crossings.size() >= 3);
        const float period = 1.0f / (1.0f * std::sqrt(1.0f - 0.1f * 0.1f));
        assert(near(crossings[2] - crossings[1], period, 0.03f * period));

        const std::vector<float> critical = springPath(1.0f, 1.0f, 1.0f / 60.0f, 10.0f);
        assert(*std::max_element(critical.begin(), critical.end()) <= 1.0f + 1e-3f && critical.back() > 0.99f);

        const std::vector<float> coarse = springPath(2.0f, 0.3f, 0.5f, 60.0f);

        for (const float value : coarse) {
            assert(std::isfinite(value) && std::abs(value) < 3.0f);
        }

        assert(near(coarse.back(), 1.0f, 1e-3f));
    }

    /*
    * The window round a place: WIND_CELLS cells a side, the place's cell in its middle; each texel holds the one cell of
    * the window that it is the same as modulo the side, so every cell has one texel and moving the window keeps the cells
    * both share where they were.
    */
    void testTheWindow() {
        const WindWindow window = windWindowAround(Vec2{-37.5f, 1001.0f}, 2.0f);
        assert(window.originX == static_cast<int>(std::floor(-37.5f / 2.0f)) - WIND_CELLS / 2);
        assert(window.originZ == static_cast<int>(std::floor(1001.0f / 2.0f)) - WIND_CELLS / 2);
        std::set<std::pair<int, int>> cells;

        for (int x = 0; x < WIND_CELLS; ++x) {
            for (int z = 0; z < WIND_CELLS; ++z) {
                const WindCell cell = windCellOfTexel(x, z, window);
                assert(windCellInWindow(cell, window));
                assert(((cell.x % WIND_CELLS) + WIND_CELLS) % WIND_CELLS == x && ((cell.z % WIND_CELLS) + WIND_CELLS) % WIND_CELLS == z);
                cells.insert({cell.x, cell.z});
            }
        }

        assert(cells.size() == static_cast<std::size_t>(WIND_CELLS * WIND_CELLS));
        const WindWindow moved{window.originX + 3, window.originZ - 5};
        const WindCell kept = windCellOfTexel(7, 9, window);

        if (windCellInWindow(kept, moved)) {
            const WindCell again = windCellOfTexel(7, 9, moved);
            assert(again.x == kept.x && again.z == kept.z);
        }

        assert(!windCellInWindow(WindCell{window.originX - 1, window.originZ}, window));
    }

    /*
    * The record the GPU reads: the wind's direction, speed and the time; each category's spring; each cascade's window,
    * last frame's window and its cell size; off without wind or with the setting off.
    */
    void testTheRecord() {
        static_assert(sizeof(GpuWindField) == 192);
        WindSettings settings;
        Wind wind;
        wind.towardsDegrees = 90.0f;
        wind.speed = 5.0f;
        const std::array<WindWindow, WIND_CASCADES> windows{WindWindow{1, 2}, WindWindow{3, 4}};
        const std::array<WindWindow, WIND_CASCADES> previous{WindWindow{0, 2}, WindWindow{3, 3}};
        const GpuWindField field = packWindField(&wind, settings, windows, previous, true, 12.5f, 1.0f / 60.0f);
        assert(near(field.wind[0], 0.0f, 1e-6f) && near(field.wind[1], 1.0f, 1e-6f) && field.wind[2] == 5.0f && field.wind[3] == 12.5f);
        assert(field.step[1] == 1.0f && field.flags[0] == 1);
        assert(field.springs[WIND_CATEGORY_GRASS][0] == settings.springs[WIND_CATEGORY_GRASS].frequency);
        assert(field.windows[1][0] == 3 && field.windows[1][1] == 4 && field.windows[1][2] == 3 && field.windows[1][3] == 3);
        assert(field.cells[0][0] == settings.nearCellMetres && field.cells[1][0] == settings.farCellMetres);

        assert(packWindField(nullptr, settings, windows, previous, true, 0.0f, 0.0f).step[1] == 0.0f);
        WindSettings off = settings;
        off.enabled = false;
        assert(packWindField(&wind, off, windows, previous, true, 0.0f, 0.0f).step[1] == 0.0f);
    }

    // The settings: checked, and the component reaches the frame (the first one placed).
    void testTheSettings() {
        WindSettings settings;
        validateWind(settings);
        WindSettings badCell = settings;
        badCell.nearCellMetres = 0.0f;
        bool threw = false;

        try {
            validateWind(badCell);
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);
        WindSettings badSpring = settings;
        badSpring.springs[0].frequency = 0.0f;
        threw = false;

        try {
            validateWind(badSpring);
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);

        World world;
        Wind first;
        first.speed = 6.0f;
        world.add(world.create(), first);
        Wind second;
        second.speed = 1.0f;
        world.add(world.create(), second);
        const Camera camera{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.0f, 0.1f, 10.0f};
        const FrameDescription frame = buildFrame(world, camera, 0.0f);
        assert(frame.wind && frame.wind->speed == 6.0f);
        World empty;
        assert(!buildFrame(empty, camera, 0.0f).wind);
    }
}

namespace {
    /*
    * The field's debug view: an arrow at each point of a grid round the eye, standing on the ground, along the wind
    * there; a steady wind draws them all the same way.
    */
    void testTheFieldsArrows() {
        Wind steady;
        steady.towardsDegrees = 0.0f;
        steady.speed = 4.0f;
        steady.gustStrength = 0.0f;
        steady.turbulence = 0.0f;
        DebugDraw debug;
        drawWindField(steady, Vec3{10.0f, 5.0f, -3.0f}, 2.0f, nullptr, debug);
        const std::size_t lines = debug.getLines().size();
        assert(lines == static_cast<std::size_t>(windArrowsAcross * windArrowsAcross * 3));

        for (std::size_t line = 0; line < lines; line += 3) {
            const DebugLine& shaft = debug.getLines()[line];
            const Vec3 along = shaft.to - shaft.from;
            assert(along.x > 0.0f && near(along.z, 0.0f, 1e-4f) && near(along.y, 0.0f, 1e-4f));
            assert(near(shaft.from.y, 5.0f - windArrowEyeDrop, 1e-4f));
        }
    }
}

namespace {
    // The forest's wind blows down its valley, live from its settings (F5 Forest, Wind).
    void testTheForestsWind() {
        TestForest forest;
        const Camera camera{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.0f, 0.1f, 10.0f};
        const FrameDescription frame = buildFrame(forest.world, camera, 0.0f);
        assert(frame.wind && frame.wind->speed == forest.game.getWindSettings().speed && frame.wind->towardsDegrees == 270.0f);

        FieldCounter counter;
        visit(forest.game.getWindSettings(), counter);
        assert(counter.fields == 6 && counter.live == 6);
    }
}

void testWind() {
    testTheFieldsArrows();
    testTheForestsWind();
    testTheNoise();
    testTheWind();
    testTheTarget();
    testTheSpring();
    testTheWindow();
    testTheRecord();
    testTheSettings();
}
