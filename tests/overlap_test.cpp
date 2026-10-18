#include "physics/overlap.h"

#include <cassert>

namespace {
    // Checks a pair in both orders and through the Shape variant, so every entry point agrees.
    template <typename First, typename Second>
    void assertOverlap(const First& first, const Second& second, bool expected) {
        assert(overlaps(first, second) == expected);
        assert(overlaps(second, first) == expected);
        assert(overlaps(Shape{first}, Shape{second}) == expected);
        assert(overlaps(Shape{second}, Shape{first}) == expected);
    }
}

void testOverlap() {
    const Sphere unitSphere{Vec3{0.0f, 0.0f, 0.0f}, 1.0f};
    const Aabb unitBox{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}};
    // A standing character: 2 tall in all, feet at y = 0.
    const Capsule character{Vec3{5.0f, 0.5f, 0.0f}, Vec3{5.0f, 1.5f, 0.0f}, 0.5f};

    // Sphere and sphere: overlapping, exactly touching (counts), apart.
    assertOverlap(unitSphere, Sphere{Vec3{1.5f, 0.0f, 0.0f}, 1.0f}, true);
    assertOverlap(unitSphere, Sphere{Vec3{0.0f, 2.0f, 0.0f}, 1.0f}, true);
    assertOverlap(unitSphere, Sphere{Vec3{0.0f, 2.01f, 0.0f}, 1.0f}, false);

    // Sphere and capsule: beside its middle, above its top end, and just out of reach diagonally.
    assertOverlap(Sphere{Vec3{5.8f, 1.0f, 0.0f}, 0.4f}, character, true);
    assertOverlap(Sphere{Vec3{5.0f, 2.3f, 0.0f}, 0.4f}, character, true);
    assertOverlap(Sphere{Vec3{5.7f, 2.2f, 0.0f}, 0.4f}, character, false);

    // Sphere and box: off a face, off a corner (where a test against the sphere's own box would wrongly say yes), inside.
    assertOverlap(Sphere{Vec3{1.9f, 0.0f, 0.0f}, 1.0f}, unitBox, true);
    assertOverlap(Sphere{Vec3{1.7f, 1.7f, 1.7f}, 1.0f}, unitBox, false);
    assertOverlap(Sphere{Vec3{0.0f, 0.0f, 0.0f}, 0.1f}, unitBox, true);

    // Capsule and capsule: two characters side by side, then crossed rods.
    assertOverlap(character, Capsule{Vec3{5.9f, 0.5f, 0.0f}, Vec3{5.9f, 1.5f, 0.0f}, 0.5f}, true);
    assertOverlap(character, Capsule{Vec3{6.1f, 0.5f, 0.0f}, Vec3{6.1f, 1.5f, 0.0f}, 0.5f}, false);
    assertOverlap(
        Capsule{Vec3{-1.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 0.2f},
        Capsule{Vec3{0.0f, 0.3f, -1.0f}, Vec3{0.0f, 0.3f, 1.0f}, 0.2f}, true);
    assertOverlap(
        Capsule{Vec3{-1.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 0.2f},
        Capsule{Vec3{0.0f, 0.5f, -1.0f}, Vec3{0.0f, 0.5f, 1.0f}, 0.2f}, false);

    // A capsule whose ends meet is a sphere.
    assertOverlap(Capsule{Vec3{0.0f, 2.0f, 0.0f}, Vec3{0.0f, 2.0f, 0.0f}, 1.0f}, unitSphere, true);

    // Capsule and box: standing on it, lying beside an edge, passing right through, then clear of a corner and just reaching it
    // (the segment passes the corner at a distance of about 0.707).
    const Aabb floor{Vec3{3.0f, -1.0f, -2.0f}, Vec3{7.0f, 0.0f, 2.0f}};
    assertOverlap(character, floor, true);
    assertOverlap(Capsule{Vec3{-3.0f, 1.3f, 0.0f}, Vec3{3.0f, 1.3f, 0.0f}, 0.4f}, unitBox, true);
    assertOverlap(Capsule{Vec3{-3.0f, 0.0f, 0.0f}, Vec3{3.0f, 0.0f, 0.0f}, 0.1f}, unitBox, true);
    assertOverlap(Capsule{Vec3{4.0f, -1.0f, 0.0f}, Vec3{-1.0f, 4.0f, 0.0f}, 0.5f}, unitBox, false);
    assertOverlap(Capsule{Vec3{4.0f, -1.0f, 0.0f}, Vec3{-1.0f, 4.0f, 0.0f}, 0.75f}, unitBox, true);

    // Box and box: overlapping, sharing a face (counts), apart on one axis only.
    assertOverlap(unitBox, Aabb{Vec3{0.5f, 0.5f, 0.5f}, Vec3{2.0f, 2.0f, 2.0f}}, true);
    assertOverlap(unitBox, Aabb{Vec3{1.0f, -1.0f, -1.0f}, Vec3{2.0f, 1.0f, 1.0f}}, true);
    assertOverlap(unitBox, Aabb{Vec3{-1.0f, -1.0f, 1.5f}, Vec3{1.0f, 1.0f, 2.0f}}, false);
}
