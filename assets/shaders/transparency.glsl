// Transparent surfaces' blending, included with #include "transparency.glsl": mirrors render/gpu/transparency.cpp,
// which tests it, and tests/gpu checks these functions against it on the GPU.

#ifndef TRANSPARENCY_GLSL
#define TRANSPARENCY_GLSL

// The light a transparent fragment adds, and the share of what is behind that still shows (per channel).
struct TransparentLight {
    vec3 added;
    vec3 transmittance;
};

// Coverage scales the whole surface; transmission swaps its diffuse part for what is behind, tinted, less what it
// reflects. Metals pass no light.
TransparentLight transparentBlend(float alpha, float transmission, float metallic, vec3 diffuseLight, vec3 specularLight, vec3 emissive,
                                  vec3 baseColour, vec3 specularAlbedo) {
    float passing = transmission * (1.0 - metallic);
    vec3 surface = diffuseLight * (1.0 - passing) + specularLight + emissive;
    vec3 through = baseColour * (vec3(1.0) - specularAlbedo) * passing;
    return TransparentLight(surface * alpha, through * alpha + vec3(1.0) * (1.0 - alpha));
}

#endif
