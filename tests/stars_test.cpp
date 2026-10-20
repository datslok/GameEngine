#include "assets/star_catalogue.h"
#include "math/angles.h"
#include "render/gpu/stars.h"
#include "scene/celestial.h"
#include "scene/stars.h"

#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace {
    float luminance(const Vec3& colour) {
        return 0.2126f * colour.x + 0.7152f * colour.y + 0.0722f * colour.z;
    }

    // Ballesteros's formula: the sun's colour index gives the sun's temperature, about 5780 K; bluer stars are hotter.
    void testTemperature() {
        assert(near(starTemperature(0.65f), 5778.0f, 30.0f));
        assert(starTemperature(-0.2f) > starTemperature(0.0f));
        assert(starTemperature(0.0f) > 9000.0f);
        assert(near(starTemperature(1.85f), 3300.0f, 200.0f));
    }

    /*
    * Every colour has a luminance of 1; near 6500 K a blackbody is about white (sRGB's white is 6504 K); cool stars are
    * orange (red over green over blue) and hot ones blue.
    */
    void testBlackbodyColour() {
        for (float temperature = 2500.0f; temperature <= 30000.0f; temperature += 1500.0f) {
            const Vec3 colour = blackbodyColour(temperature);
            assert(near(luminance(colour), 1.0f, 1e-3f));
            assert(colour.x >= 0.0f && colour.y >= 0.0f && colour.z >= 0.0f);
        }

        const Vec3 white = blackbodyColour(6500.0f);
        assert(near(white.x, 1.0f, 0.1f) && near(white.y, 1.0f, 0.1f) && near(white.z, 1.0f, 0.1f));

        const Vec3 betelgeuse = blackbodyColour(starTemperature(1.85f));
        assert(betelgeuse.x > betelgeuse.y && betelgeuse.y > betelgeuse.z);

        const Vec3 rigel = blackbodyColour(starTemperature(-0.03f));
        assert(rigel.z > rigel.x);
    }

    // A catalogue star becomes its direction, its magnitude and its colour.
    void testMakeStar() {
        const CatalogueStar vega{radiansFromDegrees(279.2346f), radiansFromDegrees(38.7836f), 0.03f, 0.0f};
        const Star star = makeStar(vega);
        const Vec3 expected = equatorialDirection(vega.rightAscension, vega.declination);

        assert(near(star.direction.x, expected.x, 1e-6f) && near(star.direction.y, expected.y, 1e-6f) && near(star.direction.z, expected.z, 1e-6f));
        assert(star.magnitude == 0.03f);
        const Vec3 colour = blackbodyColour(starTemperature(0.0f));
        assert(star.colour.x == colour.x && star.colour.z == colour.z);
    }

    // Comment lines are skipped, angles turn into radians, and a missing colour index becomes the sun's.
    void testParseCatalogue() {
        std::istringstream text{"# a comment\n# another\n2491,101.2871,-16.7161,-1.46,0.00\n9999,90.0,0.0,6.20,\n"};
        const std::vector<CatalogueStar> stars = parseStarCatalogue(text);

        assert(stars.size() == 2);
        assert(near(stars[0].rightAscension, radiansFromDegrees(101.2871f), 1e-6f));
        assert(near(stars[0].declination, radiansFromDegrees(-16.7161f), 1e-6f));
        assert(stars[0].magnitude == -1.46f && stars[0].colourIndex == 0.0f);
        assert(stars[1].magnitude == 6.2f && stars[1].colourIndex == unknownColourIndex);

        std::istringstream broken{"2491,101.2871\n"};
        bool threw = false;
        try {
            parseStarCatalogue(broken);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
    }

    // The real catalogue: every star there, Sirius the brightest.
    void testTheCatalogue() {
        const std::vector<CatalogueStar> stars = loadStarCatalogue(brightStarCataloguePath);
        assert(stars.size() == 9096);

        float brightest = 10.0f;
        for (const CatalogueStar& star : stars) {
            brightest = std::min(brightest, star.magnitude);
        }
        assert(brightest == -1.46f);
    }

    // Magnitude 0 gives the brightness; 5 magnitudes fainter a hundredth of it, or a tenth with the spread halved.
    void testStarLight() {
        assert(near(starLight(0.0f, 2.0f, 1.0f), 2.0f, 1e-6f));
        assert(near(starLight(5.0f, 2.0f, 1.0f), 0.02f, 1e-6f));
        assert(near(starLight(5.0f, 2.0f, 0.5f), 0.2f, 1e-6f));
        assert(near(starLight(-1.46f, 1.0f, 1.0f), 3.837f, 1e-3f));
    }

    // The screen position inverts the camera's ray for that position.
    void testScreenPosition() {
        const Vec3 forward = Vec3{0.2f, 0.1f, -1.0f}.normalized();
        const Vec3 right = forward.cross(Vec3{0.0f, 1.0f, 0.0f}).normalized() * 0.9f;
        const Vec3 up = right.cross(forward).normalized() * 0.6f;

        for (const float x : {-0.9f, -0.2f, 0.0f, 0.5f}) {
            for (const float y : {-0.7f, 0.0f, 0.3f, 0.95f}) {
                const Vec3 direction = cameraRayDirection(right, up, forward, x, y).normalized();
                const Vec2 position = starScreenPosition(right, up, forward, direction);
                assert(near(position.x, x, 1e-5f) && near(position.y, y, 1e-5f));
            }
        }
    }

    // Wherever a star falls inside a pixel, the four pixels round it share exactly all its light.
    void testSplatSumsToOne() {
        for (float x = 0.0f; x < 1.0f; x += 0.137f) {
            for (float y = 0.0f; y < 1.0f; y += 0.091f) {
                float sum = 0.0f;

                for (int pixelY = -2; pixelY <= 2; ++pixelY) {
                    for (int pixelX = -2; pixelX <= 2; ++pixelX) {
                        const Vec2 centre{static_cast<float>(pixelX) + 0.5f, static_cast<float>(pixelY) + 0.5f};
                        sum += starSplatWeight(Vec2{centre.x - x, centre.y - y});
                    }
                }

                assert(near(sum, 1.0f, 1e-5f));
            }
        }

        assert(starSplatWeight(Vec2{0.0f, 0.0f}) == 1.0f);
        assert(starSplatWeight(Vec2{1.0f, 0.2f}) == 0.0f);
        assert(starSplatWeight(Vec2{-0.5f, 0.5f}) == 0.25f);
    }

    /*
    * Every pixel's solid angle summed over the screen is the view's: a rectangular pyramid with half-angles a and b
    * covers 4 asin(sin a sin b) steradians (a cube's face, 45 degrees each way, a sixth of the sphere).
    */
    void testPixelSolidAngle() {
        const Vec3 forward{0.0f, 0.0f, -1.0f};
        const Vec3 right = Vec3{1.0f, 0.0f, 0.0f} * std::tan(0.7f);
        const Vec3 up = Vec3{0.0f, 1.0f, 0.0f} * std::tan(0.5f);
        const int width = 160;
        const int height = 120;
        double sum = 0.0;

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const float screenX = (static_cast<float>(x) + 0.5f) / static_cast<float>(width) * 2.0f - 1.0f;
                const float screenY = (static_cast<float>(y) + 0.5f) / static_cast<float>(height) * 2.0f - 1.0f;
                const Vec3 direction = cameraRayDirection(right, up, forward, screenX, screenY).normalized();
                sum += pixelSolidAngle(right, up, static_cast<float>(width), static_cast<float>(height), direction.dot(forward));
            }
        }

        const double expected = 4.0 * std::asin(std::sin(0.7) * std::sin(0.5));
        assert(std::abs(sum - expected) < 1e-3 * expected);
        assert(near(pixelSolidAngle(right, up, 160.0f, 120.0f, 1.0f), (2.0f * std::tan(0.7f) / 160.0f) * (2.0f * std::tan(0.5f) / 120.0f), 1e-9f));
    }

    // What the star pass reads: each star's direction, magnitude and colour; the camera, and the sky's rotation with the
    // brightness and contrast.
    void testPacking() {
        const std::vector<Star> stars{Star{Vec3{0.0f, 0.6f, 0.8f}, 1.5f, Vec3{0.9f, 1.0f, 1.2f}}};
        const std::vector<GpuStar> packed = packStars(stars);
        assert(packed.size() == 1);
        assert(packed[0].direction[1] == 0.6f && packed[0].magnitude == 1.5f && packed[0].colour[2] == 1.2f);

        StarrySky sky;
        sky.toWorld = equatorialToWorld(radiansFromDegrees(45.0f), 1.0);
        sky.brightness = 0.25f;
        sky.contrast = 0.7f;
        GpuRayDebugView camera{};
        camera.screen[0] = 1920.0f;
        const GpuStarView view = packStarView(camera, sky);

        assert(view.camera.screen[0] == 1920.0f);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                assert(view.toWorld[row][column] == sky.toWorld.values[row][column]);
            }
        }
        assert(view.toWorld[0][3] == 0.25f && view.toWorld[1][3] == 0.7f);
    }
}

void testStars() {
    testTemperature();
    testBlackbodyColour();
    testMakeStar();
    testParseCatalogue();
    testTheCatalogue();
    testStarLight();
    testScreenPosition();
    testSplatSumsToOne();
    testPixelSolidAngle();
    testPacking();
}
