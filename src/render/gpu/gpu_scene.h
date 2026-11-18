#pragma once

#include "core/job_system.h"
#include "math/mat4.h"
#include "math/vec3.h"
#include "math/vec4.h"
#include "render/gpu/clusters.h"
#include "render/gpu/instance_shared.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/material_uniforms.h"
#include "render/gpu/shadow_map.h"
#include "scene/asset_handles.h"
#include "scene/frame_description.h"
#include "scene/indexed_mesh.h"
#include "scene/level_of_detail.h"
#include "scene/material.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

/*
* The GPU scene: the frame's objects, materials and lights as arrays in storage buffers, written once per frame, which
* the shaders index themselves: every object of the frame once (its instance), then a draw list of instance numbers, one
* run per view and mesh, each vertex shader reading draw list entry gl_InstanceIndex; an instance carries its material's
* number, a material its textures' numbers. A draw then names only a mesh and a run of the draw list, so every object
* with the same mesh is one instanced draw in each view, and nothing is bound per object. These are the parts that do not touch the
* graphics API: the records' layouts (std430, mirrored in the shaders) and how the frame's draws are grouped.
*/

/*
* One object as the scene's vertex shaders read it from the instance buffer, the same for every view (the camera's and
* every shadow tile's), so it is written once per frame: the world model matrix, the normal matrix and the handedness
* (all column-major), and the number of its material in the frame's material table. Each view multiplies in its own
* matrix on the GPU. Ray tracing's instances (6d) and GPU culling will read the same records.
*/
// An instance's kinds (GpuInstance::material[3]), from instance_shared.h.
inline constexpr std::uint32_t instanceKindPlain = INSTANCE_KIND_PLAIN;
inline constexpr std::uint32_t instanceKindImpostor = INSTANCE_KIND_IMPOSTOR;
inline constexpr std::uint32_t instanceKindFollowsGround = INSTANCE_KIND_FOLLOWS_GROUND;

struct GpuInstance {
    float model[16];        // places the vertex in the world
    float normalMatrix[16]; // inverse transpose of the model's 3x3 part (zero if squashed flat)
    float handedness[4];    // x: -1 if the model matrix mirrors space, else +1; y: its scale (uniformScale), for lengths in model units
    std::uint32_t material[4]; // x: the material's number in the frame's material buffer, y: the mesh's (for ray hits), z: its terrain patch's one past (0 none), w: its kind (instance_shared.h)
    float sway[4];          // how it sways in the wind (render/gpu/wind.h, packSway): category (-1 still), height, bend, flutter
    std::uint32_t hanging[4]; // x: one past its mesh's first hanging vertex (DrawItem::hangingEntry, 0 none), y: its swing's bits (WindSway::swing), z: one past its mesh's card thinning header (DrawItem::thinningEntry, 0 none)
    float levelSphere[4];     // the sphere its level of detail is chosen by, in the world (DrawItem::levelBounds, else its bounds): xyz centre, w radius (below 0 unknown)
};

static_assert(sizeof(GpuInstance) == 208, "GpuInstance must match the shader's InstanceData struct");

// A shadow tile's view, pushed before its draws: the view-projection (column-major, world space), the plane its draws
// are clipped at (ShadowTile::clipPlane, world space; noClipPlane for all but an image light's), and where its light is
// (ShadowTile::lightPlace: a position with w 1, the direction towards the sun with w 0), which the coloured shadow map's
// glass (glass_shadow.vert) needs, and its options: x 1 when its casters sway in the wind (render/gpu/wind.h, tileSways).
struct GpuShadowView {
    float matrix[16];
    float clipPlane[4];
    float lightPlace[4];
    float options[4];
};

static_assert(sizeof(GpuShadowView) == 112, "GpuShadowView must match shadow.vert's push constants");

/*
* A capture's view (a reflection probe cube face's, or a flat mirror's), pushed before its draws: the view-projection
* (column-major, camera-relative) and the plane triangle.vert clips at (render/gpu/planar_reflection.h, planarClipPlane;
* noClipPlane for the cube and for the passes that push it only because the shader names it), and the eye the capture
* is seen from, measured from the camera (xyz; w its error allowed a metre away, for card thinning): the camera itself for the cube, the camera reflected across
* the mirror for a mirror's picture (reflectAcrossPlane), which the capture's shading takes its view direction from. Seen
* from the real camera instead, a wall facing the mirror with the camera behind it was looked up in the sky visibility
* grid from its back (its lookup moved towards the camera, through the wall) and lit as open sky.
*/
struct GpuCaptureView {
    float viewProjection[16];
    float clipPlane[4];
    float eye[4];
};

static_assert(sizeof(GpuCaptureView) == 96, "GpuCaptureView must match triangle.vert's push constants");

/*
* The frame's small uniform block (std140): for the vertex shader the camera's view-projection measured from the camera
* and the camera's position, which make each instance camera-relative (render/gpu/camera_relative.h); for every pixel
* the ambient light, how many lights of each kind the light buffers hold this frame, and the size of one shadow atlas texel;
* and the cluster grid (render/gpu/clusters.h), as clusters.glsl's ClusterGrid, for the main pass and cluster_lights.comp.
*/
struct GpuFrameData {
    float relativeViewProjection[16];
    float cameraOrigin[4];  // xyz
    float ambient[4];
    std::int32_t counts[4]; // x: directional lights, y: point lights, z: spotlights
    float atlasTexel[4];    // xy: one texel's size in atlas coordinates
    std::int32_t clusterTiles[4]; // x: tiles across, y: tiles down, z: depth slices
    float clusterScreen[4];       // xy: the screen in pixels, z: half the view's width at depth 1, w: its half height
    float clusterSlicing[4];      // x: slice scale, y: slice bias
    float viewRight[4];           // xyz: the view's axes in the world
    float viewUp[4];
    float viewForward[4];
    std::int32_t shadingOptions[4]; // x: 1 shades from the clusters' lists, y: 1 shows their light counts (the heatmap)
    float cascadeOptions[4];        // x: the sun cascades' blend band (a share of a radius), y: 1 dithers between them instead
    // The light probes (render/gpu/ddgi_sampling.h, packProbeSampling), all zero while they are not sampled.
    float probeShading[4];          // x: the self-shadow bias setting, y: the share of probe light a probe ray's hit passes on
    std::int32_t probeAtlas[4];     // xy: the probe atlases' columns and rows (ddgi.h, probeAtlasGrid), w: how many cascades
    std::int32_t probeLights[4];    // x: point lights listed for probe rays' hits, y: spotlights, z: how many each hit keeps
    float textureOptions[4];        // x: the surface textures' mip bias (render/gpu/upscaling.h, textureLodBias), y: 1 filters normal maps (Toksvig), z: geometric specular anti-aliasing's footprint variance (0 off), w: 1 draws clear coats
    std::int32_t mirroredLightOptions[4]; // x: 1 when the main pass adds the light mirrors reflect (render/gpu/mirrored_light.h), y: the frame, turning rough mirrors' samples
    std::int32_t ambientOcclusionOptions[4]; // x: 1 when the main pass reads ambient occlusion (render/gpu/ambient_occlusion.h), y: 1 with multiple bounces, z: 1 with specular occlusion
    float skyVisibilityOptions[4]; // the sky visibility grid (render/gpu/sky_visibility.h, packSkyVisibilityOptions): x its cascades read (0: not read), y the ambient floor, z the lookup bias, w 1 for its debug view
    float contactShadowOptions[4]; // x: 1 when the main pass reads the sun's contact shadows (render/gpu/contact_shadows.h), y: 1 when it reads the local lights'
    std::int32_t contactLightSlots[4]; // the local lights with a contact shadow channel, by channel: a point light's slot, a spotlight's CONTACT_SPOT_SLOT_BASE past its slot, -1 none
    float materialOptions[4]; // parallax occlusion mapping (render/gpu/parallax.h): x 1 draws it, y 1 self-shadows from the sun, z the most steps; w 1 lets light through leaves (brdf.h, addDiffuseTransmission)
    float levelOptions[4];    // x: the camera's error allowed a metre away, for card thinning (scene/card_thinning.h, thinningErrorPerMetre; 0 thins nothing), y: the frame turning the level fades' dither (render/gpu/level_fade.h)
};

static_assert(sizeof(GpuFrameData) == 432, "GpuFrameData must match the shader's FrameData block");

// One object's instance, with the number its material got in the frame's MaterialTable.
GpuInstance packInstance(const DrawItem& draw, std::uint32_t material);

GpuShadowView packShadowView(const Mat4& tileMatrix, const Vec4& clipPlane, const Vec4& lightPlace, bool sways = false);

// thinningPerMetre: the capture's error allowed a metre away, for card thinning (thinningErrorPerMetre; 0 thins nothing).
GpuCaptureView packCaptureView(const Mat4& viewProjection, const Vec4& clipPlane, const Vec3& eye = Vec3{}, float thinningPerMetre = 0.0f);

GpuFrameData makeFrameData(const GpuLights& lights, const Mat4& relativeViewProjection, const Vec3& cameraOrigin,
                           const ClusterGrid& clusters, bool clusteredShading, bool clusterHeatmap, const ShadowSettings& shadows = {});

/*
* The frame's materials, one entry per different material: draws carry their materials by value, and many share one (a
* hundred pillars of the same stone), so add() returns the number of an identical entry when there is one.
*
* A table may sit over a base (the still draws' table, render/gpu/still_instances.h), which must outlive it: the base's
* entries keep their numbers, and only materials the base lacks are held here, numbered after the base's, so the GPU
* holds the base's entries then these.
*/
class MaterialTable {
public:
    // An empty table over base.
    static MaterialTable over(const MaterialTable& base);

    std::uint32_t add(const Material& material);

    // The same for a material already packed (packMaterial, not a blended ground) and the hash of its bytes
    // (materialHash): the index of an equal entry, or a new one.
    std::uint32_t addPacked(const GpuMaterial& packed, std::uint64_t hash);

    // This table's own entries (numbered from getFirstIndex), and the entry a number names, the base's included.
    const std::vector<GpuMaterial>& getEntries() const;
    const GpuMaterial& at(std::uint32_t index) const;
    std::uint32_t getFirstIndex() const;

private:
    // The number of an equal entry among this table's own, if any.
    std::optional<std::uint32_t> findOwn(const GpuMaterial& packed, std::uint64_t hash) const;

    const MaterialTable* base = nullptr;
    std::uint32_t firstIndex = 0;
    std::vector<GpuMaterial> entries;

    // Entries by a hash of their bytes; equal hashes are compared in full, so a collision only costs a comparison.
    std::unordered_multimap<std::uint64_t, std::uint32_t> byHash;

    // The last few entries found or made, checked first: a frame's draws use a few dozen materials in runs that
    // alternate (a tree's bark, then its leaves), so most look-ups end here without touching the hash table.
    static constexpr std::size_t recentCount = 8;
    std::array<std::uint64_t, recentCount> recentHashes{};
    std::array<std::uint32_t, recentCount> recentIndices{};
    std::size_t recentFilled = 0;
    std::size_t recentNext = 0;
};

// The hash of a packed material's bytes, as MaterialTable files it.
std::uint64_t materialHash(const GpuMaterial& packed);

/*
* Every draw's instance (packInstance) with its material's index in materials, as adding them one by one in order would
* give: the materials packed and hashed and the instances packed on every core, the indices handed out in draw order on
* one (with 20,000 objects, packing on one thread took 11 ms).
*/
std::vector<GpuInstance> packInstances(std::span<const DrawItem> draws, MaterialTable& materials, JobSystem& jobs);

// One instanced draw: mesh, drawn instanceCount times, as instances firstInstance onwards, and how its draws are drawn:
// cut-outs by pipelines that throw away their holes, double-sided ones without culling back faces.
struct DrawBatch {
    MeshHandle mesh;
    std::uint32_t firstInstance = 0;
    std::uint32_t instanceCount = 0;
    bool cutOut = false;
    bool doubleSided = false;
};

// The chosen draws grouped by mesh: order lists the draw indices in instance order (a fading draw twice, once for each
// of its levels), and each batch covers a run of them; marks holds, in the same order, the bits each entry adds to its
// instance: the thinned bit where it is drawn with its thinned mesh (DrawItem::thinningEntry, the mesh itself or a level
// sharing its vertices), whose cards the vertex shaders thin, and its level fade field (render/gpu/level_fade.h).
struct BatchedDraws {
    std::vector<std::size_t> order;
    std::vector<DrawBatch> batches;
    std::vector<std::uint32_t> marks;
};

// A draw list entry: the instance's number in its low bits, its top bit set where the view draws it thinned, its fade
// field between (instance_shared.h).
inline constexpr std::uint32_t drawListThinnedBit = DRAW_LIST_THINNED_BIT;
inline constexpr std::uint32_t drawListIndexMask = DRAW_LIST_INSTANCE_MASK;

// Appends a view's draw list entries, in instance order.
void appendDrawList(std::vector<std::uint32_t>& list, const BatchedDraws& batched);

/*
* A frame's draw list as the CPU lays it out (docs/architecture/gpu-driven.md): regions the GPU culling writes are only
* counted (reserve), the CPU's own entries (glass, its shadows) kept in runs at their places (append), runs side by side
* joined. Writing the runs alone leaves the GPU's regions unwritten, which nothing reads before the GPU fills them.
*/
class DrawListLayout {
public:
    struct Run {
        std::uint32_t first = 0;
        std::vector<std::uint32_t> entries;
    };

    // Room for entries the GPU writes; returns their first place.
    std::uint32_t reserve(std::size_t entries);

    // The CPU's entries next; returns their first place.
    std::uint32_t append(std::span<const std::uint32_t> entries);

    // Every place, the GPU's regions included.
    std::uint32_t size() const;

    const std::vector<Run>& getRuns() const;

private:
    std::uint32_t entries = 0;
    std::vector<Run> runs;
};

/*
* The card thinning records (scene/card_thinning.h), std430: a thinned mesh's header (card size, growth, least share)
* and then one record a vertex (its card's anchor and rank). Mirrored in card_thinning_data.glsl.
*/
struct GpuThinningRecord {
    float values[4];
};

static_assert(sizeof(GpuThinningRecord) == 16, "GpuThinningRecord must match the shader's records");

// A mesh's header and records, none for a mesh not thinned. Throws std::invalid_argument without one card a vertex.
std::vector<GpuThinningRecord> packCardThinning(const IndexedMesh& mesh);

/*
* Group the chosen draws (indices into draws) by how they are drawn (plain, double-sided, cut-out, double-sided cut-out)
* and then by mesh, meshes in handle order and each mesh's draws in the order they were chosen, so the same frame always gives the same instances. Instances are numbered from firstInstance, for views that
* write theirs after another view's in the same buffer (shadow tiles). With a view, each draw is drawn with the mesh
* its level of detail picks there (scene/level_of_detail.h), and grouped by it; a draw drawing nothing there is left out;
* a draw fading there (levelFade) is drawn with both its levels, each marked with its fade field.
*/
BatchedDraws batchByMesh(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& chosen, std::uint32_t firstInstance = 0,
                         const LevelView* levels = nullptr);

// One batch's draw as the GPU reads it from a buffer (vkCmdDrawIndexedIndirect): byte for byte
// VkDrawIndexedIndirectCommand, so GPU-driven drawing (docs/research/GPUDRIVEN.md) can later write instanceCount itself.
struct GpuDrawCommand {
    std::uint32_t indexCount = 0;
    std::uint32_t instanceCount = 0;
    std::uint32_t firstIndex = 0;
    std::int32_t vertexOffset = 0;
    std::uint32_t firstInstance = 0;
};

static_assert(sizeof(GpuDrawCommand) == 20, "GpuDrawCommand must match VkDrawIndexedIndirectCommand");

