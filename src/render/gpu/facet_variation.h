#pragma once

#include "math/vec2.h"

/*
* Each flat facet's own tone and roughness (assets/shaders/facet_variation.glsl mirrors these): a faceted mesh gives every
* corner of a triangle the same texture coordinate, two random numbers 0..1 (games/forest/structures), and a material
* with facet variation (Material::facetTone, facetRoughness) reads them, so neighbouring facets at nearly the same angle
* do not merge into one shape.
*/

// What the facet's albedo is multiplied by: its first number turned to -1..1, lightening or darkening by up to amount
// (a share: 0.12 is 12 %), never below black.
float facetToneScale(Vec2 facetNumbers, float amount);

// The facet's roughness: moved by its second number turned to -1..1, up to amount either way, kept in 0..1.
float facetRoughness(float roughness, Vec2 facetNumbers, float amount);
