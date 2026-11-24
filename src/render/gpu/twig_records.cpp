#include "render/gpu/twig_records.h"

#include "render/gpu/bone_poses.h"
#include "render/gpu/bone_records.h"
#include "render/gpu/column_major.h"
#include "render/gpu/mesh_cluster_culling.h"
#include "core/half_float.h"
#include "math/quat.h"
#include "scene/twig_far_levels.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

std::uint32_t packTwigBinding(const TwigInstance& twig) {
    return twig.binding.bone < 0 ? swayUnbound : packSwayBinding(twig.binding);
}

namespace {
    // A value in -1..1 as a snorm16 (GLSL's packSnorm2x16: rounded, clamped) and back (unpackSnorm2x16).
    std::uint32_t snorm16Of(float value) {
        return static_cast<std::uint32_t>(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f))));
    }

    float snorm16Value(std::uint32_t bits) {
        return std::clamp(static_cast<float>(static_cast<std::int16_t>(static_cast<std::uint16_t>(bits & 0xffffu))) / 32767.0f, -1.0f, 1.0f);
    }

    // A one-row leaf's corners in its cell's units (across the midrib, up it), vertex by vertex.
    Vec2 farLeafCorner(const GpuFarLeafShape& shape, std::uint32_t vertex) {
        switch (vertex) {
        case 0:
            return Vec2{0.0f, shape.shape[0]};
        case 1:
            return Vec2{-shape.shape[3], shape.shape[2]};
        case 2:
            return Vec2{0.0f, shape.shape[2]};
        case 3:
            return Vec2{shape.right, shape.shape[2]};
        default:
            return Vec2{0.0f, shape.shape[1]};
        }
    }
}

GpuFarLeaf packFarLeaf(const FarLeaf& leaf) {
    if (leaf.cell > 0xffu || leaf.number > 0xffu) {
        throw std::invalid_argument("A far leaf's cell or number does not fit a byte");
    }

    const Vec3 across = leaf.towardsTip.cross(leaf.facing);
    const Quat turn = Quat::fromBasis(across, leaf.towardsTip, leaf.facing);
    GpuFarLeaf packed;
    packed.foot[0] = leaf.foot.x;
    packed.foot[1] = leaf.foot.y;
    packed.foot[2] = leaf.foot.z;
    packed.turn[0] = snorm16Of(turn.x) | (snorm16Of(turn.y) << 16);
    packed.turn[1] = snorm16Of(turn.z) | (snorm16Of(turn.w) << 16);
    packed.sideCellNumber = static_cast<std::uint32_t>(floatToHalf(leaf.side)) | (leaf.cell << 16) | (leaf.number << 24);
    packed.twig = leaf.twig;
    return packed;
}

GpuFarLeafShape packFarLeafShape(const FarLeafShape& shape, float foldRadians, float curl) {
    GpuFarLeafShape packed;
    packed.shape[0] = shape.base;
    packed.shape[1] = shape.tip;
    packed.shape[2] = shape.up;
    packed.shape[3] = shape.left;
    packed.right = shape.right;
    packed.raise = std::tan(foldRadians);
    packed.curl = curl;
    packed.uvRect[0] = shape.uvMin.x;
    packed.uvRect[1] = shape.uvMin.y;
    packed.uvRect[2] = shape.uvMax.x;
    packed.uvRect[3] = shape.uvMax.y;
    return packed;
}

std::array<std::uint32_t, 3> farLeafTriangle(std::uint32_t triangle) {
    constexpr std::uint32_t corners[FAR_LEAF_TRIANGLES][3] = {{0, 2, 1}, {0, 3, 2}, {1, 2, 4}, {2, 3, 4}};
    const std::uint32_t* chosen = corners[std::min<std::uint32_t>(triangle, FAR_LEAF_TRIANGLES - 1)];
    return {chosen[0], chosen[1], chosen[2]};
}

/*
* Every corner's position is made (five, cheap) so a vertex's normal can sum the triangles it touches, as
* appendModelledLeaf sums them; on the GPU each thread makes its own vertex the same way.
*/
FarLeafVertex farLeafVertex(const GpuFarLeaf& leaf, const GpuFarLeafShape& shape, std::uint32_t vertex) {
    const Quat turn = Quat{snorm16Value(leaf.turn[0]), snorm16Value(leaf.turn[0] >> 16), snorm16Value(leaf.turn[1]), snorm16Value(leaf.turn[1] >> 16)}.normalized();
    const Vec3 across = turn.rotate(Vec3{1.0f, 0.0f, 0.0f});
    const Vec3 towardsTip = turn.rotate(Vec3{0.0f, 1.0f, 0.0f});
    const Vec3 facing = turn.rotate(Vec3{0.0f, 0.0f, 1.0f});
    const Vec3 foot{leaf.foot[0], leaf.foot[1], leaf.foot[2]};
    const float side = halfToFloat(static_cast<std::uint16_t>(leaf.sideCellNumber & 0xffffu));
    const float number = static_cast<float>(leaf.sideCellNumber >> 24);
    const float length = std::max(shape.shape[1] - shape.shape[0], 1e-6f);
    const std::uint32_t chosen = std::min<std::uint32_t>(vertex, FAR_LEAF_VERTICES - 1);
    Vec3 points[FAR_LEAF_VERTICES];

    for (std::uint32_t corner = 0; corner < FAR_LEAF_VERTICES; ++corner) {
        const Vec2 at = farLeafCorner(shape, corner);
        const float t = std::clamp((at.y - shape.shape[0]) / length, 0.0f, 1.0f);
        const float z = std::abs(at.x) * shape.raise - shape.curl * t * t;
        points[corner] = foot + (across * at.x + towardsTip * at.y + facing * z) * side;
    }

    Vec3 normal{};

    for (std::uint32_t triangle = 0; triangle < FAR_LEAF_TRIANGLES; ++triangle) {
        const std::array<std::uint32_t, 3> corners = farLeafTriangle(triangle);

        if (corners[0] == chosen || corners[1] == chosen || corners[2] == chosen) {
            normal = normal + (points[corners[1]] - points[corners[0]]).cross(points[corners[2]] - points[corners[0]]);
        }
    }

    const Vec2 at = farLeafCorner(shape, chosen);
    const float uMin = shape.uvRect[0] + number;
    const float uMax = shape.uvRect[2] + number;
    FarLeafVertex made;
    made.position = points[chosen];
    made.normal = normal.lengthSquared() > 1e-20f ? normal.normalized() : facing;
    made.uv = Vec2{uMin + (uMax - uMin) * (0.5f + at.x), shape.uvRect[1] + (shape.uvRect[3] - shape.uvRect[1]) * at.y};
    made.tangent = Vec4{across.x, across.y, across.z, 1.0f};
    return made;
}

GpuTwig packTwig(const TwigInstance& twig) {
    GpuTwig packed;
    const Vec3 columns[3] = {twig.across * twig.scale, twig.towardsTip * twig.scale, twig.facing * twig.scale};

    for (int column = 0; column < 3; ++column) {
        packed.rows[0][column] = columns[column].x;
        packed.rows[1][column] = columns[column].y;
        packed.rows[2][column] = columns[column].z;
    }

    packed.rows[0][3] = twig.foot.x;
    packed.rows[1][3] = twig.foot.y;
    packed.rows[2][3] = twig.foot.z;
    packed.lean[0] = twig.lean.x;
    packed.lean[1] = twig.lean.y;
    packed.lean[2] = twig.lean.z;
    packed.prototype = twig.prototype;
    return packed;
}

GpuTwigBrick packTwigBrick(const TwigBrick& brick) {
    GpuTwigBrick packed;
    packed.sphere[0] = brick.centre.x;
    packed.sphere[1] = brick.centre.y;
    packed.sphere[2] = brick.centre.z;
    packed.sphere[3] = brick.radius;
    packed.firstTwig = brick.firstTwig;
    packed.twigCount = brick.twigCount;
    return packed;
}

// Each group's clusters must run together, group by group: the chain's level k from its first cluster to the next's.
std::vector<GpuTwigLevel> packTwigLevels(const MeshClusters& clusters, std::uint32_t firstPoolCluster) {
    std::vector<GpuTwigLevel> levels(clusters.groups.size());
    std::uint32_t expected = 0;

    for (std::uint32_t number = 0; number < clusters.clusters.size(); ++number) {
        const MeshCluster& cluster = clusters.clusters[number];

        if (cluster.group != expected && cluster.group != expected + 1) {
            throw std::invalid_argument("A twig prototype's clusters are not a chain of levels");
        }

        if (cluster.group == expected + 1 || number == 0) {
            expected = cluster.group;
            levels[expected].firstCluster = firstPoolCluster + number;
        }

        if (clusters.groups[cluster.group].level != cluster.group) {
            throw std::invalid_argument("A twig prototype's clusters are not a chain of levels");
        }

        levels[expected].triangleCount += cluster.triangleCount;
        ++levels[expected].clusterCount;
    }

    for (std::size_t level = 0; level < levels.size(); ++level) {
        const MeshClusterGroup& group = clusters.groups[level];
        levels[level].sphere[0] = group.centre.x;
        levels[level].sphere[1] = group.centre.y;
        levels[level].sphere[2] = group.centre.z;
        levels[level].sphere[3] = group.radius;
        levels[level].error = group.error;

        if (levels[level].clusterCount == 0 || levels[level].clusterCount > twigMostLevelClusters) {
            throw std::invalid_argument("A twig prototype's level has no clusters or more than TWIG_MOST_LEVEL_CLUSTERS");
        }
    }

    if (levels.size() > twigMostLevels) {
        throw std::invalid_argument("A twig prototype has more than TWIG_MOST_LEVELS levels");
    }

    return levels;
}

// Level by level the records' runs (their groups' levels from 0 up, each run together), each its group's sphere and error.
std::vector<GpuTwigLevel> packTwigLevels(std::span<const GpuMeshCluster> records, std::uint32_t firstPoolCluster) {
    std::vector<GpuTwigLevel> levels;

    for (std::uint32_t number = 0; number < records.size(); ++number) {
        const GpuMeshCluster& cluster = records[number];

        if (cluster.level == levels.size()) {
            GpuTwigLevel level;
            std::copy(cluster.parentSphere, cluster.parentSphere + 4, level.sphere);
            level.error = cluster.parentError;
            level.firstCluster = firstPoolCluster + number;
            levels.push_back(level);
        } else if (levels.empty() || cluster.level + 1 != levels.size()) {
            throw std::invalid_argument("A twig prototype's clusters are not a chain of levels");
        }

        ++levels.back().clusterCount;
        levels.back().triangleCount += cluster.counts >> MESH_CLUSTER_TRIANGLE_SHIFT;
    }

    for (const GpuTwigLevel& level : levels) {
        if (level.clusterCount > twigMostLevelClusters) {
            throw std::invalid_argument("A twig prototype's level has more than TWIG_MOST_LEVEL_CLUSTERS clusters");
        }
    }

    if (levels.size() > twigMostLevels) {
        throw std::invalid_argument("A twig prototype has more than TWIG_MOST_LEVELS levels");
    }

    return levels;
}

namespace {
    // A twig's matrix: its rows as GpuTwig holds them, the last (0 0 0 1).
    Mat4 twigMatrix(const GpuTwig& twig) {
        Mat4 matrix = Mat4::identity();

        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 4; ++column) {
                matrix.values[row][column] = twig.rows[row][column];
            }
        }

        return matrix;
    }

    // Whether a sphere (plant or prototype frame) placed by an instance is kept: always where the part is seen whole, else
    // its posed sphere's or its own placed one's in the frustum.
    bool twigSphereKept(const float sphere[4], const GpuInstance& placedBy, const GpuInstance& part, std::uint32_t entry, const GpuCullRecord& record,
                        const GpuCameraCull& view, const MeshClusterPose& pose) {
        if (meshClusterSeenWhole(part, entry, record, view, pose)) {
            return true;
        }

        const std::array<float, 4> placed = pose.posed ? pose.sphere : meshClusterPlacedSphere(sphere, placedBy);
        return sphereInFrustum(view.frustum, placed.data());
    }
}

// The twig's scale is its across column's length (its frame is unit, its scale uniform).
GpuInstance twigInstance(const GpuInstance& part, const GpuTwig& twig) {
    const Mat4 matrix = twigMatrix(twig);
    Mat4 turn = matrix;
    turn.values[0][3] = 0.0f;
    turn.values[1][3] = 0.0f;
    turn.values[2][3] = 0.0f;
    GpuInstance instance = part;
    writeColumnMajor(readColumnMajor(part.model) * matrix, instance.model);
    writeColumnMajor(readColumnMajor(part.normalMatrix) * turn, instance.normalMatrix);
    const float scale = std::sqrt(twig.rows[0][0] * twig.rows[0][0] + twig.rows[1][0] * twig.rows[1][0] + twig.rows[2][0] * twig.rows[2][0]);
    instance.handedness[1] = part.handedness[1] * scale;
    return instance;
}

bool twigLevelDrawn(const GpuTwigLevel& before, bool finest, const GpuTwigLevel& level, const GpuInstance& twig, const GpuCameraCull& view) {
    const float budget = view.levels.pixelError;
    const bool ownFits = finest || meshClusterErrorPixels(before.sphere, before.error, twig, view) <= budget;
    const bool coarserFits = level.error <= static_cast<float>(MESH_CLUSTER_NEVER_COARSER) && meshClusterErrorPixels(level.sphere, level.error, twig, view) <= budget;
    return ownFits && !coarserFits;
}

std::uint32_t twigLevel(const GpuTwigLevel* levels, std::uint32_t levelCount, const GpuInstance& twig, const GpuCameraCull& view) {
    for (std::uint32_t level = 0; level < levelCount; ++level) {
        if (twigLevelDrawn(levels[level == 0 ? 0 : level - 1], level == 0, levels[level], twig, view)) {
            return level;
        }
    }

    return levelCount - 1;
}

bool twigBrickKept(const GpuTwigBrick& brick, const GpuInstance& part, std::uint32_t entry, const GpuCullRecord& record, const GpuCameraCull& view,
                   const MeshClusterPose& pose) {
    return twigSphereKept(brick.sphere, part, part, entry, record, view, pose);
}

bool twigKept(const GpuTwigLevel& coarsest, const GpuInstance& twig, std::uint32_t entry, const GpuCullRecord& record, const GpuCameraCull& view,
              const MeshClusterPose& pose) {
    return twigSphereKept(coarsest.sphere, twig, twig, entry, record, view, pose);
}

// The pose's turn on each column, then on the foot with its shift: P(M x) for every x.
GpuTwig twigPosed(const GpuTwig& twig, const WindBonePose& pose) {
    GpuTwig posed = twig;

    for (int column = 0; column < 4; ++column) {
        const Vec3 turned = pose.turn.rotate(Vec3{twig.rows[0][column], twig.rows[1][column], twig.rows[2][column]}) + (column == 3 ? pose.shift : Vec3{});
        posed.rows[0][column] = turned.x;
        posed.rows[1][column] = turned.y;
        posed.rows[2][column] = turned.z;
    }

    return posed;
}

MeshClusterPose twigBrickPose(const GpuTwigBrick& brick, const GpuInstance& part, bool partBound, const GpuBoneTree& tree) {
    return twigSpherePose(brick.sphere, part, partBound, tree);
}

MeshClusterPose twigSpherePose(const float sphere[4], const GpuInstance& part, bool partBound, const GpuBoneTree& tree) {
    if (!partBound || tree.firstPose == boneNoPose) {
        return MeshClusterPose{.bound = partBound};
    }

    const float grown[4] = {sphere[0], sphere[1], sphere[2], sphere[3] + tree.move};
    return meshClusterPosed(grown, part, part.sway[3], Vec2{tree.lean[0], tree.lean[1]});
}

bool twigFarLevelFits(const GpuTwigFarNode& node, const GpuTwigFarLevel& level, const GpuInstance& part, const GpuCameraCull& view) {
    return meshClusterErrorPixels(node.sphere, level.error, part, view) <= view.levels.pixelError;
}

bool twigFarNodeKept(const GpuTwigFarNode& node, const GpuInstance& part, std::uint32_t entry, const GpuCullRecord& record, const GpuCameraCull& view,
                     const MeshClusterPose& pose) {
    return twigSphereKept(node.sphere, part, part, entry, record, view, pose);
}

namespace {
    // Whether two shapes' records are alike in every number.
    bool sameFarLeafShape(const GpuFarLeafShape& a, const GpuFarLeafShape& b) {
        return std::memcmp(&a, &b, sizeof(GpuFarLeafShape)) == 0;
    }

    /*
    * A cluster of count leaves from first (shapes: the set's own run, named by firstShape in the table): its sphere the
    * box of every vertex the mesh shader makes of them, out to the farthest, so culling by it never loses a leaf.
    */
    GpuTwigFarCluster farClusterOf(const std::vector<GpuFarLeaf>& leaves, std::uint32_t first, std::uint32_t count,
                                   const std::vector<GpuFarLeafShape>& shapes, std::uint32_t firstShape) {
        std::vector<Vec3> points;

        for (std::uint32_t leaf = first; leaf < first + count; ++leaf) {
            const GpuFarLeafShape& shape = shapes.at((leaves[leaf].sideCellNumber >> 16) & 0xffu);

            for (std::uint32_t vertex = 0; vertex < farLeafVertices; ++vertex) {
                points.push_back(farLeafVertex(leaves[leaf], shape, vertex).position);
            }
        }

        Vec3 low = points.front();
        Vec3 high = points.front();

        for (const Vec3& point : points) {
            low = Vec3{std::min(low.x, point.x), std::min(low.y, point.y), std::min(low.z, point.z)};
            high = Vec3{std::max(high.x, point.x), std::max(high.y, point.y), std::max(high.z, point.z)};
        }

        const Vec3 centre = (low + high) * 0.5f;
        float radius = 0.0f;

        for (const Vec3& point : points) {
            radius = std::max(radius, (point - centre).length());
        }

        return GpuTwigFarCluster{.sphere = {centre.x, centre.y, centre.z, radius}, .firstLeaf = first, .leafCount = count, .firstShape = firstShape};
    }
}

/*
* Shapes are shared by comparing a set's whole run with each run already packed (the forest's sets all share one), so
* the table holds a run a distinct set of shapes.
*/
PackedTwigSets packTwigSets(std::span<const TwigSet* const> sets, std::span<const MeshHandle> prototypeMeshes) {
    PackedTwigSets packed;
    TwigTables& tables = packed.tables;
    std::vector<std::uint32_t> shapeRuns; // each distinct run's first shape; every run has the same length per set

    for (const TwigSet* set : sets) {
        const auto twigOffset = static_cast<std::uint32_t>(tables.twigs.size());
        const auto brickOffset = static_cast<std::uint32_t>(tables.bricks.size());
        GpuTwigSet record{.firstBrick = brickOffset, .brickCount = static_cast<std::uint32_t>(set->bricks.size())};

        for (const TwigBrick& brick : set->bricks) {
            GpuTwigBrick made = packTwigBrick(brick);
            made.firstTwig += twigOffset;
            tables.bricks.push_back(made);
        }

        for (const TwigInstance& twig : set->twigs) {
            const MeshHandle mesh = set->prototypes.at(twig.prototype);
            const auto found = std::find_if(prototypeMeshes.begin(), prototypeMeshes.end(), [&](const MeshHandle& other) { return other.index == mesh.index; });

            if (found == prototypeMeshes.end()) {
                throw std::invalid_argument("A twig's prototype is not among the prototype meshes");
            }

            GpuTwig made = packTwig(twig);
            made.prototype = static_cast<std::uint32_t>(found - prototypeMeshes.begin());
            tables.twigs.push_back(made);
            packed.bindings.push_back(packTwigBinding(twig));
        }

        const TwigFarLevels& far = set->far;

        if (!far.nodes.empty()) {
            const auto nodeOffset = static_cast<std::uint32_t>(tables.farNodes.size());
            const auto levelOffset = static_cast<std::uint32_t>(tables.farLevels.size());
            const auto leafOffset = static_cast<std::uint32_t>(tables.farLeaves.size());
            record.firstNode = nodeOffset;

            for (const TwigFarNode& node : far.nodes) {
                tables.farNodes.push_back(GpuTwigFarNode{.sphere = {node.centre.x, node.centre.y, node.centre.z, node.radius},
                                                         .firstLevel = levelOffset + node.firstLevel,
                                                         .levelCount = node.levelCount,
                                                         .firstBrick = brickOffset + node.firstBrick,
                                                         .parent = node.parent == twigFarNoNode ? TWIG_FAR_NO_NODE : nodeOffset + node.parent});
            }

            for (const FarLeaf& leaf : far.leaves) {
                GpuFarLeaf made = packFarLeaf(leaf);
                made.twig += twigOffset;
                tables.farLeaves.push_back(made);
            }

            std::vector<GpuFarLeafShape> shapes;

            for (const FarLeafShape& shape : far.shapes.cells) {
                shapes.push_back(packFarLeafShape(shape, far.shapes.foldRadians, far.shapes.curl));
            }

            const auto sameRun = [&](std::uint32_t first) {
                return first + shapes.size() <= tables.farShapes.size() &&
                       std::equal(shapes.begin(), shapes.end(), tables.farShapes.begin() + first, sameFarLeafShape);
            };
            const auto shared = std::find_if(shapeRuns.begin(), shapeRuns.end(), sameRun);

            if (shared != shapeRuns.end()) {
                record.firstShape = *shared;
            } else {
                record.firstShape = static_cast<std::uint32_t>(tables.farShapes.size());
                shapeRuns.push_back(record.firstShape);
                tables.farShapes.insert(tables.farShapes.end(), shapes.begin(), shapes.end());
            }

            for (const TwigFarLevel& level : far.levels) {
                GpuTwigFarLevel made{.firstCluster = static_cast<std::uint32_t>(tables.farClusters.size()), .error = level.error, .leafCount = level.leafCount};

                for (std::uint32_t first = 0; first < level.leafCount; first += farLeavesPerCluster) {
                    const std::uint32_t count = std::min(farLeavesPerCluster, level.leafCount - first);
                    tables.farClusters.push_back(farClusterOf(tables.farLeaves, leafOffset + level.firstLeaf + first, count, shapes, record.firstShape));
                    ++made.clusterCount;
                }

                tables.farLevels.push_back(made);
            }
        }

        packed.sets.push_back(record);
    }

    return packed;
}

/*
* Up the chain while the parent fits: the nodes that fit are a run from the brick up (twigFarLevelFits), so the first
* parent that does not ends it.
*/
std::uint32_t twigFarNodeDrawn(const TwigTables& tables, std::uint32_t brickNode, const GpuInstance& part, const GpuCameraCull& view) {
    const auto fits = [&](std::uint32_t node) {
        const GpuTwigFarNode& record = tables.farNodes[node];
        return twigFarLevelFits(record, tables.farLevels[record.firstLevel], part, view);
    };

    if (!fits(brickNode)) {
        return TWIG_FAR_NO_NODE;
    }

    std::uint32_t node = brickNode;

    while (tables.farNodes[node].parent != TWIG_FAR_NO_NODE && fits(tables.farNodes[node].parent)) {
        node = tables.farNodes[node].parent;
    }

    return node;
}

std::uint32_t twigFarLevel(const TwigTables& tables, const GpuTwigFarNode& node, const GpuInstance& part, const GpuCameraCull& view) {
    std::uint32_t level = node.firstLevel;

    while (level + 1 < node.firstLevel + node.levelCount && twigFarLevelFits(node, tables.farLevels[level + 1], part, view)) {
        ++level;
    }

    return level;
}

MeshClusterPose twigPose(const GpuTwigLevel& coarsest, const GpuInstance& part, const GpuTwig& twig, std::uint32_t binding, std::span<const GpuBonePose> pool,
                         const GpuBoneTree& tree) {
    const bool bound = binding != swayUnbound;

    if (!bound || tree.firstPose == boneNoPose) {
        return MeshClusterPose{.bound = bound};
    }

    const BoneVertexPoses poses = boneVertexPoses(pool, tree.firstPose, binding, false);
    const WindBonePose rigid = windRigidPose(poses.own, poses.parent, poses.weight, Vec3{twig.rows[0][3], twig.rows[1][3], twig.rows[2][3]});
    return meshClusterPosed(coarsest.sphere, twigInstance(part, twigPosed(twig, rigid)), part.sway[3], Vec2{tree.lean[0], tree.lean[1]});
}

TwigTally readTwigTally(std::span<const std::uint32_t, TWIG_TALLY_WORDS> words) {
    TwigTally tally;
    tally.tested = words[TWIG_TALLY_TESTED];
    tally.kept = words[TWIG_TALLY_KEPT];
    tally.swaying = words[TWIG_TALLY_SWAYING];

    for (std::uint32_t level = 0; level < twigMostLevels; ++level) {
        tally.levelTwigs[level] = words[TWIG_TALLY_LEVEL_TWIGS + level];
        tally.levelTriangles[level] = words[TWIG_TALLY_LEVEL_TRIANGLES + level];
    }

    return tally;
}

// The tally as twig_cull.comp counts: every twig of every brick tested, whether its brick is kept or not.
std::vector<TwigDraw> twigDrawsOnCpu(const TwigTables& tables, const GpuInstance& part, std::uint32_t entry, const GpuCullRecord& record,
                                     const GpuCameraCull& view, TwigTally* tally, const BoneCullTables* bones, bool partBound,
                                     std::vector<TwigFarDraw>* farDraws) {
    std::vector<TwigDraw> draws;
    const GpuBoneTree tree = bones != nullptr ? boneCullTree(*bones, entry & DRAW_LIST_INSTANCE_MASK) : GpuBoneTree{};
    const bool hasFar = !tables.farNodes.empty();

    for (std::uint32_t brickNumber = 0; brickNumber < tables.bricks.size(); ++brickNumber) {
        const GpuTwigBrick& brick = tables.bricks[brickNumber];

        if (tally != nullptr) {
            tally->tested += brick.twigCount;
        }

        if (hasFar) {
            const std::uint32_t drawn = twigFarNodeDrawn(tables, brickNumber, part, view);

            if (drawn != TWIG_FAR_NO_NODE) {
                const GpuTwigFarNode& node = tables.farNodes[drawn];

                const MeshClusterPose nodePose = twigSpherePose(node.sphere, part, partBound, tree);

                if (node.firstBrick == brickNumber && twigFarNodeKept(node, part, entry, record, view, nodePose) && farDraws != nullptr) {
                    const std::uint32_t level = twigFarLevel(tables, node, part, view);
                    farDraws->push_back(TwigFarDraw{drawn, level, tables.farLevels[level].firstCluster, tables.farLevels[level].clusterCount});
                }

                continue;
            }
        }

        if (!twigBrickKept(brick, part, entry, record, view, twigBrickPose(brick, part, partBound, tree))) {
            continue;
        }

        for (std::uint32_t number = brick.firstTwig; number < brick.firstTwig + brick.twigCount; ++number) {
            const GpuTwig& twig = tables.twigs[number];
            const GpuTwigPrototype& prototype = tables.prototypes[twig.prototype];
            const GpuTwigLevel* levels = &tables.levels[prototype.firstLevel];
            const GpuInstance placed = twigInstance(part, twig);

            const std::uint32_t binding = bones != nullptr && number < bones->twigBindings.size() ? bones->twigBindings[number] : swayUnbound;
            const std::span<const GpuBonePose> pool = bones != nullptr ? bones->poses : std::span<const GpuBonePose>{};
            const MeshClusterPose pose = twigPose(levels[prototype.levelCount - 1], part, twig, binding, pool, tree);

            if (!twigKept(levels[prototype.levelCount - 1], placed, entry, record, view, pose)) {
                continue;
            }

            const std::uint32_t drawable = hasFar ? std::min<std::uint32_t>(prototype.levelCount, TWIG_BRICK_LEVEL + 1) : prototype.levelCount;
            const std::uint32_t level = twigLevel(levels, drawable, placed, view);
            const GpuTwigLevel& chosen = levels[level];
            draws.push_back(TwigDraw{number, chosen.firstCluster, chosen.clusterCount});

            if (tally != nullptr) {
                ++tally->kept;
                tally->swaying += pose.posed || meshClusterSeenWhole(part, entry, record, view, pose) ? 1 : 0;
                ++tally->levelTwigs[level];
                tally->levelTriangles[level] += chosen.triangleCount;
            }
        }
    }

    return draws;
}
