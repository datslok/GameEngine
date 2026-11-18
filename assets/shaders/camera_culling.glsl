// The camera's GPU culling, included with #include "camera_culling.glsl": mirrors render/gpu/camera_culling.h
// (swayReaches, cameraCandidateFlags, boxInFrustum, chunkInView), which equal the CPU's visibleDraws, chosenMesh and
// drawSways (a chunk culled never holds a draw taken). Pure functions.
#ifndef CAMERA_CULLING_GLSL
#define CAMERA_CULLING_GLSL

#include "render/gpu/camera_culling_shared.h"
#include "culling.glsl"
#include "level_of_detail.glsl"

// A draw drawn with one of its meshes: its draw (instance) number and the level slot. Must match GpuCullCandidate.
struct CullCandidate {
    uint draw;
    uint slot;
};

// A group: its run of segments and its mesh's indices where they are. Must match GpuCullGroup.
struct CullGroup {
    uint firstSegment;
    uint segments;
    uint indexCount;
    uint firstIndex;
    int vertexOffset;
    uint unused0;
    uint unused1;
    uint unused2;
};

// A group's candidates in one chunk: a run of the candidates, and the chunk (CULL_NO_CHUNK: always tested one by one).
// Must match GpuCullSegment.
struct CullSegment {
    uint firstCandidate;
    uint candidates;
    uint chunk;
    uint unused;
};

// A chunk of still draws: its members' box, grown by the margin, and CULL_CHUNK_* flags. Must match GpuCullChunk.
struct CullChunk {
    vec3 minimum;
    uint flags;
    vec3 maximum;
    uint unused;
};

// What the segment passes found for a view: its visible candidates and segments, and workgroups of the candidates.
// Must match GpuCullViewWork.
struct CullViewWork {
    uint visibleCandidates;
    uint visibleSegments;
    uint candidateBlocks;
    uint unused;
};

// Must match GpuDrawCommand (VkDrawIndexedIndirectCommand).
struct DrawCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};

// A view culled: the camera, a probe face or the mirror. Must match GpuCameraCull.
struct CameraCull {
    CullFrustum frustum;
    LevelView levels;
    float swayFadeEnd; // the wind's sway fades out here (WindSettings::swayFadeEnd)
    uint flags;        // CAMERA_CULL_MOTION, CAMERA_CULL_SWAY
    uint candidates;   // the candidate list's length (the visible ones are in CullViewWork)
    uint groups;
    uint mainBase;     // the view's first entry in the draw list
    uint motionBase;   // the velocity pass's
    uint blocks;       // workgroups of candidates
    uint view;         // its number in the dispatch: its counts' and packed counts' place
    uvec4 runFirst;    // each style's pooled groups' first place in the drawing order
    uvec4 runCount;    // and how many
    uvec4 slices;      // CAMERA_CULL_SLICE_* (x flags, y blocks, z commands, w places): its parts of the working buffers
    vec4 reach;           // a shadow tile's light reach (w below 0: everywhere)
    float smallestCaster; // a shadow tile's: smaller casters left out
    int cascade;          // a shadow tile's sun cascade (-1 none)
    uint segments;        // the segments every view's passes walk
    uint segmentBlocks;   // workgroups of segments
    uvec4 segmentSlices;  // CAMERA_CULL_SEGMENT_SLICE_* (x offsets and visible list, y block sums)
};

// Whether a box may be inside all six planes: its corner farthest along each plane's normal tested (boxInFrustum).
bool boxInFrustum(CullFrustum frustum, vec3 minimum, vec3 maximum) {
    for (int plane = 0; plane < 6; ++plane) {
        vec4 p = frustum.planes[plane];
        vec3 corner = vec3(p.x >= 0.0 ? maximum.x : minimum.x, p.y >= 0.0 ? maximum.y : minimum.y, p.z >= 0.0 ? maximum.z : minimum.z);

        if (p.x * corner.x + p.y * corner.y + p.z * corner.z + p.w < 0.0) {
            return false;
        }
    }

    return true;
}

/*
* Whether a view may take any of a chunk's draws (chunkInView): a camera view one it can see, a shadow tile a caster whose
* box touches the light's reach; then the box in the frustum. A draw of unknown size keeps the chunk.
*/
bool chunkInView(CullChunk chunk, CameraCull view) {
    bool shadow = (view.flags & CAMERA_CULL_SHADOW) != 0u;

    if ((chunk.flags & (shadow ? CULL_CHUNK_CASTS : CULL_CHUNK_SEEN)) == 0u) {
        return false;
    }

    if ((chunk.flags & CULL_CHUNK_ALWAYS) != 0u) {
        return true;
    }

    if (shadow && view.reach.w >= 0.0) {
        vec3 gap = view.reach.xyz - clamp(view.reach.xyz, chunk.minimum, chunk.maximum);

        if (gap.x * gap.x + gap.y * gap.y + gap.z * gap.z > view.reach.w * view.reach.w) {
            return false;
        }
    }

    return boxInFrustum(view.frustum, chunk.minimum, chunk.maximum);
}

// Whether part of a sphere is nearer the eye than the sway's fade end (drawSways; unknown bounds count from the centre).
bool swayReaches(vec4 bounds, vec3 eye, float fadeEnd) {
    return length(bounds.xyz - eye) - max(bounds.w, 0.0) < fadeEnd;
}

/*
* Whether a shadow tile takes a draw: a solid caster, not hidden, touching the light's reach, maybe in view, at least the
* smallest caster's size (unknown sizes kept) and within its cascades.
*/
bool shadowTakes(CullRecord record, CameraCull view) {
    if ((record.flags & (CULL_TRANSPARENT | CULL_HIDDEN)) != 0u || (record.flags & CULL_CASTS_SHADOWS) == 0u) {
        return false;
    }

    if (view.reach.w >= 0.0 && record.bounds.w >= 0.0) {
        float touching = view.reach.w + record.bounds.w;
        vec3 offset = record.bounds.xyz - view.reach.xyz;

        if (dot(offset, offset) >= touching * touching) {
            return false;
        }
    }

    if (!sphereInFrustum(view.frustum, record.bounds)) {
        return false;
    }

    if (record.bounds.w >= 0.0 && record.bounds.w < view.smallestCaster) {
        return false;
    }

    return view.cascade < 0 || record.shadowCascades <= 0 || view.cascade < record.shadowCascades;
}

/*
* A candidate's CAMERA_CULL_* flags: drawn where the camera takes the draw and chooses this slot; moving where the
* velocity pass is built and the draw moved, its mesh deformed or it sways within reach; thinned as slotThinned says.
* A shadow tile: drawn where shadowTakes and the slot is chosen, moving instead where it keeps moved casters apart;
* never thinned.
*/
uint cameraCandidateFlags(CullRecord record, MeshLevels levels, CameraCull view, uint slot) {
    bool shadow = (view.flags & CAMERA_CULL_SHADOW) != 0u;

    if (shadow ? !shadowTakes(record, view) : !cameraTakes(record, view.frustum)) {
        return 0u;
    }

    int chosen = chooseLevelSlot(record, levels, view.levels);

    if (chosen != int(slot)) {
        return 0u;
    }

    if (shadow) {
        bool apart = (view.flags & CAMERA_CULL_MOTION) != 0u && (record.flags & CULL_MOVED) != 0u;
        return apart ? CAMERA_CULL_MOVING : CAMERA_CULL_DRAWN;
    }

    uint flags = CAMERA_CULL_DRAWN;
    bool sways = (view.flags & CAMERA_CULL_SWAY) != 0u && (record.flags & CULL_SWAYS) != 0u &&
                 swayReaches(record.bounds, view.levels.eye.xyz, view.swayFadeEnd);

    if ((view.flags & CAMERA_CULL_MOTION) != 0u && ((record.flags & CULL_MOVED) != 0u || levels.changesShape != 0u || sways)) {
        flags |= CAMERA_CULL_MOVING;
    }

    if (slotThinned(record, levels, chosen)) {
        flags |= CAMERA_CULL_THINNED;
    }

    return flags;
}

#endif
