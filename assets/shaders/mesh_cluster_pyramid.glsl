// Whether a depth pyramid (farthest depths, exact texels) hides a screen box (meshClusterBoxHidden,
// render/gpu/mesh_cluster_culling.h): the cluster passes' sphere test (mesh_cluster_pass.glsl) and the mesh shaders' far
// leaves (mesh_cluster_draw.glsl, stage 3e-v-b). Reads textures, so not a library of its own.
#ifndef MESH_CLUSTER_PYRAMID_GLSL
#define MESH_CLUSTER_PYRAMID_GLSL

#include "mesh_cluster_culling.glsl"

/*
* Every cell the box covers, at the level where it spans at most two each way, farther than its nearest depth; never a
* box off screen. The box is measured in the pyramid's own pixels (its level 0's size).
*/
bool meshClusterBoxHiddenIn(sampler2D pyramid, MeshClusterScreenBox box) {
    if (!box.onScreen) {
        return false;
    }

    ivec2 size = textureSize(pyramid, 0);
    ivec4 pixels = meshClusterBoxPixels(box.box, size);

    if (pixels.x > pixels.z || pixels.y > pixels.w) {
        return false;
    }

    int level = meshClusterPyramidLevel(pixels, textureQueryLevels(pyramid));
    ivec2 levelSize = textureSize(pyramid, level);
    ivec4 cells = ivec4(pyramidCell(pixels.x, level, levelSize.x), pyramidCell(pixels.y, level, levelSize.y), pyramidCell(pixels.z, level, levelSize.x),
                        pyramidCell(pixels.w, level, levelSize.y));
    float farthest = GPU_NEAR_DEPTH;

    for (int y = cells.y; y <= cells.w; ++y) {
        for (int x = cells.x; x <= cells.z; ++x) {
            farthest = fartherDepth(farthest, texelFetch(pyramid, ivec2(x, y), level).r);
        }
    }

    return box.nearestDepth < farthest;
}

#endif
