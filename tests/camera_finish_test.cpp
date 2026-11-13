#include "core/hash.h"
#include "render/gpu/camera_finish.h"
#include "render/gpu/eye_adaptation.h"
#include "render/gpu/tone_map_uniforms.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    bool nearVec(const Vec3& a, const Vec3& b, float tolerance) {
        return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
    }

    Vec3 times(const Mat4& matrix, const Vec3& colour) {
        const Vec4 result = matrix * Vec4{colour.x, colour.y, colour.z, 0.0f};
        return Vec3{result.x, result.y, result.z};
    }

    bool rejects(const CameraSettings& settings) {
        try {
            validateCamera(settings);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    }

    // Every effect starts neutral or off (the user's choice): the picture is unchanged until one is switched on.
    void testDefaultsLeaveThePictureAlone() {
        const CameraSettings defaults;
        validateCamera(defaults);
        assert(!gradesColour(defaults));
        assert(defaults.whiteBalance == 6500.0f && defaults.tint == 0.0f);
        assert(defaults.contrast == 1.0f && defaults.saturation == 1.0f && defaults.lift == 0.0f && defaults.gamma == 1.0f && defaults.gain == 1.0f);
        assert(defaults.vignetting == 0.0f && defaults.chromaticAberration == 0.0f && defaults.filmGrain == 0.0f);

        CameraSettings changed;
        changed.contrast = 1.1f;
        assert(gradesColour(changed));
        changed = {};
        changed.whiteBalance = 5000.0f;
        assert(gradesColour(changed));
        changed = {};
        changed.tint = 0.01f;
        assert(gradesColour(changed));

        const float nan = std::numeric_limits<float>::quiet_NaN();
        CameraSettings wrong;
        wrong.whiteBalance = 1000.0f;
        assert(rejects(wrong));
        wrong.whiteBalance = nan;
        assert(rejects(wrong));
        wrong = {};
        wrong.tint = 0.2f;
        assert(rejects(wrong));
        wrong = {};
        wrong.contrast = 0.0f;
        assert(rejects(wrong));
        wrong = {};
        wrong.saturation = -0.1f;
        assert(rejects(wrong));
        wrong = {};
        wrong.gamma = 0.0f;
        assert(rejects(wrong));
        wrong = {};
        wrong.gain = -1.0f;
        assert(rejects(wrong));
        wrong = {};
        wrong.vignetting = 1.5f;
        assert(rejects(wrong));
        wrong = {};
        wrong.chromaticAberration = -1.0f;
        assert(rejects(wrong));
        wrong = {};
        wrong.filmGrain = -0.1f;
        assert(rejects(wrong));
    }

    // The blackbody locus against published chromaticities, and continuous where the fit's pieces meet.
    void testPlanckianLocus() {
        const Vec2 daylight = planckianChromaticity(6500.0f);
        assert(near(daylight.x, 0.3135f, 5e-4f) && near(daylight.y, 0.3237f, 5e-4f));
        const Vec2 tungsten = planckianChromaticity(3000.0f);
        assert(near(tungsten.x, 0.4369f, 5e-4f) && near(tungsten.y, 0.4041f, 5e-4f));
        const Vec2 candle = planckianChromaticity(2000.0f);
        assert(near(candle.x, 0.5267f, 1e-3f) && near(candle.y, 0.4133f, 1e-3f));

        for (const float joint : {2222.0f, 4000.0f}) {
            const Vec2 below = planckianChromaticity(joint - 0.01f);
            const Vec2 above = planckianChromaticity(joint + 0.01f);
            assert(near(below.x, above.x, 2e-3f) && near(below.y, above.y, 2e-3f));
        }
    }

    // CIE 1960 uv from xy (as Duv is measured), there and back.
    Vec2 toUv(Vec2 xy) {
        const float d = -2.0f * xy.x + 12.0f * xy.y + 3.0f;
        return Vec2{4.0f * xy.x / d, 6.0f * xy.y / d};
    }

    // Tint moves the white off the locus by that distance in CIE 1960 uv (Duv), across it, towards green when positive.
    void testTint() {
        const Vec2 onLocus = toUv(whiteBalanceChromaticity(5000.0f, 0.0f));
        const Vec2 plain = toUv(planckianChromaticity(5000.0f));
        assert(near(onLocus.x, plain.x, 1e-6f) && near(onLocus.y, plain.y, 1e-6f));

        const Vec2 tinted = toUv(whiteBalanceChromaticity(5000.0f, 0.01f));
        const Vec2 offset{tinted.x - onLocus.x, tinted.y - onLocus.y};
        assert(near(std::sqrt(offset.x * offset.x + offset.y * offset.y), 0.01f, 1e-4f));
        assert(offset.y > 0.0f);

        const Vec2 cooler = toUv(planckianChromaticity(5010.0f));
        const Vec2 along{cooler.x - plain.x, cooler.y - plain.y};
        const float alongLength = std::sqrt(along.x * along.x + along.y * along.y);
        assert(std::abs(offset.x * along.x + offset.y * along.y) / alongLength < 1e-4f);
    }

    /*
    * White balance: the neutral setting changes nothing; set for a light, it shows that light as 6500 K light shows,
    * so a tungsten room set to 3000 K looks as a daylit one, and the same grey turns blue (set for warmer light than
    * there is) or orange (for cooler).
    */
    void testWhiteBalance() {
        const Mat4 neutral = whiteBalanceMatrix(6500.0f, 0.0f);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                assert(near(neutral.values[row][column], row == column ? 1.0f : 0.0f, 1e-6f));
            }
        }

        for (const float kelvin : {2000.0f, 3000.0f, 4500.0f, 9000.0f}) {
            const Vec3 light = chromaticityToLinearSrgb(planckianChromaticity(kelvin), 1.0f);
            const Vec3 asDaylight = chromaticityToLinearSrgb(planckianChromaticity(6500.0f), 1.0f);
            assert(nearVec(times(whiteBalanceMatrix(kelvin, 0.0f), light), asDaylight, 2e-4f));
        }

        const Vec3 grey{0.5f, 0.5f, 0.5f};
        const Vec3 setWarm = times(whiteBalanceMatrix(3000.0f, 0.0f), grey);
        const Vec3 setCool = times(whiteBalanceMatrix(9000.0f, 0.0f), grey);
        assert(setWarm.z > setWarm.x);
        assert(setCool.x > setCool.z);
    }

    // The grade at its neutral values changes nothing; each control does what its name says, and nothing goes negative.
    void testGrade() {
        const CameraSettings neutral;
        for (const Vec3& colour : {Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.18f, 0.18f, 0.18f}, Vec3{3.0f, 0.5f, 0.02f}}) {
            assert(nearVec(gradeColour(colour, neutral), colour, 1e-6f * (1.0f + colour.x)));
        }

        CameraSettings contrast;
        contrast.contrast = 2.0f;
        assert(nearVec(gradeColour(Vec3{0.18f, 0.18f, 0.18f}, contrast), Vec3{0.18f, 0.18f, 0.18f}, 1e-6f)); // mid grey holds
        assert(near(gradeColour(Vec3{0.36f, 0.36f, 0.36f}, contrast).x, 0.72f, 1e-5f));                    // a stop up is two

        CameraSettings grey;
        grey.saturation = 0.0f;
        const Vec3 colour{0.8f, 0.3f, 0.1f};
        const Vec3 washed = gradeColour(colour, grey);
        const float luminance = pixelLuminance(colour);
        assert(nearVec(washed, Vec3{luminance, luminance, luminance}, 1e-6f));
        CameraSettings vivid;
        vivid.saturation = 1.5f;
        const Vec3 soft{0.6f, 0.4f, 0.3f}; // stays positive at 1.5 times its spread
        const Vec3 punchy = gradeColour(soft, vivid);
        assert(punchy.x - punchy.z > soft.x - soft.z);
        assert(near(pixelLuminance(punchy), pixelLuminance(soft), 1e-6f));

        CameraSettings cdl;
        cdl.gain = 2.0f;
        assert(near(gradeColour(Vec3{0.25f, 0.25f, 0.25f}, cdl).x, 0.5f, 1e-6f));
        cdl = {};
        cdl.lift = 0.1f;
        assert(near(gradeColour(Vec3{0.0f, 0.0f, 0.0f}, cdl).x, 0.1f, 1e-6f));
        cdl.lift = -0.1f;
        assert(gradeColour(Vec3{0.0f, 0.0f, 0.0f}, cdl).x == 0.0f);
        cdl = {};
        cdl.gamma = 2.0f;
        assert(near(gradeColour(Vec3{0.25f, 0.25f, 0.25f}, cdl).x, 0.5f, 1e-6f));
    }

    // cos^4 of the angle off the lens's axis: 1 in the middle, a quarter where the ray leaves at 45 degrees.
    void testVignetting() {
        const Vec2 tanHalf{1.0f, 0.5625f};
        assert(naturalVignetting(Vec2{0.0f, 0.0f}, tanHalf, 1.0f) == 1.0f);
        assert(naturalVignetting(Vec2{1.0f, 1.0f}, tanHalf, 0.0f) == 1.0f);
        assert(near(naturalVignetting(Vec2{1.0f, 0.0f}, tanHalf, 1.0f), 0.25f, 1e-6f));
        assert(near(naturalVignetting(Vec2{1.0f, 0.0f}, tanHalf, 0.5f), 0.625f, 1e-6f));
        assert(near(naturalVignetting(Vec2{-0.4f, 0.7f}, tanHalf, 1.0f), naturalVignetting(Vec2{0.4f, -0.7f}, tanHalf, 1.0f), 1e-7f));
        const float tanSquared = 0.4f * 0.4f + (0.7f * 0.5625f) * (0.7f * 0.5625f);
        assert(near(naturalVignetting(Vec2{0.4f, 0.7f}, tanHalf, 1.0f), 1.0f / ((1.0f + tanSquared) * (1.0f + tanSquared)), 1e-6f));
    }

    // A channel's sample moves along the line from the picture's centre, by the shift at the corner, less nearer in.
    void testAberrationPlace() {
        const Vec2 size{1920.0f, 1080.0f};
        const Vec2 centre{960.0f, 540.0f};
        const Vec2 atCentre = aberrationPlace(centre, size, 3.0f);
        assert(near(atCentre.x, 960.0f, 1e-4f) && near(atCentre.y, 540.0f, 1e-4f));

        const Vec2 corner = aberrationPlace(Vec2{1920.0f, 1080.0f}, size, 3.0f);
        const float cornerDistance = std::sqrt(960.0f * 960.0f + 540.0f * 540.0f);
        const float moved = std::sqrt((corner.x - centre.x) * (corner.x - centre.x) + (corner.y - centre.y) * (corner.y - centre.y));
        assert(near(moved, cornerDistance + 3.0f, 1e-3f));
        assert(near((corner.x - centre.x) / (corner.y - centre.y), 960.0f / 540.0f, 1e-4f));

        const Vec2 none = aberrationPlace(Vec2{100.0f, 50.0f}, size, 0.0f);
        assert(near(none.x, 100.0f, 1e-4f) && near(none.y, 50.0f, 1e-4f));
    }

    // The grain's noise: pcg3d's bits for the pixel and the frame, in 0..1, different each frame, even on average.
    void testGrainNoise() {
        const std::array<std::uint32_t, 3> bits = pcg3d({12u, 34u, 5u});
        assert(grainNoise(12u, 34u, 5u) == static_cast<float>(bits[0] >> 8u) * (1.0f / 16777216.0f));
        assert(grainNoise(12u, 34u, 5u) != grainNoise(12u, 34u, 6u));

        double sum = 0.0;
        for (std::uint32_t y = 0; y < 100; ++y) {
            for (std::uint32_t x = 0; x < 100; ++x) {
                const float noise = grainNoise(x, y, 7u);
                assert(noise >= 0.0f && noise < 1.0f);
                sum += noise;
            }
        }
        assert(std::abs(sum / 10000.0 - 0.5) < 0.01);
    }

    // Grain is strongest in the mid-tones and absent at black and at the peak, as a film's is; it never leaves 0..peak.
    void testFilmGrain() {
        const Vec3 mid{0.5f, 0.5f, 0.5f};
        assert(nearVec(filmGrain(mid, 1.0f, 0.0f, 1.0f), mid, 0.0f));
        assert(nearVec(filmGrain(mid, 0.5f, 0.3f, 1.0f), mid, 1e-7f));
        assert(nearVec(filmGrain(mid, 1.0f, 0.1f, 1.0f), Vec3{0.55f, 0.55f, 0.55f}, 1e-6f));
        assert(nearVec(filmGrain(Vec3{0.0f, 0.0f, 0.0f}, 1.0f, 0.5f, 1.0f), Vec3{0.0f, 0.0f, 0.0f}, 0.0f));
        assert(nearVec(filmGrain(Vec3{4.0f, 4.0f, 4.0f}, 0.0f, 0.5f, 4.0f), Vec3{4.0f, 4.0f, 4.0f}, 1e-6f));
        assert(nearVec(filmGrain(Vec3{2.0f, 2.0f, 2.0f}, 1.0f, 0.1f, 4.0f), Vec3{2.2f, 2.2f, 2.2f}, 1e-5f));
        const Vec3 dark = filmGrain(Vec3{0.01f, 0.0f, 0.0f}, 0.0f, 1.0f, 1.0f);
        assert(dark.x >= 0.0f && dark.y >= 0.0f && dark.z >= 0.0f);
    }

    // The tone mapping pass's record carries the camera: neutral and off by default, the matrix's rows and the lens.
    void testPacking() {
        const ToneMapUniformData plain = packToneMapping(ToneMappingSettings{});
        assert(plain.lens[3] == 0.0f && plain.lens[1] == 0.0f && plain.lens[2] == 0.0f && plain.view[2] == 0.0f);

        CameraSettings camera;
        camera.whiteBalance = 3000.0f;
        camera.contrast = 1.2f;
        camera.saturation = 0.9f;
        camera.lift = 0.01f;
        camera.gamma = 1.1f;
        camera.gain = 1.3f;
        camera.vignetting = 0.5f;
        camera.chromaticAberration = 2.0f;
        camera.filmGrain = 0.05f;
        const CameraView view{1.0f, 16.0f / 9.0f, 70000u};
        const ToneMapUniformData data = packToneMapping(ToneMappingSettings{}, DisplayOutput{}, BloomComposite{}, camera, view);
        const Mat4 balance = whiteBalanceMatrix(3000.0f, 0.0f);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                assert(data.whiteBalance[row * 4 + column] == balance.values[row][column]);
            }
        }
        assert(data.grade[0] == 1.2f && data.grade[1] == 0.9f && data.grade[2] == 0.01f && data.grade[3] == 1.1f);
        assert(data.lens[0] == 1.3f && data.lens[1] == 0.5f && data.lens[2] == 2.0f && data.lens[3] == 1.0f);
        assert(near(data.view[1], std::tan(0.5f), 1e-6f) && near(data.view[0], std::tan(0.5f) * 16.0f / 9.0f, 1e-6f));
        assert(data.view[2] == 0.05f && data.view[3] == static_cast<float>(70000u % cameraFrameCycle));

        camera.filmGrain = -1.0f;
        bool threw = false;
        try {
            packToneMapping(ToneMappingSettings{}, DisplayOutput{}, BloomComposite{}, camera, view);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }
}

void testCameraFinish() {
    testDefaultsLeaveThePictureAlone();
    testPlanckianLocus();
    testTint();
    testWhiteBalance();
    testGrade();
    testVignetting();
    testAberrationPlace();
    testGrainNoise();
    testFilmGrain();
    testPacking();
}
