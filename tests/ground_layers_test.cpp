#include "render/gpu/gpu_scene.h"
#include "render/gpu/ground_layers.h"
#include "render/gpu/material_uniforms.h"
#include "render/gpu/normal_mapping.h"
#include "math/transform.h"

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

namespace {
    bool nearVec(const Vec3& a, const Vec3& b, float tolerance) {
        return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
    }

    /*
    * The projection's frame from a model matrix built as rotation times scale (Transform's): its axes are the rotation's
    * columns, unit length and square to each other, and a mesh point p sits at the axes weighted by S p, plus the
    * object's place, which is where the model matrix puts it.
    */
    void testTheFrameUndoesTheTurnNotTheScale() {
        const Transform transform{.position = Vec3{300.0f, -20.0f, 40.0f}, .rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, 0.7f),
                                  .scale = Vec3{25.0f, 310.0f, 25.0f}};
        const Mat4 model = transform.getMatrix();
        const ProjectionFrame frame = projectionFrame(model);

        assert(near(frame.scale.x, 25.0f, 1e-3f) && near(frame.scale.y, 310.0f, 1e-2f) && near(frame.scale.z, 25.0f, 1e-3f));

        for (int axis = 0; axis < 3; ++axis) {
            assert(near(frame.axes[static_cast<std::size_t>(axis)].length(), 1.0f, 1e-5f));
            assert(near(frame.axes[static_cast<std::size_t>(axis)].dot(frame.axes[static_cast<std::size_t>((axis + 1) % 3)]), 0.0f, 1e-5f));
        }

        const Vec3 p{0.3f, 0.8f, -0.6f};
        const Vec3 inFrame{p.x * frame.scale.x, p.y * frame.scale.y, p.z * frame.scale.z};
        const Vec3 rebuilt = frame.axes[0] * inFrame.x + frame.axes[1] * inFrame.y + frame.axes[2] * inFrame.z + transform.position;
        assert(nearVec(rebuilt, transformPoint(model, p), 1e-2f));
    }

    /*
    * The projections' shares always add up to one and are never negative; a surface squarely facing an axis reads that
    * axis alone (either side), one halfway between two reads both equally, and one only a little off an axis still reads
    * that axis alone, as the small shares are dropped. Turning the normal slowly never makes a share jump.
    */
    void testProjectionSharesFollowTheFacing() {
        for (const Vec3& facing : {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}}) {
            const Vec3 weights = projectionWeights(facing);
            assert(nearVec(weights, Vec3{std::fabs(facing.x), std::fabs(facing.y), std::fabs(facing.z)}, 0.0f));
        }

        const Vec3 diagonal = projectionWeights(Vec3{1.0f, 0.0f, -1.0f}.normalized());
        assert(near(diagonal.x, 0.5f, 1e-6f) && diagonal.y == 0.0f && near(diagonal.z, 0.5f, 1e-6f));

        const Vec3 nearlyUp = projectionWeights(Vec3{0.3f, 1.0f, 0.2f}.normalized());
        assert(nearlyUp.x == 0.0f && nearlyUp.z == 0.0f && nearlyUp.y == 1.0f);

        assert(nearVec(projectionWeights(Vec3{0.0f, 0.0f, 0.0f}), Vec3{0.0f, 1.0f, 0.0f}, 0.0f));

        Vec3 previous = projectionWeights(Vec3{1.0f, 0.0f, 0.0f});

        for (int step = 1; step <= 2000; ++step) {
            const float angle = 1.5707964f * static_cast<float>(step) / 2000.0f;
            const Vec3 normal{std::cos(angle) * 0.8f, 0.6f * std::sin(3.0f * angle), std::sin(angle) * 0.8f};
            const Vec3 weights = projectionWeights(normal.normalized());
            assert(near(weights.x + weights.y + weights.z, 1.0f, 1e-5f));
            assert(weights.x >= 0.0f && weights.y >= 0.0f && weights.z >= 0.0f);
            assert(nearVec(weights, previous, 0.02f));
            previous = weights;
        }
    }

    // Each projection reads the plane across its axis, in metres, with up the world up the picture on the sides.
    void testProjectedPlaces() {
        const Vec3 p{1.5f, -7.0f, 22.0f};
        assert(projectedPlace(p, 0).x == 22.0f && projectedPlace(p, 0).y == -7.0f);
        assert(projectedPlace(p, 1).x == 1.5f && projectedPlace(p, 1).y == 22.0f);
        assert(projectedPlace(p, 2).x == 1.5f && projectedPlace(p, 2).y == -7.0f);
    }

    /*
    * The bumped normal: flat normal maps give back the surface's own normal, whichever way it faces. On a face squarely
    * across an axis it is exactly ordinary normal mapping with that projection's plane as the tangent frame (perturbNormal):
    * a tilt along the picture's first coordinate leans towards it on both sides of the object. It is unit length.
    */
    void testProjectedNormalsBendLikeNormalMaps() {
        const Vec3 flat{0.0f, 0.0f, 1.0f};

        for (const Vec3& raw : {Vec3{1.0f, 0.2f, -0.4f}, Vec3{-0.3f, -1.0f, 0.1f}, Vec3{0.2f, 0.5f, 0.9f}}) {
            const Vec3 normal = raw.normalized();
            assert(nearVec(projectedNormal(normal, {flat, flat, flat}, projectionWeights(normal)), normal, 1e-5f));
        }

        const Vec3 tilt = Vec3{0.3f, -0.2f, 0.9f}.normalized();
        const std::array<Vec3, 3> tilts{tilt, tilt, tilt};

        // Facing +x: the plane (z, y), so the tangent runs along +z and up the picture is +y.
        assert(nearVec(projectedNormal(Vec3{1.0f, 0.0f, 0.0f}, tilts, Vec3{1.0f, 0.0f, 0.0f}),
                       perturbNormal(Vec3{1.0f, 0.0f, 0.0f}, Vec4{0.0f, 0.0f, 1.0f, -1.0f}, tilt), 1e-5f));
        // Facing -x: the same plane seen from behind; the tilt still leans towards +z and -y.
        const Vec3 behind = projectedNormal(Vec3{-1.0f, 0.0f, 0.0f}, tilts, Vec3{1.0f, 0.0f, 0.0f});
        assert(behind.x < 0.0f && behind.z > 0.0f && behind.y < 0.0f && near(behind.length(), 1.0f, 1e-5f));
        // Facing +y: the plane (x, z).
        assert(nearVec(projectedNormal(Vec3{0.0f, 1.0f, 0.0f}, tilts, Vec3{0.0f, 1.0f, 0.0f}),
                       perturbNormal(Vec3{0.0f, 1.0f, 0.0f}, Vec4{1.0f, 0.0f, 0.0f, -1.0f}, tilt), 1e-5f));
        // Facing +z: the plane (x, y).
        assert(nearVec(projectedNormal(Vec3{0.0f, 0.0f, 1.0f}, tilts, Vec3{0.0f, 0.0f, 1.0f}),
                       perturbNormal(Vec3{0.0f, 0.0f, 1.0f}, Vec4{1.0f, 0.0f, 0.0f, 1.0f}, tilt), 1e-5f));

        const Vec3 blended = projectedNormal(Vec3{0.6f, 0.0f, 0.8f}, tilts, projectionWeights(Vec3{0.6f, 0.0f, 0.8f}));
        assert(near(blended.length(), 1.0f, 1e-5f));
    }

    /*
    * Strata: a height's place along the strip; and the top layer taking over facing up, none of it below its start, all
    * of it from its full, the shares still adding up to the strata's.
    */
    void testStrataAndTheTopLayer() {
        assert(near(strataPlace(-100.0f, -100.0f, 1.0f / 1500.0f), 0.0f, 0.0f));
        assert(near(strataPlace(650.0f, -100.0f, 1.0f / 1500.0f), 0.5f, 1e-6f));

        const GroundWeights strata{0.25f, 0.75f, 0.0f, 0.0f};
        const GroundWeights wall = strataPaint(strata, 3, 0.1f, 0.6f, 0.8f);
        assert(wall == strata);

        const GroundWeights top = strataPaint(strata, 3, 0.95f, 0.6f, 0.8f);
        assert(top[0] == 0.0f && top[1] == 0.0f && top[3] == 1.0f);

        const GroundWeights between = strataPaint(strata, 3, 0.7f, 0.6f, 0.8f);
        assert(near(between[3], 0.5f, 1e-6f) && near(between[0] + between[1] + between[3], 1.0f, 1e-6f));

        assert(strataPaint(strata, -1, 1.0f, 0.6f, 0.8f) == strata);
        assert(strataPaint(strata, 2, 0.59f, 0.6f, 0.6f)[2] == 0.0f && strataPaint(strata, 2, 0.6f, 0.6f, 0.6f)[2] == 1.0f);
    }

    /*
    * A projected, strata-painted material in the table: the switches as flags, the top layer as its number plus one,
    * the strip's bottom and one over its span, and the facing limits. A top layer that is not one of the layers is refused.
    */
    void testTheTableCarriesProjectionAndStrata() {
        Material rock;
        rock.groundLayers.resize(4);
        rock.groundWeights = TextureHandle{7};
        rock.groundProjected = true;
        rock.groundPaint = GroundPaint::Strata;
        rock.strataBottom = -200.0f;
        rock.strataTop = 1300.0f;
        rock.groundTopLayer = 3;
        rock.groundTopStart = 0.55f;
        rock.groundTopFull = 0.85f;

        MaterialTable table;
        const GpuMaterial& packed = table.getEntries()[table.add(rock)];
        assert(packed.groundWeights[1] == (groundProjectedFlag | groundStrataFlag) && packed.groundWeights[2] == 4);
        assert(packed.groundBlend[1] == -200.0f && near(packed.groundBlend[2], 1.0f / 1500.0f, 1e-9f));
        assert(packed.groundFacing[0] == 0.55f && packed.groundFacing[1] == 0.85f);

        Material painted;
        painted.groundLayers.resize(1);
        painted.groundWeights = TextureHandle{1};
        const GpuMaterial ground = packMaterial(painted);
        assert(ground.groundWeights[1] == 0 && ground.groundWeights[2] == 0);

        Material wrongTop = rock;
        wrongTop.groundLayers.resize(2);
        bool refused = false;
        try {
            MaterialTable{}.add(wrongTop);
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        assert(refused);
    }
}

void testGroundLayers() {
    testTheFrameUndoesTheTurnNotTheScale();
    testProjectionSharesFollowTheFacing();
    testProjectedPlaces();
    testProjectedNormalsBendLikeNormalMaps();
    testStrataAndTheTopLayer();
    testTheTableCarriesProjectionAndStrata();
    testPaintIsReadAsASmoothCurve();
    testBlendKeepsTheBasics();
    testTheHigherLayerWins();
    testUnevenAndEmptyWeights();
    testTheTableNamesTheLayers();
    testLayeringMistakesAreRefused();
}
