#pragma once

#include "cloth/cloth.h"
#include "math/vec4.h"
#include "physics/shapes.h"
#include "physics/signed_distance_field.h"
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
inline constexpr std::uint32_t clothPointLanes = CLOTH_POINT_LANES;
inline constexpr std::uint32_t clothNoPin = CLOTH_NO_PIN;
inline constexpr std::uint32_t clothNoGrab = CLOTH_NO_GRAB;

static_assert(static_cast<int>(BendingModel::Isometric) == CLOTH_BEND_ISOMETRIC && static_cast<int>(BendingModel::Hinge) == CLOTH_BEND_HINGE,
              "The GPU's bending model numbers are BendingModel's values");
static_assert(static_cast<int>(ClothMethod::Xpbd) == CLOTH_METHOD_XPBD && static_cast<int>(ClothMethod::VertexBlockDescent) == CLOTH_METHOD_BLOCK_DESCENT,
              "The GPU's solver numbers are ClothMethod's values");

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

// One tether slot (ClothTether): its anchor, or noTetherAnchor for a spare slot, and its rest length.
struct GpuClothTether {
    std::uint32_t anchor = noTetherAnchor;
    float restLength = 0.0f;
};

static_assert(sizeof(GpuClothTether) == 8, "GpuClothTether must match cloth.glsl's ClothTether");

/*
* A field body (DistanceFieldCollider): where its first sample is, with the spacing in w, its samples along each axis,
* where they start in the fields' buffer (one buffer of every field's samples, shared by the cloths:
* render/vulkan/vulkan_cloth.h, VulkanClothFields), and its band.
*/
struct GpuClothField {
    Vec4 origin;
    std::uint32_t counts[3]{};
    std::uint32_t firstSample = 0;
    float band = 0.0f;
    float unused[3]{};
};

static_assert(sizeof(GpuClothField) == 48, "GpuClothField must match cloth.glsl's ClothField");

// Where a moving pin's point starts the tick and where it ends it (ClothPin), slid evenly through the substeps.
struct GpuClothPin {
    Vec4 start;
    Vec4 target;
};

static_assert(sizeof(GpuClothPin) == 32, "GpuClothPin must match cloth.glsl's ClothPin");

/*
* One tick: the step's length and everything worked out from it once on the CPU, as the CPU solver works them out
* (XPBD's compliances over the substep's length squared, vertex block descent's stiffnesses (energyStiffness), the
* drag's share kept a substep), so both sides start from the same numbers; its pins (from pinFirst in the tick's pins),
* bodies and field bodies, the hand, the solver, and the stretch the tethers allow (1 + the slack, 0 with the tethers off).
*/
struct GpuClothTick {
    float substepSeconds = 0.0f;
    float gravity = 0.0f;
    float stretchCompliance = 0.0f; // over the substep squared
    float bendingStiffness = 0.0f;

    float perSecondSquared = 0.0f; // 1 / substep squared
    float grabCompliance = 0.0f;   // over the substep squared
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

    float stretchStiffness = 0.0f; // N/m, the energy's
    float grabStiffness = 0.0f;    // N/m, the energy's
    std::uint32_t method = CLOTH_METHOD_XPBD;
    float tetherStretch = 0.0f;

    std::uint32_t fieldFirst = 0;
    std::uint32_t fieldCount = 0;
    std::uint32_t unused[2]{};
};

static_assert(sizeof(GpuClothTick) == 112, "GpuClothTick must match cloth.glsl's ClothTick");

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

// A point on another level and its weight (ClothLink).
struct GpuClothLink {
    std::uint32_t point = 0;
    float weight = 0.0f;
};

static_assert(sizeof(GpuClothLink) == 8, "GpuClothLink must match cloth.glsl's ClothLink");

/*
* Where one coarse level's lists start in GpuClothLevels' buffers: its points (and their masses) from pointsFirst, and
* its rows of starts (each its points' count plus one, or the cloth's point count plus one for spreading) from the
* other firsts. The starts themselves count from the front of their lists' buffers.
*/
struct GpuClothLevel {
    std::uint32_t pointsFirst = 0;
    std::uint32_t pointCount = 0;
    std::uint32_t neighbourStartsFirst = 0;
    std::uint32_t gatherStartsFirst = 0;
    std::uint32_t spreadStartsFirst = 0;
    std::uint32_t unused[3]{};
};

static_assert(sizeof(GpuClothLevel) == 32, "GpuClothLevel must match cloth.glsl's ClothLevel");

/*
* A cloth's coarse levels for the GPU (ClothHierarchy): every level's lists one after another in one buffer each, and a
* record per level, so a pass is told only its level's number; each point's level (0 for a point on none); no records
* for a cloth with no levels.
*/
struct GpuClothLevels {
    std::vector<GpuClothLevel> records;
    std::vector<std::uint32_t> points;
    std::vector<float> masses;
    std::vector<std::uint32_t> neighbourStarts;
    std::vector<std::uint32_t> neighbours;
    std::vector<std::uint32_t> gatherStarts;
    std::vector<GpuClothLink> gather;
    std::vector<std::uint32_t> spreadStarts;
    std::vector<GpuClothLink> spread;
    std::vector<std::uint32_t> pointLevels;
};

// A cloth's points and velocities as the GPU had them a few frames ago (VulkanRenderer::takeClothReadbacks).
struct ClothReadback {
    std::uint64_t key = 0;
    std::vector<Vec3> positions;
    std::vector<Vec3> velocities;
};

// One of a point's edges, as its per-point list holds it: the point at the other end and the rest length.
struct GpuPointEdge {
    std::uint32_t other = 0;
    float restLength = 0.0f;
};

static_assert(sizeof(GpuPointEdge) == 8, "GpuPointEdge must match cloth.glsl's ClothPointEdge");

// One of a point's hinge corners: the hinge's points, weights, rest angle and share, and which corner the point is.
struct GpuPointHinge {
    std::uint32_t wing0 = 0;
    std::uint32_t wing1 = 0;
    std::uint32_t edgeStart = 0;
    std::uint32_t edgeEnd = 0;
    float isometricWeights[4]{};
    float restAngle = 0.0f;
    float stiffnessShare = 0.0f;
    std::uint32_t corner = 0;
    std::uint32_t unused = 0;
};

static_assert(sizeof(GpuPointHinge) == 48, "GpuPointHinge must match cloth.glsl's ClothPointHinge");

// Each point's position with its inverse mass in w.
std::vector<Vec4> packClothPoints(const ClothParticles& particles);

/*
* Each point's edges and hinge corners in Cloth::pointConstraints' order (its starts unchanged), holding what a term
* needs, so the GPU reads one entry and then the points instead of an index, a constraint and then the points.
*/
std::vector<GpuPointEdge> packPointEdges(const Cloth& cloth);
std::vector<GpuPointHinge> packPointHinges(const Cloth& cloth);

std::vector<GpuStretch> packStretch(const std::vector<StretchConstraint>& constraints);
std::vector<GpuBend> packBend(const std::vector<BendConstraint>& constraints);
GpuCollider packCollider(const Shape& shape);

// A field body's record, its samples starting at firstSample in the fields' buffer.
GpuClothField packClothField(const SignedDistanceField& field, std::uint32_t firstSample);

// Every point's tether slots (Cloth::tethers), or all spare for a cloth with none, so a pass can read any point's.
std::vector<GpuClothTether> packClothTethers(const Cloth& cloth);

// For each point, which of the cloth's pins moves it (its place in Cloth::pins), or clothNoPin.
std::vector<std::uint32_t> clothPinSlots(const Cloth& cloth);

/*
* A tick's record, its pins from pinFirst, bodies from colliderFirst and field bodies from fieldFirst in the frame's
* lists. Throws std::invalid_argument for fewer than one substep, as the CPU solver does.
*/
GpuClothTick packClothTick(const ClothTickCommands& commands, std::uint32_t pinFirst, std::uint32_t colliderFirst, std::uint32_t fieldFirst = 0);

// The levels of a cloth with pointCount points (a hierarchy built for no points, a cloth's before makeCloth, has none).
GpuClothLevels packClothLevels(const ClothHierarchy& hierarchy, std::size_t pointCount);

// Each coarse level's dispatches a substep: phi's gradient, the coarse step, spreading it, the line search's terms and
// total, the move.
inline constexpr std::uint32_t clothDispatchesPerLevel = 6;

// The coarse levels a tick uses: as many as its settings ask for (vertex block descent only), up to those the cloth has.
std::uint32_t clothLevelsUsed(const Cloth& cloth, const ClothSettings& settings);

/*
* The dispatches one tick records: per substep, the prediction, the solver's (XPBD: one per stretch colour and one per
* bending colour, none without bending; vertex block descent: one per point colour, and with coarse levels the held
* points, clothDispatchesPerLevel a level and a second sweep) and the end (the hand, the tethers, the bodies, the
* velocities).
*/
std::uint32_t clothDispatchesPerTick(const Cloth& cloth, const ClothSettings& settings);
