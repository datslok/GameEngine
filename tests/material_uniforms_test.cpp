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

    // A non-metal of middling roughness by default (the earlier default highlight, shininess 32, matches roughness 0.5),
    // with no metallic-roughness texture.
    void testDefaultMaterialIsRoughPlastic() {
        const Material material;

        assert(nearlyEqual(material.metallic, 0.0f));
        assert(nearlyEqual(material.roughness, 0.5f));
        assert(!material.metallicRoughnessTexture.isValid());
        assert(!material.normalTexture.isValid());
        assert(nearlyEqual(material.normalScale, 1.0f));
    }

    // Byte colours are sRGB like image files, so the shader gets them decoded to linear light, with full opacity.
    void testColourIsConverted() {
        Material material;
        material.colour = Pixel{255, 128, 0};

        const MaterialUniformData data = packMaterial(material);

        assert(nearlyEqual(data.baseColour, 1.0f, 0.2158f, 0.0f, 1.0f));
    }

    void testDefaultSurfaceIsPacked() {
        const MaterialUniformData data = packMaterial(Material{});

        assert(nearlyEqual(data.surface, 0.0f, 0.5f, 0.0f, 0.0f));
    }

    // Metallic and roughness are plain numbers, not colours, so they are packed as they are.
    void testCustomSurfaceIsPacked() {
        Material material;
        material.metallic = 1.0f;
        material.roughness = 0.3f;

        const MaterialUniformData data = packMaterial(material);

        assert(nearlyEqual(data.surface, 1.0f, 0.3f, 0.0f, 0.0f));
    }

    // Both are shares, 0..1: values outside are clamped rather than shaded as nonsense.
    void testSurfaceIsClamped() {
        Material material;
        material.metallic = 2.0f;
        material.roughness = -1.0f;

        const MaterialUniformData data = packMaterial(material);

        assert(nearlyEqual(data.surface, 1.0f, 0.0f, 0.0f, 0.0f));
    }
}

namespace {
    // No normal map, no bumps: the scale goes to the shader as 0, which makes it ignore the white stand-in texture.
    void testNormalScaleNeedsATexture() {
        Material material;
        material.normalScale = 0.8f;
        assert(nearlyEqual(packMaterial(material).surface, 0.0f, 0.5f, 0.0f, 0.0f));

        material.normalTexture = TextureHandle{3};
        assert(nearlyEqual(packMaterial(material).surface, 0.0f, 0.5f, 0.8f, 0.0f));
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
    testDefaultMaterialIsRoughPlastic();
    testColourIsConverted();
    testDefaultSurfaceIsPacked();
    testCustomSurfaceIsPacked();
    testSurfaceIsClamped();
    testNormalScaleNeedsATexture();
}
