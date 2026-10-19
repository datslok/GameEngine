#version 460
#extension GL_GOOGLE_include_directive : require

/*
* One probe's ball (ddgi_probes.vert): the ray from the camera through this pixel meets the ball, or the pixel is
* discarded. The ball shows the probe's stored irradiance in the direction of its surface's normal, which is what a
* white matte ball would look like lit by the probe, so a probe's colour reads like the light it holds. The depth is
* the ball's true depth at the pixel, so balls hide behind walls and each other.
*/
#include "frame_data.glsl"
#include "ddgi.glsl"
#include "ddgi_volume.glsl"

layout(location = 0) in vec3 relativePosition;
layout(location = 1) flat in int probe;

layout(set = 2, binding = 0) uniform sampler2D irradianceAtlas;

layout(location = 0) out vec4 colour;

void main() {
    vec3 centre = volumeProbePosition(probe) - cameraOrigin.xyz;
    float radius = volume.hysteresis.z;
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

    vec2 uv = probeAtlasUv(volume.counts.xyz, probe, normal, DDGI_IRRADIANCE_TEXELS);
    colour = vec4(decodeIrradiance(texture(irradianceAtlas, uv).rgb), 1.0);
}
