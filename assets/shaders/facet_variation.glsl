// Each flat facet's own tone and roughness, for faceted shapes whose meshes give every corner of a triangle the same
// texture coordinate: two random numbers 0..1, the facet's (Material::facetTone, facetRoughness; games/forest/structures).
// Without it neighbouring facets at nearly the same angle merge into one shape. Mirrors render/gpu/facet_variation.cpp,
// which tests the maths; tests/gpu checks these functions against it on the GPU.

#ifndef FACET_VARIATION_GLSL
#define FACET_VARIATION_GLSL

// What the facet's albedo is multiplied by: its first number turned to -1..1, lightening or darkening by up to amount
// (a share: 0.12 is 12 %), never below black.
float facetToneScale(vec2 facetNumbers, float amount) {
    return max(1.0 + amount * (2.0 * facetNumbers.x - 1.0), 0.0);
}

// The facet's roughness: moved by its second number turned to -1..1, up to amount either way, kept in 0..1.
float facetRoughness(float roughness, vec2 facetNumbers, float amount) {
    return clamp(roughness + amount * (2.0 * facetNumbers.y - 1.0), 0.0, 1.0);
}

#endif
