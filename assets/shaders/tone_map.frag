#version 450
#extension GL_GOOGLE_include_directive : require

// The curves, bloom's mix and the dither, shared with the GPU tests (tests/gpu) and mirrored in C++.
#include "tone_mapping.glsl"
#include "bloom.glsl"
#include "dither.glsl"
#include "camera_finish.glsl"

// The lit scene, in unbounded linear brightness (16-bit floats), the same size as the screen, read with a linear sampler
// (texelFetch ignores it; chromatic aberration reads between texels).
layout(set = 2, binding = 0) uniform sampler2D sceneColour;

// The grown bloom chain's level 0 (half the screen's size), read with a linear sampler. Only read when bloom is on.
layout(set = 2, binding = 1) uniform sampler2D bloom;

// The adapted exposure and the scene's brightness the eye is set for (render/gpu/eye_adaptation.h,
// eye_adaptation.comp). Only read when eye adaptation is on.
layout(std430, set = 2, binding = 2) readonly buffer Adaptation {
    EyeAdaptation adaptation;
};

// Must match ToneMapUniformData in render/gpu/tone_map_uniforms.h.
layout(std140, set = 3, binding = 0) uniform ToneMapData {
    vec4 parameters; // x: exposure, y: peak brightness (in units of SDR white), z: curve (a TONE_MAP_ number),
                     // w: bloom strength (0 = no bloom)
    vec4 display;    // x: what SDR white is written as (1, or an HDR screen's white level), y: 1 to dither,
                     // z: what the bloom is multiplied by (1 / its level count), w: 1 to multiply the exposure by
                     // the adapted one
    vec4 night;      // x: the rods' threshold, y: the cones' (stops), z: the blue shift, w: 1 for night vision
    vec4 whiteBalance[3]; // the camera's white balance matrix, row by row (render/gpu/camera_finish.h)
    vec4 grade;      // x: contrast, y: saturation, z: lift, w: gamma
    vec4 lens;       // x: gain, y: vignetting, z: chromatic aberration (pixels at the corner), w: 1 to grade
    vec4 view;       // xy: the tangents of the half field of view, z: film grain, w: the frame
};

layout(location = 0) out vec4 outputColour;

/*
* The final pass: each screen pixel reads the same pixel of the lit scene, mixes in the scattered light (bloom: a
* strength's share of the light replaced by the blurred scene, so no light is added), fades dim light towards the rods'
* grey (night vision, on the light itself: the receptors see the light, not the exposed image; without eye adaptation
* by the pixel's own brightness alone), scales it by the exposure (times the eye's adapted one, when it adapts),
* measures it in units of the screen's peak, curves it into 0..1 and scales back. An 8-bit screen then gets the dither (it belongs
* here, at the last step: the scene image keeps 16-bit floats, which do not band). An HDR screen takes a float image in
* scRGB, where 1 is a dim 80 nits, so the result is scaled until white sits at the white level Windows uses for the
* desktop; the peak is the screen's headroom above that white.
*
* The camera's finish (render/gpu/camera_finish.h), each skipped while off: chromatic aberration reads red and blue a
* little apart along the line from the centre; the white balance, natural vignetting and the grade work on the light
* before the curve (the grade after the exposure, which it then takes the place of in the curve's call); film grain goes
* on the curved light, before the dither.
*/
void main() {
    bool adapting = display.w > 0.5;
    float exposure = adapting ? parameters.x * exp2(adaptation.exposureEv) : parameters.x;
    float peak = parameters.y;
    int curve = int(parameters.z + 0.5);

    vec3 scene = texelFetch(sceneColour, ivec2(gl_FragCoord.xy), 0).rgb;
    vec2 size = vec2(textureSize(sceneColour, 0));

    if (lens.z > 0.0) {
        scene.r = texture(sceneColour, aberrationPlace(gl_FragCoord.xy, size, lens.z * 0.5) / size).r;
        scene.b = texture(sceneColour, aberrationPlace(gl_FragCoord.xy, size, -lens.z * 0.5) / size).b;
    }

    scene = max(scene, vec3(0.0));
    float bloomStrength = parameters.w;

    if (bloomStrength > 0.0) {
        vec2 place = gl_FragCoord.xy / size;
        scene = mixBloom(scene, texture(bloom, place).rgb * display.z, bloomStrength);
    }

    if (night.w > 0.5) {
        // Without adaptation, an eye set for nothing: the max in rodShare leaves the pixel's own stops.
        float eyeEv = adapting ? adaptation.adaptedMeanEv : -1e30;
        scene = nightVision(scene, eyeEv, night.x, night.y, night.z);
    }
    bool grading = lens.w > 0.5;

    if (grading) {
        // Row by row: each output channel is that row of the matrix dotted with the light.
        scene = max(vec3(dot(whiteBalance[0].xyz, scene), dot(whiteBalance[1].xyz, scene), dot(whiteBalance[2].xyz, scene)), vec3(0.0));
    }

    if (lens.y > 0.0) {
        vec2 ndc = vec2(gl_FragCoord.x / size.x * 2.0 - 1.0, 1.0 - gl_FragCoord.y / size.y * 2.0);
        scene *= naturalVignetting(ndc, view.xy, lens.y);
    }

    vec3 mapped = grading ? toneMap(gradeColour(scene * exposure, grade, lens.x), 1.0, peak, curve) : toneMap(scene, exposure, peak, curve);

    if (view.z > 0.0) {
        mapped = filmGrain(mapped, grainNoise(uvec3(uvec2(gl_FragCoord.xy), uint(view.w))), view.z, peak);
    }

    if (display.y > 0.5) {
        mapped = ditherForEightBits(mapped, interleavedGradientNoise(gl_FragCoord.xy));
    }

    outputColour = vec4(mapped * display.x, 1.0);
}
