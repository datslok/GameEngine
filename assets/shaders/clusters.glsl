// Clustered shading's grid, included with #include "clusters.glsl": which cluster a pixel is in, a cluster's bounds and
// the tests that decide whether a light can reach into one. Mirrors render/gpu/clusters.cpp, which tests the maths;
// tests/gpu checks these functions against it on the GPU. Positions are in cluster view space: x to the camera's right,
// y up the screen, z the depth along the view direction, measured from the camera.

#ifndef CLUSTERS_GLSL
#define CLUSTERS_GLSL

#include "render/gpu/clusters_shared.h"

// Must match ClusterGrid in render/gpu/clusters.h (the parts the GPU needs).
struct ClusterGrid {
    ivec4 tiles;   // x: tiles across, y: tiles down, z: depth slices
    vec4 screen;   // xy: the screen's size in pixels, z: half the view's width at depth 1, w: half its height
    vec4 slicing;  // x: slice scale, y: slice bias (a depth's slice is floor(log(depth) * scale + bias))
    vec4 right;    // xyz: the view's axes in the world
    vec4 up;
    vec4 forward;
};

vec3 toClusterView(ClusterGrid grid, vec3 relativePosition) {
    return vec3(dot(relativePosition, grid.right.xyz), dot(relativePosition, grid.up.xyz), dot(relativePosition, grid.forward.xyz));
}

// The slice a depth falls in, clamped to the grid; anything at or behind the camera belongs to the first slice.
int clusterSlice(float depth, float sliceScale, float sliceBias, int slices) {
    if (!(depth > 0.0)) {
        return 0;
    }

    return clamp(int(floor(log(depth) * sliceScale + sliceBias)), 0, slices - 1);
}

// Where a slice starts along the view direction: near * (far / near)^(slice / slices).
float sliceStartDepth(int slice, float sliceScale, float sliceBias) {
    return exp((float(slice) - sliceBias) / sliceScale);
}

// Tile by tile along a row, row by row down the screen, then slice by slice.
int clusterIndex(int tileX, int tileY, int slice, int tilesX, int tilesY) {
    return (slice * tilesY + tileY) * tilesX + tileX;
}

// The cluster of a pixel (in pixels from the top-left corner, like gl_FragCoord) at a depth along the view direction.
int clusterOfPixel(ClusterGrid grid, vec2 pixel, float depth) {
    int tileX = clamp(int(floor(pixel.x / float(CLUSTER_TILE_PIXELS))), 0, grid.tiles.x - 1);
    int tileY = clamp(int(floor(pixel.y / float(CLUSTER_TILE_PIXELS))), 0, grid.tiles.y - 1);
    return clusterIndex(tileX, tileY, clusterSlice(depth, grid.slicing.x, grid.slicing.y, grid.tiles.z), grid.tiles.x, grid.tiles.y);
}

/*
* A cluster's box: the corners of its piece of the view frustum (the tile's edges at the slice's two depths; the first
* slice starts at the camera), grown on every side by CLUSTER_BOUNDS_MARGIN times its far depth. The renderer's
* viewport flips y, so pixel row 0 is the top of the screen.
*/
void clusterBounds(ClusterGrid grid, int tileX, int tileY, int slice, out vec3 minimum, out vec3 maximum) {
    float nearDepth = slice == 0 ? 0.0 : sliceStartDepth(slice, grid.slicing.x, grid.slicing.y);
    float farDepth = sliceStartDepth(slice + 1, grid.slicing.x, grid.slicing.y);

    vec2 screenSize = grid.screen.xy;
    vec2 firstPixel = vec2(ivec2(tileX, tileY) * CLUSTER_TILE_PIXELS);
    vec2 endPixel = vec2(min(ivec2(tileX + 1, tileY + 1) * CLUSTER_TILE_PIXELS, ivec2(screenSize)));

    float left = (2.0 * firstPixel.x / screenSize.x - 1.0) * grid.screen.z;
    float right = (2.0 * endPixel.x / screenSize.x - 1.0) * grid.screen.z;
    float top = (1.0 - 2.0 * firstPixel.y / screenSize.y) * grid.screen.w;
    float bottom = (1.0 - 2.0 * endPixel.y / screenSize.y) * grid.screen.w;

    float margin = float(CLUSTER_BOUNDS_MARGIN) * farDepth;
    minimum = vec3(min(left * nearDepth, left * farDepth), min(bottom * nearDepth, bottom * farDepth), nearDepth) - vec3(margin);
    maximum = vec3(max(right * nearDepth, right * farDepth), max(top * nearDepth, top * farDepth), farDepth) + vec3(margin);
}

// Whether a sphere touches a box: the box's closest point to the centre is within the radius.
bool sphereTouchesBox(vec3 centre, float radius, vec3 minimum, vec3 maximum) {
    vec3 offset = centre - clamp(centre, minimum, maximum);
    return dot(offset, offset) <= radius * radius;
}

/*
* Whether a spotlight's cone can touch a sphere (Wronski 2016, "Cull that cone"): missed if wholly outside the cone's
* angle (the signed distance from the slanted edge, cos * across - sin * along, is past the radius), wholly past the
* range or wholly behind the apex. Cones of 90 degrees or wider are only range tested.
*/
bool coneTouchesSphere(vec3 apex, vec3 axis, float cosOuter, float range, vec3 centre, float radius) {
    vec3 offset = centre - apex;
    float distanceSquared = dot(offset, offset);

    if (cosOuter <= 0.0) {
        return distanceSquared <= (range + radius) * (range + radius);
    }

    float along = dot(offset, axis);
    float across = sqrt(max(0.0, distanceSquared - along * along));
    float sinOuter = sqrt(max(0.0, 1.0 - cosOuter * cosOuter));
    float fromEdge = cosOuter * across - sinOuter * along;

    return fromEdge <= radius && along <= range + radius && along >= -radius;
}

#endif
