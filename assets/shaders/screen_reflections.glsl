// Screen-space reflections' maths, included with #include "screen_reflections.glsl": mirrors
// render/gpu/screen_reflections.cpp, which explains each function and tests the maths, and tests/gpu checks these
// functions against it on the GPU. Pure functions only: the march's loop, which reads the depth pyramid, is in
// reflections.comp (mirrored by traceScreenRay).

#ifndef SCREEN_REFLECTIONS_GLSL
#define SCREEN_REFLECTIONS_GLSL

#include "render/gpu/depth_shared.h"
#include "render/gpu/screen_reflections_shared.h"

// pixelToNdc and ndcToPixel.
#include "taa.glsl"

// A step that never comes: the ray does not move on screen along that axis.
const float SSR_NEVER = 1e30;

struct ScreenRay {
    vec3 start;
    vec3 step;
    float end;
};

struct HizTrace {
    vec3 position;
    float t;
    int level;
};

int pyramidLastFine(int coarse, int coarseSize, int fineSize) {
    return coarse == coarseSize - 1 ? fineSize - 1 : 2 * coarse + 1;
}

int pyramidCell(int texel, int level, int levelSize) {
    return clamp(texel >> level, 0, levelSize - 1);
}

float nearerDepth(float a, float b) {
    return max(a, b);
}

vec4 unprojectPixel(mat4 inverseViewProjection, vec2 pixel, float depth, vec2 size) {
    return inverseViewProjection * vec4(pixelToNdc(pixel, size), depth, 1.0);
}

vec3 clipToScreen(vec4 clip, vec2 size) {
    return vec3(ndcToPixel(clip.xy / clip.w, size), clip.z / clip.w);
}

vec3 positionAtPixel(mat4 inverseViewProjection, vec2 pixel, float depth, vec2 size) {
    vec4 point = unprojectPixel(inverseViewProjection, pixel, depth, size);
    return point.xyz / point.w;
}

float viewDistanceAtPixel(mat4 inverseViewProjection, vec2 pixel, float depth, vec2 size) {
    return 1.0 / unprojectPixel(inverseViewProjection, pixel, depth, size).w;
}

float stayWithin(float start, float step, float low, float high, float end) {
    if (step > 0.0) {
        return min(end, (high - start) / step);
    }

    if (step < 0.0) {
        return min(end, (low - start) / step);
    }

    return end;
}

ScreenRay screenRay(mat4 viewProjection, vec3 position, vec3 direction, float reach, vec2 size) {
    vec4 startClip = viewProjection * vec4(position, 1.0);
    vec3 far = position + direction * reach;
    vec4 endClip = viewProjection * vec4(far, 1.0);
    const float nearest = float(SSR_NEAREST_W);

    if (endClip.w < nearest && startClip.w > nearest) {
        far = position + direction * (reach * (startClip.w - nearest) / (startClip.w - endClip.w));
        endClip = viewProjection * vec4(far, 1.0);
    }

    ScreenRay ray;
    ray.start = clipToScreen(startClip, size);
    ray.step = clipToScreen(endClip, size) - ray.start;

    float end = 1.0;
    end = stayWithin(ray.start.x, ray.step.x, 0.0, size.x, end);
    end = stayWithin(ray.start.y, ray.step.y, 0.0, size.y, end);
    end = stayWithin(ray.start.z, ray.step.z, float(GPU_FAR_DEPTH), float(GPU_NEAR_DEPTH), end);
    ray.end = max(end, 0.0);
    return ray;
}

float axisExit(float position, float step, float cellSize) {
    if (step > 0.0) {
        return ((floor(position / cellSize) + 1.0) * cellSize + float(SSR_CELL_CROSSING) - position) / step;
    }

    if (step < 0.0) {
        return (floor(position / cellSize) * cellSize - float(SSR_CELL_CROSSING) - position) / step;
    }

    return SSR_NEVER;
}

float cellExit(vec2 position, vec2 step, float cellSize) {
    return min(axisExit(position.x, step.x, cellSize), axisExit(position.y, step.y, cellSize));
}

HizTrace advanceTrace(HizTrace trace, ScreenRay ray, float t, int level) {
    trace.t += t;
    trace.position = ray.start + ray.step * trace.t;
    trace.level = level;
    return trace;
}

HizTrace hizStep(HizTrace trace, ScreenRay ray, float cellNearest, int topLevel) {
    if (!(trace.position.z > cellNearest)) {
        trace.level -= 1;
        return trace;
    }

    float cellSize = float(1 << trace.level);
    float toCellEdge = cellExit(trace.position.xy, ray.step.xy, cellSize);
    float toSurface = ray.step.z < 0.0 ? (cellNearest - trace.position.z) / ray.step.z : SSR_NEVER;

    if (toSurface < toCellEdge) {
        return advanceTrace(trace, ray, toSurface, trace.level - 1);
    }

    return advanceTrace(trace, ray, toCellEdge, min(trace.level + 1, topLevel));
}

HizTrace passTexel(HizTrace trace, ScreenRay ray) {
    return advanceTrace(trace, ray, cellExit(trace.position.xy, ray.step.xy, 1.0), 0);
}

bool withinThickness(float rayDistance, float surfaceDistance, float thickness) {
    return rayDistance - surfaceDistance <= thickness;
}

// As reflectionConfidence's ReflectionHit and ReflectionSettings: the roughness limits, the edge fade and the reach.
float reflectionConfidence(float roughness, vec2 pixel, float travelled, float facing, float towardsCamera, vec2 size,
                           float sharpRoughness, float maxRoughness, float edgeFade, float reach) {
    float rough = 1.0 - smoothstep(sharpRoughness, maxRoughness, roughness);

    vec2 uv = pixel / size;
    float inside = 2.0 * min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    float edge = smoothstep(0.0, edgeFade, inside);

    float distance = 1.0 - smoothstep(float(SSR_DISTANCE_FADE_START) * reach, reach, travelled);
    float front = 1.0 - smoothstep(-float(SSR_FRONT_FACE_COSINE), 0.0, facing);
    float outwards = 1.0 - smoothstep(float(SSR_TOWARDS_CAMERA_START), float(SSR_TOWARDS_CAMERA_END), towardsCamera);
    return rough * edge * distance * front * outwards;
}

float reflectionConeLevel(float roughness, float travelledPixels) {
    float width = 2.0 * travelledPixels * float(SSR_CONE_SPREAD) * roughness * roughness;
    return log2(max(width, 1.0));
}

vec3 reflectedScene(vec3 scene, vec3 standIn, vec3 specularAlbedo, vec3 hitLight, float confidence) {
    return max(scene + (specularAlbedo * hitLight - standIn) * confidence, vec3(0.0));
}

#endif
