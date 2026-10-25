#pragma once

#include "cloth/cloth.h"
#include "math/vec4.h"
#include "physics/shapes.h"
#include "render/gpu/cloth_gpu_shared.h"
#include "scene/asset_handles.h"
#include "scene/deforming_mesh.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/*
* Cloth on the GPU, the part that does not call the graphics API: the records the cloth passes read (std430, each C++
* mirror's size checked) and packing a Cloth and its ticks into them. The solver is the CPU's (cloth/cloth.h) ported as
* it is, so these hold the same numbers: the points, the coloured constraints, and each tick's fabric, pins, hand and
* bodies. A point's position and inverse mass share a vec4 (w), since every pass that moves a point weighs it.
*/
inline constexpr std::uint32_t clothGroupSize = CLOTH_GROUP_SIZE;
inline constexpr std::uint32_t clothNoPin = CLOTH_NO_PIN;
inline constexpr std::uint32_t clothNoGrab = CLOTH_NO_GRAB;

static_assert(static_cast<int>(BendingModel::Isometric) == CLOTH_BEND_ISOMETRIC && static_cast<int>(BendingModel::Hinge) == CLOTH_BEND_HINGE,
              "The GPU's bending model numbers are BendingModel's values");

struct GpuStretch {
    std::uint32_t first = 0;
    std::uint32_t second = 0;
    float restLength = 0.0f;
    float unused = 0.0f;
};

static_assert(sizeof(GpuStretch) == 16, "GpuStretch must match cloth.glsl's ClothStretch");

struct GpuBend {
    std::uint32_t wing0 = 0;
    std::uint32_t wing1 = 0;
    std::uint32_t edgeStart = 0;
    std::uint32_t edgeEnd = 0;
    float restAngle = 0.0f;
    float stiffnessShare = 0.0f;
    float unused[2]{};
    float isometricWeights[4]{}; // wing0, wing1, edgeStart, edgeEnd
};

static_assert(sizeof(GpuBend) == 48, "GpuBend must match cloth.glsl's ClothBend");

// A body: a sphere (a its centre), a capsule (a to b) or a box (a its minimum, b its maximum).
struct GpuCollider {
    std::uint32_t kind = CLOTH_COLLIDER_SPHERE;
    float radius = 0.0f;
    float unused[2]{};
    Vec4 a;
    Vec4 b;
};

static_assert(sizeof(GpuCollider) == 48, "GpuCollider must match cloth.glsl's ClothCollider");

// Where a moving pin's point starts the tick and where it ends it (ClothPin), slid evenly through the substeps.
struct GpuClothPin {
    Vec4 start;
    Vec4 target;
};

static_assert(sizeof(GpuClothPin) == 32, "GpuClothPin must match cloth.glsl's ClothPin");

/*
* One tick: the step's length and everything worked out from it once on the CPU, as the CPU solver works them out
* (compliances over the substep's length squared, the drag's share kept a substep), so both sides start from the same
* numbers; its pins (pinCount from pinFirst in the tick's pins) and bodies, and the hand.
*/
struct GpuClothTick {
    float substepSeconds = 0.0f;
    float gravity = 0.0f;
    float stretchCompliance = 0.0f; // over the substep squared
    float bendingStiffness = 0.0f;

    float bendPerSecondSquared = 0.0f; // 1 / substep squared
    float grabCompliance = 0.0f;       // over the substep squared
    float kept = 0.0f;                 // of each velocity, a substep
    float thickness = 0.0f;

    float staticFriction = 0.0f;
    float kineticFriction = 0.0f;
    std::uint32_t bendingModel = CLOTH_BEND_ISOMETRIC;
    std::uint32_t substeps = 0;

    std::uint32_t pinFirst = 0;
    std::uint32_t colliderFirst = 0;
    std::uint32_t colliderCount = 0;
    std::uint32_t grabParticle = CLOTH_NO_GRAB;

    Vec4 grabTarget;
};

static_assert(sizeof(GpuClothTick) == 80, "GpuClothTick must match cloth.glsl's ClothTick");

/*
* A cloth the renderer simulates this frame (VulkanRenderer::setClothWork): which cloth (key, its entity, the same every
* frame it lives), its state on the CPU (uploaded the first frame the renderer sees it, or after its pins change; read
* only during that frame's render), the deforming mesh shaped from its points on the GPU with its topology (none to
* simulate only) and the ticks the game queued since the last frame. A cloth sent with no ticks is still drawn, between
* its last two.
*/
struct ClothWork {
    std::uint64_t key = 0;
    const Cloth* cloth = nullptr;
    MeshHandle mesh;
    const DeformingTopology* topology = nullptr;
    std::vector<ClothTickCommands> ticks;
};

// A cloth's points and velocities as the GPU had them a few frames ago (VulkanRenderer::takeClothReadbacks).
struct ClothReadback {
    std::uint64_t key = 0;
    std::vector<Vec3> positions;
    std::vector<Vec3> velocities;
};

// Each point's position with its inverse mass in w.
std::vector<Vec4> packClothPoints(const ClothParticles& particles);

std::vector<GpuStretch> packStretch(const std::vector<StretchConstraint>& constraints);
std::vector<GpuBend> packBend(const std::vector<BendConstraint>& constraints);
GpuCollider packCollider(const Shape& shape);

// For each point, which of the cloth's pins moves it (its place in Cloth::pins), or clothNoPin.
std::vector<std::uint32_t> clothPinSlots(const Cloth& cloth);

/*
* A tick's record, its pins from pinFirst and bodies from colliderFirst in the frame's lists. Throws
* std::invalid_argument for fewer than one substep, as the CPU solver does.
*/
GpuClothTick packClothTick(const ClothTickCommands& commands, std::uint32_t pinFirst, std::uint32_t colliderFirst);

/*
* The dispatches one tick records: per substep, the prediction, one per stretch colour, one per bending colour (none
* without bending) and the end (the hand, the bodies, the velocities).
*/
std::uint32_t clothDispatchesPerTick(const Cloth& cloth, const ClothSettings& settings);
