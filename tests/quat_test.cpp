#include "math/quat.h"
#include "math/mat4.h"
#include "math/vec4.h"

#include <cassert>
#include <cmath>
#include <numbers>

namespace {
    constexpr float pi = std::numbers::pi_v<float>;
    constexpr float tolerance = 0.0001f;

    const Vec3 xAxis{1.0f, 0.0f, 0.0f};
    const Vec3 yAxis{0.0f, 1.0f, 0.0f};
    const Vec3 zAxis{0.0f, 0.0f, 1.0f};

    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < tolerance;
    }

    bool nearlyEqual(const Vec3& actual, const Vec3& expected) {
        return nearlyEqual(actual.x, expected.x) && nearlyEqual(actual.y, expected.y) && nearlyEqual(actual.z, expected.z);
    }

    bool sameRotation(const Quat& actual, const Quat& expected) {
        return angleBetween(actual, expected) < tolerance;
    }

    // The upper 3x3 parts agree (the rest of a rotation matrix is the identity's).
    bool nearlyEqual(const Mat4& actual, const Mat4& expected) {
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                if (!nearlyEqual(actual.values[row][column], expected.values[row][column])) {
                    return false;
                }
            }
        }
        return true;
    }

    Vec3 rotatedByMatrix(const Mat4& matrix, const Vec3& vector) {
        const Vec4 result = matrix * Vec4{vector.x, vector.y, vector.z, 0.0f};
        return Vec3{result.x, result.y, result.z};
    }

    // A few orientations with no special symmetry, so the tests are not fooled by a lucky axis.
    const Quat someTurn = Quat::fromAxisAngle(Vec3{1.0f, 2.0f, -2.0f}.normalized(), 0.8f);
    const Quat otherTurn = Quat::fromAxisAngle(Vec3{-3.0f, 0.0f, 4.0f}.normalized(), 2.3f);

    void testIdentity() {
        // The default quaternion is the identity: scalar part 1, vector part 0, and it leaves every vector alone.
        const Quat identity;
        assert(identity.w == 1.0f && identity.x == 0.0f && identity.y == 0.0f && identity.z == 0.0f);
        assert(nearlyEqual(identity.rotate(Vec3{2.0f, -3.0f, 4.0f}), Vec3{2.0f, -3.0f, 4.0f}));
        assert(nearlyEqual(identity.toMatrix(), Mat4::identity()));

        // A turn by the identity, in either order, is the turn itself.
        assert(sameRotation(identity * someTurn, someTurn));
        assert(sameRotation(someTurn * identity, someTurn));
    }

    void testAxisAngle() {
        // The half angle: a turn by theta stores cos(theta / 2) and sin(theta / 2) times the axis.
        const Quat quarterAboutZ = Quat::fromAxisAngle(zAxis, pi / 2.0f);
        assert(nearlyEqual(quarterAboutZ.w, std::cos(pi / 4.0f)));
        assert(nearlyEqual(quarterAboutZ.z, std::sin(pi / 4.0f)));
        assert(nearlyEqual(quarterAboutZ.x, 0.0f) && nearlyEqual(quarterAboutZ.y, 0.0f));

        // Turns about each axis match the matrices the engine already trusts, as rotations of vectors and as matrices.
        const Vec3 point{1.0f, 2.0f, 3.0f};
        for (const float angle : {pi / 2.0f, 0.3f, -2.0f, pi}) {
            assert(nearlyEqual(Quat::fromAxisAngle(xAxis, angle).rotate(point), rotatedByMatrix(Mat4::rotationX(angle), point)));
            assert(nearlyEqual(Quat::fromAxisAngle(yAxis, angle).rotate(point), rotatedByMatrix(Mat4::rotationY(angle), point)));
            assert(nearlyEqual(Quat::fromAxisAngle(zAxis, angle).rotate(point), rotatedByMatrix(Mat4::rotationZ(angle), point)));

            assert(nearlyEqual(Quat::fromAxisAngle(xAxis, angle).toMatrix(), Mat4::rotationX(angle)));
            assert(nearlyEqual(Quat::fromAxisAngle(yAxis, angle).toMatrix(), Mat4::rotationY(angle)));
            assert(nearlyEqual(Quat::fromAxisAngle(zAxis, angle).toMatrix(), Mat4::rotationZ(angle)));
        }

        // The axis itself does not move, and a rotation keeps every length.
        const Vec3 axis = Vec3{1.0f, 2.0f, -2.0f}.normalized();
        assert(nearlyEqual(someTurn.rotate(axis), axis));
        assert(nearlyEqual(someTurn.rotate(point).length(), point.length()));
        assert(nearlyEqual(someTurn.rotate(point), rotatedByMatrix(someTurn.toMatrix(), point)));
    }

    void testComposition() {
        const Vec3 point{1.0f, 2.0f, 3.0f};

        // a * b turns by b first, then a: the same order as matrices multiplying column vectors.
        assert(nearlyEqual((someTurn * otherTurn).rotate(point), someTurn.rotate(otherTurn.rotate(point))));
        assert(nearlyEqual((someTurn * otherTurn).toMatrix(), someTurn.toMatrix() * otherTurn.toMatrix()));

        // Turns do not commute: a quarter turn about x then about y is not the same as y then x.
        const Quat aboutX = Quat::fromAxisAngle(xAxis, pi / 2.0f);
        const Quat aboutY = Quat::fromAxisAngle(yAxis, pi / 2.0f);
        assert(!sameRotation(aboutX * aboutY, aboutY * aboutX));

        // Two quarter turns about one axis make a half turn.
        assert(sameRotation(aboutX * aboutX, Quat::fromAxisAngle(xAxis, pi)));

        // The conjugate undoes a turn.
        assert(sameRotation(someTurn * someTurn.conjugate(), Quat{}));
        assert(nearlyEqual(someTurn.conjugate().rotate(someTurn.rotate(point)), point));
    }

    void testDoubleCover() {
        // q and -q are the same rotation, so they give the same matrix and the angle between them is zero.
        const Quat negated = -someTurn;
        assert(nearlyEqual(negated.toMatrix(), someTurn.toMatrix()));
        assert(nearlyEqual(negated.rotate(Vec3{1.0f, 2.0f, 3.0f}), someTurn.rotate(Vec3{1.0f, 2.0f, 3.0f})));
        assert(angleBetween(negated, someTurn) < tolerance);

        // A full turn about any axis stores -1, the same rotation as the identity.
        const Quat fullTurn = Quat::fromAxisAngle(yAxis, 2.0f * pi);
        assert(nearlyEqual(fullTurn.w, -1.0f));
        assert(sameRotation(fullTurn, Quat{}));

        // The angle between two orientations is the smallest turn from one to the other, at most pi.
        assert(nearlyEqual(angleBetween(Quat{}, Quat::fromAxisAngle(zAxis, 0.7f)), 0.7f));
        assert(nearlyEqual(angleBetween(Quat{}, Quat::fromAxisAngle(zAxis, 2.0f * pi - 0.7f)), 0.7f));
        assert(nearlyEqual(angleBetween(someTurn, someTurn), 0.0f));
    }

    void testEulerAngles() {
        // The engine's old order: x first, then y, then z, as Transform's matrix used to be built.
        for (const Vec3 angles : {Vec3{pi / 2.0f, pi / 2.0f, pi / 2.0f}, Vec3{0.3f, -1.2f, 2.5f}, Vec3{0.0f, 0.5f, 0.0f}}) {
            const Mat4 expected = Mat4::rotationZ(angles.z) * Mat4::rotationY(angles.y) * Mat4::rotationX(angles.x);
            assert(nearlyEqual(Quat::fromEulerAngles(angles).toMatrix(), expected));
        }

        // A single angle is a turn about that axis.
        assert(sameRotation(Quat::fromEulerAngles(Vec3{0.0f, 0.5f, 0.0f}), Quat::fromAxisAngle(yAxis, 0.5f)));
    }

    void testNormalize() {
        // Rounding slowly stretches a quaternion away from unit length; normalizing brings it back without turning it.
        const Quat stretched = someTurn * 1.01f;
        const Quat repaired = stretched.normalized();
        assert(nearlyEqual(repaired.length(), 1.0f));
        assert(sameRotation(repaired, someTurn));
    }

    void testSlerp() {
        // The ends are the two orientations.
        assert(sameRotation(slerp(someTurn, otherTurn, 0.0f), someTurn));
        assert(sameRotation(slerp(someTurn, otherTurn, 1.0f), otherTurn));

        // Between two turns about one axis, it turns about that axis by the blended angle.
        const Quat from = Quat::fromAxisAngle(yAxis, 0.2f);
        const Quat to = Quat::fromAxisAngle(yAxis, 1.8f);
        assert(sameRotation(slerp(from, to, 0.5f), Quat::fromAxisAngle(yAxis, 1.0f)));
        assert(sameRotation(slerp(from, to, 0.25f), Quat::fromAxisAngle(yAxis, 0.6f)));

        // Constant speed along the great circle: after t of the way it has turned exactly t of the angle, and every
        // in-between orientation is a unit quaternion.
        const float totalAngle = angleBetween(someTurn, otherTurn);
        for (const float t : {0.1f, 0.3f, 0.5f, 0.7f, 0.9f}) {
            const Quat between = slerp(someTurn, otherTurn, t);
            assert(nearlyEqual(between.length(), 1.0f));
            assert(nearlyEqual(angleBetween(someTurn, between), t * totalAngle));
            assert(nearlyEqual(angleBetween(between, otherTurn), (1.0f - t) * totalAngle));
        }

        // The short way round: just below a full turn is a small turn backwards, so halfway is about the identity, not a
        // half turn. Its quaternion has a negative scalar part, which slerp must flip first.
        const Quat almostFullTurn = Quat::fromAxisAngle(yAxis, 2.0f * pi - 0.1f);
        const Quat slightlyPast = Quat::fromAxisAngle(yAxis, 0.1f);
        assert(sameRotation(slerp(almostFullTurn, slightlyPast, 0.5f), Quat{}));
        assert(sameRotation(slerp(someTurn, -otherTurn, 0.5f), slerp(someTurn, otherTurn, 0.5f)));

        // Nearly equal orientations (one 120 Hz tick of a slow turn) blend without dividing by almost zero.
        const Quat nearby = Quat::fromAxisAngle(yAxis, 0.2f + 1.0e-5f);
        const Quat tiny = slerp(from, nearby, 0.5f);
        assert(std::isfinite(tiny.w) && nearlyEqual(tiny.length(), 1.0f));
        assert(sameRotation(tiny, from));
        assert(sameRotation(slerp(from, from, 0.5f), from));
    }

    void testRotationVector() {
        // The log map: a turn by theta about a unit axis becomes the vector theta times the axis.
        const Vec3 axis = Vec3{1.0f, 2.0f, -2.0f}.normalized();
        for (const float angle : {0.8f, 0.01f, 2.5f, pi - 0.001f}) {
            assert(nearlyEqual(Quat::fromAxisAngle(axis, angle).toRotationVector(), axis * angle));
        }
        assert(nearlyEqual(Quat{}.toRotationVector(), Vec3{}));

        // Tiny turns keep their precision (a joint drive's error near its target).
        const Vec3 tinyTurn = Quat::fromAxisAngle(axis, 1.0e-6f).toRotationVector();
        assert(std::abs(tinyTurn.length() - 1.0e-6f) < 1.0e-9f);

        // Always the shorter way: -q, and a turn just short of a full one, give the small turn.
        assert(nearlyEqual((-someTurn).toRotationVector(), someTurn.toRotationVector()));
        assert(nearlyEqual(Quat::fromAxisAngle(yAxis, 2.0f * pi - 0.1f).toRotationVector(), yAxis * -0.1f));

        // The exp map undoes the log map, both ways.
        assert(sameRotation(Quat::fromRotationVector(someTurn.toRotationVector()), someTurn));
        assert(nearlyEqual(Quat::fromRotationVector(Vec3{0.3f, -0.2f, 0.5f}).toRotationVector(), Vec3{0.3f, -0.2f, 0.5f}));
        assert(sameRotation(Quat::fromRotationVector(Vec3{}), Quat{}));

        // The error between two orientations as a vector: what is left to turn, and about which axis, to get from current to
        // target (in world axes).
        const Quat current = Quat::fromAxisAngle(zAxis, 0.25f);
        const Quat target = Quat::fromAxisAngle(zAxis, 1.0f);
        assert(nearlyEqual((target * current.conjugate()).toRotationVector(), zAxis * 0.75f));
        assert(sameRotation(Quat::fromRotationVector((otherTurn * someTurn.conjugate()).toRotationVector()) * someTurn, otherTurn));
    }
}

void testQuat() {
    testIdentity();
    testAxisAngle();
    testComposition();
    testDoubleCover();
    testEulerAngles();
    testNormalize();
    testSlerp();
    testRotationVector();
}
