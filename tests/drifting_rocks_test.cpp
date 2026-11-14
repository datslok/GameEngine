#include "games/forest/drifting_rocks.h"
#include "games/forest/forest_game.h"
#include "games/forest/valley_shapes.h"
#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "math/angles.h"
#include "scene/interpolation.h"
#include "scene/model_renderer.h"

#include "forest_helpers.h"
#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <functional>
#include <vector>

namespace {
    DriftingRock testRock() {
        return DriftingRock{.rest = Vec3{10.0f, 400.0f, -30.0f}, .size = 6.0f, .bobMetres = 3.0f, .bobSeconds = 40.0f, .phase = 0.7f,
                            .turnRadians = 1.1f, .turnSpeed = 0.02f};
    }

    /*
    * A drifting rock's pose at a time: risen bobMetres x sin(2 pi t / bobSeconds + phase) above its rest, straight up
    * and down, turned about the vertical by turnRadians + turnSpeed t; the same a whole bob later, apart from the turn.
    */
    void testARockBobsAndTurns() {
        const DriftingRock rock = testRock();
        const Transform start = driftingRockPose(rock, 0.0);
        assert(near(start.position.y, 400.0f + 3.0f * std::sin(0.7f), 1e-4f));
        assert(start.position.x == 10.0f && start.position.z == -30.0f);

        for (double seconds : {0.0, 5.0, 13.7, 40.0, 1000.0}) {
            const Transform pose = driftingRockPose(rock, seconds);
            assert(pose.position.x == 10.0f && pose.position.z == -30.0f);
            assert(pose.position.y >= 397.0f - 1e-4f && pose.position.y <= 403.0f + 1e-4f);
            assert(pose.scale.x == 1.0f && pose.scale.y == 1.0f);

            // Turned about the vertical only: up stays up.
            const Vec3 up = pose.rotation.rotate(Vec3{0.0f, 1.0f, 0.0f});
            assert(near(up.y, 1.0f, 1e-5f));
            const Vec3 east = pose.rotation.rotate(Vec3{1.0f, 0.0f, 0.0f});
            const float expected = wrapAngle(1.1f + 0.02f * static_cast<float>(seconds));
            assert(near(wrapAngle(std::atan2(-east.z, east.x)), expected, 1e-3f));
        }

        assert(near(driftingRockPose(rock, 40.0).position.y, start.position.y, 1e-3f));
        assert(near(driftingRockPose(rock, 10.0).position.y, 400.0f + 3.0f * std::sin(0.7f + 0.25f * fullTurnRadians), 1e-3f));
    }

    // The rocks of one shape stay apart however they bob: their rests further apart than their sizes and both bobs.
    void assertApart(const std::vector<DriftingRock>& rocks) {
        for (std::size_t a = 0; a < rocks.size(); ++a) {
            for (std::size_t b = a + 1; b < rocks.size(); ++b) {
                const float gap = (rocks[a].rest - rocks[b].rest).length();
                assert(gap >= rocks[a].size + rocks[b].size + rocks[a].bobMetres + rocks[b].bobMetres);
            }
        }
    }

    /*
    * Round an island: islandRocksPerTenMetres for each 10 m of radius, in a loose ring clear of its jagged rim (out past
    * 1.3 radii), from just under the rim down to 0.6 radii below it, apart, 10-100% of the largest; the same island
    * always gives the same rocks.
    */
    void testRocksFloatRoundAnIsland() {
        const IslandPlacement island{.rim = Vec3{100.0f, 500.0f, -200.0f}, .radius = 80.0f, .turnRadians = 0.4f, .seed = 21u};
        const ForestDriftSettings settings;
        const std::vector<DriftingRock> rocks = islandRocks(island, settings);
        assert(rocks.size() == static_cast<std::size_t>(std::lround(settings.islandRocksPerTenMetres * 8.0f)));
        assertApart(rocks);

        for (const DriftingRock& rock : rocks) {
            const float out = Vec2{rock.rest.x - island.rim.x, rock.rest.z - island.rim.z}.length();
            assert(out - rock.size >= 1.3f * island.radius);
            assert(out <= 2.2f * island.radius + settings.largestRock);
            assert(rock.rest.y <= island.rim.y && rock.rest.y >= island.rim.y - 0.6f * island.radius - 1e-3f);
            assert(rock.size >= 0.1f * settings.largestRock - 1e-4f && rock.size <= settings.largestRock + 1e-4f);
            assert(rock.bobMetres == settings.bobMetres && rock.bobSeconds >= 0.5f * settings.bobSeconds && rock.bobSeconds <= 1.5f * settings.bobSeconds);
        }

        const std::vector<DriftingRock> again = islandRocks(island, settings);
        assert(again.size() == rocks.size() && again.back().rest.x == rocks.back().rest.x && again.back().phase == rocks.back().phase);
    }

    /*
    * Round the pillars: about pillarShare of them have one to three rocks, clear of the rock (out past 1.6 radii, the
    * pillars leaning and bulging), at 45-90% of their height, apart.
    */
    void testSomePillarsHaveRocks() {
        const ForestDriftSettings settings;
        int withRocks = 0;
        const int pillars = 300;

        for (int index = 0; index < pillars; ++index) {
            const PillarPlacement pillar{.foot = Vec3{static_cast<float>(index) * 200.0f, 10.0f, 50.0f}, .radius = 25.0f, .height = 300.0f,
                                         .turnRadians = 0.0f, .seed = static_cast<std::uint32_t>(index * 7 + 3)};
            const std::vector<DriftingRock> rocks = pillarRocks(pillar, settings);
            assert(rocks.size() <= 3);
            withRocks += rocks.empty() ? 0 : 1;
            assertApart(rocks);

            for (const DriftingRock& rock : rocks) {
                const float out = Vec2{rock.rest.x - pillar.foot.x, rock.rest.z - pillar.foot.z}.length();
                assert(out - rock.size >= 1.6f * pillar.radius);
                assert(rock.rest.y >= pillar.foot.y + 0.45f * pillar.height - 1e-3f && rock.rest.y <= pillar.foot.y + 0.9f * pillar.height + 1e-3f);
            }
        }

        const float share = static_cast<float>(withRocks) / static_cast<float>(pillars);
        assert(share > 0.6f * settings.pillarShare && share < 1.4f * settings.pillarShare);
    }

    /*
    * A drifting rock's shape: an island's at its size, uncarved (no beds' ledges and no joints, which at 2.5 m deep
    * would cut a small rock apart), so its rim stays near its size all round.
    */
    void testARockIsASmallIsland() {
        const SurfaceGrid surface = driftingRockSurface(testRock(), 3u);
        assert(!surface.points.empty());
        float nearest = 1e9f;
        float farthest = 0.0f;

        // The underside just below the rim, where the rock is widest (jagged by a quarter either way).
        for (const Vec3& point : surface.points) {
            if (point.y < -0.01f * 6.0f && point.y > -0.1f * 6.0f) {
                const float out = Vec2{point.x, point.z}.length();
                nearest = std::min(nearest, out);
                farthest = std::max(farthest, out);
            }
        }

        assert(farthest > 0.0f && nearest >= 0.6f * 6.0f && farthest <= 1.3f * 6.0f);
        assert(RockCarving{}.jointMetres == rockJointDepth);
    }

    // The system: each rock's Transform set to its pose at the simulation time, or at time 0 while drifting is off.
    void testTheSystemMovesThem() {
        World world;
        const Entity entity = world.create();
        world.add(entity, testRock());
        world.add(entity, Transform{});
        updateDriftingRocks(world, 13.0, true);
        assert(world.get<Transform>(entity).position.y == driftingRockPose(testRock(), 13.0).position.y);
        updateDriftingRocks(world, 13.0, false);
        assert(world.get<Transform>(entity).position.y == driftingRockPose(testRock(), 0.0).position.y);
    }

    // Counts the fields a visit names, and how many apply every frame.
    class FieldCounter : public SettingsVisitor {
    public:
        int fields = 0;
        int live = 0;

    protected:
        void visitFloat(const char*, float&, const FloatLimits& limits) override {
            ++fields;
            live += limits.applies == FieldApplies::EveryFrame ? 1 : 0;
        }

        void visitInt(const char*, int&, const IntLimits& limits) override {
            ++fields;
            live += limits.applies == FieldApplies::EveryFrame ? 1 : 0;
        }

        void visitBool(const char*, bool&, FieldApplies applies) override {
            ++fields;
            live += applies == FieldApplies::EveryFrame ? 1 : 0;
        }

        void visitChoice(const char*, int&, const char* const*, int, FieldApplies applies) override {
            ++fields;
            live += applies == FieldApplies::EveryFrame ? 1 : 0;
        }

        void visitGroup(const char*, const std::function<void()>& contents) override {
            contents();
        }
    };

    // Six settings; only whether they drift is live (to compare the cost of their moving shadows), the rest at start-up.
    void testTheSettings() {
        ForestDriftSettings settings;
        assert(settings.moving);
        FieldCounter counter;
        visit(settings, counter);
        assert(counter.fields == 6 && counter.live == 1);
    }

    /*
    * The game floats them: drawn-only rocks casting shadows, drawn smoothly between ticks (a PreviousTransform), in the
    * rock painted one bed's tones (beds by world height would slide over a moving rock), and a tick moves them.
    */
    void testTheGameFloatsThem() {
        TestForest forest{ForestValleySettings{.groundCoarseness = 2, .pillarCount = 20, .islandCount = 4}};
        const std::vector<Entity>& rocks = forest.game.getDriftingRocks();
        assert(!rocks.empty());

        for (const Entity rock : rocks) {
            assert(forest.world.has<DriftingRock>(rock) && forest.world.has<PreviousTransform>(rock));
            const ModelRenderer& renderer = forest.world.get<ModelRenderer>(rock);
            assert(renderer.castsShadows && forest.assets.isDrawnOnly(renderer.parts.front().mesh));
            const Material& material = renderer.parts.front().material;
            assert(material.groundPaint == GroundPaint::Strata && material.groundProjected && material.groundWeights.isValid());
        }

        const Entity first = rocks.front();
        const float before = forest.world.get<Transform>(first).position.y;
        forest.game.onFixedUpdate(forest.world, 1.0f / 120.0f, 7.5);
        const float after = forest.world.get<Transform>(first).position.y;
        assert(after != before && after == driftingRockPose(forest.world.get<DriftingRock>(first), 7.5).position.y);
    }
}

void testDriftingRocks() {
    testARockBobsAndTurns();
    testRocksFloatRoundAnIsland();
    testSomePillarsHaveRocks();
    testARockIsASmallIsland();
    testTheSystemMovesThem();
    testTheSettings();
    testTheGameFloatsThem();
}
