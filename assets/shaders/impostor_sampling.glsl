// Reading an impostor's atlases at a card point (scene/impostor.h): the four frames round the view direction, each read
// where the point falls on its own plane, blended bilinearly; a point off a frame's square reads nothing there. The mips
// come from how fast the card point moves across the screen, carried onto each frame's plane. Needs materials.glsl's
// textures.
#ifndef IMPOSTOR_SAMPLING_GLSL
#define IMPOSTOR_SAMPLING_GLSL

#include "impostor.glsl"

struct ImpostorSample {
    vec4 colour; // rgb as stored (sRGB decoded by the sampler); a: how much of the tree covers the point
    vec3 normal; // in the tree's frame, unit (or zero where nothing covers it)
    vec3 point;  // the surface seen there (impostorSurfacePoint, as the card point; the card point where nothing covers it)
};

ImpostorSample sampleImpostor(uint colourSlot, uint normalSlot, float frames, vec3 cardPoint, vec3 viewDirection, vec3 stepAcross, vec3 stepDown,
                              float mipBias) {
    float last = frames - 1.0;
    vec2 place = hemiOctahedralEncode(normalize(viewDirection)) * last;
    vec2 first = clamp(floor(place), vec2(0.0), vec2(last - 1.0));
    vec2 fraction = clamp(place - first, vec2(0.0), vec2(1.0));
    float scale = exp2(mipBias) * 0.5 / frames;

    ImpostorSample result;
    result.colour = vec4(0.0);
    vec3 normalSum = vec3(0.0);
    vec3 pointSum = vec3(0.0);
    float coverage = 0.0;

    for (int corner = 0; corner < 4; ++corner) {
        vec2 offset = vec2(float(corner % 2), float(corner / 2));
        vec2 frame = first + offset;
        float weight = (offset.x > 0.5 ? fraction.x : 1.0 - fraction.x) * (offset.y > 0.5 ? fraction.y : 1.0 - fraction.y);
        vec3 direction = impostorFrameDirection(frame, frames);
        vec3 right = impostorFrameRight(direction);
        vec3 up = impostorFrameUp(direction, right);
        vec2 frameUv = vec2(dot(cardPoint, right), dot(cardPoint, up)) * 0.5 + 0.5;

        if (any(lessThan(frameUv, vec2(0.0))) || any(greaterThan(frameUv, vec2(1.0)))) {
            continue;
        }

        vec2 atlas = impostorAtlasUv(frameUv, frame, frames);
        vec2 gradientAcross = vec2(dot(stepAcross, right), dot(stepAcross, up)) * scale;
        vec2 gradientDown = vec2(dot(stepDown, right), dot(stepDown, up)) * scale;
        vec4 colour = textureGrad(textures[nonuniformEXT(colourSlot)], atlas, gradientAcross, gradientDown);
        vec4 normalTexel = textureGrad(textures[nonuniformEXT(normalSlot)], atlas, gradientAcross, gradientDown);
        result.colour += weight * colour;
        normalSum += weight * colour.a * (normalTexel.rgb * 2.0 - 1.0);
        pointSum += weight * colour.a * impostorSurfacePoint(cardPoint, direction, impostorDepthDecode(normalTexel.a));
        coverage += weight * colour.a;
    }

    result.normal = dot(normalSum, normalSum) > 0.0 ? normalize(normalSum) : vec3(0.0);
    result.point = coverage > 0.0 ? pointSum / coverage : cardPoint;
    return result;
}

/*
* The plant's own shadow at a point on a leaf (over the radius, in the tree's frame; its normal, unit, zero for none) from
* the sun (towards it, unit, in the tree's frame), 1 lit (scene/impostor.h, impostorSunLight, which mirrors it on the
* bake): from the leaf moved off by impostorShadowPoint, the four frames round the sun read as shadow maps, each the
* 2 x 2 texels round the point's place gathered (kept inside the frame) from the depths (the normal atlas's alpha) and
* the coverage (the colour's), tested by impostorGatherLit, blended bilinearly. No derivatives, so it may run inside a
* light's branch.
*/
float impostorSunLight(uint colourSlot, uint normalSlot, float frames, vec3 leaf, vec3 normal, vec3 sunDirection, float biasTexels) {
    float last = frames - 1.0;
    vec2 place = hemiOctahedralEncode(sunDirection) * last;
    vec2 first = clamp(floor(place), vec2(0.0), vec2(last - 1.0));
    vec2 fraction = clamp(place - first, vec2(0.0), vec2(1.0));
    vec2 size = vec2(textureSize(textures[nonuniformEXT(normalSlot)], 0));
    float frameTexels = size.x / frames;
    float bias = impostorShadowBias(biasTexels, frameTexels);
    vec3 point = impostorShadowPoint(leaf, normal, sunDirection, bias);
    float lit = 0.0;

    for (int corner = 0; corner < 4; ++corner) {
        vec2 offset = vec2(float(corner % 2), float(corner / 2));
        vec2 frame = first + offset;
        float weight = (offset.x > 0.5 ? fraction.x : 1.0 - fraction.x) * (offset.y > 0.5 ? fraction.y : 1.0 - fraction.y);
        vec3 direction = impostorFrameDirection(frame, frames);
        vec2 frameUv = impostorFrameUv(point, direction);

        if (any(lessThan(frameUv, vec2(0.0))) || any(greaterThan(frameUv, vec2(1.0)))) {
            lit += weight;
            continue;
        }

        vec2 texelPlace = impostorAtlasUv(frameUv, frame, frames) * size - 0.5;
        vec2 frameCorner = frame * frameTexels;
        vec2 corner00 = clamp(floor(texelPlace), frameCorner, frameCorner + frameTexels - 2.0);
        vec2 within = clamp(texelPlace - corner00, vec2(0.0), vec2(1.0));
        // Between the four texels' middles, so the gather takes exactly those.
        vec2 gatherUv = (corner00 + 1.0) / size;
        vec4 depths = textureGather(textures[nonuniformEXT(normalSlot)], gatherUv, 3);
        vec4 covered = textureGather(textures[nonuniformEXT(colourSlot)], gatherUv, 3);
        lit += weight * impostorGatherLit(depths, covered, within, dot(point, direction), bias);
    }

    return lit;
}

#endif
