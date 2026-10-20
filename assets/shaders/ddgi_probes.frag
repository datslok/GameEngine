#version 460
#extension GL_GOOGLE_include_directive : require

/*
* One probe's ball (ddgi_probes.vert): the ray from the camera through this pixel meets the ball, or the pixel is
* discarded. The ball shows the probe's stored irradiance in the direction of its surface's normal, which is what a
* white matte ball would look like lit by the probe (both atlases, still and fast lights), so a probe's colour reads
* like the light it holds. The depth is
* the ball's true depth at the pixel, so balls hide behind walls and each other. Untraced probes show their state
* instead of stale light: off (buried in the level) red, sleeping dark blue; an awake probe (woken by something that
* moves) shows its light inside a yellow rim.
*/

// The colours of the states the balls mark, in the scene's linear light.
const vec3 offBallColour = vec3(0.8, 0.02, 0.02);
const vec3 sleepingBallColour = vec3(0.02, 0.04, 0.25);
const vec3 awakeRimColour = vec3(0.9, 0.75, 0.05);

// How far round the ball the awake rim reaches: where the surface turns more than this cosine from the camera.
const float awakeRimCosine = 0.4;
#include "frame_data.glsl"
#include "ddgi.glsl"
#include "ddgi_volume.glsl"
#include "scene_probe_states.glsl"

layout(location = 0) in vec3 relativePosition;
layout(location = 1) flat in int probe;
layout(location = 2) flat in vec4 ball; // xyz its centre, w its radius

layout(set = 2, binding = 0) uniform sampler2D irradianceAtlas;
layout(set = 2, binding = 1) uniform sampler2D fastIrradianceAtlas;

layout(location = 0) out vec4 colour;

void main() {
    vec3 centre = ball.xyz;
    float radius = ball.w;
    vec3 direction = normalize(relativePosition);

    // Where the ray first meets the ball: t^2 - 2 t (d . c) + |c|^2 - r^2 = 0, the nearer root.
    float along = dot(direction, centre);
    float discriminant = along * along - (dot(centre, centre) - radius * radius);

    if (discriminant < 0.0) {
        discard;
    }

    vec3 hit = direction * (along - sqrt(discriminant));
    vec3 normal = (hit - centre) / radius;

    vec4 clip = relativeViewProjection * vec4(hit, 1.0);
    gl_FragDepth = clip.z / clip.w;

    int state = probeStates[probe].state;

    if (state == DDGI_PROBE_OFF) {
        colour = vec4(offBallColour, 1.0);
        return;
    }

    if (state == DDGI_PROBE_SLEEPING) {
        colour = vec4(sleepingBallColour, 1.0);
        return;
    }

    if (state == DDGI_PROBE_AWAKE && dot(normal, -direction) < awakeRimCosine) {
        colour = vec4(awakeRimColour, 1.0);
        return;
    }

    vec2 uv = probeAtlasUv(volumeAtlasGrid(), probe, normal, DDGI_IRRADIANCE_TEXELS);
    colour = vec4(textureLod(irradianceAtlas, uv, 0.0).rgb + textureLod(fastIrradianceAtlas, uv, 0.0).rgb, 1.0);
}
