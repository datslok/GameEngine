#pragma once

#include <span>

#include "scene/flat_faces.h"
#include "core/job_system.h"
#include "ecs/world.h"
#include "math/mat4.h"
#include "scene/asset_handles.h"
#include "scene/camera.h"
#include "scene/debug_draw.h"
#include "scene/deforming_mesh.h"
#include "scene/lighting.h"
#include "scene/material.h"
#include "scene/mesh_level.h"
#include "scene/model_renderer.h"
#include "scene/terrain_heights.h"
#include "scene/wind.h"
#include "physics/static_level.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

// A sphere around something, in world space. A negative radius means the size is unknown.
struct BoundingSphere {
    Vec3 centre{0.0f, 0.0f, 0.0f};
    float radius = -1.0f;
};

// One mesh to draw: which mesh, where (its full model matrix), and how it looks.
struct DrawItem {
    MeshHandle mesh;
    Mat4 model = Mat4::identity();
    Material material;
    bool castsShadows = true;

    // Part of the level that never moves (ModelRenderer::isStatic): light probes move off it and stay asleep beside it.
    bool isStatic = false;

    // Filled in by the engine from the mesh's bounds (attachBoundingSpheres), so views can skip what they cannot see.
    BoundingSphere bounds;

    // The model matrix the same part was drawn with last frame (MotionHistory), so the renderer can tell how it moved
    // (motion vectors for temporal anti-aliasing). None on its first frame, or without a history: treated as not moving.
    std::optional<Mat4> previousModel;

    // Drawn into shadow maps only (ModelRenderer::shadowOnly): no camera view picks it (visibleDraws), no ray meets it.
    bool shadowOnly = false;

    // How many of the sun's cascades it casts into (ModelRenderer::shadowCascades; 0 all).
    int shadowCascades = 0;

    // The mesh's flat faces (scene/flat_faces.h, attachFlatFaces), held by the engine, which planar reflections choose from.
    std::span<const FlatFace> flatFaces{};

    // The mesh's coarser levels (scene/mesh_level.h, attachMeshLevels), held by the engine: each view draws the one its
    // distance allows (scene/level_of_detail.h). The other uses of mesh (bounds, rays, flat faces) keep the mesh itself.
    std::span<const MeshLevel> levels{};

    // The mesh's level options (MeshLevelOptions, set by attachMeshLevels): drawn only where this error fits (an impostor;
    // 0 always), and the sphere levels are chosen by (radius below 0: bounds).
    float hiddenBelowError = 0.0f;
    BoundingSphere levelBounds{};

    // Drawn by no view and met by no ray, but part of the static level (ModelRenderer::hidden).
    bool hidden = false;

    // A terrain patch's index in FrameDescription::terrainPatches (scene/terrain.h), -1 for every other draw: its mesh is a
    // flat grid the vertex shaders lift onto the land.
    int terrainPatch = -1;

    // Lifted onto the land by the vertex shaders (ModelRenderer::followsGround, scene/ground_follow.h); its bounds lifted
    // to match by the engine (liftGroundDraws).
    bool followsGround = false;

    // A deforming mesh's shape this frame (DeformedMesh::shape, set by attachShapes), 0 for every other mesh: the shadow
    // cache's fingerprint of the casters includes it, so a caster that changes shape where it stands is drawn again.
    std::uint64_t shape = 0;

    // How it sways in the wind (ModelRenderer::sway): the vertex shaders bend it, the velocity pass redraws it every
    // frame and the shadow tiles that sway redraw while it is in them (render/gpu/wind.h).
    WindSway sway{};

    // One past its mesh's first entry in the renderer's hanging vertices (IndexedMesh::hanging, attachVertexEntries;
    // 0 for a mesh with none), so the vertex shaders find each vertex's anchors and arm.
    std::uint32_t hangingEntry = 0;

    // One past its mesh's header in the renderer's card thinning records (IndexedMesh::thinning, attachVertexEntries; 0 for
    // a mesh not thinned), so the vertex shaders shrink and grow its cards (scene/card_thinning.h).
    std::uint32_t thinningEntry = 0;
};

// Where a mesh's per-vertex records begin in the renderer's buffers, as it gave them when the mesh was uploaded: one past
// its first hanging vertex (DrawItem::hangingEntry), one past its card thinning header (DrawItem::thinningEntry); 0 none.
struct VertexEntries {
    std::uint32_t hanging = 0;
    std::uint32_t thinning = 0;
};

/*
* What every model part was drawn with last frame, so each draw can say how it moved since. Kept by the engine between
* frames and render-only, so it never touches the simulation. Keyed by the entity (its slot and its generation, so a new
* entity in a destroyed one's slot starts afresh) and the part's place in its model.
*/
class MotionHistory {
public:
    // Last frame's matrix for this part, if it was drawn then.
    std::optional<Mat4> previous(Entity entity, std::size_t part) const;

    // This frame's matrix for this part.
    void remember(Entity entity, std::size_t part, const Mat4& model);

    // Ends a frame: what it remembered becomes last frame's, and parts it did not draw are forgotten.
    void endFrame();

    // Room for every entity slot below count, made before entities remember from several threads at once (each entity
    // writes only its own slot's record, so they never meet).
    void reserveSlots(std::size_t count);

private:
    /*
    * One entity slot's matrices, a part each, and the frame they were remembered in and the generation of the entity
    * that remembered them. Two sets of records take turns by the frame's parity: this frame's and last frame's. A
    * record counts as last frame's only when its frame stamp says so, so nothing is cleared between frames, and once
    * every slot's parts have been seen nothing is allocated either (with 20,000 objects, two hash maps rebuilt every
    * frame cost 14 ms).
    */
    struct Record {
        std::uint32_t generation = 0;
        std::uint64_t frame = 0;
        std::vector<Mat4> parts;
    };

    std::vector<Record> records[2];
    std::uint64_t frame = 2; // starts past the records' stamp of 0, so none counts as last frame's on the first
};

// Draws first to first + count of a frame's list.
struct DrawRange {
    std::size_t first = 0;
    std::size_t count = 0;
};

class StillDraws;

/*
* Everything the renderer needs for one frame, as plain data with no GPU types: the camera, the lights and every mesh to draw.
* Handing the renderer the whole frame at once lets it draw the scene more than once (a shadow map from the light, then the
* camera's view) and skip what the camera cannot see, which a draw-one-mesh-now interface cannot do.
*/
struct FrameDescription {
    Camera camera;
    FrameLighting lighting{};
    std::vector<DrawItem> draws{};

    // The draws of still entities (ModelRenderer::still) come first, stillDraws of them, kept from last frame but for
    // builtStillDraws, built this frame; every draw after them was built this frame (scene/still_draws.h). None without
    // kept draws.
    std::size_t stillDraws = 0;
    std::vector<DrawRange> builtStillDraws{};

    // Each frame from kept draws is a new version and names the one before, so the renderer, which keeps what it packed
    // from them, knows builtStillDraws is all that changed only when it packed that one (render/gpu/still_instances.h).
    std::uint64_t stillVersion = 0;
    std::uint64_t stillPreviousVersion = 0;

    // Where the action is: when there are more lights than the renderer can use, the brightest ones here win.
    // The camera's position unless the game names a better point (a MOBA camera floats far above the ground it shows).
    Vec3 lightFocus{0.0f, 0.0f, 0.0f};

    // Real time since the last frame, for things the renderer animates itself (lights fading between seats).
    float frameSeconds = 0.0f;

    // Wireframe shapes drawn over the scene (see DebugDraw), and lines on the screen itself in window pixels (overlays, text).
    std::vector<DebugLine> debugLines{};
    std::vector<DebugLine> debugScreenLines{};

    // This frame's shape of every deforming mesh (scene/deforming_mesh.h), for the renderer to upload before drawing.
    std::vector<DeformedMesh> deformedMeshes{};

    // The level's parts that never move and stand in the sky's way, as triangles for rays on the CPU (the sky visibility
    // grid, render/gpu/sky_visibility_bake.h), built by the engine from the static draws (engine/static_geometry.h); none
    // when nothing blocks the sky. The version changes whenever the level does.
    std::shared_ptr<const StaticLevel> staticLevel{};
    std::uint64_t staticLevelVersion = 0;

    // Each of its triangles' diffuse albedo, in linear light and in its triangle order, for the bounce light the grid
    // gathers; changing it changes the version too.
    std::shared_ptr<const std::vector<Vec3>> staticAlbedo{};

    // The terrain's patches this frame (DrawItem::terrainPatch indexes them) and the heights they are lifted onto, which
    // the renderer uploads again only when terrainHeightsVersion changes (scene/terrain.h); none without a terrain.
    std::vector<TerrainPatch> terrainPatches{};
    std::shared_ptr<const TerrainHeights> terrainHeights{};
    std::uint64_t terrainHeightsVersion = 0;

    // The wind over the land (scene/wind.h, the first Wind placed), which the renderer's wind field turns into sway;
    // none without one.
    std::optional<Wind> wind{};

    // What hanging things hang from (scene/wind.h, the first WindAnchors placed), none without.
    std::shared_ptr<const std::vector<WindAnchor>> windAnchors{};
};

// Give each draw of a deformed mesh its shape's fingerprint.
void attachShapes(std::span<DrawItem> draws, const std::vector<DeformedMesh>& deformedMeshes);

// Describe the World as seen by camera, alpha (0..1) of the way between the last two ticks. With a history, each draw
// gets its matrix from last frame, and this frame's are remembered for the next. The list keeps room for laterDraws
// more (the terrain's), so appending them never moves it. With still, the still entities' draws come first from it,
// kept from last frame (give the list back with StillDraws::giveBack once drawn), and the rest are built after them.
FrameDescription buildFrame(World& world, const Camera& camera, float alpha, MotionHistory* history = nullptr,
                            JobSystem& jobs = serialJobs(), std::size_t laterDraws = 0, StillDraws* still = nullptr);

// One entity's draws, a part each, into draws (as many as its parts), placed by its transform at alpha; with a
// history, each part's last-frame matrix from it, and this frame's remembered.
void buildEntityDraws(const World& world, Entity entity, const ModelRenderer& renderer, float alpha, MotionHistory* history,
                      std::span<DrawItem> draws);
