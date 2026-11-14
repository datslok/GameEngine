#include "render/gpu/gpu_scene.h"
#include "render/gpu/ground_layers.h"
#include "render/gpu/material_uniforms.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::fabs(a - b) <= tolerance;
    }

    float sum(const GroundWeights& weights) {
        return weights[0] + weights[1] + weights[2] + weights[3];
    }

    /*
    * Height blending (render/gpu/ground_layers.h): the layers' painted weights sharpened by their textures' heights, so
    * where two layers meet the one standing higher shows (moss in the cracks between stones, grass poking through leaf
    * litter). The result always adds up to one; a layer not painted there never shows, whatever its height; one layer
    * painted alone is all of it.
    */
    void testBlendKeepsTheBasics() {
        const GroundWeights alone = groundBlendWeights({0.0f, 1.0f, 0.0f, 0.0f}, {0.9f, 0.1f, 1.0f, 0.5f}, 0.2f);
        assert(alone[0] == 0.0f && alone[1] == 1.0f && alone[2] == 0.0f && alone[3] == 0.0f);

        const GroundWeights mixed = groundBlendWeights({0.5f, 0.3f, 0.2f, 0.0f}, {0.2f, 0.7f, 0.4f, 1.0f}, 0.2f);
        assert(near(sum(mixed), 1.0f, 1e-6f) && mixed[3] == 0.0f);

        for (const float weight : mixed) {
            assert(weight >= 0.0f && weight <= 1.0f);
        }
    }

    /*
    * Two layers painted half and half: with a shallow blend depth the higher texel takes (nearly) all of it, so the
    * border follows the textures' own shapes; the deeper the blend, the nearer the plain painted weights. Equal heights
    * keep the painted weights exactly.
    */
    void testTheHigherLayerWins() {
        const GroundWeights sharp = groundBlendWeights({0.5f, 0.5f, 0.0f, 0.0f}, {0.9f, 0.3f, 0.0f, 0.0f}, 0.1f);
        assert(sharp[0] > 0.99f);

        const GroundWeights soft = groundBlendWeights({0.5f, 0.5f, 0.0f, 0.0f}, {0.9f, 0.3f, 0.0f, 0.0f}, 100.0f);
        assert(near(soft[0], 0.5f, 0.01f));

        const GroundWeights level = groundBlendWeights({0.7f, 0.3f, 0.0f, 0.0f}, {0.4f, 0.4f, 0.0f, 0.0f}, 0.2f);
        assert(near(level[0], 0.7f, 1e-5f) && near(level[1], 0.3f, 1e-5f));

        // Painted more strongly, a lower layer still shows where it is not too far below.
        const GroundWeights stronger = groundBlendWeights({0.9f, 0.1f, 0.0f, 0.0f}, {0.5f, 0.6f, 0.0f, 0.0f}, 0.3f);
        assert(stronger[0] > stronger[1]);
    }

    // Weights read from an 8-bit texture may not add up to one; nothing painted at all falls back to the first layer.
    void testUnevenAndEmptyWeights() {
        const GroundWeights uneven = groundBlendWeights({0.3f, 0.3f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f, 0.5f}, 0.2f);
        assert(near(uneven[0], 0.5f, 1e-5f) && near(uneven[1], 0.5f, 1e-5f));

        const GroundWeights none = groundBlendWeights({0.0f, 0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f, 0.5f}, 0.2f);
        assert(none[0] == 1.0f && none[1] == 0.0f);
    }

    /*
    * A layered ground in the frame's material table: its layers are ordinary materials, each added (shared with any
    * other draw using the same) before the ground itself, whose record names them by their place in the table plus one
    * (0 for no layer), with each layer's repeats over the mesh's texture coordinates, the weights texture's slot and the
    * blend depth. A material without layers packs all of that as zero.
    */
    void testTheTableNamesTheLayers() {
        Material grass;
        grass.colour = Pixel{60, 120, 40};
        Material mud;
        mud.colour = Pixel{90, 70, 50};
        mud.roughness = 0.9f;

        Material ground;
        ground.groundLayers = {grass, mud};
        ground.groundRepeats = {100.0f, 50.0f, 0.0f, 0.0f};
        ground.groundWeights = TextureHandle{4};
        ground.groundBlendDepth = 0.25f;

        MaterialTable table;
        const std::uint32_t mudAlone = table.add(mud);
        const std::uint32_t index = table.add(ground);
        const GpuMaterial& packed = table.getEntries()[index];

        assert(packed.groundLayers[1] == mudAlone + 1 && packed.groundLayers[0] != 0 && packed.groundLayers[2] == 0 && packed.groundLayers[3] == 0);
        assert(table.getEntries()[packed.groundLayers[0] - 1].baseColour[1] > table.getEntries()[packed.groundLayers[0] - 1].baseColour[0]);
        assert(packed.groundRepeats[0] == 100.0f && packed.groundRepeats[1] == 50.0f);
        assert(packed.groundWeights[0] == 5 && near(packed.groundBlend[0], 0.25f, 0.0f));
        assert(table.add(ground) == index);

        const GpuMaterial plain = packMaterial(Material{});
        assert(plain.groundLayers[0] == 0 && plain.groundWeights[0] == 0 && plain.groundRepeats[0] == 0.0f);
    }

    // At most four layers, none layered itself, and a weights texture to blend them by.
    void testLayeringMistakesAreRefused() {
        const auto refused = [](const Material& material) {
            MaterialTable table;
            try {
                table.add(material);
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };

        Material five;
        five.groundLayers.resize(5);
        five.groundWeights = TextureHandle{1};
        assert(refused(five));

        Material nested;
        nested.groundLayers.resize(1);
        nested.groundLayers[0].groundLayers.resize(1);
        nested.groundWeights = TextureHandle{1};
        assert(refused(nested));

        Material unweighted;
        unweighted.groundLayers.resize(2);
        assert(refused(unweighted));
    }
}

namespace {
    // A row of texels as the GPU reads it straight: between the two nearest centres (at k + 1/2), held at the ends.
    float readStraight(const std::vector<float>& row, float place) {
        const float centred = place - 0.5f;
        const int lower = static_cast<int>(std::floor(centred));
        const float share = centred - static_cast<float>(lower);
        const auto at = [&](int texel) {
            return row[static_cast<std::size_t>(std::clamp(texel, 0, static_cast<int>(row.size()) - 1))];
        };
        return at(lower) * (1.0f - share) + at(lower + 1) * share;
    }

    // The same row read smoothly: the cubic B-spline's four weights on the four nearest texels.
    float readCubic(const std::vector<float>& row, float place) {
        const float centred = place - 0.5f;
        const int cell = static_cast<int>(std::floor(centred));
        const float f = centred - static_cast<float>(cell);
        const float weights[4] = {(1.0f - f) * (1.0f - f) * (1.0f - f) / 6.0f, (3.0f * f * f * f - 6.0f * f * f + 4.0f) / 6.0f,
                                  (-3.0f * f * f * f + 3.0f * f * f + 3.0f * f + 1.0f) / 6.0f, f * f * f / 6.0f};
        float sum = 0.0f;

        for (int tap = 0; tap < 4; ++tap) {
            sum += weights[tap] * row[static_cast<std::size_t>(std::clamp(cell - 1 + tap, 0, static_cast<int>(row.size()) - 1))];
        }

        return sum;
    }

    // The two reads at the taps give the B-spline's value; and across a texel border its slope does not jump, where the
    // straight read's does: the kinks that made the forest's borders staircases.
    void testPaintIsReadAsASmoothCurve() {
        const std::vector<float> row{0.0f, 0.0f, 0.1f, 0.9f, 1.0f, 1.0f, 0.3f, 0.0f, 0.0f, 0.6f, 0.0f, 0.0f};

        for (float place = 2.0f; place < 10.0f; place += 0.037f) {
            const GroundPaintTaps taps = groundPaintTaps(Vec2{place, place});
            assert(taps.first.x >= place - 2.0f && taps.first.x <= place && taps.second.x >= place && taps.second.x <= place + 2.0f);
            assert(taps.firstShare.x > 0.0f && taps.firstShare.x < 1.0f && taps.first.y == taps.first.x);
            const float twoReads = taps.firstShare.x * readStraight(row, taps.first.x) + (1.0f - taps.firstShare.x) * readStraight(row, taps.second.x);
            assert(std::fabs(twoReads - readCubic(row, place)) < 1e-5f);
        }

        const float border = 4.0f; // between texels 3 and 4's centres lies 4.0; the centre of texel 3 is 3.5
        const float step = 1e-3f;
        const auto slopeJump = [&](float (*read)(const std::vector<float>&, float), float at) {
            const float left = (read(row, at) - read(row, at - step)) / step;
            const float right = (read(row, at + step) - read(row, at)) / step;
            return std::fabs(right - left);
        };

        assert(slopeJump(readStraight, border - 0.5f) > 0.5f); // a texel's centre, where the straight read bends
        assert(slopeJump(readCubic, border - 0.5f) < 0.01f);
    }
}

void testGroundLayers() {
    testPaintIsReadAsASmoothCurve();
    testBlendKeepsTheBasics();
    testTheHigherLayerWins();
    testUnevenAndEmptyWeights();
    testTheTableNamesTheLayers();
    testLayeringMistakesAreRefused();
}
