#include "render/gpu/facet_variation.h"
#include "render/gpu/material_uniforms.h"
#include "scene/material.h"
#include "test_levels.h"

#include <cassert>

/*
* A facet's numbers at their middle leave it as it is; at their ends they lighten or darken by the whole amount and move
* the roughness by it either way; the tone never goes below black and the roughness stays in 0..1; no amount changes
* nothing. The material packs both amounts for the shader, none for a material without them.
*/
void testFacetVariation() {
    assert(near(facetToneScale(Vec2{0.5f, 0.5f}, 0.12f), 1.0f, 1e-6f));
    assert(near(facetToneScale(Vec2{1.0f, 0.0f}, 0.12f), 1.12f, 1e-6f));
    assert(near(facetToneScale(Vec2{0.0f, 0.0f}, 0.12f), 0.88f, 1e-6f));
    assert(facetToneScale(Vec2{0.0f, 0.0f}, 2.0f) == 0.0f);
    assert(facetToneScale(Vec2{0.9f, 0.1f}, 0.0f) == 1.0f);

    assert(near(facetRoughness(0.65f, Vec2{0.0f, 0.5f}, 0.07f), 0.65f, 1e-6f));
    assert(near(facetRoughness(0.65f, Vec2{0.0f, 1.0f}, 0.07f), 0.72f, 1e-6f));
    assert(near(facetRoughness(0.65f, Vec2{0.0f, 0.0f}, 0.07f), 0.58f, 1e-6f));
    assert(facetRoughness(0.98f, Vec2{0.0f, 1.0f}, 0.07f) == 1.0f);
    assert(facetRoughness(0.02f, Vec2{0.0f, 0.0f}, 0.07f) == 0.0f);

    Material faceted;
    faceted.facetTone = 0.12f;
    faceted.facetRoughness = 0.07f;
    const GpuMaterial packed = packMaterial(faceted);
    assert(packed.facetVariation[0] == 0.12f && packed.facetVariation[1] == 0.07f);
    assert(packMaterial(Material{}).facetVariation[0] == 0.0f && packMaterial(Material{}).facetVariation[1] == 0.0f);
}
