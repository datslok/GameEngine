#include "games/forest/bastion.h"
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
    * A flat face keeps one tone: two triangles sharing an edge in one plane (a side quad's halves, a cap's fan) share
    * their number pair, and no pair is shared by triangles facing different ways. Every triangle shaded flat.
    */
    bool facesShareTheirNumbers(const Mesh& mesh) {
        std::map<std::pair<std::size_t, std::size_t>, std::vector<std::size_t>> byEdge;
        std::map<std::pair<float, float>, Vec3> normalByNumbers;

        for (std::size_t index = 0; index < mesh.triangles.size(); ++index) {
            const Triangle& triangle = mesh.triangles[index];

            if (!triangle.uvs[0] || triangle.normals[0]) {
                return false;
            }

            const Vec3 normal = triangleNormal(mesh, triangle);
            const auto [known, added] = normalByNumbers.try_emplace({triangle.uvs[0]->x, triangle.uvs[0]->y}, normal);

            if (!added && known->second.dot(normal) < 0.999f) {
                return false;
            }

            const std::size_t corners[3]{triangle.first, triangle.second, triangle.third};

            for (int side = 0; side < 3; ++side) {
                byEdge[{std::min(corners[side], corners[(side + 1) % 3]), std::max(corners[side], corners[(side + 1) % 3])}].push_back(index);
            }
        }

        for (const auto& [edge, triangles] : byEdge) {
            for (std::size_t a = 0; a < triangles.size(); ++a) {
                for (std::size_t b = a + 1; b < triangles.size(); ++b) {
                    const Triangle& first = mesh.triangles[triangles[a]];
                    const Triangle& second = mesh.triangles[triangles[b]];
                    const bool coplanar = triangleNormal(mesh, first).dot(triangleNormal(mesh, second)) > 0.9999f;

                    if (coplanar && (*first.uvs[0] - *second.uvs[0]).length() > 0.0f) {
                        return false;
                    }
                }
            }
        }

        return true;
    }

    // How far the mesh reaches along a direction among its points between two heights.
    float reachAlong(const Mesh& mesh, const Vec3& direction, float low, float high) {
        float reach = -1e9f;

        for (const Vec4& point : mesh.vertices) {
            if (point.y >= low && point.y <= high) {
                reach = std::max(reach, Vec3{point.x, point.y, point.z}.dot(direction));
            }
        }

        return reach;
    }

    /*
    * Fins across a face: an odd number, symmetric about the middle, evenly spaced each in the middle of its share of the
    * span (so the outer ones end inside it, never cut at a corner), the spacing as near the pitch as a whole number
    * allows; the middle one major, and every majorEvery-th from it, mirrored. None on a span narrower than the pitch.
    */
    void testFinPlaces() {
        const BastionFins fins{.pitch = 7.0f, .width = 1.5f, .depth = 2.5f, .majorEvery = 4, .majorWidth = 6.0f, .majorDepth = 8.0f};

        for (const float span : {20.0f, 64.0f, 176.0f, 188.0f, 300.5f}) {
            const std::vector<FinPlace> places = finPlaces(span, fins);
            const std::size_t count = places.size();
            assert(count % 2 == 1);
            const float step = span / static_cast<float>(count);
            assert(std::fabs(step - fins.pitch) <= fins.pitch * 0.34f);
            assert(near(places.front().centre, -0.5f * span + 0.5f * step, 1e-3f) && near(places.back().centre, 0.5f * span - 0.5f * step, 1e-3f));
            assert(places[count / 2].major && near(places[count / 2].centre, 0.0f, 1e-3f));

            for (std::size_t index = 0; index < count; ++index) {
                assert(near(places[index].centre, -places[count - 1 - index].centre, 1e-3f));
                assert(places[index].major == places[count - 1 - index].major);
                const bool everyFourth = (index > count / 2 ? index - count / 2 : count / 2 - index) % 4 == 0;
                assert(places[index].major == everyFourth);

                if (index > 0) {
                    assert(near(places[index].centre - places[index - 1].centre, step, 1e-3f));
                }
            }
        }

        assert(finPlaces(5.0f, fins).empty());
        assert(finPlaces(100.0f, BastionFins{}).empty());
    }

    /*
    * The Bastion at the Monolith's place: its foot on the clearing's level, its first tier's front face on the Monolith's
    * line; the glacis wider at the ground and meeting the first tier; tiers touching and stepping in; the head overhanging
    * the top tier on every side and topping out at the height asked.
    */
    void testBastionShape() {
        const float level = 3.0f;
        const ForestBastionSettings settings;
        const BastionShape bastion = monolithBastion(level, settings);
        assert(bastion.foot.y == level && bastion.sink > 0.0f);
        assert(near(bastion.foot.z + 0.5f * bastion.depthShare * bastion.tiers.front().width, forestMonolithPlace.y, 1e-3f));
        assert(near(bastion.foot.x, forestMonolithPlace.x, 1e-3f));
        assert(bastion.tiers.size() == 3 && bastion.tiers.front().width == settings.shaftWidth);
        assert(near(bastion.glacisWidth, settings.glacisShare * settings.shaftWidth, 1e-3f) && bastion.glacisTop == settings.glacisHeight);
        assert(bastion.tiers.front().bottom == bastion.glacisTop);

        for (std::size_t index = 1; index < bastion.tiers.size(); ++index) {
            assert(bastion.tiers[index].bottom == bastion.tiers[index - 1].top && bastion.tiers[index].width < bastion.tiers[index - 1].width);
        }

        assert(bastion.headBottom == bastion.tiers.back().top && near(bastion.headTop, settings.height, 1e-3f));
        assert(bastion.headOverhang == settings.headOverhang && bastion.headTop - bastion.headBottom > 0.05f * settings.height);
        assert(bastionWidthAt(bastion, 0.5f * (bastion.tiers[1].bottom + bastion.tiers[1].top)) == bastion.tiers[1].width);
        assert(bastionWidthAt(bastion, settings.height + 1.0f) == 0.0f);
    }

    /*
    * The Bastion's surface: closed and facing out, every flat face one tone; from the sunk glacis to the head's top; the
    * glacis's foot the widest part; fins standing out of every face of the first tier (the major ones as deep as asked;
    * a fin's corners are at its tier's bottom and top, so the whole tier is searched);
    * corbels under the head reaching its edge; and the head overhanging the top tier. Indexed, each vertex keeps its face's
    * normal, so it is shaded flat.
    */
    void testBastionMesh() {
        const ForestBastionSettings settings;
        const BastionShape bastion = monolithBastion(0.0f, settings);
        const Mesh mesh = bastionMesh(bastion);
        assert(closedAndFacingOut(mesh) && facesShareTheirNumbers(mesh));
        assert(near(highest(mesh), settings.height, 1e-3f) && near(lowest(mesh), -bastion.sink, 1e-3f));

        const float halfGlacis = 0.5f * bastion.glacisWidth;
        assert(near(reachAlong(mesh, Vec3{1.0f, 0.0f, 0.0f}, -1e9f, 1e9f), halfGlacis, 1e-3f));

        const BastionTier& first = bastion.tiers.front();
        const float halfWidth = 0.5f * first.width;
        const float halfDepth = 0.5f * bastion.depthShare * first.width;
        assert(near(reachAlong(mesh, Vec3{0.0f, 0.0f, 1.0f}, first.bottom + 0.01f, first.top + 0.01f), halfDepth + settings.majorFinDepth, 1e-3f));
        assert(near(reachAlong(mesh, Vec3{0.0f, 0.0f, -1.0f}, first.bottom + 0.01f, first.top + 0.01f), halfDepth + settings.majorFinDepth, 1e-3f));
        assert(near(reachAlong(mesh, Vec3{1.0f, 0.0f, 0.0f}, first.bottom + 0.01f, first.top + 0.01f), halfWidth + settings.majorFinDepth, 1e-3f));
        assert(near(reachAlong(mesh, Vec3{-1.0f, 0.0f, 0.0f}, first.bottom + 0.01f, first.top + 0.01f), halfWidth + settings.majorFinDepth, 1e-3f));

        const BastionTier& top = bastion.tiers.back();
        const float headHalfWidth = 0.5f * top.width + bastion.headOverhang;
        assert(near(reachAlong(mesh, Vec3{1.0f, 0.0f, 0.0f}, bastion.headBottom + 1.0f, bastion.headTop), headHalfWidth, 1e-3f));
        // The corbels at 45 degrees: each starts on the top tier's face an overhang below the head (a corner there, at the
        // face) and meets the head's underside at its edge.
        const float topHalfDepth = 0.5f * bastion.depthShare * top.width;
        bool corbelFoot = false;

        for (const Vec4& point : mesh.vertices) {
            corbelFoot = corbelFoot || (near(point.y, bastion.headBottom - bastion.headOverhang, 1e-3f) && near(point.z, topHalfDepth, 0.5f));
        }

        assert(corbelFoot);

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
    * The far towers: ten at the old spires' places, each on the ground under it, 300-800 m tall and 4-8 times as tall as
    * wide, their fins only the major kind, their surfaces closed.
    */
    void testFarTowers() {
        const std::vector<BastionShape> towers = farTowers();
        assert(towers.size() == 10);

        for (const BastionShape& tower : towers) {
            const float width = tower.tiers.front().width;
            assert(tower.headTop >= 300.0f && tower.headTop <= 800.0f);
            assert(tower.headTop / width >= 4.0f && tower.headTop / width <= 8.0f);
            assert(near(tower.foot.y, forestHeight(tower.foot.x, tower.foot.z), 1e-3f));
            assert(tower.fins.majorEvery == 1 && tower.fins.pitch >= 20.0f);
            const Mesh mesh = bastionMesh(tower);
            assert(closedAndFacingOut(mesh) && near(highest(mesh), tower.headTop, 1e-3f));
        }
    }

    /*
    * The bridge: from inside the Monolith's pillar, below its top, straight into the Bastion's west side at the deck's
    * height (inside its face, not through it), level; its surface closed, its deck walkable (the highest points are the
    * parapets', a little above the deck).
    */
    void testBridge() {
        const float level = 3.0f;
        const BastionShape bastion = monolithBastion(level, ForestBastionSettings{});
        const BridgeShape bridge = monolithBridge(level, bastion);
        const PillarPlacement pillar = monolithPillar(level);
        assert((Vec2{bridge.from.x, bridge.from.z} - Vec2{pillar.foot.x, pillar.foot.z}).length() < pillar.radius);
        assert(bridge.from.y < pillar.foot.y + pillar.height && near(bridge.from.y, level + 331.0f, 1e-3f) && bridge.to.y == bridge.from.y);

        const float face = bastion.foot.x - 0.5f * bastionWidthAt(bastion, bridge.to.y - level);
        assert(bridge.to.x > face + 1.0f && bridge.to.x < face + 10.0f);
        assert(std::fabs(bridge.to.z - bastion.foot.z) < 0.5f * bastion.depthShare * bastionWidthAt(bastion, bridge.to.y - level));

        const Mesh mesh = bridgeMesh(bridge);
        assert(closedAndFacingOut(mesh) && facesShareTheirNumbers(mesh));
        assert(highest(mesh) > bridge.from.y + 0.5f && highest(mesh) < bridge.from.y + 2.0f && lowest(mesh) < bridge.from.y - 0.8f * bridge.depth);
    }

    /*
    * The window rows: a visor band of rows on the head, mostly lit; command rows just above each setback; all inside the
    * tower's height, apart from each other, their shares in 0..1; and the openings under 5 % of the wall (military).
    */
    void testWindowRows() {
        const BastionShape bastion = monolithBastion(0.0f, ForestBastionSettings{});
        const std::vector<WindowRow> rows = bastionWindowRows(bastion);
        assert(!rows.empty());
        int visor = 0;
        float opened = 0.0f;

        for (std::size_t index = 0; index < rows.size(); ++index) {
            const WindowRow& row = rows[index];
            assert(row.bottom > bastion.glacisTop && row.bottom + row.height < bastion.headTop && row.height > 0.0f);
            assert(row.lit >= 0.0f && row.lit <= 1.0f && row.cool >= 0.0f && row.cool <= 1.0f && row.fill > 0.0f && row.fill < 1.0f);
            visor += row.bottom > bastion.headBottom ? 1 : 0;
            opened += row.height * row.fill;

            if (index > 0) {
                assert(row.bottom > rows[index - 1].bottom + rows[index - 1].height + 0.5f);
            }
        }

        assert(visor >= 2);

        for (std::size_t tier = 1; tier < bastion.tiers.size(); ++tier) {
            const float setback = bastion.tiers[tier].bottom;
            const bool commanded = std::any_of(rows.begin(), rows.end(), [&](const WindowRow& row) {
                return row.bottom > setback && row.bottom < setback + 12.0f;
            });
            assert(commanded);
        }

        assert(opened / (bastion.headTop - bastion.glacisTop) < 0.05f);
    }

    /*
    * The band strip: from the sunk foot to the head's top at windowStripTexelsPerMetre, one texel tall, linear and kept
    * exact; a row's middle fully covered with its shares, a blank wall uncovered but carrying the nearest row's shares
    * (so mips do not dilute them), a row's edge texel partly covered; the lit share scaled as asked.
    */
    void testWindowBandImage() {
        const BastionShape bastion = monolithBastion(0.0f, ForestBastionSettings{});
        const std::vector<WindowRow> rows = bastionWindowRows(bastion);
        const ImageData strip = windowBandImage(bastion, rows, 1.0f);
        const float span = bastion.headTop + bastion.sink;
        assert(strip.height == 1 && near(static_cast<float>(strip.width), span * windowStripTexelsPerMetre, 1.0f));
        assert(strip.encoding == TextureEncoding::Linear && strip.storage == TextureStorage::Exact);

        const auto texelAt = [&](const ImageData& image, float height) {
            const auto texel = static_cast<std::size_t>((height + bastion.sink) / span * static_cast<float>(image.width));
            return &image.pixels[texel * 4];
        };
        const WindowRow& row = rows.front();
        const std::uint8_t* middle = texelAt(strip, row.bottom + 0.5f * row.height);
        assert(middle[0] == 255 && near(middle[1], 255.0f * row.lit, 1.0f) && near(middle[2], 255.0f * row.cool, 1.0f) && near(middle[3], 255.0f * row.fill, 1.0f));
        const std::uint8_t* blank = texelAt(strip, 0.5f * (bastion.glacisTop + 1.0f));
        assert(blank[0] == 0 && near(blank[1], 255.0f * row.lit, 1.0f) && near(blank[3], 255.0f * row.fill, 1.0f));

        const ImageData half = windowBandImage(bastion, rows, 0.5f);
        assert(near(texelAt(half, row.bottom + 0.5f * row.height)[1], 127.5f * row.lit, 1.0f));

        // A row's edge falls inside a texel (the strip's texels are not aligned to the rows): somewhere it is partly covered.
        bool partly = false;

        for (std::size_t texel = 0; texel < strip.width; ++texel) {
            partly = partly || (strip.pixels[texel * 4] > 0 && strip.pixels[texel * 4] < 255);
        }

        assert(partly);
    }

    /*
    * The light strips: a bar on the front of every major fin of every tier tall enough, standing just proud of it,
    * closed; the far towers' too.
    */
    void testLightStrips() {
        const BastionShape bastion = monolithBastion(0.0f, ForestBastionSettings{});
        const Mesh strips = bastionStripsMesh(bastion);
        assert(closedAndFacingOut(strips) && !strips.triangles.empty());
        const BastionTier& first = bastion.tiers.front();
        const float halfDepth = 0.5f * bastion.depthShare * first.width;
        const float front = reachAlong(strips, Vec3{0.0f, 0.0f, 1.0f}, first.bottom, first.top);
        assert(front > halfDepth + bastion.fins.majorDepth && front < halfDepth + bastion.fins.majorDepth + 0.5f);
        assert(lowest(strips) > first.bottom && highest(strips) < bastion.headBottom);

        for (const BastionShape& tower : farTowers()) {
            assert(closedAndFacingOut(bastionStripsMesh(tower)));
        }
    }

    /*
    * The floods: at the foot of the glacis and on each setback's ledge, six a level (two on the front and back, one on
    * each side), each just out from the wall above it, aimed up it and leaning a little towards it.
    */
    void testFloods() {
        const BastionShape bastion = monolithBastion(0.0f, ForestBastionSettings{});
        const std::vector<FloodPlace> floods = bastionFloods(bastion);
        assert(floods.size() == 6 * bastion.tiers.size());

        for (const FloodPlace& flood : floods) {
            const Vec3 aim = flood.direction.normalized();
            assert(aim.y > 0.9f);
            const Vec2 out{flood.position.x, flood.position.z};
            const Vec2 level{aim.x, aim.z};
            assert(level.dot(out) < 0.0f);
            const bool atFoot = near(flood.position.y, 0.0f, 1e-3f);
            const bool onLedge = std::any_of(bastion.tiers.begin() + 1, bastion.tiers.end(), [&](const BastionTier& tier) {
                return near(flood.position.y, tier.bottom, 1e-3f);
            });
            assert(atFoot || onLedge);

            const float width = atFoot ? bastion.glacisWidth : bastionWidthAt(bastion, flood.position.y + 0.5f);
            assert(std::fabs(flood.position.x) > 0.5f * width - 1e-3f || std::fabs(flood.position.z) > 0.5f * bastion.depthShare * width - 1e-3f);
        }
    }

    /*
    * The obstruction lights: even levels from the head's roof down, never more than the spacing apart (FAA: about 105 m)
    * nor more than one level a spacing, four a level at the section's chamfered corners just outside it, the top level on
    * the roof; the same on a far tower.
    */
    void testBeacons() {
        const BastionShape bastion = monolithBastion(0.0f, ForestBastionSettings{});
        const float spacing = 105.0f;
        const std::vector<Vec3> beacons = bastionBeacons(bastion, spacing);
        const std::size_t levels = static_cast<std::size_t>(std::ceil(bastion.headTop / spacing));
        assert(levels == 6 && beacons.size() == 4 * levels);

        float lowest = bastion.headTop;
        float highest = 0.0f;

        for (std::size_t level = 0; level < levels; ++level) {
            const float height = beacons[4 * level].y;
            lowest = std::min(lowest, height);
            highest = std::max(highest, height);

            for (std::size_t corner = 0; corner < 4; ++corner) {
                const Vec3& beacon = beacons[4 * level + corner];
                assert(near(beacon.y, height, 1e-3f));

                // Outside the section at its height: past its chamfer's line, |x| / half width + |z| / half depth beyond 1 there.
                const float width = bastionWidthAt(bastion, std::min(height, bastion.headTop) - 0.5f);
                const float depth = height > bastion.headBottom ? bastion.depthShare * bastion.tiers.back().width + 2.0f * bastion.headOverhang
                                                                : bastion.depthShare * width;
                const float chamfer = bastion.chamferShare * width;
                assert(std::fabs(beacon.x) + std::fabs(beacon.z) > 0.5f * width + 0.5f * depth - chamfer);
                assert(std::fabs(beacon.x) < 0.5f * width + 2.0f && std::fabs(beacon.z) < 0.5f * depth + 2.0f);
            }

            if (level > 0) {
                assert(std::fabs(height - beacons[4 * (level - 1)].y) <= spacing + 1e-3f);
            }
        }

        assert(highest >= bastion.headTop && highest < bastion.headTop + 2.0f);
        assert(lowest <= spacing + 1e-3f && lowest > 0.0f);

        // Each corner a different one: the four of a level are apart.
        assert((beacons[0] - beacons[1]).length() > 10.0f && (beacons[0] - beacons[3]).length() > 10.0f);

        const BastionShape far = farTowers().front();
        const std::vector<Vec3> farBeacons = bastionBeacons(far, spacing);
        assert(farBeacons.size() == 4 * static_cast<std::size_t>(std::ceil(far.headTop / spacing)));
        assert(bastionBeacons(far, 0.0f).empty());
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
    * Formwork: pours of equal height, as near formworkPour as a whole number fits, all straight; each row's panels equal,
    * every other row's moved half a panel; no hatches; tie holes on a grid fitted to the tile, a seam at each hole's
    * middle and none between holes away from the joints.
    */
    void testFormworkLayout() {
        const SeamLayout layout = formworkLayout(16.5f);
        assert(layout.tile == 16.5f && layout.rows.size() == 6 && layout.hatches.empty());
        assert(near(layout.tiePitch, 16.5f / 28.0f, 1e-6f));

        for (std::size_t row = 0; row < layout.rows.size(); ++row) {
            const SeamRow& seamRow = layout.rows[row];
            assert(near(seamRow.bottom.at, 2.75f * static_cast<float>(row), 1e-4f) && !seamRow.bottom.sawtooth);
            assert(seamRow.splits.size() == 7);
            const float panel = 16.5f / 7.0f;
            assert(near(seamRow.splits.front().at, row % 2 == 0 ? 0.0f : 0.5f * panel, 1e-4f));

            for (std::size_t split = 1; split < seamRow.splits.size(); ++split) {
                assert(near(seamRow.splits[split].at - seamRow.splits[split - 1].at, panel, 1e-4f) && !seamRow.splits[split].sawtooth);
            }
        }

        const float pitch = layout.tiePitch;
        const Vec2 hole{3.5f * pitch, 3.5f * pitch};
        assert(near(seamDistance(layout, hole), 0.0f, 1e-5f));
        assert(near(seamDistance(layout, hole + Vec2{16.5f, -16.5f}), 0.0f, 1e-4f));
        // Halfway between four holes, over a metre from the first row's joints (at 0 and 2.36 m across, 0 and 2.75 m up).
        assert(seamDistance(layout, Vec2{2.0f * pitch, 3.0f * pitch}) > 0.4f);
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
    testFormworkLayout();
    testSeamImages();
    testFinPlaces();
    testBastionShape();
    testBastionMesh();
    testFarTowers();
    testBridge();
    testWindowRows();
    testWindowBandImage();
    testLightStrips();
    testFloods();
    testBeacons();
    testRing();
}
