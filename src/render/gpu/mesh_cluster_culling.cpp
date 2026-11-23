#include "render/gpu/mesh_cluster_culling.h"
#include "render/gpu/column_major.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/instance_shared.h"
#include "render/gpu/taa.h"
#include "render/gpu/wind.h"
#include "scene/planting.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
    // A model matrix's column (GpuInstance::model is column-major, as the shaders read it).
    Vec3 modelColumn(const GpuInstance& instance, int column) {
        return Vec3{instance.model[4 * column], instance.model[4 * column + 1], instance.model[4 * column + 2]};
    }

    // A group's sphere as a record's (xyz centre, w radius).
    void writeGroupSphere(const MeshClusterGroup& group, float sphere[4]) {
        sphere[0] = group.centre.x;
        sphere[1] = group.centre.y;
        sphere[2] = group.centre.z;
        sphere[3] = group.radius;
    }

    // The next block's first pair at or after a count of pairs.
    std::uint32_t roundUpToBlock(std::uint32_t pairs) {
        return (pairs + meshClusterGroupSize - 1) / meshClusterGroupSize * meshClusterGroupSize;
    }

    // A place's group in the drawing order: its command and its clusters (first, count), a draw group's or, numbered past
    // the draw groups, a placed group's.
    struct OrderedGroup {
        GpuDrawCommand command;
        std::uint32_t firstCluster = 0;
        std::uint32_t clusterCount = 0;
    };

    /*
    * A group's command and clusters: a draw group's, a placed group's past them, or from twigGroupBase a twig region's:
    * its entries written (at most its room) from its first place, each with its own first cluster (meshClusterPairAt),
    * the region's cluster count for all.
    */
    OrderedGroup orderedGroup(const MeshClusterTables& tables, std::uint32_t group) {
        const std::size_t drawGroups = tables.groups.size();

        if (group >= twigGroupBase) {
            const std::uint32_t region = group - twigGroupBase;
            GpuDrawCommand command{};
            command.instanceCount = std::min(tables.twigs.counts[region], tables.twigs.room);
            command.firstInstance = twigInstanceBase + region * tables.twigs.room;
            return OrderedGroup{command, 0, region + 1};
        }

        if (group < drawGroups) {
            return OrderedGroup{tables.commands[group], tables.groups[group].firstCluster, tables.groups[group].clusterCount};
        }

        const std::size_t placed = group - drawGroups;
        return OrderedGroup{tables.placed.commands[placed], tables.placed.groups[placed].firstCluster, tables.placed.groups[placed].clusterCount};
    }

    // A group's pairs in its list: its drawn instances times its clusters.
    std::uint32_t groupPairs(const MeshClusterTables& tables, std::uint32_t group) {
        const OrderedGroup ordered = orderedGroup(tables, group);
        return ordered.command.instanceCount * ordered.clusterCount;
    }

    // What a list makes of a pair: kept in it, and (the late list's) a candidate, hidden by last frame's pyramid.
    struct PairChoice {
        bool kept = false;
        bool candidate = false;
    };

    /*
    * The pair's choice, as mesh_cluster_pass.glsl's meshClusterPairKept makes it: hidden in neither list; seen whole, or
    * with occlusion off, in the early list only; else the early list keeps what last frame's pyramid does not hide
    * (everything, with no history), and the late list what it hid that this frame's pyramid does not.
    */
    PairChoice choosePair(const MeshClusterTables& tables, const MeshClusterPair& pair, const GpuCameraCull& view,
                          const MeshClusterOcclusionView* occlusion) {
        const MeshClusterDrawn drawn = meshClusterDrawnAt(tables, pair.entry);
        const GpuMeshCluster& cluster = tables.clusters[pair.cluster];
        const MeshClusterPose pose = meshClusterPoseAt(tables, drawn, cluster);
        const std::uint32_t visibility = meshClusterVisibility(cluster, drawn.instance, drawn.entry, drawn.record, view, pose);
        const bool late = occlusion != nullptr && occlusion->list == MESH_CLUSTER_LATE;

        if (visibility == MESH_CLUSTER_HIDDEN) {
            return {};
        }

        if (visibility == MESH_CLUSTER_SEEN || occlusion == nullptr || !occlusion->on) {
            return {!late, false};
        }

        const std::array<float, 4> world = meshClusterCullSphere(cluster, drawn.instance, pose);
        const std::array<float, 4> sphere{world[0] - occlusion->origin.x, world[1] - occlusion->origin.y, world[2] - occlusion->origin.z, world[3]};
        const bool hiddenBefore = occlusion->history && meshClusterHidden(*occlusion->lastPyramid, sphere, occlusion->previous);

        if (!late) {
            return {!hiddenBefore, false};
        }

        return {hiddenBefore && !meshClusterHidden(*occlusion->thisPyramid, sphere, occlusion->current), hiddenBefore};
    }
}

std::uint32_t meshClusterTriangleWords(std::uint32_t triangles) {
    return (triangles * 3 + 3) / 4;
}

/*
* The bones gathered in a small sorted list: a cluster has at most 64 vertices, and stopping past the most keeps it
* short. A bone appears once however many vertices hold it.
*/
std::array<std::uint32_t, 4> meshClusterBones(const MeshClusters& built, const MeshCluster& cluster, std::span<const SwayBinding> bindings) {
    constexpr std::uint32_t noBones = meshClusterNoBone | (meshClusterNoBone << 16u);

    if (bindings.empty()) {
        return {noBones, noBones, noBones, noBones};
    }

    std::array<std::uint32_t, meshClusterMostBones> bones{};
    std::uint32_t count = 0;

    for (std::uint32_t vertex = 0; vertex < cluster.vertexCount; ++vertex) {
        const std::uint32_t number = built.vertices.at(cluster.firstVertex + vertex);

        if (number >= bindings.size()) {
            throw std::invalid_argument("A cluster's vertex has no binding: bindings must be one a mesh vertex");
        }

        const auto bone = static_cast<std::uint32_t>(bindings[number].bone);

        if (std::find(bones.begin(), bones.begin() + count, bone) != bones.begin() + count) {
            continue;
        }

        if (count == meshClusterMostBones) {
            return {meshClusterManyBones | (meshClusterNoBone << 16u), noBones, noBones, noBones};
        }

        bones[count++] = bone;
    }

    std::sort(bones.begin(), bones.begin() + count);
    std::array<std::uint32_t, meshClusterMostBones> slots{};
    slots.fill(meshClusterNoBone);
    std::copy(bones.begin(), bones.begin() + count, slots.begin());
    std::array<std::uint32_t, 4> words{};

    for (std::size_t word = 0; word < words.size(); ++word) {
        words[word] = slots[2 * word] | (slots[2 * word + 1] << 16u);
    }

    return words;
}

std::uint32_t meshClusterBone(const GpuMeshCluster& cluster, std::uint32_t slot) {
    return (cluster.bones[slot / 2] >> (16u * (slot % 2))) & 0xFFFFu;
}

/*
* Vertex numbers become the vertex pool's (the mesh's first vertex added), so a mesh shader needs no offset; each
* cluster's corner bytes start a word of their own, so a cluster reads whole words.
*/
PackedMeshClusters packMeshClusters(const MeshClusters& built, std::uint32_t meshFirstVertex, std::uint32_t firstClusterVertex,
                                    std::uint32_t firstTriangleWord, std::span<const SwayBinding> bindings) {
    PackedMeshClusters packed;
    packed.clusters.reserve(built.clusters.size());
    packed.vertices.reserve(built.vertices.size());

    for (const MeshCluster& cluster : built.clusters) {
        GpuMeshCluster record;
        record.sphere[0] = cluster.centre.x;
        record.sphere[1] = cluster.centre.y;
        record.sphere[2] = cluster.centre.z;
        record.sphere[3] = cluster.radius;
        record.cone[0] = cluster.coneApex.x;
        record.cone[1] = cluster.coneApex.y;
        record.cone[2] = cluster.coneApex.z;
        record.cone[3] = cluster.coneCutoff;
        record.axis[0] = cluster.coneAxis.x;
        record.axis[1] = cluster.coneAxis.y;
        record.axis[2] = cluster.coneAxis.z;
        record.firstVertex = firstClusterVertex + static_cast<std::uint32_t>(packed.vertices.size());
        record.firstTriangleWord = firstTriangleWord + static_cast<std::uint32_t>(packed.triangleWords.size());
        record.counts = cluster.vertexCount | (cluster.triangleCount << MESH_CLUSTER_TRIANGLE_SHIFT);
        record.meshFirstVertex = meshFirstVertex;
        writeGroupSphere(built.groups[cluster.group], record.parentSphere);
        record.parentError = built.groups[cluster.group].error;
        record.level = built.groups[cluster.group].level;
        const std::array<std::uint32_t, 4> bones = meshClusterBones(built, cluster, bindings);
        std::copy(bones.begin(), bones.end(), std::begin(record.bones));

        if (cluster.refinedGroup == meshClusterNoGroup) {
            std::copy(std::begin(record.sphere), std::end(record.sphere), std::begin(record.ownSphere));
        } else {
            writeGroupSphere(built.groups[cluster.refinedGroup], record.ownSphere);
            record.ownError = built.groups[cluster.refinedGroup].error;
        }

        packed.clusters.push_back(record);

        for (std::uint32_t vertex = 0; vertex < cluster.vertexCount; ++vertex) {
            packed.vertices.push_back(meshFirstVertex + built.vertices[cluster.firstVertex + vertex]);
        }

        const std::uint32_t words = meshClusterTriangleWords(cluster.triangleCount);
        const std::size_t start = packed.triangleWords.size();
        packed.triangleWords.resize(start + words, 0u);

        for (std::uint32_t corner = 0; corner < cluster.triangleCount * 3; ++corner) {
            const std::uint32_t value = built.triangles[cluster.firstTriangle * 3 + corner];
            packed.triangleWords[start + corner / 4] |= value << (8u * (corner % 4));
        }
    }

    return packed;
}

float meshClusterLargestScale(const GpuInstance& instance) {
    const Vec3 x = modelColumn(instance, 0);
    const Vec3 y = modelColumn(instance, 1);
    const Vec3 z = modelColumn(instance, 2);
    return std::sqrt(std::max(std::max(x.dot(x), y.dot(y)), z.dot(z)));
}

std::array<float, 4> meshClusterPlacedSphere(const float sphere[4], const GpuInstance& instance) {
    const Vec3 centre = modelColumn(instance, 0) * sphere[0] + modelColumn(instance, 1) * sphere[1] + modelColumn(instance, 2) * sphere[2] +
                        modelColumn(instance, 3);
    return {centre.x, centre.y, centre.z, sphere[3] * meshClusterLargestScale(instance)};
}

std::array<float, 4> meshClusterWorldSphere(const GpuMeshCluster& cluster, const GpuInstance& instance) {
    return meshClusterPlacedSphere(cluster.sphere, instance);
}

/*
* The error stretched by the longest a unit can stretch, so it is never under what the placed mesh strays; the distance
* to the placed sphere's surface, so every point in it is at least that far.
*/
float meshClusterErrorPixels(const float sphere[4], float error, const GpuInstance& instance, const GpuCameraCull& view) {
    const float scale = meshClusterLargestScale(instance);
    const float pixels = error * scale * view.levels.eye[3];

    if ((view.levels.flags & CULL_LEVELS_ORTHOGRAPHIC) != 0u) {
        return pixels;
    }

    const std::array<float, 4> placed = meshClusterPlacedSphere(sphere, instance);
    const Vec3 offset{placed[0] - view.levels.eye[0], placed[1] - view.levels.eye[1], placed[2] - view.levels.eye[2]};
    const float distance = std::max(std::sqrt(offset.dot(offset)) - placed[3], static_cast<float>(MESH_CLUSTER_NEAREST_ERROR_DISTANCE));
    return pixels / distance;
}

bool meshClusterInCut(const GpuMeshCluster& cluster, const GpuInstance& instance, const GpuCameraCull& view) {
    const float budget = view.levels.pixelError;

    if (meshClusterErrorPixels(cluster.ownSphere, cluster.ownError, instance, view) > budget) {
        return false;
    }

    return cluster.parentError > static_cast<float>(MESH_CLUSTER_NEVER_COARSER) ||
           meshClusterErrorPixels(cluster.parentSphere, cluster.parentError, instance, view) > budget;
}

float meshClusterDeterminant(const GpuInstance& instance) {
    return modelColumn(instance, 0).dot(modelColumn(instance, 1).cross(modelColumn(instance, 2)));
}

// The inverse of the turning part is its columns' cross products (as rows) over its determinant.
Vec3 meshClusterEyeInMesh(const GpuInstance& instance, const Vec3& eye, float determinant) {
    const Vec3 x = modelColumn(instance, 0);
    const Vec3 y = modelColumn(instance, 1);
    const Vec3 z = modelColumn(instance, 2);
    const Vec3 offset = eye - modelColumn(instance, 3);
    return Vec3{y.cross(z).dot(offset), z.cross(x).dot(offset), x.cross(y).dot(offset)} / determinant;
}

bool meshClusterFacesAway(const GpuMeshCluster& cluster, const Vec3& eye) {
    MeshCluster cone;
    cone.coneApex = Vec3{cluster.cone[0], cluster.cone[1], cluster.cone[2]};
    cone.coneAxis = Vec3{cluster.axis[0], cluster.axis[1], cluster.axis[2]};
    cone.coneCutoff = cluster.cone[3];
    return clusterFacesAway(cone, eye);
}

bool meshClusterShapeFixed(const GpuInstance& instance, std::uint32_t entry) {
    return instance.material[2] == 0u && instance.material[3] == INSTANCE_KIND_PLAIN && instance.hanging[3] == 0u && (entry & DRAW_LIST_THINNED_BIT) == 0u;
}

bool meshClusterSways(const GpuCullRecord& record, const GpuCameraCull& view) {
    const Vec3 eye{view.levels.eye[0], view.levels.eye[1], view.levels.eye[2]};
    return (view.flags & CAMERA_CULL_SWAY) != 0u && (record.flags & CULL_SWAYS) != 0u && swayReaches(record.bounds, eye, view.swayFadeEnd);
}

MeshClusterPose meshClusterPosed(const float sphere[4], const GpuInstance& placedBy, float flutter, const Vec2& lean) {
    std::array<float, 4> placed = meshClusterPlacedSphere(sphere, placedBy);
    placed[3] += windFlutterReach(flutter, lean);
    return MeshClusterPose{.bound = true, .posed = true, .sphere = placed};
}

bool meshClusterSeenWhole(const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record, const GpuCameraCull& view,
                          const MeshClusterPose& pose) {
    return !meshClusterShapeFixed(instance, entry) || (!pose.bound && meshClusterSways(record, view));
}

bool meshClusterBound(const GpuMeshCluster& cluster) {
    return meshClusterBone(cluster, 0) != meshClusterNoBone;
}

// Each bone's pose names its parent, so the pool alone gives both (boneVertexPoses, the bone as a binding of no weight).
Vec4 meshClusterBonesSphere(const GpuMeshCluster& cluster, std::span<const GpuBonePose> pool, const GpuBoneTree& tree) {
    const Vec4 rest{cluster.sphere[0], cluster.sphere[1], cluster.sphere[2], cluster.sphere[3]};

    if (meshClusterBone(cluster, 0) == meshClusterManyBones) {
        return Vec4{rest.x, rest.y, rest.z, rest.w + tree.move};
    }

    Vec4 posed = rest;

    for (std::uint32_t slot = 0; slot < meshClusterMostBones; ++slot) {
        const std::uint32_t bone = meshClusterBone(cluster, slot);

        if (bone == meshClusterNoBone) {
            break;
        }

        const BoneVertexPoses poses = boneVertexPoses(pool, tree.firstPose, bone, false);
        const Vec4 blended = windBlendedSphere(poses.own, poses.parent, rest);
        posed = slot == 0 ? blended : windSphereUnion(posed, blended);
    }

    return posed;
}

std::array<float, 4> meshClusterCullSphere(const GpuMeshCluster& cluster, const GpuInstance& instance, const MeshClusterPose& pose) {
    return pose.posed ? pose.sphere : meshClusterWorldSphere(cluster, instance);
}

std::uint32_t meshClusterVisibility(const GpuMeshCluster& cluster, const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record,
                                    const GpuCameraCull& view, const MeshClusterPose& pose) {
    if (!meshClusterInCut(cluster, instance, view)) {
        return MESH_CLUSTER_HIDDEN;
    }

    if (meshClusterSeenWhole(instance, entry, record, view, pose)) {
        return MESH_CLUSTER_SEEN;
    }

    const std::array<float, 4> sphere = meshClusterCullSphere(cluster, instance, pose);

    if (!sphereInFrustum(view.frustum, sphere.data())) {
        return MESH_CLUSTER_HIDDEN;
    }

    const float determinant = meshClusterDeterminant(instance);

    if (pose.posed || (record.flags & CULL_DOUBLE_SIDED) != 0u || !(determinant > 0.0f)) {
        return MESH_CLUSTER_SPHERE;
    }

    const Vec3 eye{view.levels.eye[0], view.levels.eye[1], view.levels.eye[2]};
    return meshClusterFacesAway(cluster, meshClusterEyeInMesh(instance, eye, determinant)) ? MESH_CLUSTER_HIDDEN : MESH_CLUSTER_SPHERE;
}

bool meshClusterKept(const GpuMeshCluster& cluster, const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record,
                     const GpuCameraCull& view, const MeshClusterPose& pose) {
    return meshClusterVisibility(cluster, instance, entry, record, view, pose) != MESH_CLUSTER_HIDDEN;
}

GpuMeshClusterOcclusion packMeshClusterOcclusion(const MeshClusterOcclusionView& view, RenderSize pyramid, std::uint32_t levels) {
    GpuMeshClusterOcclusion packed;
    writeColumnMajor(view.previous, packed.previous);
    writeColumnMajor(view.current, packed.current);
    packed.origin[0] = view.origin.x;
    packed.origin[1] = view.origin.y;
    packed.origin[2] = view.origin.z;
    packed.pyramid[0] = pyramid.width;
    packed.pyramid[1] = pyramid.height;
    packed.pyramid[2] = levels;
    packed.pyramid[3] = (view.on ? MESH_CLUSTER_OCCLUSION_ON : 0u) | (view.history ? MESH_CLUSTER_OCCLUSION_HISTORY : 0u);
    return packed;
}

/*
* The sphere's box (a cube round it) holds it, and since every corner is in front of the camera the box's picture is the
* hull of its corners' pictures: their least and most pixels bound the sphere's. Depth (z / w) along a segment in front
* of the camera is monotonic, so the nearest point of the box is a corner too. A box is looser than the sphere's own
* outline (Mara and McGuire 2013) but takes any view-projection, jittered or last frame's, with no view-space step.
*/
MeshClusterScreenBox meshClusterScreenBox(const std::array<float, 4>& sphere, const Mat4& viewProjection, Vec2 size) {
    MeshClusterScreenBox result;
    result.box = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(),
                  std::numeric_limits<float>::lowest()};
    result.nearestDepth = std::numeric_limits<float>::lowest();
    const float radius = sphere[3];

    for (int corner = 0; corner < 8; ++corner) {
        const Vec4 point{sphere[0] + ((corner & 1) != 0 ? radius : -radius), sphere[1] + ((corner & 2) != 0 ? radius : -radius),
                         sphere[2] + ((corner & 4) != 0 ? radius : -radius), 1.0f};
        const Vec4 clip = viewProjection * point;

        if (!(clip.w > static_cast<float>(MESH_CLUSTER_SMALLEST_W))) {
            return MeshClusterScreenBox{};
        }

        const Vec2 pixel = ndcToPixel(Vec2{clip.x / clip.w, clip.y / clip.w}, size);
        result.box[0] = std::min(result.box[0], pixel.x);
        result.box[1] = std::min(result.box[1], pixel.y);
        result.box[2] = std::max(result.box[2], pixel.x);
        result.box[3] = std::max(result.box[3], pixel.y);
        result.nearestDepth = std::max(result.nearestDepth, clip.z / clip.w);
    }

    result.onScreen = true;
    return result;
}

// Clamped a pixel past the screen before turning to whole pixels, so a huge box converts safely.
std::array<int, 4> meshClusterBoxPixels(const std::array<float, 4>& box, int width, int height) {
    const auto whole = [](float value, int size) { return static_cast<int>(std::floor(std::clamp(value, -1.0f, static_cast<float>(size)))); };
    return {std::max(whole(box[0], width), 0), std::max(whole(box[1], height), 0), std::min(whole(box[2], width), width - 1),
            std::min(whole(box[3], height), height - 1)};
}

// Pixels at most 2^level apart fall in at most two cells of that level (a cell is 2^level pixels, the last more).
std::uint32_t meshClusterPyramidLevel(const std::array<int, 4>& pixels, std::uint32_t levels) {
    const int extent = std::max(pixels[2] - pixels[0], pixels[3] - pixels[1]);
    const std::uint32_t level = extent <= 1 ? 0u : static_cast<std::uint32_t>(std::bit_width(static_cast<unsigned>(extent - 1)));
    return std::min(level, levels - 1);
}

bool meshClusterHidden(const DepthPyramid& pyramid, const std::array<float, 4>& sphere, const Mat4& viewProjection) {
    const RenderSize size = pyramid.sizes[0];
    const MeshClusterScreenBox box = meshClusterScreenBox(sphere, viewProjection, Vec2{static_cast<float>(size.width), static_cast<float>(size.height)});

    if (!box.onScreen) {
        return false;
    }

    const std::array<int, 4> pixels = meshClusterBoxPixels(box.box, static_cast<int>(size.width), static_cast<int>(size.height));

    if (pixels[0] > pixels[2] || pixels[1] > pixels[3]) {
        return false;
    }

    const std::uint32_t level = meshClusterPyramidLevel(pixels, static_cast<std::uint32_t>(pyramid.sizes.size()));
    const int width = static_cast<int>(pyramid.sizes[level].width);
    const int height = static_cast<int>(pyramid.sizes[level].height);
    float farthest = gpuNearDepth;

    for (int y = pyramidCell(pixels[1], level, height); y <= pyramidCell(pixels[3], level, height); ++y) {
        for (int x = pyramidCell(pixels[0], level, width); x <= pyramidCell(pixels[2], level, width); ++x) {
            farthest = fartherDepth(farthest, pyramid.at(level, x, y));
        }
    }

    return box.nearestDepth < farthest;
}

// The segments are the still ones, then the moving draws' (CameraCullFrame::join's order).
std::uint64_t meshClusterPairBound(const CameraCullFrame& table) {
    std::uint64_t bound = static_cast<std::uint64_t>(MESH_CLUSTER_STYLES) * meshClusterGroupSize;
    const std::size_t still = table.stillSegments.size();

    for (const GpuCullGroup& group : table.groups) {
        std::uint64_t candidates = 0;

        for (std::uint32_t segment = group.firstSegment; segment < group.firstSegment + group.segments; ++segment) {
            candidates += segment < still ? table.stillSegments[segment].candidates : table.movingSegments[segment - still].candidates;
        }

        bound += candidates * group.clusterCount;
    }

    return bound;
}

/*
* Style by style in the drawing order's runs, each from a block's first; a place's end is the pairs before it and its
* own. The runs must follow one another in style order, as cullDrawOrder lays them out.
*/
std::vector<std::uint32_t> meshClusterWorkEnds(const MeshClusterTables& tables, const GpuCameraCull& view,
                                               std::array<std::uint32_t, MESH_CLUSTER_WORK_WORDS>& work) {
    std::vector<std::uint32_t> ends;
    std::uint32_t carried = 0;
    std::uint32_t real = 0;
    work.fill(0);

    for (std::uint32_t style = 0; style < MESH_CLUSTER_STYLES; ++style) {
        if (view.runFirst[style] != ends.size() && view.runCount[style] > 0) {
            throw std::invalid_argument("The drawing order's runs must follow one another in style order");
        }

        carried = roundUpToBlock(carried);
        work[MESH_CLUSTER_WORK_STYLE_FIRST + style] = carried;

        for (std::uint32_t place = view.runFirst[style]; place < view.runFirst[style] + view.runCount[style]; ++place) {
            const std::uint32_t pairs = groupPairs(tables, tables.drawOrder[place]);
            carried += pairs;
            real += pairs;
            ends.push_back(carried);
        }
    }

    work[MESH_CLUSTER_WORK_END] = roundUpToBlock(carried);
    work[MESH_CLUSTER_WORK_REAL] = real;
    return ends;
}

// The first place ending past the pair; none past the last place or in the padding before a style's first block.
MeshClusterPair meshClusterPairAt(const MeshClusterTables& tables, std::span<const std::uint32_t> ends, std::uint32_t pair) {
    const auto found = std::upper_bound(ends.begin(), ends.end(), pair);

    if (found == ends.end()) {
        return {};
    }

    const std::uint32_t place = static_cast<std::uint32_t>(found - ends.begin());
    const std::uint32_t group = tables.drawOrder[place];
    const std::uint32_t first = *found - groupPairs(tables, group);

    if (pair < first) {
        return {};
    }

    const std::uint32_t within = pair - first;
    const OrderedGroup ordered = orderedGroup(tables, group);
    const std::uint32_t entry = ordered.command.firstInstance + within / ordered.clusterCount;
    const std::uint32_t firstCluster = group >= twigGroupBase ? tables.twigs.entries[entry - twigInstanceBase].firstCluster : ordered.firstCluster;
    return MeshClusterPair{true, entry, firstCluster + within % ordered.clusterCount};
}

std::uint64_t twigMeshClusterPairBound(std::uint32_t room) {
    std::uint64_t bound = 0;

    for (std::uint32_t clusters = 1; clusters <= twigMostLevelClusters; ++clusters) {
        bound += std::uint64_t{room} * clusters;
    }

    return bound;
}

// Each style's pooled draw groups, then its placed groups (all pooled: buildPlacedTable refuses the rest), then its twig regions.
CullDrawOrder meshClusterDrawOrder(const CullDrawOrder& draws, std::uint32_t drawGroups, const CullDrawOrder& placed, int twigStyle) {
    CullDrawOrder both;

    for (std::size_t style = 0; style < cullStyles; ++style) {
        both.runFirst[style] = static_cast<std::uint32_t>(both.order.size());

        for (std::uint32_t place = draws.runFirst[style]; place < draws.runFirst[style] + draws.runCount[style]; ++place) {
            both.order.push_back(draws.order[place]);
        }

        for (std::uint32_t place = placed.runFirst[style]; place < placed.runFirst[style] + placed.runCount[style]; ++place) {
            both.order.push_back(drawGroups + placed.order[place]);
        }

        if (static_cast<int>(style) == twigStyle) {
            for (std::uint32_t region = 0; region < twigMostLevelClusters; ++region) {
                both.order.push_back(twigGroupBase + region);
            }
        }

        both.runCount[style] = static_cast<std::uint32_t>(both.order.size()) - both.runFirst[style];
    }

    both.pooled = static_cast<std::uint32_t>(both.order.size());
    return both;
}

MeshClusterDrawn meshClusterDrawnAt(const MeshClusterTables& tables, std::uint32_t place) {
    MeshClusterDrawn drawn;

    // A twig: its part placed by its plant as a placed entry's, then the twig after it (twigInstance).
    if (place >= twigInstanceBase) {
        const GpuTwigEntry& twig = tables.twigs.entries[place - twigInstanceBase];
        drawn = meshClusterDrawnAt(tables, placedInstanceBase + twig.placedEntry);
        drawn.instance = twigInstance(drawn.part, tables.twigs.twigs[twig.twig]);
        drawn.twig = twig.twig;
        return drawn;
    }

    if (place >= placedInstanceBase) {
        const GpuPlacedEntry& placed = tables.placed.entries[place - placedInstanceBase];
        drawn.plant = placed.plant & DRAW_LIST_INSTANCE_MASK;
        const GpuPlacedPlant& plant = tables.placed.plants[drawn.plant];
        drawn.instance = placedInstance(tables.placed.partInstances[placed.part], plant);
        drawn.record = placedCullRecord(tables.placed.parts[placed.part].record, plant);
        drawn.entry = placed.plant;
        drawn.part = drawn.instance;
        return drawn;
    }

    drawn.entry = tables.drawList[place];
    const std::uint32_t draw = drawn.entry & DRAW_LIST_INSTANCE_MASK;
    drawn.instance = tables.instances[draw];
    drawn.record = tables.records[draw];
    drawn.part = drawn.instance;
    return drawn;
}

// A twig's cluster sphere is in its prototype's frame, as its coarsest group's is: twigPose takes it the same way.
MeshClusterPose meshClusterPoseAt(const MeshClusterTables& tables, const MeshClusterDrawn& drawn, const GpuMeshCluster& cluster) {
    const GpuBoneTree tree = drawn.plant == boneNoTree ? GpuBoneTree{} : boneCullTree(tables.bones, drawn.plant);

    if (drawn.twig != meshClusterNoTwig) {
        const std::uint32_t binding = drawn.twig < tables.bones.twigBindings.size() ? tables.bones.twigBindings[drawn.twig] : swayUnbound;
        GpuTwigLevel own;
        std::copy(std::begin(cluster.sphere), std::end(cluster.sphere), std::begin(own.sphere));
        return twigPose(own, drawn.part, tables.twigs.twigs[drawn.twig], binding, tables.bones.poses, tree);
    }

    if (!meshClusterBound(cluster)) {
        return {};
    }

    if (tree.firstPose == boneNoPose) {
        return MeshClusterPose{.bound = true};
    }

    const Vec4 posed = meshClusterBonesSphere(cluster, tables.bones.poses, tree);
    const float sphere[4] = {posed.x, posed.y, posed.z, posed.w};
    return meshClusterPosed(sphere, drawn.instance, drawn.instance.sway[3], Vec2{tree.lean[0], tree.lean[1]});
}

// A part's level slots are firstGroup to firstGroup + levels; a block's layer is its first kind's.
std::uint64_t placedMeshClusterPairBound(const PlacedTable& table, std::span<const GpuPlacedBlock> blocks, std::span<const GpuPlacedItem> items,
                                         const GpuPlacedView& view) {
    const std::size_t kindsPerLayer = static_cast<std::size_t>(plantingMostKinds);
    std::vector<std::uint64_t> layerMost(table.kinds.size() / kindsPerLayer, 0);

    for (std::size_t kind = 0; kind < table.kinds.size(); ++kind) {
        std::uint64_t pairs = 0;

        for (std::uint32_t part = 0; part < table.kinds[kind].parts; ++part) {
            const GpuPlacedPart& placed = table.parts[table.kinds[kind].part[part]];

            for (std::uint32_t slot = 0; slot <= placed.levels.levels; ++slot) {
                pairs += table.groups[placed.firstGroup + slot].clusterCount;
            }
        }

        layerMost[kind / kindsPerLayer] = std::max(layerMost[kind / kindsPerLayer], pairs);
    }

    std::uint64_t bound = 0;

    for (std::uint32_t item = view.firstItem; item < view.firstItem + view.items; ++item) {
        bound += placedGroupSize * layerMost[blocks[items[item].block].firstKind / kindsPerLayer];
    }

    return bound;
}

MeshClusterList cullMeshClustersOnCpu(const MeshClusterTables& tables, const GpuCameraCull& view, std::uint32_t room,
                                      const MeshClusterOcclusionView* occlusion) {
    std::array<std::uint32_t, MESH_CLUSTER_WORK_WORDS> work{};
    const std::vector<std::uint32_t> ends = meshClusterWorkEnds(tables, view, work);
    const std::uint32_t blocks = work[MESH_CLUSTER_WORK_END] / meshClusterGroupSize;

    // Each block's survivors, then their prefix sum (one more for the total).
    std::vector<std::uint32_t> blockFirst(blocks + 1, 0);
    std::vector<MeshClusterPair> pairs(work[MESH_CLUSTER_WORK_END]);
    std::vector<bool> kept(pairs.size(), false);
    std::uint32_t candidates = 0;

    for (std::uint32_t index = 0; index < pairs.size(); ++index) {
        pairs[index] = meshClusterPairAt(tables, ends, index);
        const PairChoice choice = pairs[index].real ? choosePair(tables, pairs[index], view, occlusion) : PairChoice{};
        kept[index] = choice.kept;
        candidates += choice.candidate ? 1u : 0u;
        blockFirst[index / meshClusterGroupSize + 1] += kept[index] ? 1u : 0u;
    }

    for (std::uint32_t block = 0; block < blocks; ++block) {
        blockFirst[block + 1] += blockFirst[block];
    }

    MeshClusterList list;
    list.tested = work[MESH_CLUSTER_WORK_REAL];
    list.kept = blockFirst[blocks];
    list.candidates = candidates;
    list.items.resize(std::min(list.kept, room));
    std::uint32_t within = 0;

    for (std::uint32_t index = 0; index < pairs.size(); ++index) {
        if (index % meshClusterGroupSize == 0) {
            within = 0;
        }

        if (kept[index]) {
            const std::uint32_t place = blockFirst[index / meshClusterGroupSize] + within;
            ++within;

            if (place < room) {
                list.items[place] = GpuMeshClusterItem{pairs[index].entry, pairs[index].cluster};
                list.triangles += tables.clusters[pairs[index].cluster].counts >> MESH_CLUSTER_TRIANGLE_SHIFT;
            }
        }
    }

    for (std::uint32_t style = 0; style < MESH_CLUSTER_STYLES; ++style) {
        const std::uint32_t firstBlock = work[MESH_CLUSTER_WORK_STYLE_FIRST + style] / meshClusterGroupSize;
        const std::uint32_t endBlock = (style + 1 < MESH_CLUSTER_STYLES ? work[MESH_CLUSTER_WORK_STYLE_FIRST + style + 1] : work[MESH_CLUSTER_WORK_END]) /
                                       meshClusterGroupSize;
        list.runFirst[style] = std::min(blockFirst[firstBlock], room);
        list.runCount[style] = std::min(blockFirst[endBlock], room) - list.runFirst[style];
    }

    return list;
}

bool nearAMeshClusterEdge(const GpuMeshCluster& cluster, const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record,
                          const GpuCameraCull& view, const MeshClusterPose& pose) {
    const Vec3 eye{view.levels.eye[0], view.levels.eye[1], view.levels.eye[2]};
    const float budget = view.levels.pixelError;
    const auto nearTheBudget = [&](const float sphere[4], float error) {
        return std::fabs(meshClusterErrorPixels(sphere, error, instance, view) - budget) <= 1e-4f * budget;
    };

    if (nearTheBudget(cluster.ownSphere, cluster.ownError) ||
        (cluster.parentError <= static_cast<float>(MESH_CLUSTER_NEVER_COARSER) && nearTheBudget(cluster.parentSphere, cluster.parentError))) {
        return true;
    }

    if (record.bounds[3] >= 0.0f) {
        const Vec3 centre{record.bounds[0], record.bounds[1], record.bounds[2]};
        const Vec3 offset = centre - eye;

        if (std::fabs(std::sqrt(offset.dot(offset)) - record.bounds[3] - view.swayFadeEnd) < 1e-3f) {
            return true;
        }
    }

    if (meshClusterSeenWhole(instance, entry, record, view, pose)) {
        return false;
    }

    const std::array<float, 4> sphere = meshClusterCullSphere(cluster, instance, pose);

    for (const auto& plane : view.frustum.planes) {
        const float distance = plane[0] * sphere[0] + plane[1] * sphere[1] + plane[2] * sphere[2] + plane[3];

        if (std::fabs(distance + sphere[3]) < 1e-3f) {
            return true;
        }
    }

    const float determinant = meshClusterDeterminant(instance);

    if (pose.posed || cluster.cone[3] >= 1.0f || !(determinant > 0.0f)) {
        return false;
    }

    const Vec3 inMesh = meshClusterEyeInMesh(instance, eye, determinant);
    const Vec3 toApex = Vec3{cluster.cone[0], cluster.cone[1], cluster.cone[2]} - inMesh;
    const float distance = std::sqrt(toApex.dot(toApex));
    const float facing = toApex.dot(Vec3{cluster.axis[0], cluster.axis[1], cluster.axis[2]}) - cluster.cone[3] * distance;
    return std::fabs(facing) < 1e-4f * std::max(distance, 1.0f);
}

bool nearAMeshClusterEdge(const MeshClusterTables& tables, const GpuMeshClusterItem& item, const GpuCameraCull& view) {
    const MeshClusterDrawn drawn = meshClusterDrawnAt(tables, item.entry);
    const GpuMeshCluster& cluster = tables.clusters[item.cluster];
    return nearAMeshClusterEdge(cluster, drawn.instance, drawn.entry, drawn.record, view, meshClusterPoseAt(tables, drawn, cluster));
}

// Sorted, then walked together: equal pairs match, the rest are one side's only.
MeshClusterComparison compareMeshClusterLists(std::span<const GpuMeshClusterItem> gpu, std::span<const GpuMeshClusterItem> cpu,
                                              const std::function<bool(const GpuMeshClusterItem&)>& nearEdge) {
    const auto before = [](const GpuMeshClusterItem& a, const GpuMeshClusterItem& b) {
        return a.entry != b.entry ? a.entry < b.entry : a.cluster < b.cluster;
    };
    std::vector<GpuMeshClusterItem> gpuSorted(gpu.begin(), gpu.end());
    std::vector<GpuMeshClusterItem> cpuSorted(cpu.begin(), cpu.end());
    std::sort(gpuSorted.begin(), gpuSorted.end(), before);
    std::sort(cpuSorted.begin(), cpuSorted.end(), before);
    MeshClusterComparison comparison;
    comparison.entries = cpuSorted.size();
    std::size_t g = 0;
    std::size_t c = 0;

    while (g < gpuSorted.size() || c < cpuSorted.size()) {
        if (g < gpuSorted.size() && c < cpuSorted.size() && gpuSorted[g] == cpuSorted[c]) {
            ++comparison.matching;
            ++g;
            ++c;
        } else if (c < cpuSorted.size() && (g == gpuSorted.size() || before(cpuSorted[c], gpuSorted[g]))) {
            ++comparison.missing;
            comparison.nearAnEdge += nearEdge(cpuSorted[c]) ? 1u : 0u;
            ++c;
        } else {
            ++comparison.extra;
            comparison.nearAnEdge += nearEdge(gpuSorted[g]) ? 1u : 0u;
            ++g;
        }
    }

    return comparison;
}

std::string describeMeshClusterComparison(const MeshClusterComparison& comparison, std::uint32_t tested, std::uint32_t kept, bool occlusion) {
    const bool agree = occlusion ? comparison.extra <= comparison.nearAnEdge : comparison.missing + comparison.extra == comparison.nearAnEdge;
    const std::string missing = occlusion ? std::to_string(comparison.missing) + " hidden by occlusion, " : std::to_string(comparison.missing) + " missing, ";
    return "Mesh clusters: " + std::to_string(tested) + " tested, " + std::to_string(kept) + " kept; " + std::to_string(comparison.entries) +
           " on the CPU, " + std::to_string(comparison.matching) + " matching, " + missing +
           std::to_string(comparison.extra) + " extra (" + std::to_string(comparison.nearAnEdge) + " near an edge). " +
           (agree ? "The GPU's list agrees with the CPU's." : "The GPU's list DIFFERS from the CPU's.");
}

std::array<std::uint32_t, 3> meshClusterTaskGroups(std::uint32_t count) {
    if (count == 0) {
        return {0, 1, 1};
    }

    return {std::min(count, MESH_CLUSTER_TASK_WIDTH), (count + MESH_CLUSTER_TASK_WIDTH - 1) / MESH_CLUSTER_TASK_WIDTH, 1};
}

std::uint32_t meshClusterTaskIndex(std::uint32_t x, std::uint32_t y) {
    return y * MESH_CLUSTER_TASK_WIDTH + x;
}

void packMeshClusterBuffers(const MeshClusterBuffers& buffers, std::uint32_t words[12]) {
    const std::uint64_t addresses[6] = {buffers.items, buffers.totals, buffers.clusters, buffers.clusterVertices, buffers.triangleWords, buffers.vertices};

    for (int buffer = 0; buffer < 6; ++buffer) {
        words[2 * buffer] = static_cast<std::uint32_t>(addresses[buffer] & 0xFFFFFFFFu);
        words[2 * buffer + 1] = static_cast<std::uint32_t>(addresses[buffer] >> 32u);
    }
}
