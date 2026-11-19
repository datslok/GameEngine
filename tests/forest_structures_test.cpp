#include "games/forest/forest_terrain.h"
#include "games/forest/structure_seams.h"
#include "games/forest/structures.h"
#include "games/forest/valley_shapes.h"
#include "scene/indexed_mesh.h"
#include "test_jobs.h"
#include "test_levels.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <map>
#include <utility>

namespace {
    Vec3 corner(const Mesh& mesh, std::size_t index) {
        const Vec4& point = mesh.vertices[index];
        return Vec3{point.x, point.y, point.z};
    }

    Vec3 triangleNormal(const Mesh& mesh, const Triangle& triangle) {
        return faceNormal(mesh.vertices[triangle.first], mesh.vertices[triangle.second], mesh.vertices[triangle.third]);
    }

    Vec3 triangleMiddle(const Mesh& mesh, const Triangle& triangle) {
        return (corner(mesh, triangle.first) + corner(mesh, triangle.second) + corner(mesh, triangle.third)) / 3.0f;
    }

    /*
    * Closed and facing out: every edge is walked once each way by the triangles beside it (so the surface is closed and
    * its triangles agree on which side is out), and the volume the triangles enclose, the sum of their signed tetrahedra
    * from the origin, is positive (so that side is the outside).
    */
    bool closedAndFacingOut(const Mesh& mesh) {
        std::map<std::pair<std::size_t, std::size_t>, int> walked;
        float volume = 0.0f;

        for (const Triangle& triangle : mesh.triangles) {
            const std::size_t corners[3]{triangle.first, triangle.second, triangle.third};

            for (int side = 0; side < 3; ++side) {
                ++walked[{corners[side], corners[(side + 1) % 3]}];
            }

            volume += corner(mesh, triangle.first).dot(corner(mesh, triangle.second).cross(corner(mesh, triangle.third))) / 6.0f;
        }

        for (const auto& [edge, count] : walked) {
            if (count != 1 || walked.count({edge.second, edge.first}) == 0 || walked.at({edge.second, edge.first}) != 1) {
                return false;
            }
        }

        return volume > 0.0f;
    }

    // Every corner of a triangle shares one texture coordinate (the facet's two random numbers), inside 0..1, and has no
    // normal of its own (so the triangle is shaded flat); the facets' numbers differ.
    bool facetsCarryTheirOwnNumbers(const Mesh& mesh) {
        std::size_t repeats = 0;

        for (std::size_t index = 0; index < mesh.triangles.size(); ++index) {
            const Triangle& triangle = mesh.triangles[index];

            for (int side = 0; side < 3; ++side) {
                if (!triangle.uvs[side] || triangle.normals[side] || !triangle.tangents[side]) {
                    return false;
                }

                if ((*triangle.uvs[side] - *triangle.uvs[0]).length() > 0.0f) {
                    return false;
                }
            }

            const Vec2 numbers = *triangle.uvs[0];

            if (numbers.x < 0.0f || numbers.x > 1.0f || numbers.y < 0.0f || numbers.y > 1.0f) {
                return false;
            }

            if (index > 0 && (*mesh.triangles[index - 1].uvs[0] - numbers).length() == 0.0f) {
                ++repeats;
            }
        }

        return repeats == 0;
    }

    float highest(const Mesh& mesh) {
        float top = -1e9f;

        for (const Vec4& point : mesh.vertices) {
            top = std::max(top, point.y);
        }

        return top;
    }

    float lowest(const Mesh& mesh) {
        float bottom = 1e9f;

        for (const Vec4& point : mesh.vertices) {
            bottom = std::min(bottom, point.y);
        }

        return bottom;
    }

    /*
    * A slab: closed, flat facets each with its own numbers, as tall as asked and sunk below its foot; its sides below the
    * shoulder nearly upright (no moss), its shoulder's facets turned to the sky as much as the rock's moss asks (an upward
    * part past 0.55), and its top cut to the side its cut says. Indexed, every vertex keeps its triangle's face normal.
    */
    void testSlab() {
        const SlabShape slab = makeSlab(Vec3{10.0f, 3.0f, -20.0f}, 100.0f, 400.0f, 1.0f, 0.5f, 0.25f, 7u);
        const Mesh mesh = slabMesh(slab);
        assert(closedAndFacingOut(mesh));
        assert(facetsCarryTheirOwnNumbers(mesh));
        assert(near(highest(mesh), 400.0f, 1e-3f));
        assert(lowest(mesh) < -5.0f);

        const float shoulder = 0.5f * 400.0f;
        bool sawShoulder = false;
        float topX = 0.0f;

        for (const Triangle& triangle : mesh.triangles) {
            const Vec3 middle = triangleMiddle(mesh, triangle);
            const Vec3 normal = triangleNormal(mesh, triangle);

            if (middle.y > 0.0f && middle.y < shoulder - 1.0f) {
                assert(std::fabs(normal.y) < 0.1f);
            }

            if (middle.y > shoulder && middle.y < shoulder + slabShoulderRise) {
                assert(normal.y > 0.55f && normal.y < 0.95f);
                sawShoulder = true;
            }
        }

        for (const Vec4& point : mesh.vertices) {
            if (point.y > 399.0f) {
                topX = point.x;
            }
        }

        assert(sawShoulder && topX > 0.2f * 100.0f);
        assert(makeSlab(Vec3{}, 100.0f, 400.0f, -1.0f, 0.5f, 0.25f, 7u).levels.back().offset < 0.0f);

        const IndexedMesh indexed = buildIndexedMesh(mesh);

        for (std::size_t i = 0; i + 2 < indexed.indices.size(); i += 3) {
            const MeshVertex& a = indexed.vertices[indexed.indices[i]];
            const MeshVertex& b = indexed.vertices[indexed.indices[i + 1]];
            const MeshVertex& c = indexed.vertices[indexed.indices[i + 2]];
            const Vec3 face = (b.position - a.position).cross(c.position - a.position).normalized();
            assert(face.dot(a.normal) > 0.999f && face.dot(b.normal) > 0.999f && face.dot(c.normal) > 0.999f);
        }
    }

    /*
    * The Monolith: five slabs on the clearing's level, the tallest 600 m with its front ridge on the Monolith's line,
    * each sunk into the ground; their fronts staggered, so no two faces lie in one plane; the strut from the bridge's
    * pillar reaching into a slab tall enough to hold it.
    */
    void testMonolith() {
        const float level = 3.0f;
        const std::vector<SlabShape> slabs = monolithSlabs(level);
        assert(slabs.size() == 5);
        float tallest = 0.0f;
        std::size_t tallestIndex = 0;

        for (std::size_t index = 0; index < slabs.size(); ++index) {
            const Mesh mesh = slabMesh(slabs[index]);
            assert(closedAndFacingOut(mesh));
            assert(slabs[index].foot.y == level && lowest(mesh) < 0.0f);

            if (highest(mesh) > tallest) {
                tallest = highest(mesh);
                tallestIndex = index;
            }

            for (std::size_t other = index + 1; other < slabs.size(); ++other) {
                assert(std::fabs(slabs[index].foot.z - slabs[other].foot.z) > 1.0f);
            }
        }

        assert(near(tallest, 600.0f, 1e-3f));
        const SlabShape& main = slabs[tallestIndex];
        assert(near(main.foot.z + 0.5f * main.depth, forestMonolithPlace.y, 1e-3f));

        const StrutShape strut = monolithStrut(level);
        const Mesh strutShape = strutMesh(strut);
        assert(closedAndFacingOut(strutShape) && facetsCarryTheirOwnNumbers(strutShape));
        const PillarPlacement pillar = monolithPillar(level);
        assert((Vec2{strut.from.x, strut.from.z} - Vec2{pillar.foot.x, pillar.foot.z}).length() < pillar.radius);
        assert(strut.from.y < pillar.foot.y + pillar.height);

        const auto holds = std::any_of(slabs.begin(), slabs.end(), [&](const SlabShape& slab) {
            return std::fabs(strut.to.x - slab.foot.x) < 0.5f * slab.width && slab.foot.y + slab.height > strut.to.y + 50.0f;
        });
        assert(holds);
    }

    // The far spires: ten, each as tall as asked above the ground under it, closed, coming to a point.
    void testSpires() {
        const std::vector<SlabShape> spires = farSpires();
        assert(spires.size() == 10);

        for (const SlabShape& spire : spires) {
            const Mesh mesh = slabMesh(spire);
            assert(closedAndFacingOut(mesh));
            assert(near(highest(mesh), spire.height, 1e-3f) && spire.height >= 300.0f && spire.height <= 800.0f);
            assert(near(spire.foot.y, forestHeight(spire.foot.x, spire.foot.z), 1e-3f));
            assert(spire.levels.back().widthShare < 0.05f);
        }
    }

    /*
    * The Ring: segments round its centre in the plane facing along z, each a closed prism; gaps between them; the lower
    * half below its centre (the ground); the segments near the top drifted outwards, the rest on the polygon.
    */
    void testRing() {
        const Mesh ring = ringSegmentsMesh(200.0f, 12.0f, 11u);
        assert(closedAndFacingOut(ring) && facetsCarryTheirOwnNumbers(ring));
        assert(ring.triangles.size() == static_cast<std::size_t>(ringSegments * ringSegmentTriangles));
        assert(lowest(ring) < -190.0f && highest(ring) > 205.0f);

        float farthest = 0.0f;
        float nearest = 1e9f;

        for (const Vec4& point : ring.vertices) {
            const float reach = Vec2{point.x, point.y}.length();
            assert(std::fabs(point.z) <= 12.0f + 1e-3f);
            assert(reach > 200.0f - 12.0f - 1.0f);

            if (point.y > 150.0f) {
                farthest = std::max(farthest, reach);
            }

            nearest = std::min(nearest, reach);
        }

        assert(farthest > 200.0f / std::cos(3.14159265f / ringSegments) + 12.0f + 2.0f);
        // The inner corners at the segments' ends, the nearest points: the inner face's distance along the end, a half
        // segment from the middle.
        const float halfSegment = 200.0f * std::tan(3.14159265f / ringSegments);
        assert(near(nearest, Vec2{200.0f - 12.0f, halfSegment}.length(), 1.0f));

        // No point of one segment touches another: every vertex is at least a gap from every other segment's.
        const std::size_t perSegment = ring.vertices.size() / ringSegments;

        for (std::size_t a = 0; a < ring.vertices.size(); a += 7) {
            for (std::size_t b = 0; b < ring.vertices.size(); b += 5) {
                if (a / perSegment != b / perSegment) {
                    assert((corner(ring, a) - corner(ring, b)).length() > 1.0f);
                }
            }
        }
    }
}

namespace {
    /*
    * The seam layout: rows and panels within their sizes (a little outside after scaling to fill the tile), filling it;
    * some sawtooth edges of each kind and some hatches over a few tiles' seeds; the same for the same seed.
    */
    void testSeamLayout() {
        int sawtoothRows = 0;
        int sawtoothSplits = 0;
        std::size_t hatches = 0;

        for (std::uint32_t seed = 0; seed < 8; ++seed) {
            const SeamLayout layout = seamLayout(16.0f, seed);
            assert(layout.tile == 16.0f && layout.rows.size() >= 4 && layout.rows.size() <= 8);
            assert(layout.rows.front().bottom.at == 0.0f && !layout.rows.front().bottom.sawtooth);

            for (std::size_t row = 0; row < layout.rows.size(); ++row) {
                const float top = row + 1 < layout.rows.size() ? layout.rows[row + 1].bottom.at : 16.0f;
                const float height = top - layout.rows[row].bottom.at;
                assert(height > 1.5f && height < 5.5f);
                sawtoothRows += layout.rows[row].bottom.sawtooth ? 1 : 0;
                const std::vector<SeamLine>& splits = layout.rows[row].splits;
                assert(splits.size() >= 2);

                for (std::size_t split = 0; split < splits.size(); ++split) {
                    const float next = split + 1 < splits.size() ? splits[split + 1].at : splits.front().at + 16.0f;
                    assert(splits[split].at >= 0.0f && splits[split].at < 16.0f && next - splits[split].at > 1.5f && next - splits[split].at < 8.5f);
                    sawtoothSplits += splits[split].sawtooth ? 1 : 0;
                }
            }

            hatches += layout.hatches.size();
        }

        assert(sawtoothRows > 0 && sawtoothSplits > 0 && hatches > 0);
        const SeamLayout again = seamLayout(16.0f, 3);
        const SeamLayout first = seamLayout(16.0f, 3);
        assert(again.rows.size() == first.rows.size() && again.rows[1].splits[0].at == first.rows[1].splits[0].at);
        assert(seamLayout(16.0f, 4).rows[1].splits[0].at != first.rows[1].splits[0].at);
    }

    /*
    * The distance to the nearest seam: zero on a straight row boundary and on a straight split, the tile wrapping (the
    * same a tile over), a sawtooth's teeth meeting across the tile's edge, never more than half a row. A groove's height
    * is 0 inside its half width and 1 past the bevel, rising in between.
    */
    void testSeamDistance() {
        SeamLayout layout;
        layout.tile = 16.0f;
        layout.rows = {SeamRow{.bottom = {0.0f, false}, .splits = {{2.0f, false}, {9.0f, true}}},
                       SeamRow{.bottom = {6.0f, true}, .splits = {{5.0f, false}, {12.0f, false}}},
                       SeamRow{.bottom = {11.0f, false}, .splits = {{1.0f, false}, {8.0f, false}}}};

        assert(near(seamDistance(layout, Vec2{4.0f, 11.0f}), 0.0f, 1e-5f));
        assert(near(seamDistance(layout, Vec2{2.0f, 3.0f}), 0.0f, 1e-5f));
        assert(near(seamDistance(layout, Vec2{18.0f, 3.0f}), 0.0f, 1e-5f) && near(seamDistance(layout, Vec2{2.0f, -13.0f}), 0.0f, 1e-5f));
        assert(near(seamDistance(layout, Vec2{4.0f, 13.5f}), seamDistance(layout, Vec2{20.0f, 29.5f}), 1e-4f));
        assert(near(seamDistance(layout, Vec2{4.0f, 13.5f}), 2.5f, 1e-4f));

        // The sawtooth boundary at 6 m: its teeth rise to half a period between the tile's edges, the same either side.
        float highest = 0.0f;

        for (int step = 0; step <= 400; ++step) {
            const float x = 3.0f + 0.002f * static_cast<float>(step);
            float lowest = 1e9f;
            float lineAt = 0.0f;

            for (int up = 0; up <= 500; ++up) {
                const float y = 5.9f + 0.001f * static_cast<float>(up);
                const float distance = seamDistance(layout, Vec2{x, y});

                if (distance < lowest) {
                    lowest = distance;
                    lineAt = y;
                }
            }

            assert(lowest < 0.002f && lineAt >= 6.0f - 0.002f && lineAt <= 6.0f + sawtoothTooth + 0.01f);
            highest = std::max(highest, lineAt - 6.0f);
        }

        assert(highest > 0.25f);
        assert(near(seamDistance(layout, Vec2{0.0f, 6.0f}), seamDistance(layout, Vec2{16.0f, 6.0f}), 1e-4f));

        assert(seamHeight(0.0f) == 0.0f && seamHeight(seamHalfWidth) == 0.0f && seamHeight(seamHalfWidth + seamBevel) == 1.0f && seamHeight(1.0f) == 1.0f);
        assert(seamHeight(seamHalfWidth + 0.5f * seamBevel) > 0.4f && seamHeight(seamHalfWidth + 0.5f * seamBevel) < 0.6f);
    }

    /*
    * The textures: square, sRGB albedo, linear height and normals; panels lighter and higher than grooves; the normal
    * flat in a panel's middle and leaning towards a groove on its bevel (just under a boundary, up the picture);
    * seamless, the first column as close to the last as any two neighbours.
    */
    void testSeamImages() {
        SeamLayout layout;
        layout.tile = 16.0f;
        layout.rows = {SeamRow{.bottom = {0.0f, false}, .splits = {{2.0f, false}, {9.0f, false}}},
                       SeamRow{.bottom = {8.0f, false}, .splits = {{5.0f, false}, {12.0f, false}}}};
        const int size = 512;
        const SeamImages images = seamImages(layout, size, testJobs());
        assert(images.albedo.width == 512 && images.albedo.height == 512 && images.albedo.encoding == TextureEncoding::Srgb);
        assert(images.normal.width == 512 && images.normal.encoding == TextureEncoding::Linear);

        const auto texel = [&](const ImageData& image, int column, int row) {
            return &image.pixels[(static_cast<std::size_t>(row) * size + static_cast<std::size_t>(column)) * 4];
        };
        const int boundary = size / 2;
        assert(texel(images.albedo, 160, boundary)[0] < texel(images.albedo, 160, boundary - 40)[0]);
        assert(images.height.encoding == TextureEncoding::Linear && texel(images.height, 160, boundary)[0] < 8 && texel(images.height, 160, boundary - 40)[0] == 255);
        const std::uint8_t* flat = texel(images.normal, 160, boundary - 40);
        assert(flat[0] == 128 && flat[1] == 128 && flat[2] == 255);

        // A texel on the bevel just under the boundary (16 m over 512 texels: 3.1 cm a texel), leaning up towards it.
        assert(texel(images.normal, 160, boundary - 2)[1] > 140);

        int worstEdge = 0;
        int worstInside = 0;

        for (int row = 0; row < size; ++row) {
            worstEdge = std::max(worstEdge, std::abs(texel(images.albedo, 0, row)[0] - texel(images.albedo, size - 1, row)[0]));
            worstInside = std::max(worstInside, std::abs(texel(images.albedo, 300, row)[0] - texel(images.albedo, 301, row)[0]));
        }

        assert(worstEdge <= std::max(worstInside, 12));
    }
}

void testForestStructures() {
    testSeamLayout();
    testSeamDistance();
    testSeamImages();
    testSlab();
    testMonolith();
    testSpires();
    testRing();
}
