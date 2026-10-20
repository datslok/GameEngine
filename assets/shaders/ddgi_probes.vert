#version 460
#extension GL_GOOGLE_include_directive : require

/*
* The light probes' debug view (GlobalIlluminationSettings::showProbes): a ball at each probe. Each instance is one
* probe, drawn as a square through the ball's centre square-on to the line from the camera, just covering the ball's
* outline; ddgi_probes.frag finds the ball's surface within it. No vertex buffer: the six corners of the square's two triangles come from the vertex number.
* Camera-relative like the main pass: positions are measured from the camera. Each ball sits where relocation moved its
* probe, every cascade's, each cascade's balls as much larger than the densest's as its spacing is.
*/
#include "frame_data.glsl"
#include "ddgi.glsl"
#include "ddgi_volume.glsl"
#include "scene_probe_cascades.glsl"
#include "scene_probe_states.glsl"

layout(location = 0) out vec3 relativePosition;
layout(location = 1) flat out int probe;
layout(location = 2) flat out vec4 ball; // xyz its centre, w its radius

const vec2 squareCorners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

void main() {
    probe = gl_InstanceIndex;
    ProbeCascade cascade = probeCascades[volumeCascadeOf(probe)];
    vec3 centre = cascadeProbePosition(cascade, volume.counts.xyz, probe) + probeStates[probe].offset;
    float radius = volume.hysteresis.z * cascade.spacing.x / probeCascades[0].spacing.x;
    float distance = length(centre);
    ball = vec4(centre, radius);

    // A ball seen from distance d fills a cone whose edge crosses the plane through its centre, square-on to the cone's
    // axis, r d / sqrt(d^2 - r^2) from the centre, a little more than r. (A square facing the camera's forward axis
    // instead would cut off balls near the edges of the view, whose outlines stretch away from the middle.) From inside
    // the ball there is nothing to draw.
    if (distance <= radius) {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        relativePosition = vec3(0.0);
        return;
    }

    float halfSide = radius * distance / sqrt(distance * distance - radius * radius);
    vec3 towards = centre / distance;
    vec3 across = normalize(cross(towards, abs(towards.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 upwards = cross(across, towards);
    vec2 corner = squareCorners[gl_VertexIndex];
    relativePosition = centre + (across * corner.x + upwards * corner.y) * halfSide;
    gl_Position = relativeViewProjection * vec4(relativePosition, 1.0);
}
