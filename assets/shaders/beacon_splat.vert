#version 460
#extension GL_GOOGLE_include_directive : require

/*
* The beacons (render/gpu/beacon_splat.h), one instance each: a square round where the beacon falls, through the frame's
* own (jittered) view, so it stands where the scene does and is hidden by what is in front of it (the depth test, the
* beacon's own depth). Wide enough for the star's tent under a pixel, or the lens's disc once it is bigger. Its light
* reaching the eye over a pixel's solid angle is the radiance its weights share out (beacon_splat.frag), fogged there.
* Beacons behind the camera, or nearer than its near plane, are moved off screen.
*/
#include "render/gpu/depth_shared.h"
#include "frame_data.glsl"
#include "beacon_splat.glsl"

// Must match GpuBeacon in render/gpu/beacon_splat.h.
struct Beacon {
    vec4 positionAndLens; // xyz: measured from the camera, w: the lens's radius
    vec4 light;           // rgb: colour times intensity times level
};

layout(std430, set = 2, binding = 0) readonly buffer Beacons {
    Beacon beacons[];
};

layout(location = 0) flat out vec3 radiance;
layout(location = 1) flat out vec2 beaconPixel;
layout(location = 2) flat out float lensPixels;
layout(location = 3) flat out vec3 relativePoint;

void hide() {
    gl_Position = vec4(2.0, 2.0, GPU_FAR_DEPTH, 1.0);
    radiance = vec3(0.0);
    beaconPixel = vec2(0.0);
    lensPixels = 0.0;
    relativePoint = vec3(0.0);
}

void main() {
    const vec2 corners[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    Beacon beacon = beacons[gl_InstanceIndex];
    vec3 point = beacon.positionAndLens.xyz;
    vec4 clip = relativeViewProjection * vec4(point, 1.0);

    if (clip.w <= 0.0) {
        hide();
        return;
    }

    vec3 device = clip.xyz / clip.w;

    // Nearer than the near plane (reversed depth: past 1).
    if (device.z > 1.0) {
        hide();
        return;
    }

    vec2 size = clusterScreen.xy;
    float distance = length(point);
    float cosOffAxis = dot(point, viewForward.xyz) / max(distance, BEACON_NEAREST_DISTANCE);
    float pixel = pixelSolidAngle(viewRight.xyz * clusterScreen.z, viewUp.xyz * clusterScreen.w, size.x, size.y, cosOffAxis);
    float lens = beaconLensPixels(beacon.positionAndLens.w, distance, pixel);

    radiance = beacon.light.rgb * beaconSplatRadiance(1.0, distance, pixel);
    beaconPixel = vec2((device.x + 1.0) * 0.5 * size.x, (1.0 - device.y) * 0.5 * size.y);
    lensPixels = lens;
    relativePoint = point;

    // Past the far plane it stays at the far depth, seen only where nothing else is.
    float depth = max(device.z, GPU_FAR_DEPTH);
    gl_Position = vec4(device.xy + corners[gl_VertexIndex] * beaconExtentPixels(lens) * 2.0 / size, depth, 1.0);
}
