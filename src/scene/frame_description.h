#pragma once

#include "ecs/world.h"
#include "math/mat4.h"
#include "scene/asset_handles.h"
#include "scene/camera.h"
#include "scene/debug_draw.h"
#include "scene/deforming_mesh.h"
#include "scene/lighting.h"
#include "scene/material.h"

#include <cstddef>
#include <cstdint>
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

    // A deforming mesh's shape this frame (DeformedMesh::shape, set by attachShapes), 0 for every other mesh: the shadow
    // cache's fingerprint of the casters includes it, so a caster that changes shape where it stands is drawn again.
    std::uint64_t shape = 0;
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

private:
    struct Key {
        std::uint32_t index = 0;
        std::uint32_t generation = 0;
        std::uint64_t part = 0;

        bool operator==(const Key& other) const = default;
    };

    struct KeyHash {
        std::size_t operator()(const Key& key) const;
    };

    std::unordered_map<Key, Mat4, KeyHash> last;
    std::unordered_map<Key, Mat4, KeyHash> current;
};

/*
* Everything the renderer needs for one frame, as plain data with no GPU types: the camera, the lights and every mesh to draw.
* Handing the renderer the whole frame at once lets it draw the scene more than once (a shadow map from the light, then the
* camera's view) and skip what the camera cannot see, which a draw-one-mesh-now interface cannot do.
*/
struct FrameDescription {
    Camera camera;
    FrameLighting lighting;
    std::vector<DrawItem> draws;

    // Where the action is: when there are more lights than the renderer can use, the brightest ones here win.
    // The camera's position unless the game names a better point (a MOBA camera floats far above the ground it shows).
    Vec3 lightFocus{0.0f, 0.0f, 0.0f};

    // Real time since the last frame, for things the renderer animates itself (lights fading between seats).
    float frameSeconds = 0.0f;

    // Wireframe shapes drawn over the scene (see DebugDraw), and lines on the screen itself in window pixels (overlays, text).
    std::vector<DebugLine> debugLines;
    std::vector<DebugLine> debugScreenLines;

    // This frame's shape of every deforming mesh (scene/deforming_mesh.h), for the renderer to upload before drawing.
    std::vector<DeformedMesh> deformedMeshes;
};

// Give each draw of a deformed mesh its shape's fingerprint.
void attachShapes(std::vector<DrawItem>& draws, const std::vector<DeformedMesh>& deformedMeshes);

// Describe the World as seen by camera, alpha (0..1) of the way between the last two ticks. With a history, each draw
// gets its matrix from last frame, and this frame's are remembered for the next.
FrameDescription buildFrame(World& world, const Camera& camera, float alpha, MotionHistory* history = nullptr);
