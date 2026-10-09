#include "render/gpu/material_uniforms.h"
#include "scene/material.h"

#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.001f;
    }

    bool nearlyEqual(const float (&actual)[4], float x, float y, float z, float w) {
        return nearlyEqual(actual[0], x) && nearlyEqual(actual[1], y) && nearlyEqual(actual[2], z) && nearlyEqual(actual[3], w);
    }

    // Most surfaces are slightly shiny, so that is the default; matte materials set the strength to 0.
    void testDefaultMaterialIsSlightlyShiny() {
        const Material material;

        assert(nearlyEqual(material.specularStrength, 0.25f));
        assert(nearlyEqual(material.shininess, 32.0f));
    }

    // Byte colours are sRGB like image files, so the shader gets them decoded to linear light, with full opacity.
    void testColourIsConverted() {
        Material material;
        material.colour = Pixel{255, 128, 0};

        const MaterialUniformData data = packMaterial(material);

        assert(nearlyEqual(data.baseColour, 1.0f, 0.2158f, 0.0f, 1.0f));
    }

    void testDefaultSpecularIsPacked() {
        const MaterialUniformData data = packMaterial(Material{});

        assert(nearlyEqual(data.specular, 0.25f, 32.0f, 0.0f, 0.0f));
    }

    void testCustomSpecularIsPacked() {
        Material material;
        material.specularStrength = 0.0f;
        material.shininess = 128.0f;

        const MaterialUniformData data = packMaterial(material);

        assert(nearlyEqual(data.specular, 0.0f, 128.0f, 0.0f, 0.0f));
    }
}

namespace {
    // Surfaces do not glow unless asked to: the default emissive colour is black.
    void testDefaultMaterialDoesNotGlow() {
        const MaterialUniformData data = packMaterial(Material{});

        assert(nearlyEqual(data.emissive, 0.0f, 0.0f, 0.0f, 0.0f));
    }

    // Emitted light is added as is, whatever lights the scene has, so a glowing moon or marker keeps its colour at night.
    void testEmissiveIsPacked() {
        Material moon;
        moon.emissive = Vec3{0.85f, 0.9f, 1.0f};

        const MaterialUniformData data = packMaterial(moon);

        assert(nearlyEqual(data.emissive, 0.85f, 0.9f, 1.0f, 0.0f));
    }
}

void testMaterialUniforms() {
    testDefaultMaterialDoesNotGlow();
    testEmissiveIsPacked();
    testDefaultMaterialIsSlightlyShiny();
    testColourIsConverted();
    testDefaultSpecularIsPacked();
    testCustomSpecularIsPacked();
}
