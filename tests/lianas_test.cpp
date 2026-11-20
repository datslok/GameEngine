#include "games/forest/forest_game.h"
#include "games/forest/lianas.h"
#include "scene/model_renderer.h"

#include "forest_helpers.h"
#include "test_jobs.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    float polylineLength(const std::vector<Vec3>& points) {
        float length = 0.0f;

        for (std::size_t point = 1; point < points.size(); ++point) {
            length += (points[point] - points[point - 1]).length();
        }

        return length;
    }

    /*
    * A liana's stem across by the Barro Colorado census: half 1-2 cm, 42% 2-5 cm, 7% 5-10 cm, 1% 10-20 cm, rising with
    * the share drawn.
    */
    void testDiametersFollowTheCensus() {
        constexpr int draws = 10000;
        int bands[4] = {0, 0, 0, 0};
        float last = 0.0f;

        for (int draw = 0; draw < draws; ++draw) {
            const float diameter = lianaDiameter((static_cast<float>(draw) + 0.5f) / static_cast<float>(draws));
            assert(diameter >= last && diameter >= 0.01f && diameter <= 0.2f);
            last = diameter;
            bands[diameter < 0.02f ? 0 : diameter < 0.05f ? 1 : diameter < 0.1f ? 2 : 3] += 1;
        }

        assert(near(static_cast<float>(bands[0]) / draws, 0.50f, 0.01f) && near(static_cast<float>(bands[1]) / draws, 0.42f, 0.01f));
        assert(near(static_cast<float>(bands[2]) / draws, 0.07f, 0.01f) && near(static_cast<float>(bands[3]) / draws, 0.01f, 0.005f));
    }

    /*
    * A catenary of a given length between two holds: it starts and ends on them, its points at most a step apart and as
    * long as asked; between holds at one height it is lowest midway and never above them; between holds at different
    * heights, ends and length still hold, and it sags below its chord.
    */
    void testTheCatenary() {
        const HangingStrand level = catenaryStrand(Vec3{0.0f, 10.0f, 0.0f}, Vec3{10.0f, 10.0f, 0.0f}, 12.0f, 0.02f);
        assert((level.points.front() - Vec3{0.0f, 10.0f, 0.0f}).length() < 1e-4f && (level.points.back() - Vec3{10.0f, 10.0f, 0.0f}).length() < 1e-3f);
        assert(near(polylineLength(level.points), 12.0f, 0.12f) && level.radii.size() == level.points.size());
        const auto lowest = std::min_element(level.points.begin(), level.points.end(), [](const Vec3& a, const Vec3& b) { return a.y < b.y; });
        // 2a sinh(5 / a) = 12 gives a = 4.697, and the sag a (cosh(5 / a) - 1) = 2.92 m, midway.
        assert(near(lowest->x, 5.0f, 0.3f) && near(10.0f - lowest->y, 2.92f, 0.03f));

        for (std::size_t point = 1; point < level.points.size(); ++point) {
            assert((level.points[point] - level.points[point - 1]).length() <= lianaStepMetres * 1.01f);
            assert(level.points[point].y <= 10.0f + 1e-4f && level.radii[point] == 0.02f);
        }

        const Vec3 high{0.0f, 10.0f, 0.0f};
        const Vec3 low{8.0f, 4.0f, 3.0f};
        const float straight = (low - high).length();
        const HangingStrand sloping = catenaryStrand(high, low, straight * 1.1f, 0.05f);
        assert((sloping.points.front() - high).length() < 1e-4f && (sloping.points.back() - low).length() < 1e-3f);
        assert(near(polylineLength(sloping.points), straight * 1.1f, straight * 0.011f));
        const Vec3& middle = sloping.points[sloping.points.size() / 2];
        assert(middle.y < 0.5f * (high.y + low.y));

        // Nearly taut: still its length, and the points still close.
        const HangingStrand taut = catenaryStrand(high, low, straight * 1.005f, 0.05f);
        assert(near(polylineLength(taut.points), straight * 1.005f, straight * 0.01f) && (taut.points.back() - low).length() < 1e-3f);
    }

    // Crowns in rows 15 m apart along the path (x = 0) and away from it, 25 m up, 8 m round, over flat ground, each its own identity.
    std::vector<LianaCrown> testCrowns() {
        std::vector<LianaCrown> crowns;

        for (int row = 0; row < 6; ++row) {
            for (int column = 0; column < 10; ++column) {
                crowns.push_back(LianaCrown{.centre = Vec3{static_cast<float>(column) * 15.0f - 30.0f, 25.0f, static_cast<float>(row) * 15.0f},
                                            .radius = 8.0f,
                                            .identity = 1000u + static_cast<std::uint64_t>(row * 10 + column)});
            }
        }

        return crowns;
    }

    float flatGround(float, float) {
        return 0.0f;
    }

    float pathAlongZ(float x, float) {
        return std::abs(x);
    }

    /*
    * Which crowns carry lianas and how they hang: the same every run; more often near the path; loops from the lower part
    * of one crown into a neighbour's, longer than straight; ropes from a crown to the ground a little way off, nearly
    * taut; thicknesses from the census; some of each kind.
    */
    void testChoosingLianas() {
        const std::vector<LianaCrown> crowns = testCrowns();
        const ForestLianaSettings settings;
        const std::vector<Liana> lianas = chooseLianas(crowns, settings, 7, flatGround, pathAlongZ);
        const std::vector<Liana> again = chooseLianas(crowns, settings, 7, flatGround, pathAlongZ);
        assert(lianas.size() == again.size() && !lianas.empty());

        std::vector<int> perCrown(crowns.size(), 0);
        int loops = 0;
        int ropes = 0;

        for (std::size_t index = 0; index < lianas.size(); ++index) {
            const Liana& liana = lianas[index];
            assert((liana.from - again[index].from).length() == 0.0f && liana.length == again[index].length);
            const LianaCrown& crown = crowns[static_cast<std::size_t>(liana.crown)];
            assert((liana.from - crown.centre).length() < crown.radius && liana.from.y < crown.centre.y);
            // The census's 1-20 cm across, times the settings' thickness.
            assert(liana.radius >= 0.005f * settings.thickness && liana.radius <= 0.1f * settings.thickness);
            perCrown[static_cast<std::size_t>(liana.crown)] += 1;
            const float straight = (liana.to - liana.from).length();

            if (liana.otherCrown >= 0) {
                const LianaCrown& other = crowns[static_cast<std::size_t>(liana.otherCrown)];
                assert(liana.otherCrown != liana.crown && (liana.to - other.centre).length() < other.radius && liana.to.y < other.centre.y);
                assert(liana.length > straight * 1.01f && liana.length < straight * 1.6f);
                loops += 1;
            } else {
                const float off = Vec3{liana.to.x - liana.from.x, 0.0f, liana.to.z - liana.from.z}.length();
                assert(near(liana.to.y, -lianaGroundSink, 1e-4f) && off >= 1.0f && off <= 6.0f);
                assert(liana.length > straight && liana.length < straight * 1.06f);
                ropes += 1;
            }
        }

        assert(loops > 0 && ropes > 0);

        for (std::size_t crown = 0; crown < crowns.size(); ++crown) {
            assert(perCrown[crown] <= settings.mostPerTree);
        }

        // The path's rows (|x| up to 15 m: 3 of each 10) carry more often than the rest: shares far enough apart (0.95
        // against 0.3) that 60 crowns show it every time, where the defaults' (0.95 against 0.8) are within chance.
        ForestLianaSettings contrast = settings;
        contrast.treeShare = 0.3f;
        int nearCarrying = 0;
        int farCarrying = 0;
        std::vector<bool> carrying(crowns.size(), false);

        for (const Liana& liana : chooseLianas(crowns, contrast, 7, flatGround, pathAlongZ)) {
            carrying[static_cast<std::size_t>(liana.crown)] = true;
        }

        for (std::size_t crown = 0; crown < crowns.size(); ++crown) {
            (std::abs(crowns[crown].centre.x) <= settings.pathReach ? nearCarrying : farCarrying) += carrying[crown] ? 1 : 0;
        }

        assert(static_cast<float>(nearCarrying) / 18.0f > static_cast<float>(farCarrying) / 42.0f);
        assert(farCarrying > 0);
    }

    /*
    * A crown's lianas are its own (ROADMAP phase 18, d2a): chosen among only it and its neighbours within neighbourSpan,
    * the neighbours carrying none, they are exactly those chosen among every crown, their far crowns the same by
    * identity; a crown that does not carry hangs none of its own, yet others loop into it.
    */
    void testACrownsLianasAreItsOwn() {
        const std::vector<LianaCrown> crowns = testCrowns();
        const ForestLianaSettings settings;
        const std::vector<Liana> all = chooseLianas(crowns, settings, 7, flatGround, pathAlongZ);
        int compared = 0;
        int loopedIntoOthers = 0;

        for (std::size_t chosen = 0; chosen < crowns.size(); ++chosen) {
            std::vector<LianaCrown> few;

            for (const LianaCrown& crown : crowns) {
                const Vec3 apart = crown.centre - crowns[chosen].centre;

                if (Vec3{apart.x, 0.0f, apart.z}.length() <= settings.neighbourSpan) {
                    few.push_back(crown);
                    few.back().carries = crown.identity == crowns[chosen].identity;
                }
            }

            const std::vector<Liana> alone = chooseLianas(few, settings, 7, flatGround, pathAlongZ);
            std::vector<Liana> expected;
            std::copy_if(all.begin(), all.end(), std::back_inserter(expected), [&](const Liana& liana) {
                return static_cast<std::size_t>(liana.crown) == chosen;
            });
            assert(alone.size() == expected.size());

            for (std::size_t index = 0; index < alone.size(); ++index) {
                const Liana& mine = alone[index];
                const Liana& theirs = expected[index];
                assert(few[static_cast<std::size_t>(mine.crown)].carries);
                assert((mine.from - theirs.from).length() == 0.0f && (mine.to - theirs.to).length() == 0.0f);
                assert(mine.length == theirs.length && mine.radius == theirs.radius && mine.seed == theirs.seed);
                assert((mine.otherCrown < 0) == (theirs.otherCrown < 0));

                if (mine.otherCrown >= 0) {
                    assert(few[static_cast<std::size_t>(mine.otherCrown)].identity == crowns[static_cast<std::size_t>(theirs.otherCrown)].identity);
                    loopedIntoOthers += 1;
                }

                compared += 1;
            }
        }

        assert(compared == static_cast<int>(all.size()) && loopedIntoOthers > 0);

        // Each liana's own seed for its leaves.
        for (std::size_t index = 1; index < all.size(); ++index) {
            assert(all[index].seed != all[index - 1].seed);
        }
    }

    // An ivy card's vertices (appendLeafCards draws whole squares).
    constexpr std::size_t leafCardVertices = 4;

    // Whether a point is inside any of the crowns.
    bool insideACrown(const Vec3& point, const std::vector<LianaCrown>& crowns) {
        return std::any_of(crowns.begin(), crowns.end(), [&](const LianaCrown& crown) {
            return (point - crown.centre).length() < crown.radius;
        });
    }

    /*
    * Ivy all along a liana, sparse outside the crowns and dense inside them: a loop between two crowns carries leaves on
    * both stretches, and more of them a metre inside.
    */
    void testIvyIsDensestInTheCrowns() {
        const std::vector<LianaCrown> crowns{LianaCrown{Vec3{0.0f, 25.0f, 0.0f}, 8.0f}, LianaCrown{Vec3{30.0f, 25.0f, 0.0f}, 8.0f}};
        const Liana loop{.from = Vec3{2.0f, 20.0f, 0.0f}, .to = Vec3{28.0f, 20.0f, 0.0f}, .length = 32.0f, .radius = 0.03f, .crown = 0, .otherCrown = 1};
        const ForestLianaSettings settings;
        const std::vector<LianaMeshes> chunks = lianaMeshes({loop}, crowns, settings, 2.0f, 1000.0f, testJobs());
        assert(chunks.size() == 1);

        // Each stretch's length along the line, and the leaves whose stems sit on it (a card's first vertex).
        const HangingStrand strand = catenaryStrand(loop.from, loop.to, loop.length, loop.radius);
        float insideLength = 0.0f;
        float outsideLength = 0.0f;

        for (std::size_t point = 1; point < strand.points.size(); ++point) {
            const float step = (strand.points[point] - strand.points[point - 1]).length();
            (insideACrown(strand.points[point], crowns) ? insideLength : outsideLength) += step;
        }

        int insideCards = 0;
        int outsideCards = 0;
        const std::vector<MeshVertex>& leaves = chunks.front().leaves.vertices;

        for (std::size_t vertex = 0; vertex < leaves.size(); vertex += leafCardVertices) {
            (insideACrown(leaves[vertex].position, crowns) ? insideCards : outsideCards) += 1;
        }

        assert(insideLength > 0.0f && outsideLength > 0.0f && outsideCards > 0);
        assert(static_cast<float>(insideCards) / insideLength > 2.0f * static_cast<float>(outsideCards) / outsideLength);
    }

    // The meshes, a chunk of the ground each: bark tubes round every liana, facing out, the same every run.
    void testLianaMeshes() {
        const std::vector<LianaCrown> crowns = testCrowns();
        const ForestLianaSettings settings;
        const std::vector<Liana> lianas = chooseLianas(crowns, settings, 7, flatGround, pathAlongZ);
        const std::vector<LianaMeshes> chunks = lianaMeshes(lianas, crowns, settings, 2.0f, 40.0f, testJobs());
        assert(chunks.size() > 1);
        std::size_t barkVertices = 0;

        for (const LianaMeshes& chunk : chunks) {
            assert(!chunk.bark.vertices.empty() && chunk.bark.indices.size() % 3 == 0 && trianglesFaceTheirNormals(chunk.bark));
            barkVertices += chunk.bark.vertices.size();
        }

        assert(barkVertices > 0);

        // The same every run.
        const std::vector<LianaMeshes> again = lianaMeshes(lianas, crowns, settings, 2.0f, 40.0f, testJobs());
        assert(again.size() == chunks.size() && again.front().bark.vertices.size() == chunks.front().bark.vertices.size());
    }

    /*
    * The forest hangs them from its canopy and emergent trees: still, drawn only, casting shadows into the nearest two
    * cascades; their settings start-up only.
    */
    void testTheForestHangsThem() {
        TestForest forest;
        const std::vector<Entity>& lianas = forest.game.getLianas();
        assert(!lianas.empty());

        for (const Entity entity : lianas) {
            const ModelRenderer& renderer = forest.world.get<ModelRenderer>(entity);
            assert(renderer.isStill() && renderer.shadowCascades == lianaShadowCascades);
            assert(forest.assets.isDrawnOnly(renderer.parts.front().mesh));
        }

        FieldCounter counter;
        visit(forest.game.getLianaSettings(), counter);
        assert(counter.fields == 13 && counter.live == 0);
    }
}

void testLianas() {
    testDiametersFollowTheCensus();
    testTheCatenary();
    testChoosingLianas();
    testACrownsLianasAreItsOwn();
    testLianaMeshes();
    testIvyIsDensestInTheCrowns();
    testTheForestHangsThem();
}
