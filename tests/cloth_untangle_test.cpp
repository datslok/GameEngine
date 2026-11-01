#include "cloth/cloth.h"
#include "cloth/cloth_self_collision.h"
#include "core/job_system.h"
#include "engine/settings_file.h"
#include "scene/mesh.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace {
    // A flat grid of squares each way, size across, in the plane z = 0, as a cloth keeps it: points, triangles, topology.
    struct Grid {
        int squares = 0;
        std::vector<Vec3> positions;
        std::vector<std::array<std::uint32_t, 3>> triangles;
        ClothSelfTopology topology;

        Grid(int squares, float size): squares(squares) {
            const Mesh mesh = Mesh::grid(squares, squares, size, size);
            for (const Vec4& vertex : mesh.vertices) {
                positions.push_back(Vec3{vertex.x, vertex.y, vertex.z});
            }
            for (const Triangle& triangle : mesh.triangles) {
                triangles.push_back({static_cast<std::uint32_t>(triangle.first), static_cast<std::uint32_t>(triangle.second),
                                     static_cast<std::uint32_t>(triangle.third)});
            }
            topology = clothSelfTopology(positions, triangles, {});
        }

        std::uint32_t at(int row, int column) const {
            return static_cast<std::uint32_t>(row * (squares + 1) + column);
        }

        // The points of the block of rows and columns from first to last (inclusive).
        std::set<std::uint32_t> block(int firstRow, int lastRow, int firstColumn, int lastColumn) const {
            std::set<std::uint32_t> points;
            for (int row = firstRow; row <= lastRow; ++row) {
                for (int column = firstColumn; column <= lastColumn; ++column) {
                    points.insert(at(row, column));
                }
            }
            return points;
        }

        // Every edge with one end in the block and the other out of it: where a closed intersection path round the block
        // would cut the mesh.
        std::vector<std::uint32_t> edgesLeaving(const std::set<std::uint32_t>& points) const {
            std::vector<std::uint32_t> leaving;
            for (std::uint32_t index = 0; index < topology.edges.size(); ++index) {
                if (points.count(topology.edges[index][0]) != points.count(topology.edges[index][1])) {
                    leaving.push_back(index);
                }
            }
            return leaving;
        }

        // A triangle with every corner in the block.
        std::uint32_t triangleIn(const std::set<std::uint32_t>& points) const {
            for (std::uint32_t index = 0; index < triangles.size(); ++index) {
                if (points.count(triangles[index][0]) && points.count(triangles[index][1]) && points.count(triangles[index][2])) {
                    return index;
                }
            }
            assert(false);
            return 0;
        }

        // A point of the block not on the triangle.
        std::uint32_t pointBesides(const std::set<std::uint32_t>& points, std::uint32_t triangle) const {
            for (const std::uint32_t point : points) {
                if (std::find(triangles[triangle].begin(), triangles[triangle].end(), point) == triangles[triangle].end()) {
                    return point;
                }
            }
            assert(false);
            return 0;
        }
    };

    // Each of the edges crossing the triangle.
    std::vector<std::array<std::uint32_t, 2>> crossingsOf(const std::vector<std::uint32_t>& edges, std::uint32_t triangle) {
        std::vector<std::array<std::uint32_t, 2>> crossings;
        for (const std::uint32_t edge : edges) {
            crossings.push_back({edge, triangle});
        }
        return crossings;
    }

    /*
    * A block of the cloth folded through itself (Baraff et al. 2003's figure 7): its edges round it cross a triangle of
    * the block itself. Cut along them, the block is a part of its own, the smaller side of each cut edge (inside), and it
    * has passed through itself (looped). Pairs between its own points and triangles are left free; anything else keeps
    * apart. A block at the cloth's corner, cut off by a path from edge to edge, is found the same way.
    */
    void testAFoldThroughItselfIsLooped() {
        const Grid grid{8, 0.16f};
        for (const auto& [firstRow, lastRow, firstColumn, lastColumn] : {std::array<int, 4>{3, 4, 3, 4}, std::array<int, 4>{0, 1, 0, 2}}) {
            const std::set<std::uint32_t> folded = grid.block(firstRow, lastRow, firstColumn, lastColumn);
            const std::uint32_t inFold = grid.triangleIn(folded);
            const ClothTangles tangles = analyseClothTangles(crossingsOf(grid.edgesLeaving(folded), inFold), grid.triangles, grid.topology);

            const std::uint32_t part = tangles.part[*folded.begin()];
            for (const std::uint32_t point : folded) {
                assert(tangles.part[point] == part);
            }
            assert(tangles.part[grid.at(7, 7)] != part);
            assert(tangles.inside[part] == 1 && tangles.looped[part] == 1);
            assert(tangles.inside[tangles.part[grid.at(7, 7)]] == 0);
            assert(tangles.through.empty());

            const ClothSelfPair own = pointTrianglePair(grid.positions, grid.pointBesides(folded, inFold), grid.triangles[inFold]);
            assert(pairTangle(own, tangles) == ClothPairTangle::Free);
            const ClothSelfPair outside = pointTrianglePair(grid.positions, grid.at(7, 7), grid.triangles[inFold]);
            assert(pairTangle(outside, tangles) == ClothPairTangle::Apart);
        }
    }

    /*
    * Two blocks passed through each other (figure 6): each one's edges round it cross a triangle of the other. They are
    * each other's black and white: pairs between them go back through; pairs with the rest of the cloth keep apart.
    */
    void testTwoBlocksPassedThroughEachOther() {
        const Grid grid{8, 0.16f};
        const std::set<std::uint32_t> first = grid.block(1, 2, 1, 2);
        const std::set<std::uint32_t> second = grid.block(5, 6, 5, 6);
        const std::uint32_t inFirst = grid.triangleIn(first);
        const std::uint32_t inSecond = grid.triangleIn(second);
        std::vector<std::array<std::uint32_t, 2>> crossings = crossingsOf(grid.edgesLeaving(first), inSecond);
        const std::vector<std::array<std::uint32_t, 2>> back = crossingsOf(grid.edgesLeaving(second), inFirst);
        crossings.insert(crossings.end(), back.begin(), back.end());

        const ClothTangles tangles = analyseClothTangles(crossings, grid.triangles, grid.topology);
        const std::uint32_t one = tangles.part[grid.at(1, 1)];
        const std::uint32_t other = tangles.part[grid.at(5, 5)];
        assert(one != other && tangles.inside[one] == 1 && tangles.inside[other] == 1);
        assert(tangles.looped[one] == 0 && tangles.looped[other] == 0);
        assert(tangles.through.size() == 1);
        assert(tangles.through[0] == (std::array<std::uint32_t, 2>{std::min(one, other), std::max(one, other)}));

        assert(pairTangle(pointTrianglePair(grid.positions, grid.at(1, 1), grid.triangles[inSecond]), tangles) == ClothPairTangle::Through);
        assert(pairTangle(pointTrianglePair(grid.positions, grid.at(5, 5), grid.triangles[inFirst]), tangles) == ClothPairTangle::Through);
        assert(pairTangle(pointTrianglePair(grid.positions, grid.at(7, 0), grid.triangles[inSecond]), tangles) == ClothPairTangle::Apart);

        // A pair straddling the two (a point of one, a triangle with a corner each side of the other's edge) is left free.
        std::uint32_t straddling = 0;
        for (std::uint32_t index = 0; index < grid.triangles.size(); ++index) {
            const int inside = static_cast<int>(second.count(grid.triangles[index][0]) + second.count(grid.triangles[index][1]) +
                                                second.count(grid.triangles[index][2]));
            if (inside == 1 || inside == 2) {
                straddling = index;
                break;
            }
        }
        assert(pairTangle(pointTrianglePair(grid.positions, grid.at(1, 1), grid.triangles[straddling]), tangles) == ClothPairTangle::Free);

        // Marked, a pair through the other part is turned round: it now wants the other side.
        std::vector<ClothSelfPair> pairs{pointTrianglePair(grid.positions, grid.at(1, 1), grid.triangles[inSecond])};
        const Vec3 normal = pairs[0].normal;
        markPairTangles(pairs, tangles);
        assert(pairs[0].tangle == ClothPairTangle::Through);
        assert(pairs[0].normal.dot(normal) < -0.999f);
    }

    /*
    * A path that does not close (the edges cut along a line in the middle of the cloth, as where two hems pass through
    * each other) leaves the cloth in one piece: nothing is inside, nothing changes. Neither do no crossings at all.
    */
    void testAnOpenPathChangesNothing() {
        const Grid grid{8, 0.16f};
        std::vector<std::uint32_t> line;
        for (std::uint32_t index = 0; index < grid.topology.edges.size(); ++index) {
            const std::array<std::uint32_t, 2>& edge = grid.topology.edges[index];
            const bool across = (edge[0] / 9 == 4 && edge[1] / 9 == 5) || (edge[0] / 9 == 5 && edge[1] / 9 == 4);
            const int column = static_cast<int>(edge[0] % 9);
            if (across && column >= 2 && column <= 5) {
                line.push_back(index);
            }
        }
        const std::uint32_t triangle = grid.triangleIn(grid.block(0, 1, 0, 1));
        const ClothTangles tangles = analyseClothTangles(crossingsOf(line, triangle), grid.triangles, grid.topology);
        assert(std::all_of(tangles.inside.begin(), tangles.inside.end(), [](std::uint8_t inside) { return inside == 0; }));
        assert(tangles.through.empty());
        assert(pairTangle(pointTrianglePair(grid.positions, grid.at(4, 3), grid.triangles[triangle]), tangles) == ClothPairTangle::Apart);

        const ClothTangles none = analyseClothTangles({}, grid.triangles, grid.topology);
        assert(none.through.empty() && pairTangle(pointTrianglePair(grid.positions, grid.at(4, 3), grid.triangles[triangle]), none) == ClothPairTangle::Apart);
    }

    // Stitched points are one: two sheets sewn along a side are one part, however the cut runs elsewhere.
    void testStitchedPointsAreOne() {
        Grid grid{4, 0.08f};
        const std::size_t half = grid.positions.size();
        std::vector<Vec3> positions = grid.positions;
        std::vector<std::array<std::uint32_t, 3>> triangles = grid.triangles;
        for (std::size_t index = 0; index < half; ++index) {
            positions.push_back(grid.positions[index] + Vec3{0.0f, 0.0f, 0.1f});
        }
        for (const std::array<std::uint32_t, 3>& triangle : grid.triangles) {
            triangles.push_back({triangle[0] + static_cast<std::uint32_t>(half), triangle[1] + static_cast<std::uint32_t>(half),
                                 triangle[2] + static_cast<std::uint32_t>(half)});
        }
        std::vector<ClothStitch> stitches;
        for (int column = 0; column <= 4; ++column) {
            stitches.push_back(ClothStitch{grid.at(0, column), grid.at(0, column) + static_cast<std::uint32_t>(half)});
        }
        const ClothSelfTopology topology = clothSelfTopology(positions, triangles, stitches);
        const ClothTangles tangles = analyseClothTangles({}, triangles, topology);
        assert(tangles.part[grid.at(4, 4)] == tangles.part[grid.at(4, 4) + static_cast<std::uint32_t>(half)]);
    }

    /*
    * A cloth of two sheets: a fine one flat at z = 0, and a coarse one 2 cm under it whose middle point is pulled up 2 cm
    * through it, a tent poking through the sheet. The finder sees the tent's edges through the sheet and the sheet's
    * through the tent's sides, the same on one thread or many, as the slow counter does.
    */
    struct TentThroughSheet {
        Mesh mesh;
        std::size_t sheetPoints = 0;
        std::uint32_t peak = 0;

        TentThroughSheet() {
            const Mesh sheet = Mesh::grid(20, 20, 0.4f, 0.4f);
            const Mesh tent = Mesh::grid(4, 4, 0.2f, 0.2f);
            mesh = sheet;
            sheetPoints = sheet.vertices.size();
            for (const Vec4& vertex : tent.vertices) {
                mesh.vertices.push_back(Vec4{vertex.x, vertex.y, -0.02f, 1.0f});
            }
            peak = static_cast<std::uint32_t>(sheetPoints + 12);
            mesh.vertices[peak].z = 0.02f;
            for (Triangle triangle : tent.triangles) {
                triangle.first += sheetPoints;
                triangle.second += sheetPoints;
                triangle.third += sheetPoints;
                mesh.triangles.push_back(triangle);
            }
        }

        Cloth cloth(ClothSettings settings) const {
            settings.gravity = 0.0f;
            settings.selfCollision = true;
            settings.tethers = false;
            return makeCloth(mesh, Mat4::identity(), settings);
        }
    };

    void testCrossingsAreFound() {
        const TentThroughSheet scene;
        const Cloth cloth = scene.cloth(ClothSettings{});
        JobSystem jobs{3};
        const std::vector<std::array<std::uint32_t, 2>> serial = findClothCrossings(cloth.particles.positions, cloth.triangles, cloth.selfTopology, serialJobs());
        assert(!serial.empty());
        assert(findClothCrossings(cloth.particles.positions, cloth.triangles, cloth.selfTopology, jobs) == serial);
        assert(clothCrossings(cloth.particles.positions, cloth.triangles, cloth.selfTopology) == serial);

        // The tent's peak and the sheet round it are each other's inside.
        const ClothTangles tangles = analyseClothTangles(serial, cloth.triangles, cloth.selfTopology);
        assert(tangles.inside[tangles.part[scene.peak]] == 1);
        assert(tangles.through.size() == 1);
    }

    /*
    * Each crossing whose edge's ends are in different parts pushes itself back: the end in the inside part (first side)
    * against the crossed triangle, turned towards the side the edge's other end is on, marked through. The tent's peak
    * is pushed down, under the sheet, where its edges' other ends are; the sheet's points round the peak up, above the
    * tent's sides.
    */
    void testCrossingsPushThemselvesBack() {
        const TentThroughSheet scene;
        const Cloth cloth = scene.cloth(ClothSettings{});
        const std::vector<Vec3>& positions = cloth.particles.positions;
        const std::vector<std::array<std::uint32_t, 2>> crossings = findClothCrossings(positions, cloth.triangles, cloth.selfTopology, serialJobs());
        const ClothTangles tangles = analyseClothTangles(crossings, cloth.triangles, cloth.selfTopology);
        const std::vector<ClothSelfPair> pairs = clothCrossingPairs(crossings, positions, cloth.triangles, cloth.selfTopology, tangles);

        assert(!pairs.empty());
        bool peakPushedDown = false;
        bool sheetPushedUp = false;
        for (const ClothSelfPair& pair : pairs) {
            assert(pair.kind == ClothPairKind::PointTriangle && pair.tangle == ClothPairTangle::Through);
            assert(tangles.inside[tangles.part[pair.points[0]]] == 1);
            peakPushedDown = peakPushedDown || (pair.points[0] == scene.peak && pair.normal.z < -0.9f);
            sheetPushedUp = sheetPushedUp || (pair.points[0] < scene.sheetPoints && pair.normal.z > 0.5f);
        }
        assert(peakPushedDown && sheetPushedUp);
    }

    /*
    * The response, on a point 3 mm under a level triangle, its corners held: kept apart, the point goes down to the
    * thickness; marked through, it goes up through the triangle, no faster than the untangle step a call, and stops the
    * thickness above; free, it stays.
    */
    void testThroughPairsGoBackThrough() {
        ClothSettings settings;
        ClothParticles particles;
        particles.positions = {Vec3{0.02f, 0.02f, -0.003f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.1f, 0.0f, 0.0f}, Vec3{0.0f, 0.1f, 0.0f}};
        particles.previousPositions = particles.positions;
        particles.velocities.assign(4, Vec3{});
        particles.inverseMasses = {1.0f, 0.0f, 0.0f, 0.0f};
        const ClothSelfPair found = pointTrianglePair(particles.positions, 0, {1, 2, 3});

        ClothParticles apart = particles;
        solveSelfContacts(apart, {found}, settings);
        assert(std::fabs(apart.positions[0].z + settings.thickness) < 1e-5f);

        ClothSelfPair free = found;
        free.tangle = ClothPairTangle::Free;
        ClothParticles left = particles;
        solveSelfContacts(left, {free}, settings);
        assert(left.positions[0].z == particles.positions[0].z);

        ClothSelfPair through = found;
        through.tangle = ClothPairTangle::Through;
        through.normal = through.normal * -1.0f;
        const float step = 0.001f;
        ClothParticles back = particles;
        solveSelfContacts(back, {through}, settings, {}, {}, step);
        assert(std::fabs(back.positions[0].z - (-0.003f + step)) < 1e-6f);
        for (int call = 0; call < 20; ++call) {
            solveSelfContacts(back, {through}, settings, {}, {}, step);
        }
        assert(std::fabs(back.positions[0].z - settings.thickness) < 1e-5f);
        assert(std::fabs(back.positions[0].x - 0.02f) < 1e-6f);
    }

    // The cut leaves pairs going through, and free pairs, alone: it would hold a tangle where it is. (3 mm of motion,
    // within the cap every point keeps.)
    void testTheCutIgnoresTangledPairs() {
        const std::vector<Vec3> start{Vec3{0.02f, 0.02f, -0.002f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.1f, 0.0f, 0.0f}, Vec3{0.0f, 0.1f, 0.0f}};
        const std::vector<float> inverseMasses{1.0f, 0.0f, 0.0f, 0.0f};
        for (const ClothPairTangle tangle : {ClothPairTangle::Apart, ClothPairTangle::Through, ClothPairTangle::Free}) {
            std::vector<ClothSelfPair> pairs{pointTrianglePair(start, 0, {1, 2, 3})};
            pairs[0].tangle = tangle;
            std::vector<Vec3> positions = start;
            positions[0].z = 0.001f;
            cutClothMotion(positions, start, start, inverseMasses, pairs, 0.0075f);
            if (tangle == ClothPairTangle::Apart) {
                assert(positions[0].z < 0.0f);
            } else {
                assert(positions[0].z == 0.001f);
            }
        }
    }

    /*
    * Simulated: the tent comes back out of the sheet. With untangling on, every crossing is gone within half a second and
    * the peak ends under the sheet; with it off, the self-contacts hold the tangle as they found it.
    */
    void testATentComesBackOut() {
        const TentThroughSheet scene;
        for (const bool untangle : {true, false}) {
            ClothSettings settings;
            settings.untangle = untangle;
            Cloth cloth = scene.cloth(settings);
            assert(countClothCrossings(cloth.particles.positions, cloth.triangles, cloth.selfTopology) > 0);
            for (int tick = 0; tick < 60; ++tick) {
                stepCloth(cloth, 1.0f / 120.0f);
            }
            const std::size_t crossings = countClothCrossings(cloth.particles.positions, cloth.triangles, cloth.selfTopology);
            if (untangle) {
                assert(crossings == 0);
                assert(cloth.particles.positions[scene.peak].z < 0.0f);
            } else {
                assert(crossings > 0);
            }
        }
    }

    // On by default, at 1 m/s, both reachable by key.
    void testTheSettings() {
        const ClothSettings defaults;
        assert(defaults.untangle && defaults.untangleSpeed == 1.0f);
        ClothSettings settings;
        const SettingsWalk walk = [&settings](SettingsVisitor& visitor) {
            visit(settings, visitor);
        };
        const std::vector<std::string> warnings = applySettings({{"untangle", "false"}, {"untangleSpeed", "0.5"}}, walk, StartupFields::Keep);
        assert(warnings.empty());
        assert(!settings.untangle && settings.untangleSpeed == 0.5f);
    }
}

void testClothUntangle() {
    testAFoldThroughItselfIsLooped();
    testTwoBlocksPassedThroughEachOther();
    testAnOpenPathChangesNothing();
    testStitchedPointsAreOne();
    testCrossingsAreFound();
    testCrossingsPushThemselvesBack();
    testThroughPairsGoBackThrough();
    testTheCutIgnoresTangledPairs();
    testATentComesBackOut();
    testTheSettings();
}
