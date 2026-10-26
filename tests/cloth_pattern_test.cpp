#include "cloth/cloth_pattern.h"
#include "cloth_helpers.h"
#include "test_levels.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
    // A closed outline of straight edges through the corners, counterclockwise, one edge per corner.
    PatternPiece polygonPiece(const std::vector<Vec2>& corners) {
        PatternPiece piece;
        for (std::size_t index = 0; index < corners.size(); ++index) {
            piece.edges.push_back(PatternEdge{{corners[index], corners[(index + 1) % corners.size()]}});
        }
        return piece;
    }

    // A rectangle from its bottom-left corner: its edges the bottom, right, top and left sides, in that order.
    PatternPiece rectanglePiece(const Vec2& corner, float width, float height) {
        return polygonPiece({corner, corner + Vec2{width, 0.0f}, corner + Vec2{width, height}, corner + Vec2{0.0f, height}});
    }

    Vec2 flatOf(const Vec4& vertex) {
        return Vec2{vertex.x, vertex.y};
    }

    double triangleArea(const Mesh& mesh, const Triangle& triangle) {
        const Vec2 a = flatOf(mesh.vertices[triangle.first]);
        const Vec2 b = flatOf(mesh.vertices[triangle.second]);
        const Vec2 c = flatOf(mesh.vertices[triangle.third]);
        return 0.5 * ((static_cast<double>(b.x) - a.x) * (static_cast<double>(c.y) - a.y) - (static_cast<double>(b.y) - a.y) * (static_cast<double>(c.x) - a.x));
    }

    double meshArea(const Mesh& mesh) {
        double area = 0.0;
        for (const Triangle& triangle : mesh.triangles) {
            area += triangleArea(mesh, triangle);
        }
        return area;
    }

    /*
    * A sound piece of fabric: every triangle counterclockwise (facing +z, the fabric's outside) with area, each inside
    * edge shared by exactly two triangles, and each of the outline's segments the edge of exactly one, turning the
    * outline's way: one piece of cloth with no holes, overlaps or cuts.
    */
    void assertSoundPiece(const PatternMesh& meshed, std::uint32_t piece) {
        std::map<std::pair<std::size_t, std::size_t>, int> directed;
        for (const Triangle& triangle : meshed.mesh.triangles) {
            if (meshed.pieces[triangle.first] != piece) {
                continue;
            }
            assert(meshed.pieces[triangle.second] == piece && meshed.pieces[triangle.third] == piece);
            assert(triangleArea(meshed.mesh, triangle) > 0.0);
            const std::size_t corners[3]{triangle.first, triangle.second, triangle.third};
            for (int side = 0; side < 3; ++side) {
                const int uses = ++directed[std::make_pair(corners[side], corners[(side + 1) % 3])];
                assert(uses == 1);
            }
        }

        std::map<std::pair<std::size_t, std::size_t>, bool> border;
        for (const std::vector<std::uint32_t>& edge : meshed.edgePoints[piece]) {
            for (std::size_t index = 0; index + 1 < edge.size(); ++index) {
                border[std::make_pair(edge[index], edge[index + 1])] = true;
                assert(directed.count(std::make_pair(edge[index], edge[index + 1])) == 1);
            }
        }
        for (const auto& [edge, uses] : directed) {
            const bool hasTwin = directed.count(std::make_pair(edge.second, edge.first)) == 1;
            assert(hasTwin != (border.count(edge) == 1));
        }
    }

    // An edge's length is along its polyline; resampled, its points lie evenly along it, its ends exactly kept.
    void testEdgeResampledEvenly() {
        const PatternEdge edge{{Vec2{0.0f, 0.0f}, Vec2{3.0f, 0.0f}, Vec2{3.0f, 4.0f}}};
        assert(near(edgeLength(edge), 7.0f, 1e-6f));

        const std::vector<Vec2> points = resampleEdge(edge, 7);
        assert(points.size() == 8);
        for (std::size_t index = 0; index < 4; ++index) {
            assert(near(points[index].x, static_cast<float>(index), 1e-5f) && near(points[index].y, 0.0f, 1e-5f));
        }
        for (std::size_t index = 4; index < 8; ++index) {
            assert(near(points[index].x, 3.0f, 1e-5f) && near(points[index].y, static_cast<float>(index - 3), 1e-5f));
        }
        assert(points.front().x == 0.0f && points.back().x == 3.0f && points.back().y == 4.0f);
    }

    /*
    * A rectangle 30 x 20 cm at 2 cm: the area kept, the cloth sound, its triangles near the lattice's equilateral ones
    * (none sharper than 25 degrees, even at the border), each edge's points no further apart than the spacing and
    * shared at the corners, the UVs the flat positions, and the first piece left where its outline is.
    */
    void testRectangleMeshed() {
        Pattern pattern;
        pattern.pieces.push_back(rectanglePiece(Vec2{-0.1f, 0.05f}, 0.3f, 0.2f));
        const PatternMesh meshed = meshPattern(pattern, 0.02f);

        assert(std::abs(meshArea(meshed.mesh) - 0.06) < 1e-6);
        assertSoundPiece(meshed, 0);
        assert(smallestAngleDegrees(meshed.mesh) > 25.0);

        // About one point per sqrt(3)/2 spacing^2 of fabric.
        assert(meshed.mesh.vertices.size() > 150 && meshed.mesh.vertices.size() < 220);

        const std::vector<std::vector<std::uint32_t>>& edges = meshed.edgePoints[0];
        assert(edges.size() == 4 && edges[0].size() == 16 && edges[1].size() == 11 && edges[2].size() == 16 && edges[3].size() == 11);
        for (std::size_t index = 0; index < 4; ++index) {
            assert(edges[index].back() == edges[(index + 1) % 4].front());
        }

        for (std::size_t index = 0; index < meshed.mesh.vertices.size(); ++index) {
            assert(meshed.pieces[index] == 0);
            assert(meshed.mesh.vertices[index].x == meshed.flatPositions[index].x && meshed.mesh.vertices[index].y == meshed.flatPositions[index].y);
            assert(meshed.mesh.vertices[index].z == 0.0f);
        }
        for (const Triangle& triangle : meshed.mesh.triangles) {
            const std::size_t corners[3]{triangle.first, triangle.second, triangle.third};
            for (int corner = 0; corner < 3; ++corner) {
                assert(triangle.uvs[corner] && triangle.uvs[corner]->x == meshed.flatPositions[corners[corner]].x &&
                       triangle.uvs[corner]->y == meshed.flatPositions[corners[corner]].y);
            }
        }
    }

    /*
    * An outline with a notch cut into its top, like the neckline between two shoulder straps: nothing is left in the
    * notch, and the fabric round it is whole.
    */
    void testConcaveOutline() {
        Pattern pattern;
        pattern.pieces.push_back(polygonPiece({Vec2{0.0f, 0.0f}, Vec2{0.3f, 0.0f}, Vec2{0.3f, 0.3f}, Vec2{0.2f, 0.3f}, Vec2{0.2f, 0.1f},
                                               Vec2{0.1f, 0.1f}, Vec2{0.1f, 0.3f}, Vec2{0.0f, 0.3f}}));
        const PatternMesh meshed = meshPattern(pattern, 0.015f);

        assert(std::abs(meshArea(meshed.mesh) - 0.07) < 1e-6);
        assertSoundPiece(meshed, 0);
        assert(smallestAngleDegrees(meshed.mesh) > 25.0);
        for (const Triangle& triangle : meshed.mesh.triangles) {
            const Vec2 centre = (flatOf(meshed.mesh.vertices[triangle.first]) + flatOf(meshed.mesh.vertices[triangle.second]) +
                                 flatOf(meshed.mesh.vertices[triangle.third])) / 3.0f;
            assert(!(centre.x > 0.1f && centre.x < 0.2f && centre.y > 0.1f));
        }
    }

    /*
    * Two pieces sewn side to side, the second a little shorter: both sewn edges get the same number of points (the
    * longer one's), and each stitch joins points level with each other, since outlines that both run counterclockwise
    * run opposite ways along the edge they are sewn on. The pieces lie apart at rest, and the seam gets a hinge for each
    * segment between its stitches.
    */
    void testSewnEdgesPairOppositeWays() {
        Pattern pattern;
        pattern.pieces.push_back(rectanglePiece(Vec2{0.0f, 0.0f}, 0.1f, 0.1f));
        pattern.pieces.push_back(rectanglePiece(Vec2{0.0f, 0.0f}, 0.1f, 0.0899f));
        pattern.seams.push_back(PatternSeam{0, 1, 1, 3});
        const PatternMesh meshed = meshPattern(pattern, 0.03f);

        assertSoundPiece(meshed, 0);
        assertSoundPiece(meshed, 1);
        assert(meshed.edgePoints[0][1].size() == 5 && meshed.edgePoints[1][3].size() == 5);
        assert(meshed.seams.size() == 1 && meshed.seams[0].size() == 5);
        for (std::size_t index = 0; index < 5; ++index) {
            const ClothStitch& stitch = meshed.seams[0][index];
            assert(stitch.first == meshed.edgePoints[0][1][index] && stitch.second == meshed.edgePoints[1][3][4 - index]);
            assert(meshed.pieces[stitch.first] == 0 && meshed.pieces[stitch.second] == 1);
            assert(near(meshed.flatPositions[stitch.first].y / 0.1f, meshed.flatPositions[stitch.second].y / 0.0899f, 1e-5f));
        }

        float firstRight = -1.0f;
        float secondLeft = 1.0f;
        for (std::size_t index = 0; index < meshed.mesh.vertices.size(); ++index) {
            if (meshed.pieces[index] == 0) {
                firstRight = std::max(firstRight, meshed.mesh.vertices[index].x);
            } else {
                secondLeft = std::min(secondLeft, meshed.mesh.vertices[index].x);
            }
        }
        assert(secondLeft > firstRight);

        const ClothSewing sewing = clothSewing(meshed);
        assert(sewing.stitches.size() == 5 && sewing.hinges.size() == 4);
    }

    // A corner where two sewn edges meet would be in two stitches, which the stitches cannot take: rejected.
    void testPointInTwoStitchesRejected() {
        Pattern pattern;
        pattern.pieces.push_back(rectanglePiece(Vec2{0.0f, 0.0f}, 0.1f, 0.1f));
        pattern.pieces.push_back(rectanglePiece(Vec2{0.0f, 0.0f}, 0.1f, 0.1f));
        pattern.pieces.push_back(rectanglePiece(Vec2{0.0f, 0.0f}, 0.1f, 0.1f));
        pattern.seams.push_back(PatternSeam{0, 1, 1, 3});
        pattern.seams.push_back(PatternSeam{0, 2, 2, 0});

        bool threw = false;
        try {
            meshPattern(pattern, 0.03f);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    // Outlines that run clockwise or do not close, a spacing that is no size, and seams on edges that do not exist are errors.
    void testBadPatternsRejected() {
        const auto throws = [](const Pattern& pattern, float spacing) {
            try {
                meshPattern(pattern, spacing);
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };

        Pattern clockwise;
        clockwise.pieces.push_back(polygonPiece({Vec2{0.0f, 0.0f}, Vec2{0.0f, 0.1f}, Vec2{0.1f, 0.1f}, Vec2{0.1f, 0.0f}}));
        assert(throws(clockwise, 0.02f));

        Pattern open;
        open.pieces.push_back(rectanglePiece(Vec2{0.0f, 0.0f}, 0.1f, 0.1f));
        open.pieces[0].edges[2].points.back().x += 0.01f;
        assert(throws(open, 0.02f));

        Pattern square;
        square.pieces.push_back(rectanglePiece(Vec2{0.0f, 0.0f}, 0.1f, 0.1f));
        assert(throws(square, 0.0f));
        assert(!throws(square, 0.02f));

        square.seams.push_back(PatternSeam{0, 1, 1, 3});
        assert(throws(square, 0.02f));
    }

    /*
    * Wrapped round an upright cylinder, a piece keeps its lengths across (a cylinder is flat cut open): its x runs
    * round the circle from the side it faces, its y straight up from the axis's foot, and its outside faces outwards.
    */
    void testWrapOnCylinder() {
        const Vec3 foot{1.0f, 2.0f, 3.0f};
        const float radius = 0.5f;
        const float quarter = 0.25f * 6.2831853f;

        const Vec3 middle = wrapOnCylinder(Vec2{0.0f, 0.4f}, foot, radius, 0.0f);
        assert(near(middle.x, 1.0f, 1e-6f) && near(middle.y, 2.4f, 1e-6f) && near(middle.z, 3.5f, 1e-6f));
        const Vec3 side = wrapOnCylinder(Vec2{radius * quarter, 0.0f}, foot, radius, 0.0f);
        assert(near(side.x, 1.5f, 1e-5f) && near(side.z, 3.0f, 1e-5f));

        // Facing the back, the piece's +x goes to the world's -x, as seen from behind.
        const Vec3 back = wrapOnCylinder(Vec2{0.1f, 0.0f}, foot, radius, 2.0f * quarter);
        assert(back.x < foot.x && back.z < foot.z);

        // Along a row, the straight line between two points is the chord of their arc.
        const Vec3 a = wrapOnCylinder(Vec2{-0.2f, 0.1f}, foot, radius, 0.3f);
        const Vec3 b = wrapOnCylinder(Vec2{0.3f, 0.1f}, foot, radius, 0.3f);
        assert(near((b - a).length(), 2.0f * radius * std::sin(0.5f / (2.0f * radius)), 1e-5f));

        // A counterclockwise triangle of the pattern faces away from the axis.
        const Vec3 p0 = wrapOnCylinder(Vec2{0.0f, 0.0f}, foot, radius, 1.0f);
        const Vec3 p1 = wrapOnCylinder(Vec2{0.05f, 0.0f}, foot, radius, 1.0f);
        const Vec3 p2 = wrapOnCylinder(Vec2{0.0f, 0.05f}, foot, radius, 1.0f);
        const Vec3 normal = (p1 - p0).cross(p2 - p0);
        assert(normal.dot(p0 - Vec3{foot.x, p0.y, foot.z}) > 0.0f);
    }
}

void testClothPattern() {
    testEdgeResampledEvenly();
    testRectangleMeshed();
    testConcaveOutline();
    testSewnEdgesPairOppositeWays();
    testPointInTwoStitchesRejected();
    testBadPatternsRejected();
    testWrapOnCylinder();
}
