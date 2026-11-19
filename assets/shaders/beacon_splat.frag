#version 460
#extension GL_GOOGLE_include_directive : require

/*
* A beacon's splat (beacon_splat.vert): each pixel its weighted share of the beacon's light (render/gpu/beacon_splat.h,
* beaconSplatWeight), so the shares sum to all of it wherever it falls, dimmed by the fog between the camera and the
* beacon itself (the picture behind it was fogged to its own depth, which may be the far sky).
*/
#include "frame_data.glsl"
#include "scene_lights.glsl"
#include "scene_sky.glsl"
#include "scene_fog.glsl"
#include "beacon_splat.glsl"

layout(location = 0) flat in vec3 radiance;
layout(location = 1) flat in vec2 beaconPixel;
layout(location = 2) flat in float lensPixels;
layout(location = 3) flat in vec3 relativePoint;

layout(location = 0) out vec4 colour;

void main() {
    float weight = beaconSplatWeight(gl_FragCoord.xy - beaconPixel, lensPixels);

    if (weight <= 0.0) {
        discard;
    }

    float through = fogOn() ? fogAlongView(relativePoint, false).transmittance : 1.0;
    colour = vec4(radiance * weight * through, 1.0);
}
