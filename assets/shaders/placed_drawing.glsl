// The placed plants' drawing, included with #include "placed_drawing.glsl": mirrors render/gpu/placed_drawing.h
// (placedPlantModel, placedSphere, placedCullRecord, placedInstance, placedPartEntries) and declares its records. Pure
// functions, every result precise, so the passes choose and the vertex shaders place as the CPU's mirror does.
#ifndef PLACED_DRAWING_GLSL
#define PLACED_DRAWING_GLSL

#include "render/gpu/placed_drawing_shared.h"
#include "camera_culling.glsl"
#include "instance_data.glsl"
#include "placed_plant.glsl"

// One part of a kind, in the plant's frame. Must match GpuPlacedPart.
struct PlacedPart {
    CullRecord record;
    MeshLevels levels;
    uint firstGroup; // its level slot k's group is firstGroup + k
    uint unused0;
    uint unused1;
    uint unused2;
};

// A layer's kind: how many parts and which (indices into the parts). Must match GpuPlacedKind.
struct PlacedKind {
    uint parts;
    uint variants;    // its looks, chosen by the plant's pick (placedKindSlot)
    uint variantSlot; // the slot of its variant 1, the rest after it
    uint unused;
    uint part[PLACED_MOST_PARTS];
};

// A workgroup's plants. Must match GpuPlacedBlock.
struct PlacedBlock {
    uint firstPlant;   // its first plant in the slots' plants
    uint countPlace;   // its slot's layer's count
    uint firstInLayer; // its first plant within its layer's run
    uint firstKind;    // its layer's kinds in the kind table
};

// A group's mesh in the shared mesh buffer. Must match GpuPlacedGroup.
struct PlacedGroup {
    uint indexCount;
    uint firstIndex;
    int vertexOffset;
    uint unused;
};

// A block one view culls. Must match GpuPlacedItem.
struct PlacedItem {
    uint block;
    uint view;
};

// A view's culling. Must match GpuPlacedView.
struct PlacedView {
    CameraCull camera;
    vec4 region;    // north, south, spread
    uint firstItem; // its items, from here
    uint items;
    uint groups;
    uint entryBase; // its entries, from here
    uint entryRoom;
    uint viewCount; // every view's
    uint itemCount;
    uint padding;
};

// The turn's two numbers the matrix is made of, as Quat::fromAxisAngle about up and toMatrix make them: the diagonal's
// 1 - 2 sin^2 and the corners' 2 cos sin (half angles).
vec2 placedTurn(float turnRadians) {
    precise float halfAngle = turnRadians * 0.5;
    precise float sine = sin(halfAngle);
    precise float cosine = cos(halfAngle);
    precise float diagonal = 1.0 - 2.0 * (sine * sine + 0.0);
    precise float corner = 2.0 * (0.0 + cosine * sine);
    return vec2(diagonal, corner);
}

// The slot of its layer's kinds a plant of kind draws: the kind's own, or, with variants, the one its pick chooses,
// min(pick * variants, variants - 1), variant 1 on at variantSlot. Mirrors placedKindSlot (render/gpu/placed_drawing.cpp).
uint placedKindSlot(uint kind, uint variants, uint variantSlot, float pick) {
    if (variants <= 1u) {
        return kind;
    }

    precise float scaled = pick * float(variants);
    uint variant = min(uint(scaled), variants - 1u);
    return variant == 0u ? kind : variantSlot + variant - 1u;
}

// The plant's model matrix from its turn already made (placedTurnedModel), so a vertex makes the sine and cosine once.
mat4 placedTurnedModel(PlacedPlant plant, vec2 turn) {
    precise float across = turn.x * plant.scale;
    precise float corner = turn.y * plant.scale;
    precise float backCorner = -turn.y * plant.scale;
    return mat4(vec4(across, 0.0, backCorner, 0.0), vec4(0.0, plant.scale, 0.0, 0.0), vec4(corner, 0.0, across, 0.0), vec4(plant.foot, 1.0));
}

// The plant's model matrix (placedPlantModel): Transform{foot, turn about up, scale}.getMatrix().
mat4 placedPlantModel(PlacedPlant plant) {
    return placedTurnedModel(plant, placedTurn(plant.turnRadians));
}

// A sphere placed by the plant's matrix already made (placedSphereBy): its centre through it, its radius scaled.
vec4 placedSphereBy(vec4 sphere, mat4 model, float scale) {
    precise float x = model[0][0] * sphere.x + model[2][0] * sphere.z + model[3][0];
    precise float y = model[1][1] * sphere.y + model[3][1];
    precise float z = model[0][2] * sphere.x + model[2][2] * sphere.z + model[3][2];
    precise float radius = sphere.w < 0.0 ? sphere.w : sphere.w * scale;
    return vec4(x, y, z, radius);
}

// A sphere in the plant's frame placed in the world (placedSphere).
vec4 placedSphere(vec4 sphere, PlacedPlant plant) {
    return placedSphereBy(sphere, placedPlantModel(plant), plant.scale);
}

// A part's record placed in the world (placedCullRecord), the plant's matrix made once for both spheres.
CullRecord placedCullRecord(CullRecord part, PlacedPlant plant) {
    mat4 model = placedPlantModel(plant);
    CullRecord record = part;
    record.bounds = placedSphereBy(part.bounds, model, plant.scale);
    record.levelSphere = placedSphereBy(part.levelSphere, model, plant.scale);
    return record;
}

// A part's instance placed in the world (placedInstance): the plant's matrix after the part's own, the plant's turn
// before the part's normals (normalised later), its scale on the part's, its level sphere placed. Run for every
// vertex, so the turn's sine and cosine are made once, and the plant's matrix once.
InstanceData placedInstance(InstanceData part, PlacedPlant plant) {
    vec2 turn = placedTurn(plant.turnRadians);
    mat4 rotation = mat4(vec4(turn.x, 0.0, -turn.y, 0.0), vec4(0.0, 1.0, 0.0, 0.0), vec4(turn.y, 0.0, turn.x, 0.0), vec4(0.0, 0.0, 0.0, 1.0));
    mat4 model = placedTurnedModel(plant, turn);
    InstanceData instance = part;
    instance.model = model * part.model;
    instance.normalMatrix = rotation * part.normalMatrix;
    instance.handedness.y = part.handedness.y * plant.scale;
    instance.levelSphere = placedSphereBy(part.levelSphere, model, plant.scale);
    return instance;
}

// What a part of a plant draws in a view: up to two level slots (two only while fading) whose flags say drawn,
// each its group and entry (the plant's number and the draw list's bits: cameraCandidateEntry), or a group of
// PLACED_MOST_GROUPS for none (placedPartEntries).
struct PlacedPartDraws {
    uvec2 groups;
    uvec2 entries;
};

PlacedPartDraws placedPartEntries(PlacedPart part, PlacedPlant plant, uint plantNumber, CameraCull view) {
    CullRecord record = placedCullRecord(part.record, plant);
    PlacedPartDraws draws;
    draws.groups = uvec2(PLACED_MOST_GROUPS);
    draws.entries = uvec2(0u);
    uint found = 0u;

    for (uint slot = 0u; slot <= part.levels.levels && slot < CULL_LEVEL_SLOTS; ++slot) {
        if (part.levels.meshes[slot] == CULL_NO_MESH) {
            continue;
        }

        uint flags = cameraCandidateFlags(record, part.levels, view, slot);

        if ((flags & CAMERA_CULL_DRAWN) == 0u || found >= 2u) {
            continue;
        }

        uint fade = (flags & CAMERA_CULL_FADES) != 0u ? cameraCandidateFade(record, part.levels, view, slot) : 0u;
        draws.groups[found] = part.firstGroup + slot;
        draws.entries[found] = cameraCandidateEntry(plantNumber, flags, fade);
        ++found;
    }

    return draws;
}

#endif
