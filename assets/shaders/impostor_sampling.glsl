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
        vec3 normalTexel = textureGrad(textures[nonuniformEXT(normalSlot)], atlas, gradientAcross, gradientDown).rgb;
        result.colour += weight * colour;
        normalSum += weight * colour.a * (normalTexel * 2.0 - 1.0);
    }

    result.normal = dot(normalSum, normalSum) > 0.0 ? normalize(normalSum) : vec3(0.0);
    return result;
}

#endif
