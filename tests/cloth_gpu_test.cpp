#include "ecs/world.h"
#include "math/transform.h"
#include "physics/collider.h"
#include "render/gpu/cloth_gpu.h"
#include "scene/deforming_mesh.h"
#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

namespace {
    Cloth smallSheet() {
        Cloth cloth = makeCloth(Mesh::grid(3, 2, 3.0f, 2.0f), Mat4::identity(), ClothSettings{});
        addClothPin(cloth, 8);
        addClothPin(cloth, 11);
        return cloth;
    }

    // Each point's position with its inverse mass beside it; pinned points weigh nothing.
    void testPoints() {
        const Cloth cloth = smallSheet();
        const std::vector<Vec4> points = packClothPoints(cloth.particles);
        assert(points.size() == cloth.particles.size());
        for (std::size_t index = 0; index < points.size(); ++index) {
            const Vec3& position = cloth.particles.positions[index];
            assert(points[index].x == position.x && points[index].y == position.y && points[index].z == position.z);
            assert(points[index].w == cloth.particles.inverseMasses[index]);
        }
        assert(points[8].w == 0.0f && points[11].w == 0.0f && points[0].w > 0.0f);
    }

    // The constraints keep their order (the colours') and every number.
    void testConstraints() {
        const Cloth cloth = smallSheet();
        const std::vector<GpuStretch> stretch = packStretch(cloth.stretch);
        assert(stretch.size() == cloth.stretch.size());
        for (std::size_t index = 0; index < stretch.size(); ++index) {
            assert(stretch[index].first == cloth.stretch[index].first && stretch[index].second == cloth.stretch[index].second);
            assert(stretch[index].restLength == cloth.stretch[index].restLength);
        }

        const std::vector<GpuBend> bend = packBend(cloth.bend);
        assert(bend.size() == cloth.bend.size() && !bend.empty());
        for (std::size_t index = 0; index < bend.size(); ++index) {
            const BendConstraint& hinge = cloth.bend[index];
            assert(bend[index].wing0 == hinge.wing0 && bend[index].wing1 == hinge.wing1);
            assert(bend[index].edgeStart == hinge.edgeStart && bend[index].edgeEnd == hinge.edgeEnd);
            assert(bend[index].restAngle == hinge.restAngle && bend[index].stiffnessShare == hinge.stiffnessShare);
            for (int weight = 0; weight < 4; ++weight) {
                assert(bend[index].isometricWeights[weight] == hinge.isometricWeights[weight]);
            }
        }
    }

    void testColliders() {
        const GpuCollider sphere = packCollider(Sphere{Vec3{1.0f, 2.0f, 3.0f}, 0.5f});
        assert(sphere.kind == CLOTH_COLLIDER_SPHERE && sphere.radius == 0.5f && sphere.a.x == 1.0f && sphere.a.z == 3.0f);

        const GpuCollider capsule = packCollider(Capsule{Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 2.0f, 0.0f}, 0.25f});
        assert(capsule.kind == CLOTH_COLLIDER_CAPSULE && capsule.radius == 0.25f && capsule.a.y == 1.0f && capsule.b.y == 2.0f);

        const GpuCollider box = packCollider(Aabb{Vec3{-1.0f, 0.0f, -2.0f}, Vec3{1.0f, 0.5f, 2.0f}});
        assert(box.kind == CLOTH_COLLIDER_BOX && box.a.z == -2.0f && box.b.y == 0.5f);
    }

    // Only the pins' points have a slot, their place in the cloth's list.
    void testPinSlots() {
        const std::vector<std::uint32_t> slots = clothPinSlots(smallSheet());
        assert(slots.size() == 12);
        assert(slots[8] == 0 && slots[11] == 1);
        for (std::size_t index = 0; index < slots.size(); ++index) {
            if (index != 8 && index != 11) {
                assert(slots[index] == clothNoPin);
            }
        }
    }

    /*
    * Every point's tether slots as the cloth holds them; a cloth with nothing pinned, which holds none, still gets a
    * full set of spare slots, so a pass can read any point's.
    */
    void testTethers() {
        const Cloth cloth = smallSheet();
        const std::vector<GpuClothTether> tethers = packClothTethers(cloth);
        assert(tethers.size() == cloth.particles.size() * tethersPerPoint && tethers.size() == cloth.tethers.size());
        for (std::size_t index = 0; index < tethers.size(); ++index) {
            assert(tethers[index].anchor == cloth.tethers[index].anchor && tethers[index].restLength == cloth.tethers[index].restLength);
        }
        assert(tethers[0].anchor == 8 && tethers[0 + 1].anchor == 11);

        const Cloth loose = makeCloth(Mesh::grid(3, 2, 3.0f, 2.0f), Mat4::identity(), ClothSettings{});
        const std::vector<GpuClothTether> spare = packClothTethers(loose);
        assert(spare.size() == loose.particles.size() * tethersPerPoint);
        for (const GpuClothTether& tether : spare) {
            assert(tether.anchor == noTetherAnchor);
        }
    }

    // The small sheet sewn to itself at two pairs of points, as a cloth sewn from pieces is.
    Cloth sewnSheet() {
        const Mesh grid = Mesh::grid(3, 2, 3.0f, 2.0f);
        std::vector<Vec3> start;
        for (const Vec4& vertex : grid.vertices) {
            start.push_back(Vec3{vertex.x, vertex.y, vertex.z});
        }
        ClothSewing sewing;
        sewing.stitches = {ClothStitch{0, 11}, ClothStitch{3, 8}};
        return makeCloth(grid, start, ClothSettings{}, sewing);
    }

    /*
    * A sewn cloth's stitches as the stitch pass reads them, one record each in the cloth's order, and each point's
    * partner across its stitch (the end's pass leaves such a point's hand and tethers to the stitch pass, and pushes it
    * out of the bodies without friction while the stitch is open); clothNoStitch for a point in none. A sheet has no
    * stitches and no partners.
    */
    void testStitches() {
        const Cloth cloth = sewnSheet();
        const std::vector<GpuClothStitch> stitches = packClothStitches(cloth);
        assert(stitches.size() == 2 && stitches[0].first == 0 && stitches[0].second == 11 && stitches[1].first == 3 && stitches[1].second == 8);

        const std::vector<std::uint32_t> partners = clothStitchPartners(cloth);
        assert(partners.size() == cloth.particles.size());
        for (std::uint32_t point = 0; point < partners.size(); ++point) {
            const std::uint32_t expected = point == 0 ? 11u : point == 11 ? 0u : point == 3 ? 8u : point == 8 ? 3u : clothNoStitch;
            assert(partners[point] == expected);
        }

        const Cloth sheet = smallSheet();
        assert(packClothStitches(sheet).empty());
        for (const std::uint32_t partner : clothStitchPartners(sheet)) {
            assert(partner == clothNoStitch);
        }
    }

    // A field body's record: where its first sample is, its spacing, its samples along each axis, where they start in the
    // fields' buffer, and its band.
    void testFieldRecords() {
        SignedDistanceField field;
        field.origin = Vec3{-1.0f, 0.5f, 2.0f};
        field.spacing = 0.005f;
        field.counts = {213, 365, 79};
        field.band = 0.03f;
        const GpuClothField record = packClothField(field, 1234);
        assert(record.origin.x == -1.0f && record.origin.y == 0.5f && record.origin.z == 2.0f && record.origin.w == 0.005f);
        assert(record.counts[0] == 213 && record.counts[1] == 365 && record.counts[2] == 79);
        assert(record.firstSample == 1234 && record.band == 0.03f);

        // A tick names its fields from fieldFirst in the frame's list.
        ClothTickCommands commands = clothTickCommands(smallSheet(), 1.0f / 120.0f, {}, {std::make_shared<const SignedDistanceField>(field)});
        const GpuClothTick tick = packClothTick(commands, 0, 0, 5);
        assert(tick.fieldFirst == 5 && tick.fieldCount == 1);
        assert(packClothTick(clothTickCommands(smallSheet(), 1.0f / 120.0f), 0, 0).fieldCount == 0);
    }

    // A tick's numbers worked out as the CPU solver works them out, so both start from the same bits.
    void testTick() {
        Cloth cloth = smallSheet();
        cloth.settings.substeps = 12;
        cloth.settings.bendingModel = BendingModel::Hinge;
        cloth.pins[1].position = Vec3{2.0f, 1.0f, 0.5f};
        cloth.grab = ClothGrab{3, Vec3{0.0f, -1.0f, 0.25f}};
        const std::vector<Shape> bodies{Sphere{Vec3{}, 1.0f}, Aabb{Vec3{}, Vec3{1.0f, 1.0f, 1.0f}}};

        const ClothTickCommands commands = clothTickCommands(cloth, 1.0f / 120.0f, bodies);
        assert(commands.pinTargets.size() == 2 && commands.pinTargets[1].x == 2.0f);
        assert(commands.grab && commands.grab->particle == 3);
        assert(commands.colliders.size() == 2);

        const GpuClothTick tick = packClothTick(commands, 4, 7);
        const ClothSettings& settings = cloth.settings;
        const float substep = (1.0f / 120.0f) / 12.0f;
        assert(tick.substepSeconds == substep && tick.substeps == 12);
        assert(tick.gravity == settings.gravity);
        assert(tick.stretchCompliance == settings.stretchCompliance / (substep * substep));
        assert(tick.grabCompliance == settings.grabCompliance / (substep * substep));
        assert(tick.perSecondSquared == 1.0f / (substep * substep));
        assert(tick.bendingStiffness == settings.bendingStiffness);
        assert(tick.stretchStiffness == energyStiffness(settings.stretchCompliance));
        assert(tick.grabStiffness == energyStiffness(settings.grabCompliance));
        assert(tick.method == CLOTH_METHOD_XPBD);
        assert(tick.kept == std::exp(-settings.drag * substep));
        assert(tick.thickness == settings.thickness);
        assert(tick.staticFriction == settings.staticFriction && tick.kineticFriction == settings.kineticFriction);
        assert(tick.bendingModel == CLOTH_BEND_HINGE);
        assert(tick.pinFirst == 4 && tick.colliderFirst == 7 && tick.colliderCount == 2);
        assert(tick.grabParticle == 3 && tick.grabTarget.y == -1.0f && tick.grabTarget.z == 0.25f);

        cloth.grab.reset();
        assert(packClothTick(clothTickCommands(cloth, 1.0f / 120.0f), 0, 0).grabParticle == clothNoGrab);

        // Tethers: the stretch they allow, 1 + the slack as the CPU solver adds it, or 0 for none.
        assert(tick.tetherStretch == 1.0f + settings.tetherSlack);
        cloth.settings.tetherSlack = 0.003f;
        assert(packClothTick(clothTickCommands(cloth, 1.0f / 120.0f), 0, 0).tetherStretch == 1.0f + 0.003f);
        cloth.settings.tethers = false;
        assert(packClothTick(clothTickCommands(cloth, 1.0f / 120.0f), 0, 0).tetherStretch == 0.0f);
        cloth.settings.tethers = true;

        // Stitches: how far a gap may close in a substep, the seam speed times the substep as the CPU solver works it out.
        assert(tick.stitchClosingStep == settings.seamSpeed * substep);

        // A perfectly stiff fabric's energy takes the stiffest the energy allows, as on the CPU.
        cloth.settings.method = ClothMethod::VertexBlockDescent;
        cloth.settings.stretchCompliance = 0.0f;
        const GpuClothTick blocks = packClothTick(clothTickCommands(cloth, 1.0f / 120.0f), 0, 0);
        assert(blocks.method == CLOTH_METHOD_BLOCK_DESCENT);
        assert(blocks.stretchStiffness == energyStiffness(0.0f) && blocks.stretchStiffness == 1.0f / stiffestClothCompliance);

        ClothTickCommands none = commands;
        none.settings.substeps = 0;
        bool threw = false;
        try {
            packClothTick(none, 0, 0);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    // Per substep: the prediction, each colour of each kind, the end; no bending passes without bending.
    void testDispatches() {
        const Cloth cloth = smallSheet();
        const std::uint32_t stretchColours = static_cast<std::uint32_t>(cloth.stretchColours.size() - 1);
        const std::uint32_t bendColours = static_cast<std::uint32_t>(cloth.bendColours.size() - 1);
        ClothSettings settings = cloth.settings;
        settings.selfCollision = false;
        const std::uint32_t substeps = static_cast<std::uint32_t>(settings.substeps);
        assert(clothDispatchesPerTick(cloth, settings) == substeps * (2 + stretchColours + bendColours));

        settings.bendingStiffness = 0.0f;
        settings.substeps = 4;
        assert(clothDispatchesPerTick(cloth, settings) == 4 * (2 + stretchColours));

        // Vertex block descent: the prediction, a pass per point colour (bending is in each point's step), the end.
        const std::uint32_t pointColours = static_cast<std::uint32_t>(cloth.vertexColours.colours.size() - 1);
        settings.method = ClothMethod::VertexBlockDescent;
        assert(clothDispatchesPerTick(cloth, settings) == 4 * (2 + pointColours));
        settings.bendingStiffness = 1e-5f;
        assert(clothDispatchesPerTick(cloth, settings) == 4 * (2 + pointColours));

        // This sheet is too small for levels, so asking for them changes nothing.
        assert(cloth.hierarchy.levels.empty());
        settings.hierarchyLevels = 3;
        assert(clothDispatchesPerTick(cloth, settings) == 4 * (2 + pointColours));

        // The tethers are in the end's pass, so they add none, with either method.
        settings.tethers = true;
        assert(clothDispatchesPerTick(cloth, settings) == 4 * (2 + pointColours));
        settings.method = ClothMethod::Xpbd;
        assert(clothDispatchesPerTick(cloth, settings) == 4 * (2 + stretchColours + bendColours));

        // A sewn cloth adds the stitch pass to every substep, with either method.
        const Cloth sewn = sewnSheet();
        assert(clothDispatchesPerTick(sewn, settings) == 4 * (3 + stretchColours + bendColours));

        // XPBD's bending only on its bending substeps: at 15 substeps every fourth, 3, 7, 11 and 14.
        settings.substeps = 15;
        settings.bendingInterval = 4;
        assert(clothDispatchesPerTick(cloth, settings) == 15 * (2 + stretchColours) + 4 * bendColours);

        // Vertex block descent's bending is in each point's step, every substep.
        settings.substeps = 4;
        settings.method = ClothMethod::VertexBlockDescent;
        assert(clothDispatchesPerTick(sewn, settings) == 4 * (3 + pointColours));
    }

    /*
    * Self-collision on the GPU: each substep four more (the contacts, their sums applied, the cut's shares, the cut with
    * the velocities), and each time pairs are found (every selfCollisionInterval-th substep, from the first) eight: the
    * hash's count, its scan in three, its scatter, the points' pairs, the edges' pairs, the pairs' dispatch sizes.
    */
    void testSelfDispatches() {
        const Cloth cloth = smallSheet();
        ClothSettings settings = cloth.settings;
        settings.substeps = 30;
        settings.selfCollisionInterval = 3;
        settings.selfCollision = false;
        const std::uint32_t plain = clothDispatchesPerTick(cloth, settings);

        settings.selfCollision = true;
        assert(clothDispatchesPerTick(cloth, settings) == plain + 30 * clothSelfDispatchesPerSubstep + 10 * clothSelfDispatchesPerSearch);
        assert(clothSelfDispatchesPerSubstep == 4 && clothSelfDispatchesPerSearch == 8);

        // 30 substeps every 4th: found at 0, 4, ..., 28, 8 times.
        settings.selfCollisionInterval = 4;
        assert(clothDispatchesPerTick(cloth, settings) == plain + 30 * clothSelfDispatchesPerSubstep + 8 * clothSelfDispatchesPerSearch);
    }

    /*
    * What the self-collision passes read, packed once: the triangles and edges, each point's representative and its
    * neighbours, the rest shape, the mean edge at rest (the hash's cells are it and two reaches wide), a hash table of a
    * power of two buckets, at least eight per triangle and edge, and room for clothSelfPairsPerFeature pairs per point and
    * edge. A pair's record holds the CPU's pair as it is.
    */
    void testSelfRecords() {
        const Cloth cloth = sewnSheet();
        const GpuClothSelf self = packClothSelf(cloth);
        const ClothSelfTopology& topology = cloth.selfTopology;

        assert(self.triangles.size() == cloth.triangles.size());
        for (std::size_t index = 0; index < cloth.triangles.size(); ++index) {
            assert(self.triangles[index].corners[0] == cloth.triangles[index][0] && self.triangles[index].corners[2] == cloth.triangles[index][2]);
        }
        assert(self.edges.size() == topology.edges.size());
        assert(self.edges.back().first == topology.edges.back()[0] && self.edges.back().second == topology.edges.back()[1]);
        assert(self.joined == topology.joined && self.neighbourStarts == topology.neighbourStarts && self.neighbours == topology.neighbours);
        assert(self.restPositions.size() == cloth.particles.size() && self.restPositions[3].x == topology.restPositions[3].x);
        assert(self.info.triangleCount == self.triangles.size() && self.info.edgeCount == self.edges.size());
        assert(self.info.meanRestEdge == topology.meanRestEdge && topology.meanRestEdge > 0.0f);

        const std::uint32_t items = self.info.triangleCount + self.info.edgeCount;
        assert(self.info.bucketCount >= 8 * items && (self.info.bucketCount & (self.info.bucketCount - 1)) == 0);
        assert(self.info.pairCapacity == clothSelfPairsPerFeature * (static_cast<std::uint32_t>(cloth.particles.size()) + self.info.edgeCount));
        assert(self.info.scanBlocks == (self.info.bucketCount + clothGroupSize - 1) / clothGroupSize);

        const ClothSelfPair pair = pointTrianglePair(cloth.particles.positions, 0, cloth.triangles.back());
        const GpuClothSelfPair packed = packClothSelfPair(pair);
        assert(sizeof(GpuClothSelfPair) == 64);
        assert(packed.points[0] == pair.points[0] && packed.points[3] == pair.points[3]);
        assert(packed.kind == CLOTH_PAIR_POINT_TRIANGLE);
        assert(packed.normalGap.x == pair.normal.x && packed.normalGap.w == pair.gap && packed.heights.z == pair.heights[2]);
        assert(packClothSelfPair(edgeEdgePair(cloth.particles.positions, topology.edges[0], topology.edges.back())).kind == CLOTH_PAIR_EDGE_EDGE);
    }

    // A tick carries self-collision's switch and the reach it looks within, at least the thickness, as the CPU uses it.
    void testSelfTick() {
        Cloth cloth = smallSheet();
        cloth.settings.selfCollisionReach = 0.009f;
        GpuClothTick tick = packClothTick(clothTickCommands(cloth, 1.0f / 120.0f), 0, 0);
        assert(tick.selfCollision == 1 && tick.selfReach == 0.009f);

        cloth.settings.selfCollisionReach = 0.001f;
        tick = packClothTick(clothTickCommands(cloth, 1.0f / 120.0f), 0, 0);
        assert(tick.selfReach == cloth.settings.thickness);

        cloth.settings.selfCollision = false;
        assert(packClothTick(clothTickCommands(cloth, 1.0f / 120.0f), 0, 0).selfCollision == 0);
        assert(sizeof(GpuClothTick) == 128);
    }

    /*
    * Each point's lists for the GPU, in ClothPointConstraints' order, holding what its terms need directly: an edge's
    * other point and rest length, a hinge corner's four points, weights, rest angle, share and which corner the point is,
    * so a pass reads one entry and then the points, not an index, then a record, then the points.
    */
    void testPointLists() {
        Cloth cloth = makeCloth(Mesh::grid(5, 4, 1.0f, 0.8f), Mat4::rotationX(0.3f), ClothSettings{});
        const ClothPointConstraints& lists = cloth.pointConstraints;
        const std::vector<GpuPointEdge> edges = packPointEdges(cloth);
        const std::vector<GpuPointHinge> hinges = packPointHinges(cloth);
        assert(edges.size() == lists.edges.size() && hinges.size() == lists.hinges.size() && !hinges.empty());

        for (std::uint32_t point = 0; point < cloth.particles.size(); ++point) {
            for (std::uint32_t at = lists.edgeStarts[point]; at < lists.edgeStarts[point + 1]; ++at) {
                const StretchConstraint& edge = cloth.stretch[lists.edges[at]];
                assert(edges[at].other == (edge.first == point ? edge.second : edge.first));
                assert(edges[at].restLength == edge.restLength);
            }
            for (std::uint32_t at = lists.hingeStarts[point]; at < lists.hingeStarts[point + 1]; ++at) {
                const BendConstraint& bend = cloth.bend[lists.hinges[at] / 4];
                const GpuPointHinge& entry = hinges[at];
                assert(entry.wing0 == bend.wing0 && entry.wing1 == bend.wing1 && entry.edgeStart == bend.edgeStart && entry.edgeEnd == bend.edgeEnd);
                assert(entry.corner == lists.hinges[at] % 4);
                assert(entry.restAngle == bend.restAngle && entry.stiffnessShare == bend.stiffnessShare);
                for (int weight = 0; weight < 4; ++weight) {
                    assert(entry.isometricWeights[weight] == bend.isometricWeights[weight]);
                }
                const std::uint32_t corners[4]{entry.wing0, entry.wing1, entry.edgeStart, entry.edgeEnd};
                assert(corners[entry.corner] == point);
            }
        }
    }

    /*
    * With levels: the first sweep, the held points, six passes a level (gradient, coarse step, spread, the line search's
    * terms and total, the move), the second sweep. A tick uses as many levels as asked for, up to those the cloth has.
    */
    void testDispatchesWithLevels() {
        ClothSettings settings;
        settings.method = ClothMethod::VertexBlockDescent;
        settings.substeps = 3;
        settings.selfCollision = false;
        const Cloth cloth = makeCloth(Mesh::grid(20, 20, 1.0f, 1.0f), Mat4::identity(), settings);
        const std::uint32_t levels = static_cast<std::uint32_t>(cloth.hierarchy.levels.size());
        assert(levels >= 2);
        const std::uint32_t pointColours = static_cast<std::uint32_t>(cloth.vertexColours.colours.size() - 1);

        settings.hierarchyLevels = 1;
        assert(clothDispatchesPerTick(cloth, settings) == 3 * (2 + 2 * pointColours + 1 + 6));
        settings.hierarchyLevels = maxClothLevels;
        assert(clothDispatchesPerTick(cloth, settings) == 3 * (2 + 2 * pointColours + 1 + 6 * levels));

        // XPBD takes no levels.
        settings.method = ClothMethod::Xpbd;
        assert(clothDispatchesPerTick(cloth, settings) ==
               3 * (2 + static_cast<std::uint32_t>(cloth.stretchColours.size() - 1) + static_cast<std::uint32_t>(cloth.bendColours.size() - 1)));
    }

    /*
    * The levels flattened for the GPU: every level's lists one after another in one buffer each, a record per level
    * saying where its own start, every start counted from the front of its buffer, so a pass needs only its level's
    * record. Each level's lists come back exactly.
    */
    void testLevels() {
        Cloth cloth = makeCloth(Mesh::grid(20, 20, 1.0f, 1.0f), Mat4::identity(), ClothSettings{});
        addClothPin(cloth, 0);
        const ClothHierarchy& hierarchy = cloth.hierarchy;
        const std::size_t pointCount = cloth.particles.size();
        const GpuClothLevels packed = packClothLevels(hierarchy, pointCount);
        assert(packed.records.size() == hierarchy.levels.size() && !packed.records.empty());
        assert(packed.pointLevels.size() == pointCount);
        for (std::size_t point = 0; point < pointCount; ++point) {
            assert(packed.pointLevels[point] == static_cast<std::uint32_t>(hierarchy.pointLevels[point]));
        }

        for (std::size_t index = 0; index < hierarchy.levels.size(); ++index) {
            const ClothLevel& level = hierarchy.levels[index];
            const GpuClothLevel& record = packed.records[index];
            assert(record.pointCount == level.points.size());
            for (std::size_t at = 0; at < level.points.size(); ++at) {
                assert(packed.points[record.pointsFirst + at] == level.points[at]);
                assert(packed.masses[record.pointsFirst + at] == level.masses[at]);

                const std::uint32_t neighbourFirst = packed.neighbourStarts[record.neighbourStartsFirst + at];
                const std::uint32_t neighbourEnd = packed.neighbourStarts[record.neighbourStartsFirst + at + 1];
                assert(neighbourEnd - neighbourFirst == level.neighbourStarts[at + 1] - level.neighbourStarts[at]);
                for (std::uint32_t next = 0; next < neighbourEnd - neighbourFirst; ++next) {
                    assert(packed.neighbours[neighbourFirst + next] == level.neighbours[level.neighbourStarts[at] + next]);
                }

                const std::uint32_t gatherFirst = packed.gatherStarts[record.gatherStartsFirst + at];
                const std::uint32_t gatherEnd = packed.gatherStarts[record.gatherStartsFirst + at + 1];
                assert(gatherEnd - gatherFirst == level.gatherStarts[at + 1] - level.gatherStarts[at]);
                for (std::uint32_t link = 0; link < gatherEnd - gatherFirst; ++link) {
                    const ClothLink& expected = level.gather[level.gatherStarts[at] + link];
                    assert(packed.gather[gatherFirst + link].point == expected.point && packed.gather[gatherFirst + link].weight == expected.weight);
                }
            }
            for (std::size_t point = 0; point < pointCount; ++point) {
                const std::uint32_t spreadFirst = packed.spreadStarts[record.spreadStartsFirst + point];
                const std::uint32_t spreadEnd = packed.spreadStarts[record.spreadStartsFirst + point + 1];
                assert(spreadEnd - spreadFirst == level.spreadStarts[point + 1] - level.spreadStarts[point]);
                for (std::uint32_t link = 0; link < spreadEnd - spreadFirst; ++link) {
                    const ClothLink& expected = level.spread[level.spreadStarts[point] + link];
                    assert(packed.spread[spreadFirst + link].point == expected.point && packed.spread[spreadFirst + link].weight == expected.weight);
                }
            }
        }

        // A cloth with no levels packs none, and every point on the cloth's own level.
        const Cloth small = smallSheet();
        const GpuClothLevels none = packClothLevels(small.hierarchy, small.particles.size());
        assert(none.records.empty() && none.points.empty() && none.spread.empty());
        assert(none.pointLevels.size() == small.particles.size());
        for (const std::uint32_t level : none.pointLevels) {
            assert(level == 0);
        }
        assert(packClothLevels(ClothHierarchy{}, 5).pointLevels.size() == 5);
    }
}

namespace {
    // The same points to the bit.
    bool samePoints(const std::vector<Vec3>& first, const std::vector<Vec3>& second) {
        return first.size() == second.size() && std::memcmp(first.data(), second.data(), first.size() * sizeof(Vec3)) == 0;
    }

    // A world with one hanging sheet drawn through a DeformingMesh, and a ball below it.
    struct QueueWorld {
        World world;
        Entity sheet;
    };

    QueueWorld sheetOverBall() {
        QueueWorld scene;
        scene.sheet = scene.world.create();
        Cloth cloth = makeCloth(Mesh::grid(4, 4, 1.0f, 1.0f), Mat4::translation(0.0f, 1.0f, 0.0f), ClothSettings{});
        addClothPin(cloth, 20);
        scene.world.add(scene.sheet, DeformingMesh{MeshHandle{0}, cloth.particles.positions, cloth.particles.positions});
        scene.world.add(scene.sheet, std::move(cloth));

        const Entity ball = scene.world.create();
        scene.world.add(ball, Transform{});
        scene.world.add(ball, Collider{Shape{Sphere{Vec3{0.0f, 0.0f, 0.0f}, 0.5f}}, nullptr});
        return scene;
    }

    /*
    * On the GPU a tick only queues what stepCloth would have been given: the cloth's points stay as they are (the
    * renderer moves the GPU's copy), its mesh keeps its shape, and the cloth is marked as the GPU's.
    */
    void testGpuTicksQueue() {
        QueueWorld scene = sheetOverBall();
        Cloth& cloth = scene.world.get<Cloth>(scene.sheet);
        const std::vector<Vec3> before = cloth.particles.positions;
        cloth.pins[0].position = Vec3{0.25f, 1.6f, 0.0f};
        cloth.grab = ClothGrab{3, Vec3{1.0f, 0.0f, 0.0f}};

        stepCloths(scene.world, 1.0f / 120.0f, serialJobs(), ClothSolver::Gpu);
        stepCloths(scene.world, 1.0f / 120.0f, serialJobs(), ClothSolver::Gpu);

        assert(cloth.onGpu);
        assert(samePoints(cloth.particles.positions, before));
        assert(samePoints(scene.world.get<DeformingMesh>(scene.sheet).positions, before));
        assert(cloth.gpuTicks.size() == 2);
        for (const ClothTickCommands& tick : cloth.gpuTicks) {
            assert(tick.seconds == 1.0f / 120.0f);
            assert(tick.pinTargets.size() == 1 && tick.pinTargets[0].x == 0.25f);
            assert(tick.grab && tick.grab->particle == 3);
            assert(tick.colliders.size() == 1 && std::holds_alternative<Sphere>(tick.colliders[0]));
            assert(tick.settings.substeps == cloth.settings.substeps);
        }
    }

    // Back on the CPU the cloth steps again, from where its points are, and any queued ticks are dropped.
    void testCpuTicksStepAgain() {
        QueueWorld scene = sheetOverBall();
        Cloth& cloth = scene.world.get<Cloth>(scene.sheet);
        const std::vector<Vec3> before = cloth.particles.positions;
        stepCloths(scene.world, 1.0f / 120.0f, serialJobs(), ClothSolver::Gpu);
        stepCloths(scene.world, 1.0f / 120.0f, serialJobs(), ClothSolver::Cpu);

        assert(!cloth.onGpu);
        assert(cloth.gpuTicks.empty());
        assert(!samePoints(cloth.particles.positions, before));
        assert(samePoints(scene.world.get<DeformingMesh>(scene.sheet).positions, cloth.particles.positions));
    }
}

void testClothGpu() {
    testPoints();
    testConstraints();
    testColliders();
    testPinSlots();
    testTethers();
    testStitches();
    testFieldRecords();
    testTick();
    testDispatches();
    testDispatchesWithLevels();
    testSelfDispatches();
    testSelfRecords();
    testSelfTick();
    testLevels();
    testPointLists();
    testGpuTicksQueue();
    testCpuTicksStepAgain();
}
