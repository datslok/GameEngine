#include "shading.h"

#include <cassert>

namespace {
    void assertColour(Pixel actual, Pixel expected) {
        assert(actual.r == expected.r);
        assert(actual.g == expected.g);
        assert(actual.b == expected.b);
    }
}

void testShading(){
    const Pixel baseColour{200, 100, 50};
    const Vec3 normal{0.0f, 0.0f, 1.0f};

    // Direct illumination preserves the base colour.
    {
        const Pixel result = shadeFlat(
            baseColour, normal, Vec3{0.0f, 0.0f, 1.0f}, 0.2f
        );

        assertColour(result, baseColour);
    }

    // Perpendicular light gives only ambient illumination.
    {
        const Pixel result = shadeFlat(
            baseColour, normal, Vec3{1.0f, 0.0f, 0.0f}, 0.2f
        );

        assertColour(result, Pixel{40, 20, 10});
    }

    // Light behind the surface also gives only ambient.
    {
        const Pixel result = shadeFlat(
            baseColour, normal, Vec3{0.0f, 0.0f, -1.0f}, 0.2f
        );

        assertColour(result, Pixel{40, 20, 10});
    }

    // Vector lengths must not affect brightness.
    {
        const Pixel result = shadeFlat(
            baseColour,
            Vec3{0.0f, 0.0f, 5.0f},
            Vec3{0.0f, 0.0f, 3.0f},
            0.2f
        );

        assertColour(result, baseColour);
    }

    // A 3-4-5 direction has a dot product of 3/5 with normal.
    // Brightness = 0.2 + 0.8 * 0.6 = 0.68.
    {
        const Pixel result = shadeFlat(
            baseColour, normal, Vec3{4.0f, 0.0f, 3.0f}, 0.2f
        );

        assertColour(result, Pixel{136, 68, 34});
    }

    // Without ambient light, an unlit surface is black.
    {
        const Pixel result = shadeFlat(
            baseColour, normal, Vec3{1.0f, 0.0f, 0.0f}, 0.0f
        );

        assertColour(result, Pixel{0, 0, 0});
    }

    // Full ambient illumination preserves the base colour.
    {
        const Pixel result = shadeFlat(
            baseColour, normal, Vec3{0.0f, 0.0f, -1.0f}, 1.0f
        );

        assertColour(result, baseColour);
    }

    // Ambient values outside [0, 1] are clamped.
    {
        const Vec3 behind{0.0f, 0.0f, -1.0f};

        assertColour(
            shadeFlat(baseColour, normal, behind, -0.5f),
            Pixel{0, 0, 0}
        );

        assertColour(
            shadeFlat(baseColour, normal, behind, 1.5f),
            baseColour
        );
    }
}