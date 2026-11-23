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
        const LianaTree tree{.foot = Vec3{0.0f, 0.0f, 0.0f}};
        return makeLianaShape(hung.from, hung.to, hung.length, 0.03f, 2.0f, tree, &tree);
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

    // The record keeps the trees it hangs from: their feet, each hold's binding and its tree's layer; the far end fixed without a second.
    void testTheRecordKeepsItsTrees() {
        const LianaTree first{.foot = Vec3{1.0f, 2.0f, 3.0f}, .layer = 0, .binding = SwayBinding{.bone = 12, .weight = 0.5f}};
        const LianaTree second{.foot = Vec3{4.0f, 5.0f, 6.0f}, .layer = 1, .binding = SwayBinding{.bone = 3, .weight = 1.0f}};
        const LianaShape both = makeLianaShape(Vec3{1.0f, 20.0f, 3.0f}, Vec3{14.0f, 22.0f, 6.0f}, 16.0f, 0.02f, 2.0f, first, &second);
        assert(both.firstTree[0] == 1.0f && both.firstTree[2] == 3.0f && both.secondTree[0] == 4.0f && both.secondTree[2] == 6.0f);
        // Each anchor's binding to its tree's bones and the tree's layer, for the bones to move it by.
        assert(both.anchors[0] == packSwayBinding(first.binding) && both.anchors[1] == packSwayBinding(second.binding));
        assert(both.anchors[2] == 0u && both.anchors[3] == 1u);
        const LianaShape rope = makeLianaShape(Vec3{1.0f, 20.0f, 3.0f}, Vec3{3.0f, 0.0f, 3.0f}, 21.0f, 0.02f, 2.0f, first, nullptr);
        assert(rope.curve[0] > 0.0f && rope.secondTree[0] == 3.0f && rope.secondTree[1] == 0.0f);
        assert(rope.anchors[1] == swayUnbound && rope.anchors[3] == static_cast<std::uint32_t>(LIANA_NO_TREE));
        // A tree of no layer (not planted by the cells) leaves its anchor unbound.
        const LianaTree unplanted{.foot = Vec3{1.0f, 2.0f, 3.0f}};
        const LianaShape loose = makeLianaShape(Vec3{1.0f, 20.0f, 3.0f}, Vec3{3.0f, 0.0f, 3.0f}, 21.0f, 0.02f, 2.0f, unplanted, nullptr);
        assert(loose.anchors[0] == swayUnbound && loose.anchors[2] == static_cast<std::uint32_t>(LIANA_NO_TREE));
        // Holds one above the other hang straight.
        const LianaShape plumb = makeLianaShape(Vec3{1.0f, 20.0f, 3.0f}, Vec3{1.0f, 0.0f, 3.0f}, 21.0f, 0.02f, 2.0f, first, nullptr);
        assert(plumb.curve[0] == 0.0f && plumb.to[3] == 20.0f);
    }

    // The forest's ivy on a liana: 5 m in its first crown and 4 m in its second, leaves 0.35 m apart there, 1.2 m
    // between, 0.3 m long, thinning from 80 m and gone by 150 m.
    LianaLeaves forestIvy() {
        return LianaLeaves{.firstCrownMetres = 5.0f, .lastCrownMetres = 4.0f, .spacing = 0.35f, .sparseSpacing = 1.2f, .size = 0.3f,
                           .thinFrom = 80.0f, .goneBy = 150.0f, .seed = 0x12345678u};
    }

    LianaShape leafyShapeOf(const Hung& hung) {
        LianaShape shape = shapeOf(hung);
        setLianaLeaves(shape, forestIvy());
        return shape;
    }

    // The crowns' runs are clamped to the length, the far one to what the first leaves; the seed keeps 24 bits.
    void testTheLeavesAreClamped() {
        LianaShape shape = shapeOf(cases().front());
        LianaLeaves leaves = forestIvy();
        leaves.firstCrownMetres = 30.0f;
        leaves.lastCrownMetres = 30.0f;
        setLianaLeaves(shape, leaves);
        assert(shape.leaves[0] == 30.0f && near(shape.leaves[1], 5.0f, 1e-5f));
        assert(shape.leaves[2] == 0.35f && shape.leaves[3] == 1.2f && shape.leafLook[0] == 0.3f);
        assert(shape.leafLook[1] == 80.0f && shape.leafLook[2] == 150.0f && shape.leafLook[3] == static_cast<float>(0x345678u));
    }

    /*
    * N is the crowns' leaves plus the rest's: 9 m / 0.35 + 26 m / 1.2 on the 35 m loop. Without stray the cards stand
    * spacing N / ceil(N) apart inside the crowns and sparse spacing N / ceil(N) between, half a gap in from either end,
    * every card used; with any stray they stay in order and on the liana.
    */
    void testTheLeavesFollowTheirDensity() {
        assert(lianaLeafCount(shapeOf(cases().front())) == 0.0f);
        const LianaShape shape = leafyShapeOf(cases().front());
        const float count = lianaLeafCount(shape);
        assert(near(count, 9.0f / 0.35f + 26.0f / 1.2f, 1e-3f));
        const int cards = static_cast<int>(std::ceil(count));
        const float squeeze = count / static_cast<float>(cards);
        assert(near(lianaLeafPlace(shape, 0.0f, 0.5f), 0.5f * 0.35f * squeeze, 1e-4f));
        assert(near(lianaLeafPlace(shape, static_cast<float>(cards - 1), 0.5f), 35.0f - 0.5f * 0.35f * squeeze, 1e-3f));
        int dense = 0;
        int sparse = 0;

        for (int card = 1; card < cards; ++card) {
            const float before = lianaLeafPlace(shape, static_cast<float>(card - 1), 0.5f);
            const float at = lianaLeafPlace(shape, static_cast<float>(card), 0.5f);
            const bool inCrowns = at < 5.0f || before > 31.0f;
            const bool between = before > 5.0f && at < 31.0f;
            assert(at > before);

            if (inCrowns) {
                assert(near(at - before, 0.35f * squeeze, 1e-4f));
                ++dense;
            } else if (between) {
                assert(near(at - before, 1.2f * squeeze, 1e-4f));
                ++sparse;
            }
        }

        assert(dense > 20 && sparse > 15);

        for (float jitter : {0.0f, 0.999f}) {
            float last = -1.0f;

            for (int card = 0; card < cards; ++card) {
                const float at = lianaLeafPlace(shape, static_cast<float>(card), jitter);
                assert(at > last && at >= 0.0f && at <= 35.0f);
                last = at;
            }
        }
    }

    /*
    * Near, every card whole; past the thinning band every other card gone and the rest sqrt 2 long (the same area);
    * gone at the last distance; none past the count; no thinning without its distances.
    */
    void testTheLeavesThinWithDistance() {
        LianaShape shape = leafyShapeOf(cases().front());
        const float cards = std::ceil(lianaLeafCount(shape));
        assert(lianaLeafGrowth(shape, 0.0f, 10.0f) == 1.0f && lianaLeafGrowth(shape, 1.0f, 80.0f) == 1.0f);
        assert(lianaLeafGrowth(shape, 0.0f, 100.0f) > 1.41f && lianaLeafGrowth(shape, 1.0f, 100.0f) == 0.0f);
        assert(near(lianaLeafGrowth(shape, 2.0f, 90.0f), 1.0f + 0.5f * (std::sqrt(2.0f) - 1.0f), 1e-5f));
        assert(near(lianaLeafGrowth(shape, 3.0f, 90.0f), 0.5f, 1e-5f));
        assert(near(lianaLeafGrowth(shape, 4.0f, 150.0f - 0.125f * 150.0f), 0.5f * std::sqrt(2.0f), 1e-5f));
        assert(lianaLeafGrowth(shape, 4.0f, 150.0f) == 0.0f && lianaLeafGrowth(shape, 4.0f, 400.0f) == 0.0f);
        assert(lianaLeafGrowth(shape, cards, 1.0f) == 0.0f && lianaLeafGrowth(shape, cards - 1.0f, 1.0f) == 1.0f);
        shape.leafLook[1] = 0.0f;
        shape.leafLook[2] = 0.0f;
        assert(lianaLeafGrowth(shape, 1.0f, 5000.0f) == 1.0f);
    }

    /*
    * Each card's stem on the tube's surface at its place, square to the line; the card a leaf long from stem to tip
    * and half as wide again, pointing out and drooping, its normal and tangent unit and square; its picture one atlas
    * cell; its share along and arm the tube's there. A card grown to nothing is its stem alone.
    */
    void testTheCardsSitOnTheLiana() {
        const Vec3 nearEye{0.0f, 20.0f, 0.0f};

        for (const Hung& hung : cases()) {
            const LianaShape shape = leafyShapeOf(hung);
            const int cards = static_cast<int>(std::ceil(lianaLeafCount(shape)));

            for (int card = 0; card < cards; card += 3) {
                const LianaLeafVertex stem = lianaLeafVertex(shape, static_cast<float>(card), 0.5f, 0.0f, nearEye);
                const LianaLeafVertex left = lianaLeafVertex(shape, static_cast<float>(card), 0.0f, 0.0f, nearEye);
                const LianaLeafVertex right = lianaLeafVertex(shape, static_cast<float>(card), 1.0f, 0.0f, nearEye);
                const LianaLeafVertex tip = lianaLeafVertex(shape, static_cast<float>(card), 0.5f, 1.0f, nearEye);
                const LianaPoint centre = lianaCentre(shape, stem.along);
                assert((stem.offset - stem.stem).length() < 1e-5f && (tip.stem - stem.stem).length() == 0.0f);
                assert(near((stem.stem - centre.offset).length(), shape.from[3], 1e-4f));
                assert(std::abs((stem.stem - centre.offset).dot(centre.along)) < 1e-4f);
                assert(near((tip.offset - stem.offset).length(), 0.3f, 1e-4f) && near((right.offset - left.offset).length(), 0.45f, 1e-4f));
                // Drooping: pointing further down than straight out from the line.
                const Vec3 radial = (stem.stem - centre.offset).normalized();
                assert((tip.offset - stem.offset).normalized().y <= radial.y + 1e-4f);
                assert(near(stem.normal.length(), 1.0f, 1e-5f) && near(stem.tangent.length(), 1.0f, 1e-5f));
                assert(std::abs(stem.normal.dot(stem.tangent)) < 1e-5f && std::abs(stem.normal.dot(tip.offset - stem.offset)) < 1e-4f);
                assert(((right.offset - left.offset).normalized() - stem.tangent).length() < 1e-4f);
                assert(near(stem.arm, lianaVertex(shape, stem.along, 0.0f, 1.0f, 0.0f).arm, 1e-4f));
                // One cell of the atlas, two across and three up.
                const float column = std::floor(left.uv.x * 2.0f);
                const float row = std::floor(left.uv.y * 3.0f);
                assert(near(left.uv.x, column / 2.0f, 1e-6f) && near(left.uv.y, row / 3.0f, 1e-6f));
                assert(near(right.uv.x - left.uv.x, 0.5f, 1e-6f) && near(tip.uv.y - stem.uv.y, 1.0f / 3.0f, 1e-6f));
            }

            // Past the count, and past the last distance, a card is its stem alone.
            const LianaLeafVertex past = lianaLeafVertex(shape, static_cast<float>(cards), 1.0f, 1.0f, nearEye);
            const LianaLeafVertex far = lianaLeafVertex(shape, 0.0f, 1.0f, 1.0f, Vec3{5000.0f, 0.0f, 0.0f});
            assert((past.offset - past.stem).length() == 0.0f && (far.offset - far.stem).length() == 0.0f);
        }
    }

    /*
    * The template: 4 corners and 2 triangles a card, each corner's card and place in its uv, its position in the box
    * the pose maps. Bent onto a liana its triangles face their normals; the leaf pose's box holds every card grown to
    * its longest; the cards a liana needs pick the smallest template holding them.
    */
    void testTheLeafTemplate() {
        const IndexedMesh cards = lianaLeafTemplateMesh(16);
        assert(cards.vertices.size() == 64u && cards.indices.size() == 96u);

        for (std::size_t index = 0; index < cards.vertices.size(); ++index) {
            const MeshVertex& vertex = cards.vertices[index];
            const float card = std::floor(vertex.uv.x * 0.5f);
            assert(card == static_cast<float>(index / 4) && vertex.uv.x - 2.0f * card >= 0.0f && vertex.uv.x - 2.0f * card <= 1.0f);
            assert(vertex.position.x >= 0.0f && vertex.position.x <= 1.0f && std::abs(vertex.position.y) <= 1.0f && std::abs(vertex.position.z) <= 1.0f);
        }

        const Vec3 thinnedEye{0.0f, 20.0f, -120.0f};

        for (const Hung& hung : cases()) {
            const LianaShape shape = leafyShapeOf(hung);
            const IndexedMesh leaves = lianaLeafTemplateMesh(static_cast<int>(lianaLeafTemplateCards[lianaLeafTemplate(lianaLeafCount(shape))]));
            const Transform pose = lianaLeafPose(shape);
            const Vec3 run = lianaRun(shape);
            const Vec3 side{-run.z, 0.0f, run.x};
            IndexedMesh bent = leaves;

            for (MeshVertex& vertex : bent.vertices) {
                const float card = std::floor(vertex.uv.x * 0.5f);
                const LianaLeafVertex placed = lianaLeafVertex(shape, card, vertex.uv.x - 2.0f * card, vertex.uv.y, fromOf(shape));
                vertex.position = placed.offset;
                vertex.normal = placed.normal;

                for (const Vec3& eye : {fromOf(shape), thinnedEye}) {
                    const LianaLeafVertex grown = lianaLeafVertex(shape, card, vertex.uv.x - 2.0f * card, vertex.uv.y, eye);
                    const Vec3 local = fromOf(shape) + grown.offset - pose.position;
                    assert(local.dot(run) >= -1e-3f && local.dot(run) <= pose.scale.x + 1e-3f);
                    assert(std::abs(local.y) <= pose.scale.y + 1e-3f && std::abs(local.dot(side)) <= pose.scale.z + 1e-3f);
                }
            }

            // Shown cards only: the rest collapse to points, which have no facing.
            IndexedMesh shown;
            shown.vertices = bent.vertices;

            for (std::size_t corner = 0; corner < bent.indices.size(); corner += 6) {
                if (std::floor(bent.vertices[bent.indices[corner]].uv.x * 0.5f) < std::ceil(lianaLeafCount(shape))) {
                    shown.indices.insert(shown.indices.end(), bent.indices.begin() + static_cast<std::ptrdiff_t>(corner),
                                         bent.indices.begin() + static_cast<std::ptrdiff_t>(corner) + 6);
                }
            }

            assert(!shown.indices.empty() && trianglesFaceTheirNormals(shown));
        }

        assert(lianaLeafTemplate(0.0f) == 0 && lianaLeafTemplate(16.0f) == 0 && lianaLeafTemplate(16.2f) == 1);
        assert(lianaLeafTemplate(100.0f) == 3 && lianaLeafTemplate(1000.0f) == lianaLeafTemplateCards.size() - 1);
    }

    // A draw names its record plus 1, its ivy with the ivy bit.
    void testTheLianaNames() {
        assert(lianaRecordOf(5u) == 4u && !lianaDrawsIvy(5u));
        assert(lianaRecordOf(5u | LIANA_IVY_BIT) == 4u && lianaDrawsIvy(5u | LIANA_IVY_BIT));
    }
}

void testLianaShape() {
    testTheCentreLineIsTheCatenary();
    testTheRings();
    testTheTemplateFacesOut();
    testThePoseHoldsTheLiana();
    testTheRecordKeepsItsTrees();
    testTheLeavesAreClamped();
    testTheLeavesFollowTheirDensity();
    testTheLeavesThinWithDistance();
    testTheCardsSitOnTheLiana();
    testTheLeafTemplate();
    testTheLianaNames();
}
