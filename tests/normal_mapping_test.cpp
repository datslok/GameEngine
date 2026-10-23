#include "math/angles.h"
#include "math/mat4.h"
#include "render/gpu/normal_mapping.h"

#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.0001f;
    }

    bool nearlyEqual(const Vec3& actual, const Vec3& expected) {
        return nearlyEqual(actual.x, expected.x) && nearlyEqual(actual.y, expected.y) && nearlyEqual(actual.z, expected.z);
    }

    bool exactlyEqual(const Vec3& actual, const Vec3& expected) {
        return actual.x == expected.x && actual.y == expected.y && actual.z == expected.z;
    }

    const Vec3 up{0.0f, 0.0f, 1.0f};
    const Vec4 alongX{1.0f, 0.0f, 0.0f, 1.0f};

    // The colour of an untouched normal map, light blue: the direction (0, 0, 1), straight out of the surface.
    const Vec3 flatTexel{0.5f, 0.5f, 1.0f};

    // Texels are 0..1 and directions -1..1: c * 2 - 1, with the material's scale on the sideways parts only.
    void testTexelsDecodeToDirections() {
        assert(nearlyEqual(decodeNormalTexel(flatTexel, 1.0f), Vec3{0.0f, 0.0f, 1.0f}));
        assert(nearlyEqual(decodeNormalTexel(Vec3{1.0f, 0.0f, 0.5f}, 1.0f), Vec3{1.0f, -1.0f, 0.0f}));
        assert(nearlyEqual(decodeNormalTexel(Vec3{0.75f, 0.25f, 1.0f}, 2.0f), Vec3{1.0f, -1.0f, 1.0f}));
    }

    void testFlatTexelKeepsTheNormal() {
        assert(nearlyEqual(perturbNormal(up, alongX, decodeNormalTexel(flatTexel, 1.0f)), up));
    }

    /*
    * A material without a normal map reads the white texture (1, 1, 1) with scale 0: the sideways parts vanish and the
    * normal comes back exactly, so the shader needs no branch for it.
    */
    void testScaleZeroIgnoresTheTexture() {
        const Vec3 tilted = Vec3{0.3f, 0.2f, 1.0f}.normalized();
        assert(exactlyEqual(perturbNormal(tilted, alongX, decodeNormalTexel(Vec3{1.0f, 1.0f, 1.0f}, 0.0f)), perturbNormal(tilted, alongX, Vec3{0.0f, 0.0f, 1.0f})));
        assert(nearlyEqual(perturbNormal(tilted, alongX, decodeNormalTexel(Vec3{1.0f, 1.0f, 1.0f}, 0.0f)), tilted));
    }

    // Red tilts towards the tangent (+u), green towards the bitangent (up the picture).
    void testChannelsTiltAlongTheFrame() {
        assert(nearlyEqual(perturbNormal(up, alongX, Vec3{1.0f, 0.0f, 0.0f}), Vec3{1.0f, 0.0f, 0.0f}));
        assert(nearlyEqual(perturbNormal(up, alongX, Vec3{0.0f, 1.0f, 0.0f}), Vec3{0.0f, 1.0f, 0.0f}));
        assert(nearlyEqual(perturbNormal(up, alongX, Vec3{1.0f, 0.0f, 1.0f}), Vec3{1.0f, 0.0f, 1.0f}.normalized()));
    }

    // A sign of -1 (a mirrored UV island) points the bitangent the other way.
    void testSignFlipsTheBitangent() {
        const Vec4 mirrored{1.0f, 0.0f, 0.0f, -1.0f};
        assert(nearlyEqual(perturbNormal(up, mirrored, Vec3{0.0f, 1.0f, 0.0f}), Vec3{0.0f, -1.0f, 0.0f}));
    }

    // A larger scale exaggerates the bumps: the same texel tilts the normal further from the surface's own.
    void testScaleExaggeratesBumps() {
        const Vec3 texel{0.7f, 0.5f, 0.9f};
        const float gentle = perturbNormal(up, alongX, decodeNormalTexel(texel, 1.0f)).dot(up);
        const float strong = perturbNormal(up, alongX, decodeNormalTexel(texel, 2.0f)).dot(up);
        assert(strong < gentle);
    }

    /*
    * MikkTSpace's rule: the interpolated normal and tangent are used as they arrive, not normalised first, because that is
    * what the baker did. A tangent three times longer than the normal pulls the result three times as far.
    */
    void testFrameIsUsedUnnormalised() {
        const Vec3 result = perturbNormal(Vec3{0.0f, 0.0f, 2.0f}, Vec4{6.0f, 0.0f, 0.0f, 1.0f}, Vec3{1.0f, 0.0f, 1.0f});
        assert(nearlyEqual(result, Vec3{3.0f, 0.0f, 1.0f}.normalized()));
    }

    // With no usable frame (a squashed object), there is no direction to return.
    void testDegenerateFrameGivesZero() {
        assert(exactlyEqual(perturbNormal(Vec3{}, Vec4{}, Vec3{0.0f, 0.0f, 1.0f}), Vec3{}));
    }

    // Only the determinant's sign: whether the model matrix mirrors space.
    void testHandedness() {
        assert(handedness(Mat4::identity()) == 1.0f);
        assert(handedness(Mat4::rotationY(2.0f) * Mat4::scaling(3.0f, 0.5f, 2.0f)) == 1.0f);
        assert(handedness(Mat4::scaling(-1.0f, 1.0f, 1.0f)) == -1.0f);
        assert(handedness(Mat4::scaling(-1.0f, -1.0f, 1.0f)) == 1.0f);
        assert(handedness(Mat4::scaling(0.0f, 1.0f, 1.0f)) == 1.0f);
    }

    /*
    * Tangents lie in the surface like edges, so the model matrix carries them. On a mirrored object the rebuilt bitangent
    * (normal x tangent) would come out reversed, so the sign is multiplied by the matrix's handedness: the bumps stay bumps.
    */
    void testMirroredObjectKeepsItsBumps() {
        const Mat4 mirror = Mat4::rotationY(0.4f) * Mat4::scaling(-1.0f, 1.0f, 1.0f);
        const Vec4 tangent = transformTangent(mirror, alongX);
        const Vec4 normal4 = normalMatrix(mirror) * Vec4{up.x, up.y, up.z, 0.0f};
        const Vec3 normal = Vec3{normal4.x, normal4.y, normal4.z};

        // A texel tilting up the picture must tilt towards where the mirror carried "up the picture" (+y, unchanged here).
        const Vec3 bitangent = transformDirection(mirror, Vec3{0.0f, 1.0f, 0.0f}).normalized();
        assert(nearlyEqual(perturbNormal(normal, tangent, Vec3{0.0f, 1.0f, 0.0f}), bitangent));
        assert(nearlyEqual(perturbNormal(normal, tangent, Vec3{1.0f, 0.0f, 0.0f}), transformDirection(mirror, Vec3{1.0f, 0.0f, 0.0f}).normalized()));
        assert(tangent.w == -1.0f);
    }

    // A direction stored the way an 8-bit normal map stores it: -1..1 to 0..1, rounded to the nearest of 256 steps.
    Vec3 storedTexel(const Vec3& direction) {
        const auto store = [](float component) {
            return std::round((component * 0.5f + 0.5f) * 255.0f) / 255.0f;
        };
        return Vec3{store(direction.x), store(direction.y), store(direction.z)};
    }

    // What a smaller mip holds where bumps tilted +angle and -angle about the bitangent: the plain average of the texels.
    Vec3 averagedTiltTexel(float angle) {
        return (storedTexel(Vec3{std::sin(angle), 0.0f, std::cos(angle)}) + storedTexel(Vec3{-std::sin(angle), 0.0f, std::cos(angle)})) * 0.5f;
    }

    /*
    * A texel of the full-size texture is a unit direction, short of 1 only by 8-bit rounding; Toksvig counts none of that
    * as bumps, so close up (and on flat maps) the material's own roughness comes back exactly.
    */
    void testToksvigKeepsSharpTexels() {
        assert(toksvigRoughness(0.3f, flatTexel, 1.0f) == 0.3f);

        for (int i = 0; i < 200; ++i) {
            const float azimuth = 0.0314f * static_cast<float>(i);
            const float tilt = 0.0075f * static_cast<float>(i);
            const Vec3 direction{std::sin(tilt) * std::cos(azimuth), std::sin(tilt) * std::sin(azimuth), std::cos(tilt)};
            assert(toksvigRoughness(0.1f, storedTexel(direction), 1.0f) == 0.1f);
        }
    }

    /*
    * Bumps tilted 30 degrees either way average to a texel 0.866 long. Its spread, (1 - L) / L with L lengthened by the
    * rounding slack, adds twice over to alpha squared (alpha = roughness squared): Toksvig's widening of a Phong lobe,
    * 1/s' = 1/s + spread, with GGX's alpha^2 ~ 2/s.
    */
    void testToksvigWidensAveragedBumps() {
        const float length = decodeNormalTexel(averagedTiltTexel(radiansFromDegrees(30.0f)), 1.0f).length();
        const float lengthened = length + static_cast<float>(NORMAL_TOKSVIG_QUANTIZATION_SLACK);
        const float spread = (1.0f - lengthened) / lengthened;
        const float roughness = 0.5f;
        const float expected = std::sqrt(std::sqrt(roughness * roughness * roughness * roughness + 2.0f * spread));

        assert(std::abs(length - 0.866f) < 0.005f);
        assert(nearlyEqual(toksvigRoughness(roughness, averagedTiltTexel(radiansFromDegrees(30.0f)), 1.0f), expected));
        assert(expected > 0.75f);
    }

    // The more the bumps under a texel disagree, the shorter their average and the rougher the surface.
    void testToksvigShorterIsRougher() {
        float previous = toksvigRoughness(0.2f, averagedTiltTexel(0.0f), 1.0f);
        assert(previous == 0.2f);

        for (int step = 1; step <= 16; ++step) {
            const float roughness = toksvigRoughness(0.2f, averagedTiltTexel(0.09f * static_cast<float>(step)), 1.0f);
            assert(roughness >= previous);
            previous = roughness;
        }

        assert(previous > 0.9f);
    }

    // Bumps pointing every way average to nothing: the roughest surface there is, and never rougher than 1.
    void testToksvigStaysInRange() {
        assert(toksvigRoughness(0.1f, Vec3{0.5f, 0.5f, 0.5f}, 1.0f) == 1.0f);
        assert(toksvigRoughness(1.0f, averagedTiltTexel(0.5f), 1.0f) == 1.0f);
        assert(toksvigRoughness(0.6f, averagedTiltTexel(1.2f), 4.0f) <= 1.0f);
    }

    /*
    * The material's scale stretches the sideways parts, and with them the slopes: the spread goes with its square. With
    * scale 0 (no normal map: the white texture) nothing changes.
    */
    void testToksvigFollowsTheScale() {
        assert(toksvigRoughness(0.25f, Vec3{1.0f, 1.0f, 1.0f}, 0.0f) == 0.25f);
        assert(toksvigRoughness(0.25f, averagedTiltTexel(0.6f), 0.0f) == 0.25f);

        const float half = toksvigRoughness(0.25f, averagedTiltTexel(0.6f), 0.5f);
        const float full = toksvigRoughness(0.25f, averagedTiltTexel(0.6f), 1.0f);
        const float doubled = toksvigRoughness(0.25f, averagedTiltTexel(0.6f), 2.0f);
        assert(0.25f < half && half < full && full < doubled);
    }
}

void testNormalMapping() {
    testTexelsDecodeToDirections();
    testFlatTexelKeepsTheNormal();
    testScaleZeroIgnoresTheTexture();
    testChannelsTiltAlongTheFrame();
    testSignFlipsTheBitangent();
    testScaleExaggeratesBumps();
    testFrameIsUsedUnnormalised();
    testDegenerateFrameGivesZero();
    testHandedness();
    testMirroredObjectKeepsItsBumps();
    testToksvigKeepsSharpTexels();
    testToksvigWidensAveragedBumps();
    testToksvigShorterIsRougher();
    testToksvigStaysInRange();
    testToksvigFollowsTheScale();
}
