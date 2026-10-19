#pragma once

#include "math/aabb.h"
#include "math/vec3.h"
#include "render/gpu/clusters_shared.h"
#include "render/gpu/light_uniforms.h"
#include "scene/camera.h"

#include <cstddef>
#include <cstdint>
#include <vector>

/*
* Clustered shading. The view is cut into a grid of clusters: square screen tiles of clusterTilePixels, each cut again
* along the view direction into clusterDepthSlices slices spaced exponentially (each about 1.33 times deeper than the one
* before it for the demo's near 0.1 and far 100), so a cluster looks about as deep on screen as it is wide. A compute
* pass (cluster_lights.comp) lists, for every cluster, the lights whose reach touches it, as one bit per light; each
* pixel then loops over its own cluster's lights instead of every light in the frame.
*
* This is the CPU mirror of clusters.glsl and cluster_lights.comp, for the tests: the GPU tests check the shader's
* functions against these. Positions here are in cluster view space: x to the camera's right, y up the screen, z the
* depth along the view direction (positive in front, unlike the view matrix's -z), measured from the camera.
*/

inline constexpr int clusterTilePixels = CLUSTER_TILE_PIXELS;
inline constexpr int clusterDepthSlices = CLUSTER_DEPTH_SLICES;
inline constexpr int clusterPointWords = CLUSTER_POINT_WORDS;
inline constexpr int clusterSpotWords = CLUSTER_SPOT_WORDS;
inline constexpr int clusterWords = CLUSTER_WORDS;
inline constexpr float clusterBoundsMargin = static_cast<float>(CLUSTER_BOUNDS_MARGIN);
inline constexpr std::uint32_t clusterGroupSize = CLUSTER_GROUP_SIZE;
inline constexpr int clusterHeatmapLights = CLUSTER_HEATMAP_LIGHTS;

static_assert(clusterPointWords * 32 == maxPointLights, "a cluster needs one bit per point light");
static_assert(clusterSpotWords * 32 == maxSpotLights, "a cluster needs one bit per spotlight");
static_assert(clusterWords == clusterPointWords + clusterSpotWords, "a cluster's words are its point lights' then its spotlights'");

// Everything needed to find a pixel's cluster and a cluster's bounds: the frame's camera and screen size.
struct ClusterGrid {
    int tilesX = 0;
    int tilesY = 0;
    int slices = clusterDepthSlices;
    int screenWidth = 0;
    int screenHeight = 0;

    float nearDepth = 0.0f;
    float farDepth = 0.0f;

    // A depth's slice is floor(log(depth) * sliceScale + sliceBias), so slice k starts at near * (far / near)^(k / slices).
    float sliceScale = 0.0f;
    float sliceBias = 0.0f;

    // Half the view's width and height at a depth of 1: tan(fov / 2) times the aspect ratio, and tan(fov / 2).
    float tanHalfWidth = 0.0f;
    float tanHalfHeight = 0.0f;

    // The view's axes in the world (from the view matrix's rows), which turn a point measured from the camera into
    // cluster view space.
    Vec3 right;
    Vec3 up;
    Vec3 forward;
};

// The grid for a camera drawing a screen of this size in pixels. Throws std::invalid_argument for an empty screen.
ClusterGrid makeClusterGrid(const Camera& camera, int screenWidth, int screenHeight);

int clusterCount(const ClusterGrid& grid);

// The bytes of one bit per light for every cluster (clusterWords words each): the size of the masks buffer, which
// depends only on the screen's size.
std::size_t clusterMaskBytes(const ClusterGrid& grid);
std::size_t clusterMaskBytes(int screenWidth, int screenHeight);

// A point measured from the camera (camera-relative) in cluster view space.
Vec3 toClusterView(const ClusterGrid& grid, const Vec3& relativePosition);

// The slice a depth falls in, clamped to the grid (a depth rounding a hair past the near or far plane stays in it).
int clusterSlice(float depth, float sliceScale, float sliceBias, int slices);

// Where slice k starts along the view direction: near * (far / near)^(k / slices), computed from the same scale and bias.
float sliceStartDepth(int slice, float sliceScale, float sliceBias);

// Clusters are numbered tile by tile along a row, row by row down the screen, then slice by slice.
int clusterIndex(int tileX, int tileY, int slice, int tilesX, int tilesY);

// The cluster of a pixel (in pixels from the top-left corner, like gl_FragCoord) at a depth along the view direction.
int clusterOfPixel(const ClusterGrid& grid, float pixelX, float pixelY, float depth);

/*
* A cluster's bounding box in cluster view space: the corners of its piece of the view frustum (the tile's edges at the
* slice's two depths), grown on every side by clusterBoundsMargin times its far depth. Slice 0 starts at the camera
* (depth 0), so nothing in front of the camera is ever outside every cluster.
*/
Aabb clusterBounds(const ClusterGrid& grid, int tileX, int tileY, int slice);

// Whether a sphere touches a box: the box's closest point to the centre is within the radius.
bool sphereTouchesBox(const Vec3& centre, float radius, const Aabb& box);

/*
* Whether a spotlight's cone (apex, unit axis, cosine of the outer half-angle, range) can touch a sphere (Wronski 2016,
* "Cull that cone"): the sphere is missed if it lies wholly past the range, wholly behind the apex, or wholly outside
* the cone's angle. Conservative: true can mean a near miss, false never misses a touch. Cones of 90 degrees or wider
* (cosOuter <= 0) are only range tested.
*/
bool coneTouchesSphere(const Vec3& apex, const Vec3& axis, float cosOuter, float range, const Vec3& centre, float radius);

/*
* One bit per light for every cluster, clusterWords words each (point lights in the first clusterPointWords, spotlights
* after), set where the light may reach into the cluster: its range sphere touches the cluster's box and, for a
* spotlight, its cone touches the box's bounding sphere. Lights are tested in slot order, so walking a cluster's bits
* from the lowest visits its lights in the order the shader always added them.
*/
struct ClusterMasks {
    std::vector<std::uint32_t> words;

    bool hasPointLight(int cluster, int light) const;
    bool hasSpotLight(int cluster, int light) const;
};

// What cluster_lights.comp computes, for the frame's lights as the shader gets them (positions measured from the camera).
ClusterMasks buildClusterMasks(const ClusterGrid& grid, const GpuLights& lights);

/*
* The heatmap debug view's colour ramp over 0..1: blue, green, yellow, red at 0, 1/3, 2/3 and 1, each channel a
* clamped straight line, so it has no branches and the stops are exact. clusterHeatColour turns a cluster's light count
* into it, black for none and red from clusterHeatmapLights up.
*/
Vec3 clusterHeatRamp(float fraction);
Vec3 clusterHeatColour(int lights);
