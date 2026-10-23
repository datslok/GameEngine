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
        assert(!material.occlusionTexture.isValid());
        assert(nearlyEqual(material.occlusionStrength, 1.0f));
        assert(!material.emissiveTexture.isValid());
    }

    // Byte colours are sRGB like image files, so the shader gets them decoded to linear light, with full opacity.
    void testColourIsConverted() {
        Material material;
        material.colour = Pixel{255, 128, 0};

        const GpuMaterial data = packMaterial(material);

        assert(nearlyEqual(data.baseColour, 1.0f, 0.2158f, 0.0f, 1.0f));
    }

    void testDefaultSurfaceIsPacked() {
        const GpuMaterial data = packMaterial(Material{});

        assert(nearlyEqual(data.surface, 0.0f, 0.5f, 0.0f, 0.0f));
    }

    // Metallic and roughness are plain numbers, not colours, so they are packed as they are.
    void testCustomSurfaceIsPacked() {
        Material material;
        material.metallic = 1.0f;
        material.roughness = 0.3f;

        const GpuMaterial data = packMaterial(material);

        assert(nearlyEqual(data.surface, 1.0f, 0.3f, 0.0f, 0.0f));
    }

    // Both are shares, 0..1: values outside are clamped rather than shaded as nonsense.
    void testSurfaceIsClamped() {
        Material material;
        material.metallic = 2.0f;
        material.roughness = -1.0f;

        const GpuMaterial data = packMaterial(material);

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

    // The occlusion strength goes in surface.w, clamped to glTF's 0..1. Like the normal scale it goes as 0 without a
    // texture, which tells the shader to leave the light from all around as it was.
    void testOcclusionStrengthIsPacked() {
        Material material;
        material.occlusionStrength = 0.3f;
        assert(nearlyEqual(packMaterial(material).surface[3], 0.0f));

        material.occlusionTexture = TextureHandle{1};
        material.occlusionStrength = 0.3f;
        assert(nearlyEqual(packMaterial(material).surface[3], 0.3f));

        material.occlusionStrength = 2.0f;
        assert(nearlyEqual(packMaterial(material).surface[3], 1.0f));

        material.occlusionStrength = -1.0f;
        assert(nearlyEqual(packMaterial(material).surface[3], 0.0f));
    }
}

namespace {
    // Surfaces do not glow unless asked to: the default emissive colour is black.
    void testDefaultMaterialDoesNotGlow() {
        const GpuMaterial data = packMaterial(Material{});

        assert(nearlyEqual(data.emissive, 0.0f, 0.0f, 0.0f, 0.0f));
    }

    // Emitted light is added as is, whatever lights the scene has, so a glowing moon or marker keeps its colour at night.
    void testEmissiveIsPacked() {
        Material moon;
        moon.emissive = Vec3{0.85f, 0.9f, 1.0f};

        const GpuMaterial data = packMaterial(moon);

        assert(nearlyEqual(data.emissive, 0.85f, 0.9f, 1.0f, 0.0f));
    }
}

namespace {
    // Textures are read from one big array (bindless): slot 0 holds the white stand-in that "no texture" multiplies by,
    // and a texture's slot is its handle plus one, so the numbering follows the AssetManager's.
    void testTexturesBecomeArraySlots() {
        assert(whiteTextureSlot == 0);
        assert(bindlessTextureSlot(TextureHandle{}) == whiteTextureSlot);
        assert(bindlessTextureSlot(TextureHandle{0}) == 1);
        assert(bindlessTextureSlot(TextureHandle{5}) == 6);

        const GpuMaterial plain = packMaterial(Material{});
        assert(plain.textures[0] == whiteTextureSlot && plain.textures[1] == whiteTextureSlot && plain.textures[2] == whiteTextureSlot);
        assert(plain.textures[3] == whiteTextureSlot && plain.moreTextures[0] == whiteTextureSlot);

        Material textured;
        textured.texture = TextureHandle{2};
        textured.metallicRoughnessTexture = TextureHandle{7};
        textured.normalTexture = TextureHandle{9};
        textured.emissiveTexture = TextureHandle{4};
        textured.occlusionTexture = TextureHandle{7};
        const GpuMaterial packed = packMaterial(textured);
        assert(packed.textures[0] == 3 && packed.textures[1] == 8 && packed.textures[2] == 10);
        assert(packed.textures[3] == 5 && packed.moreTextures[0] == 8);

        // The rest of the extra slots are kept for later textures and stay zero, so materials hash alike.
        assert(packed.moreTextures[1] == 0 && packed.moreTextures[2] == 0 && packed.moreTextures[3] == 0);
    }
}

void testMaterialUniforms() {
    testTexturesBecomeArraySlots();
    testDefaultMaterialDoesNotGlow();
    testEmissiveIsPacked();
    testDefaultMaterialIsRoughPlastic();
    testColourIsConverted();
    testDefaultSurfaceIsPacked();
    testCustomSurfaceIsPacked();
    testSurfaceIsClamped();
    testNormalScaleNeedsATexture();
    testOcclusionStrengthIsPacked();
}
