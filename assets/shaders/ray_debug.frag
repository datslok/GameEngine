#version 460
#extension GL_GOOGLE_include_directive : require

/*
* The ray tracing debug view (RenderSettings::rayDebugView): one ray per pixel from the camera, traced against the
* frame's top-level structure instead of drawn by rasterising, so it should show the same scene as the main pass. It
* shows the first hit's world normal, its albedo (base colour times the colour texture's full-size level), or whether
* the ray met a front face (green) or a back face (red), shaded by how squarely it met it. It sees everything drawn
* (every mask bit), the sun disc and lamp bulbs included. Misses are a dark grey.
*/
#include "ray_scene.glsl"
#include "materials.glsl"
#include "instances.glsl"

layout(location = 0) out vec4 colour;

// Must match GpuRayDebugView in render/gpu/ray_tracing.h.
layout(push_constant) uniform RayDebugView {
    vec4 right;         // scaled by the tangent of half the view's width
    vec4 up;            // scaled by the tangent of half the view's height
    vec4 forward;
    vec4 originAndView; // xyz the camera in the world, w which view (RAY_DEBUG_...)
    vec4 screen;        // xy its size in pixels
} view;

// Far enough for any level; the camera's far plane does not limit what a debug ray sees.
const float debugRayReach = 100000.0;

void main() {
    // gl_FragCoord counts down from the top; the view's y counts up.
    vec2 onScreen = gl_FragCoord.xy / view.screen.xy;
    vec3 direction = normalize(cameraRayDirection(view.right.xyz, view.up.xyz, view.forward.xyz, onScreen.x * 2.0 - 1.0, 1.0 - onScreen.y * 2.0));

    rayQueryEXT query;
    rayQueryInitializeEXT(query, rayScene, gl_RayFlagsNoneEXT, RAY_MASK_EVERYTHING, view.originAndView.xyz, 0.0, direction, debugRayReach);

    traceRayQuery(query);

    if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
        colour = vec4(0.02, 0.02, 0.025, 1.0);
        return;
    }

    InstanceData instance = instances[rayQueryGetIntersectionInstanceCustomIndexEXT(query, true)];
    MeshSurface surface = meshSurfaceAt(instance.material.y, rayQueryGetIntersectionPrimitiveIndexEXT(query, true),
                                        rayQueryGetIntersectionBarycentricsEXT(query, true));
    vec3 normal = normalize(mat3(instance.normalMatrix) * surface.normal);
    int shown = int(view.originAndView.w + 0.5);

    if (shown == RAY_DEBUG_NORMALS) {
        colour = vec4(normal * 0.5 + 0.5, 1.0);
    } else if (shown == RAY_DEBUG_ALBEDO) {
        MaterialData material = materials[instance.material.x];
        vec4 albedo = textureLod(textures[nonuniformEXT(material.textureSlots.x)], surface.uv, 0.0) * material.baseColour;
        colour = vec4(albedo.rgb, 1.0);
    } else {
        bool front = rayQueryGetIntersectionFrontFaceEXT(query, true);
        float squareness = 0.25 + 0.75 * abs(dot(normal, direction));
        colour = vec4((front ? vec3(0.1, 0.7, 0.15) : vec3(0.8, 0.1, 0.1)) * squareness, 1.0);
    }
}
