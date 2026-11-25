#include "ecs/world.h"
#include "games/forest/forest_game.h"
#include "games/forest/forest_sway.h"
#include "input/input.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/wind.h"
#include "scene/frame_description.h"
#include "scene/model_renderer.h"
#include "scene/still_draws.h"
#include "scene/wind.h"

#include "forest_helpers.h"

#include <cassert>
#include <cmath>
#include <map>
#include <random>
#include <stdexcept>
#include <utility>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    bool throwsInvalid(const WindSettings& settings) {
        try {
            validateWind(settings);
        } catch (const std::invalid_argument&) {
            return true;
        }

        return false;
    }

    // A tree's sway: 20 m tall in its own units, bending by its spring's lean, its leaves still unless asked.
    Vec4 treeSway(float bend = 1.0f, float flutter = 0.0f) {
        return packSway(WindSway{.kind = SwayKind::Tree, .height = 20.0f, .bend = bend, .flutter = flutter});
    }

    // A tree's leaves' flutter rate: 3 Hz, its phase changing every half metre.
    const Vec2 leafRate{3.0f, 0.5f};

    // Each kind's category in the field's order, still ones -1, the rest copied.
    void testThePackedSway() {
        assert(packSway(WindSway{}).x == static_cast<float>(WIND_CATEGORY_STILL));
        assert(packSway(WindSway{.kind = SwayKind::Tree}).x == static_cast<float>(WIND_CATEGORY_TREES));
        assert(packSway(WindSway{.kind = SwayKind::Plant}).x == static_cast<float>(WIND_CATEGORY_PLANTS));
        assert(packSway(WindSway{.kind = SwayKind::Grass}).x == static_cast<float>(WIND_CATEGORY_GRASS));
        assert(packSway(WindSway{.kind = SwayKind::Hanging}).x == static_cast<float>(WIND_CATEGORY_HANGING));
        const Vec4 packed = packSway(WindSway{.kind = SwayKind::Plant, .height = 2.5f, .bend = 0.3f, .flutter = 0.1f});
        assert(packed.y == 2.5f && packed.z == 0.3f && packed.w == 0.1f);
    }

    // The fade: whole to its start, none past its end, smooth and falling between.
    void testTheFade() {
        assert(windSwayFade(0.0f, 80.0f, 120.0f) == 1.0f && windSwayFade(80.0f, 80.0f, 120.0f) == 1.0f);
        assert(windSwayFade(120.0f, 80.0f, 120.0f) == 0.0f && windSwayFade(500.0f, 80.0f, 120.0f) == 0.0f);
        assert(near(windSwayFade(100.0f, 80.0f, 120.0f), 0.5f, 1e-6f));
        float last = 1.0f;

        for (float distance = 80.0f; distance <= 120.0f; distance += 1.0f) {
            const float share = windSwayFade(distance, 80.0f, 120.0f);
            assert(share <= last);
            last = share;
        }
    }

    // A breeze's lean barely changed; a storm's held under the most, along its own direction, still growing.
    void testTheSoftLean() {
        const Vec2 breeze = windSoftLean(Vec2{0.01f, 0.0f});
        assert(near(breeze.x, 0.01f, 0.01f * 0.03f) && breeze.y == 0.0f);
        const Vec2 storm = windSoftLean(Vec2{30.0f, 40.0f});
        assert(storm.length() < static_cast<float>(WIND_MOST_LEAN) && storm.length() > 0.9f * static_cast<float>(WIND_MOST_LEAN));
        assert(near(storm.x / storm.y, 0.75f, 1e-5f));
        assert(windSoftLean(Vec2{2.0f, 0.0f}).x > windSoftLean(Vec2{1.0f, 0.0f}).x);
        assert(windSoftLean(Vec2{0.0f, 0.0f}).length() == 0.0f);
    }

    // The near window's share: whole in its middle, none outside, falling over its last cells, the same on every side.
    void testTheWindowShare() {
        const WindWindow window{-32, -32};
        constexpr float cell = 2.0f;
        assert(windWindowShare(Vec2{0.0f, 0.0f}, window, cell) == 1.0f);
        assert(windWindowShare(Vec2{200.0f, 0.0f}, window, cell) == 0.0f && windWindowShare(Vec2{0.0f, -200.0f}, window, cell) == 0.0f);
        // The edge is 64 m out; the last cell beside it reads nothing of this window.
        assert(windWindowShare(Vec2{63.5f, 0.0f}, window, cell) == 0.0f);
        const float blending = windWindowShare(Vec2{55.0f, 0.0f}, window, cell);
        assert(blending > 0.0f && blending < 1.0f);
        assert(near(windWindowShare(Vec2{-55.0f, 0.0f}, window, cell), blending, 1e-6f));
        assert(near(windWindowShare(Vec2{0.0f, 55.0f}, window, cell), blending, 1e-6f));
        float last = 1.0f;

        for (float x = 30.0f; x < 64.0f; x += 0.25f) {
            const float share = windWindowShare(Vec2{x, 0.0f}, window, cell);
            assert(share <= last && last - share < 0.05f);
            last = share;
        }
    }

    // The phase changes smoothly with place: a little for a small step, about a turn over its metres.
    void testThePhase() {
        const Vec3 place{12.0f, 3.0f, -7.0f};
        assert(std::abs(windPhase(place + Vec3{0.01f, 0.0f, 0.0f}, 4.0f) - windPhase(place, 4.0f)) < 0.05f);
        assert(windPhase(place, 4.0f) != windPhase(place + Vec3{4.0f, 0.0f, 0.0f}, 4.0f));
    }

    /*
    * The main bend: nothing for a still sway, without wind, at the foot or faded out; the top leans with its spring and
    * stays as far from the foot; half way up it leans about a quarter as far; a tree twice the size leans twice as far.
    */
    void testTheBend() {
        const Vec2 spring{0.05f, 0.0f};
        const Vec3 top{0.0f, 20.0f, 0.0f};
        assert(windSwayOffset(top, 20.0f, top, packSway(WindSway{}), 1.0f, spring, 1.0f, leafRate, 3.0f).length() == 0.0f);
        assert(windSwayOffset(top, 20.0f, top, treeSway(), 1.0f, Vec2{0.0f, 0.0f}, 1.0f, leafRate, 3.0f).length() == 0.0f);
        assert(windSwayOffset(Vec3{0.0f, 0.0f, 0.0f}, 0.0f, Vec3{}, treeSway(1.0f, 0.5f), 1.0f, spring, 1.0f, leafRate, 3.0f).length() == 0.0f);
        assert(windSwayOffset(top, 20.0f, top, treeSway(1.0f, 0.5f), 1.0f, spring, 0.0f, leafRate, 3.0f).length() == 0.0f);

        const Vec3 moved = windSwayOffset(top, 20.0f, top, treeSway(), 1.0f, spring, 1.0f, leafRate, 3.0f);
        const Vec2 lean = windSoftLean(spring);
        assert(moved.x > 0.0f && near(moved.z, 0.0f, 1e-6f));
        assert(near(moved.x, lean.x * 20.0f, 0.01f * lean.x * 20.0f));
        assert(near((top + moved).length(), 20.0f, 1e-4f));
        assert(moved.y < 0.0f);

        const Vec3 half{0.0f, 10.0f, 0.0f};
        const Vec3 halfMoved = windSwayOffset(half, 10.0f, half, treeSway(), 1.0f, spring, 1.0f, leafRate, 3.0f);
        assert(near(halfMoved.x, 0.25f * moved.x, 0.02f * moved.x));

        // Along the other axis likewise, and the bend scales the spring before it is softened.
        const Vec3 sideways = windSwayOffset(top, 20.0f, top, treeSway(0.5f), 1.0f, Vec2{0.0f, -0.05f}, 1.0f, leafRate, 3.0f);
        const float halfLean = windSoftLean(Vec2{0.0f, -0.025f}).y * 20.0f;
        assert(sideways.z < 0.0f && near(sideways.z, halfLean, 0.01f * std::abs(halfLean)) && near(sideways.x, 0.0f, 1e-6f));

        const Vec3 bigTop{0.0f, 40.0f, 0.0f};
        const Vec3 bigMoved = windSwayOffset(bigTop, 20.0f, bigTop, treeSway(), 2.0f, spring, 1.0f, leafRate, 3.0f);
        assert(near(bigMoved.x, 2.0f * moved.x, 1e-4f) && near(bigMoved.y, 2.0f * moved.y, 1e-4f));

        // Half faded, about half the lean.
        const Vec3 faded = windSwayOffset(top, 20.0f, top, treeSway(), 1.0f, spring, 0.5f, leafRate, 3.0f);
        assert(near(faded.x, 0.5f * moved.x, 0.01f * moved.x));
    }

    /*
    * Branches bob up and down off the trunk as time goes on, by at most their share of the lean times their distance
    * either way, while a vertex on the trunk's axis only leans (the same offset at every time).
    */
    void testTheBranches() {
        const Vec2 spring{0.1f, 0.0f};
        const Vec3 axis{0.0f, 20.0f, 0.0f};
        const Vec3 branch{6.0f, 20.0f, 0.0f};
        const float lean = windSoftLean(spring).length();
        const Vec3 axisAtStart = windSwayOffset(axis, 20.0f, axis, treeSway(), 1.0f, spring, 1.0f, leafRate, 0.0f);
        float highest = -1.0f;
        float lowest = 1.0f;

        for (int frame = 0; frame < 240; ++frame) {
            const float seconds = 0.01f * static_cast<float>(frame);
            const Vec3 onAxis = windSwayOffset(axis, 20.0f, axis, treeSway(), 1.0f, spring, 1.0f, leafRate, seconds);
            assert(near(onAxis.y, axisAtStart.y, 1e-5f) && near(onAxis.x, axisAtStart.x, 1e-5f));
            assert(windSwayOffset(branch, 20.0f, branch, treeSway(), 1.0f, Vec2{0.0f, 0.0f}, 1.0f, leafRate, seconds).length() == 0.0f);
            const float height = windSwayOffset(branch, 20.0f, branch, treeSway(), 1.0f, spring, 1.0f, leafRate, seconds).y;
            highest = std::max(highest, height);
            lowest = std::min(lowest, height);
        }

        const float most = 2.0f * static_cast<float>(WIND_BRANCH_SHARE) * lean * 6.0f;
        assert(highest - lowest > 0.1f * most && highest - lowest <= most * 1.0001f);
    }

    // Leaves flutter within their flutter times the lean (each of three directions), changing from frame to frame, more
    // towards the top, none without wind.
    void testTheFlutter() {
        const Vec2 spring{0.08f, 0.0f};
        const float lean = windSoftLean(spring).length();
        const Vec3 leaf{0.0f, 20.0f, 0.0f};
        const Vec3 low{0.0f, 5.0f, 0.0f};
        const Vec3 bent = windSwayOffset(leaf, 20.0f, leaf, treeSway(), 1.0f, spring, 1.0f, leafRate, 1.0f);
        float largest = 0.0f;
        float largestLow = 0.0f;

        for (int frame = 0; frame < 120; ++frame) {
            const float seconds = 1.0f + 0.013f * static_cast<float>(frame);
            const Vec3 shiver = windSwayOffset(leaf, 20.0f, leaf, treeSway(1.0f, 0.5f), 1.0f, spring, 1.0f, leafRate, seconds) - bent;
            assert(std::abs(shiver.x) <= 0.5f * lean * 1.0001f && std::abs(shiver.y) <= 0.5f * lean * 1.0001f && std::abs(shiver.z) <= 0.5f * lean * 1.0001f);
            largest = std::max(largest, shiver.length());
            const Vec3 lowBent = windSwayOffset(low, 5.0f, low, treeSway(), 1.0f, spring, 1.0f, leafRate, seconds);
            largestLow = std::max(largestLow, (windSwayOffset(low, 5.0f, low, treeSway(1.0f, 0.5f), 1.0f, spring, 1.0f, leafRate, seconds) - lowBent).length());
        }

        assert(largest > 0.1f * lean && largestLow < largest);
    }

    /*
    * How far leaves flutter (windFlutterAmount, shared by the sway and the bones' cards): flutter times the lean, growing
    * with the height share from nothing at the foot (or below it) to all of it at the top and above.
    */
    void testTheFlutterAmount() {
        const Vec2 lean{0.3f, 0.4f};
        assert(windFlutterAmount(0.2f, lean, 0.0f, 10.0f) == 0.0f && windFlutterAmount(0.2f, lean, -3.0f, 10.0f) == 0.0f);
        assert(near(windFlutterAmount(0.2f, lean, 5.0f, 10.0f), 0.05f, 1e-6f));
        assert(near(windFlutterAmount(0.2f, lean, 20.0f, 10.0f), 0.1f, 1e-6f));
        assert(windFlutterAmount(0.0f, lean, 20.0f, 10.0f) == 0.0f && windFlutterAmount(0.2f, Vec2{}, 20.0f, 10.0f) == 0.0f);
    }

    /*
    * How far leaves can flutter (windFlutterReach, the culling's pad for posed trees, ROADMAP phase 21 stage 3c-iv-b):
    * the flutter of any flutter (either sign), lean, height, place, rate and time stays within it, and comes near it.
    */
    void testTheFlutterReach() {
        std::mt19937 random{41u};
        std::uniform_real_distribution<float> unit{-1.0f, 1.0f};
        float closest = 0.0f;

        for (int trial = 0; trial < 20000; ++trial) {
            const float flutter = 0.5f * unit(random);
            const Vec2 lean{0.5f * unit(random), 0.5f * unit(random)};
            const float height = 20.0f + 10.0f * unit(random);
            const float modelHeight = 1.5f * height * (unit(random) + 0.5f);
            const Vec3 place{100.0f * unit(random), 30.0f * unit(random), 100.0f * unit(random)};
            const Vec2 rate{3.0f + 2.5f * unit(random), 10.0f + 9.0f * unit(random)};
            const float amount = windFlutterAmount(flutter, lean, modelHeight, height);
            const float reach = windFlutterReach(flutter, lean);
            const float moved = windFlutterOffset(place, amount, rate, 50.0f * (unit(random) + 1.0f)).length();
            assert(moved <= reach * (1.0f + 1e-5f) + 1e-7f);
            closest = std::max(closest, reach > 1e-3f ? moved / reach : 0.0f);
        }

        assert(closest > 0.8f);
        assert(windFlutterReach(0.0f, Vec2{0.3f, 0.4f}) == 0.0f && near(windFlutterReach(-0.2f, Vec2{0.3f, 0.4f}), 0.1f * std::sqrt(3.0f), 1e-6f));
    }

    // The farthest windSwayOffset moves a vertex in a storm from any side, over ten seconds.
    float largestSway(const Vec3& fromFoot, float modelHeight, const Vec4& sway, float scale) {
        float largest = 0.0f;

        for (int side = 0; side < 16; ++side) {
            const float angle = 0.3927f * static_cast<float>(side);
            const Vec2 spring{30.0f * std::cos(angle), 30.0f * std::sin(angle)};

            for (int step = 0; step < 200; ++step) {
                const float seconds = 0.05f * static_cast<float>(step);
                largest = std::max(largest, windSwayOffset(fromFoot, modelHeight, fromFoot, sway, scale, spring, 1.0f, leafRate, seconds).length());
            }
        }

        return largest;
    }

    /*
    * How far a part of a swaying thing can move (windSwayReach, the culling's pad for clusters swaying unbound, ROADMAP
    * phase 21 4c-vi): a storm stays within it where each of its parts rules, so none can go: a tall plant scaled up
    * (the bend, in metres), a short one fluttering hard (the flutter), a low branch far from the trunk (the bob). Nothing
    * for a still sway or a part at or below its foot.
    */
    void testTheSwayReach() {
        const Vec4 plant{static_cast<float>(WIND_CATEGORY_PLANTS), 3.0f, 1.0f, 0.0f};
        const float tall = largestSway(Vec3{0.0f, 9.0f, 0.0f}, 3.0f, plant, 3.0f);
        assert(tall <= windSwayReach(plant, 3.0f, 3.0f, 0.0f) && tall > 3.5f);

        const Vec4 fluttering{static_cast<float>(WIND_CATEGORY_PLANTS), 0.5f, 1.0f, 5.0f};
        const float shiver = largestSway(Vec3{0.0f, 0.5f, 0.0f}, 0.5f, fluttering, 1.0f);
        assert(shiver <= windSwayReach(fluttering, 1.0f, 0.5f, 0.0f) && shiver > 1.0f);

        const float branch = largestSway(Vec3{6.0f, 0.3f, 0.0f}, 0.3f, plant, 1.0f);
        assert(branch <= windSwayReach(plant, 1.0f, 0.3f, 6.0f) && branch > 0.045f);

        assert(windSwayReach(packSway(WindSway{}), 1.0f, 3.0f, 6.0f) == 0.0f);
        assert(windSwayReach(plant, 1.0f, 0.0f, 6.0f) == 0.0f && windSwayReach(plant, 1.0f, -1.0f, 6.0f) == 0.0f);
    }

    /*
    * The flutter on its own: nothing for no amount, each direction within it; twice as fast at twice the hertz (the same
    * shiver at half the time) and the same shiver twice as far along at twice the metres. The sway's flutter is it, at
    * the top's full amount, with the rate given.
    */
    void testTheFlutterRate() {
        const Vec3 place{12.0f, 3.0f, -7.0f};
        assert(windFlutterOffset(place, 0.0f, leafRate, 5.0f).length() == 0.0f);

        for (int frame = 0; frame < 120; ++frame) {
            const float seconds = 2.0f + 0.017f * static_cast<float>(frame);
            const Vec3 shiver = windFlutterOffset(place, 0.2f, leafRate, seconds);
            assert(std::abs(shiver.x) <= 0.2f * 1.0001f && std::abs(shiver.y) <= 0.2f * 1.0001f && std::abs(shiver.z) <= 0.2f * 1.0001f);
            const Vec3 faster = windFlutterOffset(place, 0.2f, Vec2{2.0f * leafRate.x, leafRate.y}, 0.5f * seconds);
            assert((faster - shiver).length() < 1e-4f);
            const Vec3 longer = windFlutterOffset(place * 2.0f, 0.2f, Vec2{leafRate.x, 2.0f * leafRate.y}, seconds);
            assert((longer - shiver).length() < 1e-4f);
        }

        const Vec2 spring{0.08f, 0.0f};
        const Vec3 leaf{0.0f, 20.0f, 0.0f};
        const Vec2 slow{1.0f, 6.0f};
        const float amount = 0.5f * windSoftLean(spring).length();
        const Vec3 shiver = windSwayOffset(leaf, 20.0f, leaf, treeSway(1.0f, 0.5f), 1.0f, spring, 1.0f, slow, 4.0f) -
                            windSwayOffset(leaf, 20.0f, leaf, treeSway(), 1.0f, spring, 1.0f, slow, 4.0f);
        assert((shiver - windFlutterOffset(leaf, amount, slow, 4.0f)).length() < 1e-6f);
    }

    /*
    * The trees' leaves and the lower layers (the plants, and the grass with the floor's cover) flutter slowly and in
    * step over a crown's patch, a frond or a tuft: two leaves 30 cm apart shiver nearly alike at their rate, while at
    * the hanging things' fine rate (the old one: 3 Hz, half a metre) they part. The trees went to 1 Hz over 8 m when
    * their leaves, darting in and out of the sun flecks each on its own, sparkled (the user, Nov 18 2026).
    */
    void testTheLeavesFlutterInStep() {
        const WindSettings settings;
        const Vec2 hanging = windFlutterRate(settings, WIND_CATEGORY_HANGING);
        assert(hanging.x == 3.0f && hanging.y == 0.5f);
        assert(windFlutterRate(settings, WIND_CATEGORY_TREES).x == 1.0f && windFlutterRate(settings, WIND_CATEGORY_TREES).y == 8.0f);

        for (const int category : {WIND_CATEGORY_TREES, WIND_CATEGORY_PLANTS, WIND_CATEGORY_GRASS}) {
            const Vec2 rate = windFlutterRate(settings, category);
            assert(rate.x < 0.6f * hanging.x && rate.y >= 8.0f * hanging.y);
            const Vec3 one{4.0f, 0.5f, 9.0f};
            const Vec3 other = one + Vec3{0.3f, 0.0f, 0.0f};
            float apart = 0.0f;
            float apartAtFineRate = 0.0f;

            for (int frame = 0; frame < 300; ++frame) {
                const float seconds = 0.01f * static_cast<float>(frame);
                apart = std::max(apart, (windFlutterOffset(one, 1.0f, rate, seconds) - windFlutterOffset(other, 1.0f, rate, seconds)).length());
                apartAtFineRate =
                    std::max(apartAtFineRate, (windFlutterOffset(one, 1.0f, hanging, seconds) - windFlutterOffset(other, 1.0f, hanging, seconds)).length());
            }

            assert(apart < 0.4f && apartAtFineRate > 1.0f);
        }
    }

    // Which draws sway: a still one never; a near one with the sway on; none past the fade's end or with it off.
    void testWhichDrawsSway() {
        const WindSettings settings;
        const WindSway tree{.kind = SwayKind::Tree, .height = 30.0f};
        const Vec3 camera{0.0f, 2.0f, 0.0f};
        assert(!drawSways(WindSway{}, Vec3{5.0f, 0.0f, 0.0f}, 10.0f, camera, settings));
        assert(drawSways(tree, Vec3{5.0f, 0.0f, 0.0f}, 10.0f, camera, settings));
        assert(drawSways(tree, Vec3{settings.swayFadeEnd + 5.0f, 2.0f, 0.0f}, 10.0f, camera, settings));
        assert(!drawSways(tree, Vec3{settings.swayFadeEnd + 15.0f, 2.0f, 0.0f}, 10.0f, camera, settings));
        WindSettings off = settings;
        off.sway = false;
        assert(!drawSways(tree, Vec3{5.0f, 0.0f, 0.0f}, 10.0f, camera, off));
        WindSettings noField = settings;
        noField.enabled = false;
        assert(!drawSways(tree, Vec3{5.0f, 0.0f, 0.0f}, 10.0f, camera, noField));
    }

    // Which shadow tiles sway: the sun's nearest cascades as many as asked; another light's while its reach comes within
    // the sway's fade end of the camera (or its reach is unknown), unless the setting keeps lamps still; and a swaying
    // tile's fingerprint changes every frame something sways in it.
    void testTheShadows() {
        WindSettings settings;
        const Vec3 camera{0.0f, 2.0f, 0.0f};
        const Vec3 nowhere{0.0f, 0.0f, 0.0f};
        assert(tileSways(0, nowhere, -1.0f, camera, settings) && !tileSways(1, nowhere, -1.0f, camera, settings));
        settings.swayShadowCascades = 2;
        assert(tileSways(1, nowhere, -1.0f, camera, settings) && !tileSways(2, nowhere, -1.0f, camera, settings));
        settings.swayShadowCascades = 0;
        assert(!tileSways(0, nowhere, -1.0f, camera, settings));
        settings.swayShadowCascades = 1;

        // A lamp's tile: the flashlight at the camera, a lamp whose reach just touches the fade's end, one just past it.
        assert(tileSways(-1, camera, 30.0f, camera, settings));
        assert(tileSways(-1, Vec3{settings.swayFadeEnd + 25.0f, 2.0f, 0.0f}, 30.0f, camera, settings));
        assert(!tileSways(-1, Vec3{settings.swayFadeEnd + 35.0f, 2.0f, 0.0f}, 30.0f, camera, settings));
        assert(tileSways(-1, Vec3{settings.swayFadeEnd * 10.0f, 2.0f, 0.0f}, -1.0f, camera, settings));
        WindSettings stillLamps = settings;
        stillLamps.swayLocalShadows = false;
        assert(!tileSways(-1, camera, 30.0f, camera, stillLamps) && tileSways(0, nowhere, -1.0f, camera, stillLamps));

        settings.sway = false;
        assert(!tileSways(0, nowhere, -1.0f, camera, settings) && !tileSways(-1, camera, 30.0f, camera, settings));
        settings.sway = true;
        settings.enabled = false;
        assert(!tileSways(-1, camera, 30.0f, camera, settings));

        const Vec4 plane{0.0f, 1.0f, 0.0f, 0.0f};
        assert(packShadowView(Mat4::identity(), plane, plane, true).options[0] == 1.0f && packShadowView(Mat4::identity(), plane, plane).options[0] == 0.0f);

        assert(swayingShadowSignature(1234u, false, 7u) == 1234u && swayingShadowSignature(1234u, false, 8u) == 1234u);
        assert(swayingShadowSignature(1234u, true, 7u) != 1234u);
        assert(swayingShadowSignature(1234u, true, 7u) != swayingShadowSignature(1234u, true, 8u));
    }

    // The record carries the sway: on with the field and the setting, its fade, and last frame's time.
    void testTheRecordsSway() {
        WindSettings settings;
        Wind wind;
        const std::array<WindWindow, WIND_CASCADES> windows{};
        const GpuWindField field = packWindField(&wind, settings, windows, windows, true, 12.5f, 0.25f);
        assert(field.sway[0] == 1.0f && field.sway[1] == settings.swayFadeStart && field.sway[2] == settings.swayFadeEnd && field.sway[3] == 12.25f);
        WindSettings still = settings;
        still.sway = false;
        assert(packWindField(&wind, still, windows, windows, true, 12.5f, 0.25f).sway[0] == 0.0f);
        assert(packWindField(nullptr, settings, windows, windows, true, 12.5f, 0.25f).sway[0] == 0.0f);
    }

    // The record carries each category's flutter rate (hertz, metres).
    void testTheRecordsFlutter() {
        WindSettings settings;
        settings.flutters[WIND_CATEGORY_GRASS] = WindFlutter{.hertz = 0.7f, .metres = 9.0f};
        Wind wind;
        const std::array<WindWindow, WIND_CASCADES> windows{};
        const GpuWindField field = packWindField(&wind, settings, windows, windows, true, 12.5f, 0.25f);
        assert(field.flutters[WIND_CATEGORY_GRASS][0] == 0.7f && field.flutters[WIND_CATEGORY_GRASS][1] == 9.0f);
        assert(field.flutters[WIND_CATEGORY_TREES][0] == settings.flutters[WIND_CATEGORY_TREES].hertz);
        assert(windFlutterRate(settings, WIND_CATEGORY_GRASS).x == 0.7f && windFlutterRate(settings, WIND_CATEGORY_GRASS).y == 9.0f);
    }

    // The settings: the fade must end after it starts, no negative cascades, a flutter's hertz and metres above 0.
    void testTheSwaySettings() {
        WindSettings settings;
        assert(!throwsInvalid(settings));
        WindSettings still = settings;
        still.flutters[WIND_CATEGORY_PLANTS].hertz = 0.0f;
        assert(throwsInvalid(still));
        WindSettings everywhere = settings;
        everywhere.flutters[WIND_CATEGORY_GRASS].metres = 0.0f;
        assert(throwsInvalid(everywhere));
        WindSettings backwards = settings;
        backwards.swayFadeEnd = backwards.swayFadeStart;
        assert(throwsInvalid(backwards));
        WindSettings negative = settings;
        negative.swayShadowCascades = -1;
        assert(throwsInvalid(negative));
    }

    // A renderer's sway reaches its draws and their instances.
    void testTheSwayReachesTheGpu() {
        World world;
        const Entity entity = world.create();
        world.add(entity, Transform{});
        ModelRenderer renderer = makeMeshRenderer(MeshHandle{3});
        renderer.sway = WindSway{.kind = SwayKind::Plant, .height = 2.0f, .bend = 0.4f, .flutter = 0.2f};
        world.add(entity, renderer);
        const Camera camera{Vec3{0.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.0f, 0.1f, 100.0f};
        const FrameDescription frame = buildFrame(world, camera, 0.0f);
        assert(frame.draws.size() == 1 && frame.draws[0].sway.kind == SwayKind::Plant && frame.draws[0].sway.bend == 0.4f);
        const GpuInstance instance = packInstance(frame.draws[0], 0);
        const Vec4 packed = packSway(frame.draws[0].sway);
        assert(instance.sway[0] == packed.x && instance.sway[1] == packed.y && instance.sway[2] == packed.z && instance.sway[3] == packed.w);
    }

    /*
    * The forest: its trees sway as trees, everything on one (bark, leaves, shadows, climbers, epiphytes) with the same
    * height, so it bends as one; the understorey and palms as plants; the grass and floor cover as grass, each vertex
    * from the ground below; impostor cards and lianas still. Its settings apply live, the changed draws announced.
    */
    void testTheForestSways() {
        TestForest forest;
        const Camera camera{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.0f, 0.1f, 10.0f};
        const FrameDescription frame = buildFrame(forest.world, camera, 0.0f);
        for (const DrawItem& draw : frame.draws) {
            if (draw.material.impostorFrames > 0) {
                assert(draw.sway.kind == SwayKind::Still);
            }
        }

        // The trees are the placed trees' looks (scene/planting_cells.h): every model but the card by one height.
        std::size_t trees = 0;

        for (const PlantedKindLook& look : forest.game.getTreeLooks()) {
            for (std::size_t model = 0; model < look.models.size(); ++model) {
                const WindSway& sway = look.models[model].sway;
                assert(model == 2 ? sway.kind == SwayKind::Still : sway.kind == SwayKind::Tree && sway.height == look.models[0].sway.height);
                trees += sway.kind == SwayKind::Tree ? 1 : 0;
            }
        }

        assert(trees > 0);
        const ForestSwaySettings defaults;

        // The ground cover's patches are looks: the grass's and the floor's sway as grass.
        assert(!forest.game.getCoverLooks().empty());

        for (const PlantedKindLook& look : forest.game.getCoverLooks()) {
            const WindSway& sway = look.models.front().sway;
            assert(sway.kind == SwayKind::Grass && sway.height > 0.0f);
            assert(look.layer != forestGrassPlanting || sway.bend == defaults.grass.bend);
        }

        for (const Entity entity : forest.game.getLianas()) {
            assert(forest.world.get<ModelRenderer>(entity).sway.kind == SwayKind::Hanging);
        }

        for (const ModelRenderer& climber : forest.game.getClimberModels()) {
            assert(climber.sway.kind == SwayKind::Tree);
        }

        for (const ModelRenderer& epiphyte : forest.game.getEpiphyteModels()) {
            assert(epiphyte.sway.kind == SwayKind::Tree);
        }

        // The understorey and palms are looks too: seen mesh and stand-in sway as plants, the card holds still.
        assert(!forest.game.getPlantLooks().empty());

        for (const PlantedKindLook& look : forest.game.getPlantLooks()) {
            assert(look.models[0].sway.kind == SwayKind::Plant && look.models[1].sway.kind == SwayKind::Plant && look.models[2].sway.kind == SwayKind::Still);
        }

        Input input;
        forest.game.getSwaySettings().grass.bend = 0.9f;
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);

        // Given anew in the looks the GPU's cells draw.
        for (const PlantedKindLook& look : forest.world.get<PlantingSource>(forest.game.getAir()).looks->kinds) {
            assert(look.layer != forestGrassPlanting || look.models.front().sway.bend == 0.9f);
        }

        FieldCounter counter;
        visit(forest.game.getSwaySettings(), counter);
        assert(counter.fields == 10 && counter.live == 10);
    }
}

void testWindSway() {
    testThePackedSway();
    testTheFade();
    testTheSoftLean();
    testTheWindowShare();
    testThePhase();
    testTheBend();
    testTheBranches();
    testTheFlutter();
    testTheFlutterAmount();
    testTheFlutterReach();
    testTheSwayReach();
    testTheFlutterRate();
    testTheLeavesFlutterInStep();
    testWhichDrawsSway();
    testTheShadows();
    testTheRecordsSway();
    testTheRecordsFlutter();
    testTheSwaySettings();
    testTheSwayReachesTheGpu();
    testTheForestSways();
}
