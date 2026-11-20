#include "games/forest/island_hangings.h"
#include "games/forest/lianas.h"
#include "scene/liana_shape.h"
#include "scene/wind.h"

#include "forest_helpers.h"

#include <cassert>
#include <cmath>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    // A liana's test case: its holds and length.
    struct Hung {
        Vec3 from;
        Vec3 to;
        float length = 0.0f;
    };

    /*
    * The kinds the forest hangs: a loop sagging a quarter between crowns, one nearly taut (1% slack, a large), a rope
    * steeply down to the ground 3 m off, a loop rising to a higher crown, and one straight down (no run: straight).
    */
    std::vector<Hung> cases() {
        return {
            Hung{Vec3{2.0f, 20.0f, 1.0f}, Vec3{26.0f, 21.0f, -6.0f}, 35.0f},
            Hung{Vec3{0.0f, 25.0f, 0.0f}, Vec3{18.0f, 24.0f, 4.0f}, 18.6f},
            Hung{Vec3{5.0f, 22.0f, 5.0f}, Vec3{7.0f, -0.2f, 7.5f}, 23.0f},
            Hung{Vec3{-3.0f, 15.0f, 2.0f}, Vec3{12.0f, 27.0f, 9.0f}, 26.0f},
            Hung{Vec3{1.0f, 18.0f, 1.0f}, Vec3{1.0f, 0.0f, 1.0f}, 18.5f},
        };
    }

    LianaShape shapeOf(const Hung& hung) {
        const LianaTree tree{Vec3{0.0f, 0.0f, 0.0f}, 30.0f, 1.0f};
        return makeLianaShape(hung.from, hung.to, hung.length, 0.03f, 2.0f, tree, &tree, 0.2f);
    }

    Vec3 fromOf(const LianaShape& shape) {
        return Vec3{shape.from[0], shape.from[1], shape.from[2]};
    }

    /*
    * The centre line is the forest's catenary (catenaryStrand, in doubles, evenly along its length) to within 2 mm in
    * floats, nearly taut too, ending on the far hold; its share along and arm below the chord are hangBetween's, its
    * deepest sag (the swing's length) that of the strand's points.
    */
    void testTheCentreLineIsTheCatenary() {
        for (const Hung& hung : cases()) {
            const LianaShape shape = shapeOf(hung);
            HangingStrand strand = catenaryStrand(hung.from, hung.to, hung.length, 0.03f);
            hangBetween(strand, windAnchorFixed, windAnchorFixed);
            const std::size_t segments = strand.points.size() - 1;

            for (std::size_t point = 0; point <= segments; ++point) {
                const float share = static_cast<float>(point) / static_cast<float>(segments);
                const LianaPoint centre = lianaCentre(shape, share);
                assert((fromOf(shape) + centre.offset - strand.points[point]).length() < 2e-3f);
                const LianaVertex vertex = lianaVertex(shape, share, 0.0f, 1.0f, 0.0f);
                // Along: the strand's share by its chords' lengths, a hair short of the arc's on a curve.
                assert(near(vertex.along, strand.hanging[point].along, 1e-3f) && near(vertex.arm, strand.hanging[point].arm, 2e-3f));
                assert(near(shape.curve[2], strand.hanging[point].swingLength, 1e-2f));
                assert(near(centre.along.length(), 1.0f, 1e-5f));
            }
        }
    }

    /*
    * Each ring stands square to the line, its normal and tangent unit and square to each other, the bitangent
    * (normal x tangent) along the line (+v, appendTube's), the surface a radius out; v the bark's tiles along it.
    */
    void testTheRings() {
        for (const Hung& hung : cases()) {
            const LianaShape shape = shapeOf(hung);

            for (float share : {0.0f, 0.3f, 0.71f, 1.0f}) {
                const LianaPoint centre = lianaCentre(shape, share);

                for (float turn : {0.0f, 0.25f, 0.6f}) {
                    const float angle = turn * 6.2831853f;
                    const LianaVertex vertex = lianaVertex(shape, share, turn, std::cos(angle), std::sin(angle));
                    assert(near(vertex.normal.length(), 1.0f, 1e-5f) && near(vertex.tangent.length(), 1.0f, 1e-5f));
                    assert(std::abs(vertex.normal.dot(centre.along)) < 1e-5f && std::abs(vertex.normal.dot(vertex.tangent)) < 1e-5f);
                    assert((vertex.normal.cross(vertex.tangent) - centre.along).length() < 1e-4f);
                    assert(((vertex.offset - centre.offset) - vertex.normal * shape.from[3]).length() < 1e-5f);
                    assert(near(vertex.uv.x, turn, 0.0f) && near(vertex.uv.y, share * shape.to[3] / shape.curve[3], 1e-4f));
                }
            }
        }
    }

    // The template, and every liana bent from it, faces out of its tube; its rings and sides as asked.
    void testTheTemplateFacesOut() {
        const IndexedMesh tube = lianaTemplateMesh(16, 4);
        assert(tube.vertices.size() == 17u * 5u && tube.indices.size() == 16u * 4u * 6u);
        assert(trianglesFaceTheirNormals(tube));

        for (const Hung& hung : cases()) {
            const LianaShape shape = shapeOf(hung);
            IndexedMesh bent = tube;

            for (MeshVertex& vertex : bent.vertices) {
                const LianaVertex placed = lianaVertex(shape, vertex.position.x, vertex.uv.x, vertex.position.y, vertex.position.z);
                vertex.position = placed.offset;
                vertex.normal = placed.normal;
            }

            assert(trianglesFaceTheirNormals(bent));
        }
    }

    // The pose's box (the template's [0, 1] x [-1, 1] x [-1, 1] placed) holds every vertex of the liana's tube.
    void testThePoseHoldsTheLiana() {
        const IndexedMesh tube = lianaTemplateMesh(64, 6);

        for (const Hung& hung : cases()) {
            const LianaShape shape = shapeOf(hung);
            const Transform pose = lianaPose(shape);
            const Vec3 run = lianaRun(shape);
            const Vec3 side{-run.z, 0.0f, run.x};

            for (const MeshVertex& vertex : tube.vertices) {
                const Vec3 world = fromOf(shape) + lianaVertex(shape, vertex.position.x, vertex.uv.x, vertex.position.y, vertex.position.z).offset;
                const Vec3 local = world - pose.position;
                assert(local.dot(run) >= -1e-3f && local.dot(run) <= pose.scale.x + 1e-3f);
                assert(std::abs(local.y) <= pose.scale.y + 1e-3f && std::abs(local.dot(side)) <= pose.scale.z + 1e-3f);
            }

            // Its matrix maps the template's corner along the run.
            const Vec3 corner = transformPoint(pose.getMatrix(), Vec3{1.0f, 0.0f, 0.0f}) - pose.position;
            assert((corner - run * pose.scale.x).length() < 1e-3f);
        }
    }

    // The record keeps the trees it hangs from: the far end fixed (scale 0) without a second.
    void testTheRecordKeepsItsTrees() {
        const LianaTree first{Vec3{1.0f, 2.0f, 3.0f}, 28.0f, 1.2f};
        const LianaTree second{Vec3{4.0f, 5.0f, 6.0f}, 31.0f, 0.9f};
        const LianaShape both = makeLianaShape(Vec3{1.0f, 20.0f, 3.0f}, Vec3{14.0f, 22.0f, 6.0f}, 16.0f, 0.02f, 2.0f, first, &second, 0.3f);
        assert(both.firstTree[0] == 1.0f && both.firstTree[3] == 28.0f && both.trees[0] == 1.2f);
        assert(both.secondTree[2] == 6.0f && both.secondTree[3] == 31.0f && both.trees[1] == 0.9f && both.trees[2] == 0.3f);
        const LianaShape rope = makeLianaShape(Vec3{1.0f, 20.0f, 3.0f}, Vec3{3.0f, 0.0f, 3.0f}, 21.0f, 0.02f, 2.0f, first, nullptr, 0.3f);
        assert(rope.trees[1] == 0.0f && rope.curve[0] > 0.0f);
        // Holds one above the other hang straight.
        const LianaShape plumb = makeLianaShape(Vec3{1.0f, 20.0f, 3.0f}, Vec3{1.0f, 0.0f, 3.0f}, 21.0f, 0.02f, 2.0f, first, nullptr, 0.3f);
        assert(plumb.curve[0] == 0.0f && plumb.to[3] == 20.0f);
    }
}

void testLianaShape() {
    testTheCentreLineIsTheCatenary();
    testTheRings();
    testTheTemplateFacesOut();
    testThePoseHoldsTheLiana();
    testTheRecordKeepsItsTrees();
}
