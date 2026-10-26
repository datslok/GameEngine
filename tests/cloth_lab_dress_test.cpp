#include "cloth/cloth.h"
#include "cloth/cloth_pattern.h"
#include "cloth/cloth_yardstick.h"
#include "cloth_helpers.h"
#include "games/clothlab/cloth_lab_dress.h"
#include "scene/collision_mesh.h"
#include "test_levels.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <set>
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

namespace {
    // The hem's points: every point of both panels' hems, each once, all on the pattern's bottom line.
    void testTheDressHem() {
        const PatternMesh meshed = meshPattern(twoPanelDress(), 0.03f);
        const std::vector<std::uint32_t> hem = dressHemPoints(meshed);
        const std::uint32_t hemEdge = edgeIndex(DressEdge::Hem);
        assert(hem.size() == meshed.edgePoints[0][hemEdge].size() + meshed.edgePoints[1][hemEdge].size());
        assert(std::set<std::uint32_t>(hem.begin(), hem.end()).size() == hem.size());
        for (const std::uint32_t point : hem) {
            assert(meshed.flatPositions[point].y == 0.0f);
        }
    }

    /*
    * A tick of the dress: the fabric's settings, but weightless while it sews (ticks starting before the sewing ends),
    * and its strap pins where their ways have them at the tick's end.
    */
    void testPreparingADressTick() {
        Cloth dress = yardstickSheet(2, ClothSettings{});
        const std::vector<StrapPinPath> straps{StrapPinPath{Vec3{-0.5f, 0.0f, 0.0f}, Vec3{-0.4f, 0.1f, 0.0f}, Vec3{-0.4f, 0.0f, 0.0f}},
                                               StrapPinPath{Vec3{0.5f, 0.0f, 0.0f}, Vec3{0.4f, 0.1f, 0.0f}, Vec3{0.4f, 0.0f, 0.0f}}};
        ClothSettings fabric;
        fabric.substeps = 7;

        prepareDressTick(dress, straps, fabric, 0.0, tick);
        assert(dress.settings.gravity == 0.0f && dress.settings.substeps == 7);
        for (std::size_t index = 0; index < straps.size(); ++index) {
            const Vec3 due = strapPinPosition(straps[index], tick);
            assert(dress.pins[index].position.x == due.x && dress.pins[index].position.y == due.y);
        }

        prepareDressTick(dress, straps, fabric, dressSewingSeconds - 0.5 * tick, tick);
        assert(dress.settings.gravity == 0.0f);
        prepareDressTick(dress, straps, fabric, dressSewingSeconds, tick);
        assert(dress.settings.gravity == fabric.gravity);
        assert(near(dress.pins[1].position.x, 0.4f, 1e-6f) && near(dress.pins[1].position.y, 0.0f, 1e-6f));

        // Held by contact alone, there are no straps to move.
        Cloth unpinned = yardstickSheet(2, ClothSettings{});
        const Vec3 before = unpinned.pins[0].position;
        prepareDressTick(unpinned, {}, fabric, 0.0, tick);
        assert(unpinned.pins[0].position.x == before.x && unpinned.settings.gravity == 0.0f);
    }

    /*
    * The dress made round a body (a box standing in for the mannequin, its shoulders at dressShoulderHeight): one point
    * a vertex, sewn, its hem the pattern's; pinned, both points of every shoulder stitch are its pins, in order with
    * their ways, each way ending on the body's top, and the rest tethered; held by contact alone, no pins and no tethers.
    */
    void testTheDressRoundABody() {
        const Vec3 feet{2.0f, 0.0f, -1.0f};
        const float halfHeight = 0.5f * dressShoulderHeight;
        const SignedDistanceField body = buildSignedDistanceField(
            collisionMesh(Mesh::cube(), Mat4::translation(feet.x, feet.y + halfHeight, feet.z - 0.02f) * Mat4::scaling(0.21f, halfHeight, 0.12f)), 0.01f, 0.03f);
        const PatternMesh meshed = meshPattern(twoPanelDress(), 0.03f);

        const DressOnMannequin pinned = dressOnMannequin(body, feet, 0.03f, ClothSettings{}, true);
        assert(pinned.cloth.particles.size() == meshed.mesh.vertices.size() && pinned.mesh.vertices.size() == meshed.mesh.vertices.size());
        assert(pinned.cloth.stitches.size() == clothSewing(meshed).stitches.size());
        assert(pinned.hem == dressHemPoints(meshed));

        std::size_t shoulderStitches = 0;
        for (const std::uint32_t seam : dressShoulderSeams) {
            shoulderStitches += meshed.seams.at(seam).size();
        }
        assert(pinned.cloth.pins.size() == 2 * shoulderStitches && pinned.strapPaths.size() == pinned.cloth.pins.size());
        assert(!pinned.cloth.tethers.empty());
        for (const StrapPinPath& path : pinned.strapPaths) {
            assert(path.onShoulder.y > feet.y + dressShoulderHeight && path.onShoulder.y < feet.y + dressShoulderHeight + ClothSettings{}.thickness + 0.01f);
        }

        const DressOnMannequin byContact = dressOnMannequin(body, feet, 0.03f, ClothSettings{}, false);
        assert(byContact.cloth.pins.empty() && byContact.strapPaths.empty() && byContact.cloth.tethers.empty());
        assert(byContact.hem == pinned.hem);
    }
}

void testClothLabDress() {
    testThePattern();
    testTheShoulderSeams();
    testAStrapPinsWay();
    testTheDressMeshes();
    testTheDressStartsRoundTheMannequin();
    testTheDressSewsShutInTheAir();
    testTheDressHem();
    testPreparingADressTick();
    testTheDressRoundABody();
}
