#include "cloth/cloth.h"
#include "cloth/cloth_pattern.h"
#include "cloth_helpers.h"
#include "games/clothlab/cloth_lab_dress.h"
#include "test_levels.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
    constexpr float tick = 1.0f / 120.0f;

    std::uint32_t edgeIndex(DressEdge edge) {
        return static_cast<std::uint32_t>(edge);
    }

    const PatternEdge& edgeOf(const Pattern& pattern, DressPiece piece, DressEdge edge) {
        return pattern.pieces[static_cast<std::uint32_t>(piece)].edges[edgeIndex(edge)];
    }

    // An edge mirrored across the middle line (x to -x) and run the other way: the edge on the outline's other side.
    std::vector<Vec2> mirrored(const PatternEdge& edge) {
        std::vector<Vec2> points;
        for (auto point = edge.points.rbegin(); point != edge.points.rend(); ++point) {
            points.push_back(Vec2{-point->x, point->y});
        }
        return points;
    }

    bool samePoints(const std::vector<Vec2>& a, const std::vector<Vec2>& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t index = 0; index < a.size(); ++index) {
            if (!near(a[index].x, b[index].x, 1e-6f) || !near(a[index].y, b[index].y, 1e-6f)) {
                return false;
            }
        }
        return true;
    }

    float lowestPoint(const PatternEdge& edge) {
        float lowest = edge.points.front().y;
        for (const Vec2& point : edge.points) {
            lowest = std::min(lowest, point.y);
        }
        return lowest;
    }

    /*
    * The pattern: a front and a back of eight edges each, both outlines symmetric about their middle line, alike but
    * for the neckline (the front's deeper), sewn at the two sides and the two shoulders with each seam's edges the same
    * length; and wide enough all the way down for the mannequin, its chest about 94 cm round, so each panel's half is
    * at least a quarter of that, widening from the armpit to the hem (an A-line).
    */
    void testThePattern() {
        const Pattern pattern = twoPanelDress();
        assert(pattern.pieces.size() == 2 && pattern.seams.size() == 4);
        for (const PatternPiece& piece : pattern.pieces) {
            assert(piece.edges.size() == dressEdgeCount);
            assert(samePoints(mirrored(piece.edges[edgeIndex(DressEdge::RightSide)]), piece.edges[edgeIndex(DressEdge::LeftSide)].points));
            assert(samePoints(mirrored(piece.edges[edgeIndex(DressEdge::RightArmhole)]), piece.edges[edgeIndex(DressEdge::LeftArmhole)].points));
            assert(samePoints(mirrored(piece.edges[edgeIndex(DressEdge::RightShoulder)]), piece.edges[edgeIndex(DressEdge::LeftShoulder)].points));
            assert(samePoints(mirrored(piece.edges[edgeIndex(DressEdge::Neckline)]), piece.edges[edgeIndex(DressEdge::Neckline)].points));
            assert(samePoints(mirrored(piece.edges[edgeIndex(DressEdge::Hem)]), piece.edges[edgeIndex(DressEdge::Hem)].points));
        }
        for (const DressEdge edge : {DressEdge::Hem, DressEdge::RightSide, DressEdge::RightArmhole, DressEdge::RightShoulder}) {
            assert(samePoints(edgeOf(pattern, DressPiece::Front, edge).points, edgeOf(pattern, DressPiece::Back, edge).points));
        }
        assert(lowestPoint(edgeOf(pattern, DressPiece::Front, DressEdge::Neckline)) < lowestPoint(edgeOf(pattern, DressPiece::Back, DressEdge::Neckline)));

        for (const PatternSeam& seam : pattern.seams) {
            const float first = edgeLength(pattern.pieces[seam.firstPiece].edges[seam.firstEdge]);
            const float second = edgeLength(pattern.pieces[seam.secondPiece].edges[seam.secondEdge]);
            assert(std::abs(first - second) < 0.001f);
            assert(seam.firstPiece != seam.secondPiece);
        }

        const std::vector<Vec2>& side = edgeOf(pattern, DressPiece::Front, DressEdge::RightSide).points;
        for (std::size_t index = 0; index + 1 < side.size(); ++index) {
            assert(side[index].x >= 0.235f && side[index + 1].y > side[index].y && side[index + 1].x <= side[index].x);
        }
    }

    /*
    * Cut at a centimetre: about ten thousand points, no triangle sharper than 25 degrees round the curved armholes
    * and neckline either, and a stitch for every point of each sewn edge.
    */
    void testTheDressMeshes() {
        const PatternMesh meshed = meshPattern(twoPanelDress(), dressSpacing);
        assert(meshed.mesh.vertices.size() > 8000 && meshed.mesh.vertices.size() < 14000);
        assert(smallestAngleDegrees(meshed.mesh) > 25.0);

        std::size_t stitches = 0;
        for (const std::vector<ClothStitch>& seam : meshed.seams) {
            stitches += seam.size();
        }
        // Two side seams of about 63 cm and two shoulders of 7.6 cm: 2 x 64 + 2 x 9 stitches.
        assert(stitches == clothSewing(meshed).stitches.size() && stitches == 146);
    }

    /*
    * Laid round the mannequin before sewing: every point on the wrapping cylinder round its middle, the front in front
    * and the back behind, the shoulder straps above its shoulders.
    */
    void testTheDressStartsRoundTheMannequin() {
        const Vec3 feet{-7.5f, 0.0f, -1.0f};
        const PatternMesh meshed = meshPattern(twoPanelDress(), 0.03f);
        const std::vector<Vec3> start = dressAroundMannequin(meshed, feet);
        assert(start.size() == meshed.mesh.vertices.size());

        const Vec3 axis = feet + dressAxisOffset;
        float highest = 0.0f;
        for (std::size_t index = 0; index < start.size(); ++index) {
            const Vec3 out{start[index].x - axis.x, 0.0f, start[index].z - axis.z};
            assert(near(out.length(), dressWrapRadius, 1e-4f));
            assert(meshed.pieces[index] == static_cast<std::uint32_t>(DressPiece::Front) ? out.z > 0.0f : out.z < 0.0f);
            highest = std::max(highest, start[index].y);
        }
        assert(highest > feet.y + dressShoulderHeight + 0.05f && highest < feet.y + dressShoulderHeight + 0.15f);
    }

    /*
    * Sewn in the air with nothing in the way and no weight: every seam shuts within the sewing time, and stays shut,
    * the dress one closed garment.
    */
    void testTheDressSewsShutInTheAir() {
        const PatternMesh meshed = meshPattern(twoPanelDress(), 0.03f);
        ClothSettings settings;
        settings.gravity = 0.0f;
        Cloth dress = makeCloth(meshed.mesh, dressAroundMannequin(meshed, Vec3{}), settings, clothSewing(meshed));

        const int ticks = static_cast<int>(dressSewingSeconds / tick);
        for (int step = 0; step < ticks; ++step) {
            stepCloth(dress, tick);
        }
        for (const ClothStitch& stitch : dress.stitches) {
            const Vec3& first = dress.particles.positions[stitch.first];
            const Vec3& second = dress.particles.positions[stitch.second];
            assert(first.x == second.x && first.y == second.y && first.z == second.z);
        }
    }
}

namespace {
    // The shoulder seams are the ones the strap pins hold: each a front shoulder sewn to a back one.
    void testTheShoulderSeams() {
        const Pattern pattern = twoPanelDress();
        for (const std::uint32_t seam : dressShoulderSeams) {
            const PatternSeam& shoulder = pattern.seams.at(seam);
            assert(shoulder.firstEdge == edgeIndex(DressEdge::RightShoulder) || shoulder.firstEdge == edgeIndex(DressEdge::LeftShoulder));
            assert(shoulder.secondEdge == edgeIndex(DressEdge::RightShoulder) || shoulder.secondEdge == edgeIndex(DressEdge::LeftShoulder));
        }
    }

    /*
    * A strap pin's way onto its shoulder: from its start across to the meeting point over the shoulder, then down
    * onto it, each leg eased in and out (no jolt where one meets the next), and there it stays.
    */
    void testAStrapPinsWay() {
        const StrapPinPath path{Vec3{0.0f, 1.5f, 0.3f}, Vec3{0.0f, 1.5f, 0.0f}, Vec3{0.0f, 1.4f, 0.0f}};
        const auto at = [&](float seconds) { return strapPinPosition(path, seconds); };

        assert(at(0.0f).z == 0.3f && at(0.0f).y == 1.5f);
        assert(near(at(0.5f * dressStrapsMeetSeconds).z, 0.15f, 1e-5f));
        assert(near(at(dressStrapsMeetSeconds).z, 0.0f, 1e-6f) && near(at(dressStrapsMeetSeconds).y, 1.5f, 1e-6f));
        assert(near(at(0.5f * (dressStrapsMeetSeconds + dressSewingSeconds)).y, 1.45f, 1e-5f));
        assert(near(at(dressSewingSeconds).y, 1.4f, 1e-6f));
        assert(at(dressSewingSeconds + 3.0f).y == at(dressSewingSeconds).y);

        // Eased: a moment after each leg starts or before it ends, it has hardly moved.
        const float moment = 0.001f;
        assert(std::abs(at(moment).z - 0.3f) < 1e-4f);
        assert(std::abs(at(dressStrapsMeetSeconds - moment).z) < 1e-4f);
        assert(std::abs(at(dressStrapsMeetSeconds + moment).y - 1.5f) < 1e-4f);
        assert(std::abs(at(dressSewingSeconds - moment).y - 1.4f) < 1e-4f);
    }
}

void testClothLabDress() {
    testThePattern();
    testTheShoulderSeams();
    testAStrapPinsWay();
    testTheDressMeshes();
    testTheDressStartsRoundTheMannequin();
    testTheDressSewsShutInTheAir();
}
