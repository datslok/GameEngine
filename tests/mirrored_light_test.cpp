#include "math/mat4.h"
#include "render/gpu/brdf.h"
#include "render/gpu/light_falloff.h"
#include "render/gpu/mirrored_light.h"

#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
    bool nearVec(const Vec3& a, const Vec3& b, float tolerance) {
        return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
    }

    Vec3 unit(const Vec3& v) {
        return v * (1.0f / v.length());
    }

    // The mirror in the tests: a rectangle in the plane z = 0 facing +z, halfWidth across x and halfHeight up y round
    // the origin, as a ray from the receiver meets it.
    MirrorProjection flatMirror(const Vec3& receiver, float halfWidth, float halfHeight) {
        return [=](const Vec3& towards) -> std::optional<MirrorSurface> {
            const Vec3 direction = towards - receiver;

            if (receiver.z <= 0.0f || direction.z >= 0.0f) {
                return std::nullopt;
            }

            const Vec3 hit = receiver + direction * (-receiver.z / direction.z);

            if (std::abs(hit.x) > halfWidth || std::abs(hit.y) > halfHeight) {
                return std::nullopt;
            }

            return MirrorSurface{Vec3{hit.x, hit.y, 0.0f}, tangentFrame(Vec3{0.0f, 0.0f, 1.0f}), TangentMatrix{}};
        };
    }

    // The first point a ray from origin along a unit direction meets a sphere from outside, or none.
    std::optional<float> sphereEntry(const Vec3& origin, const Vec3& direction, const Vec3& centre, float radius) {
        const Vec3 offset = origin - centre;
        const float along = offset.dot(direction);
        const float discriminant = along * along - (offset.dot(offset) - radius * radius);

        if (discriminant < 0.0f) {
            return std::nullopt;
        }

        const float t = -along - std::sqrt(discriminant);
        return t > 0.0f ? std::optional<float>{t} : std::nullopt;
    }

    // A chrome ball: its outside, curving away from its normal by 1 / radius every way.
    MirrorProjection convexBall(const Vec3& receiver, const Vec3& centre, float radius) {
        return [=](const Vec3& towards) -> std::optional<MirrorSurface> {
            const Vec3 direction = unit(towards - receiver);
            const std::optional<float> t = sphereEntry(receiver, direction, centre, radius);

            if (!t) {
                return std::nullopt;
            }

            const Vec3 hit = receiver + direction * *t;
            const float curvature = 1.0f / radius;
            return MirrorSurface{hit, tangentFrame(unit(hit - centre)), TangentMatrix{curvature, 0.0f, 0.0f, curvature}};
        };
    }

    // The angle of incidence equals the angle of reflection, in one plane: the way to the receiver is the way to the
    // light reflected in the normal.
    bool obeysReflection(const Vec3& receiver, const Vec3& light, const MirrorSurface& surface, float tolerance) {
        const Vec3 toReceiver = unit(receiver - surface.position);
        const Vec3 toLight = unit(light - surface.position);
        const Vec3 normal = surface.frame.normal;
        const Vec3 reflected = normal * (2.0f * toLight.dot(normal)) - toLight;
        return nearVec(reflected, toReceiver, tolerance);
    }

    void testSettings() {
        const MirroredLightSettings defaults;
        assert(!defaults.enabled);
        assert(defaults.mirrorRoughness == 0.1f);
        validateMirroredLights(defaults);

        for (float roughness : {-0.1f, 1.5f, std::numeric_limits<float>::quiet_NaN()}) {
            MirroredLightSettings wrong;
            wrong.mirrorRoughness = roughness;
            bool threw = false;

            try {
                validateMirroredLights(wrong);
            } catch (const std::invalid_argument&) {
                threw = true;
            }

            assert(threw);
        }
    }

    // Unit, at right angles and right-handed (tangent x bitangent = normal) for any normal, the axes included.
    void testTangentFrame() {
        const Vec3 normals[] = {{0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f},
                                unit(Vec3{0.3f, -0.8f, 0.1f}), unit(Vec3{1e-4f, 1e-4f, -1.0f}), unit(Vec3{-0.5f, 0.5f, 0.7f})};

        for (const Vec3& normal : normals) {
            const TangentFrame frame = tangentFrame(normal);
            assert(nearVec(frame.normal, normal, 1e-6f));
            assert(near(frame.tangent.length(), 1.0f, 1e-5f));
            assert(near(frame.bitangent.length(), 1.0f, 1e-5f));
            assert(near(frame.tangent.dot(normal), 0.0f, 1e-5f));
            assert(near(frame.bitangent.dot(normal), 0.0f, 1e-5f));
            assert(near(frame.tangent.dot(frame.bitangent), 0.0f, 1e-5f));
            assert(nearVec(frame.tangent.cross(frame.bitangent), normal, 1e-5f));
        }

        assert(tangentDeterminant(TangentMatrix{2.0f, 1.0f, 3.0f, 4.0f}) == 5.0f);
    }

    /*
    * A flat triangle does not curve. A small one on a ball of radius 2 with the ball's own normals at its corners curves
    * by 1 / 2 every way, outwards (positive) or, with its normals turned inwards (a bowl), negatively.
    */
    void testTriangleCurvature() {
        const TangentFrame flat = tangentFrame(Vec3{0.0f, 1.0f, 0.0f});
        const TangentMatrix none = triangleCurvature(Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec3{}, Vec3{}, 1.0f, flat);
        assert(none.xx == 0.0f && none.xy == 0.0f && none.yx == 0.0f && none.yy == 0.0f);
        assert(isFlatMirror(none));
        assert(!isFlatMirror(TangentMatrix{0.0f, 0.0f, 0.0f, 0.01f}));

        const float radius = 2.0f;
        const Vec3 corners[] = {unit(Vec3{0.0f, 1.0f, 0.0f}), unit(Vec3{0.05f, 1.0f, 0.0f}), unit(Vec3{0.0f, 1.0f, 0.05f})};

        for (float side : {1.0f, -1.0f}) {
            const Vec3 normals[] = {corners[0] * side, corners[1] * side, corners[2] * side};
            const Vec3 interpolated = (normals[0] + normals[1] + normals[2]) * (1.0f / 3.0f);
            const TangentFrame frame = tangentFrame(unit(interpolated));
            const TangentMatrix curvature = triangleCurvature((corners[1] - corners[0]) * radius, (corners[2] - corners[0]) * radius,
                                                              normals[1] - normals[0], normals[2] - normals[0], interpolated.length(), frame);
            assert(near(curvature.xx, side * 0.5f, 0.01f));
            assert(near(curvature.yy, side * 0.5f, 0.01f));
            assert(near(curvature.xy, 0.0f, 0.01f));
            assert(curvature.xy == curvature.yx);
        }
    }

    // The flat mirror step: where the line to the light's image crosses the plane, and none with either end behind it.
    void testPlaneMirrorPoint() {
        const Vec3 normal{0.0f, 0.0f, 1.0f};
        const std::optional<Vec3> point = planeMirrorPoint(Vec3{-1.0f, 0.0f, 1.0f}, Vec3{3.0f, 0.0f, 3.0f}, Vec3{5.0f, 5.0f, 0.0f}, normal);
        assert(point && nearVec(*point, Vec3{0.0f, 0.0f, 0.0f}, 1e-5f));
        assert(!planeMirrorPoint(Vec3{-1.0f, 0.0f, -1.0f}, Vec3{3.0f, 0.0f, 3.0f}, Vec3{}, normal));
        assert(!planeMirrorPoint(Vec3{-1.0f, 0.0f, 1.0f}, Vec3{3.0f, 0.0f, -3.0f}, Vec3{}, normal));
    }

    /*
    * A flat mirror: the search lands on the light's image point in its first (flat mirror) step and takes no Newton
    * step; the spread area is the unfolded path squared, so the mirrored light is exactly an image light: the light
    * moved behind the mirror, its falloff over the whole path.
    */
    void testFlatMirrorIsImageLight() {
        const Vec3 receiver{0.4f, -1.2f, 2.0f};
        const Vec3 light{-0.3f, 0.5f, 1.5f};
        const std::optional<MirrorPath> path = solveMirrorPath(receiver, light, Vec3{0.0f, 0.0f, 0.0f}, flatMirror(receiver, 2.0f, 2.0f));
        assert(path);
        assert(path->steps == 0);
        assert(near(path->surface.position.z, 0.0f, 1e-6f));
        assert(obeysReflection(receiver, light, path->surface, 1e-4f));
        assert(mirrorConstraint(receiver, light, path->surface).length() < 1e-5f);

        const Vec3 image{light.x, light.y, -light.z};
        const float unfolded = (receiver - image).length();
        assert(near((receiver - path->surface.position).length() + (light - path->surface.position).length(), unfolded, 1e-4f));
        assert(near(mirrorSpreadArea(receiver, light, path->surface), unfolded * unfolded, 1e-3f * unfolded * unfolded));

        const Vec3 intensity{20.0f, 18.0f, 15.0f};
        const Vec3 reflectance{0.9f, 0.9f, 0.9f};
        const float range = 12.0f;
        const float sourceRadius = 0.05f;
        const Vec3 mirrored = mirroredIrradiance(intensity, reflectance, mirrorSpreadArea(receiver, light, path->surface), unfolded, range, sourceRadius);
        const Vec3 imageLight = intensity * reflectance * distanceFalloff(unfolded, range, sourceRadius);
        assert(nearVec(mirrored, imageLight, 1e-3f * imageLight.x));
    }

    // No light where the point of reflection would be off the mirror's edge, or the receiver or light behind it.
    void testFlatMirrorMisses() {
        const MirrorProjection small = flatMirror(Vec3{3.0f, 0.0f, 1.0f}, 0.5f, 0.5f);
        assert(!solveMirrorPath(Vec3{3.0f, 0.0f, 1.0f}, Vec3{3.0f, 0.0f, 1.5f}, Vec3{}, small));

        const Vec3 behind{0.0f, 0.0f, -1.0f};
        assert(!solveMirrorPath(behind, Vec3{0.0f, 0.0f, 2.0f}, Vec3{}, flatMirror(behind, 2.0f, 2.0f)));

        const Vec3 receiver{0.5f, 0.0f, 1.0f};
        assert(!solveMirrorPath(receiver, Vec3{0.0f, 0.0f, -2.0f}, Vec3{}, flatMirror(receiver, 2.0f, 2.0f)));

        const MirrorProjection nothing = [](const Vec3&) { return std::optional<MirrorSurface>{}; };
        assert(!solveMirrorPath(receiver, Vec3{0.0f, 0.0f, 2.0f}, Vec3{}, nothing));

        // On a flat mirror the first step is exact, so a point of reflection off its edge (or an end behind it) is
        // known to be no light after two rays: the seed's and the step's, with no Newton steps creeping to the edge.
        const Vec3 aside{3.0f, 0.0f, 1.0f};
        int rays = 0;
        const MirrorProjection counted = [&, mirror = flatMirror(aside, 0.5f, 0.5f)](const Vec3& towards) {
            ++rays;
            return mirror(towards);
        };
        assert(!solveMirrorPath(aside, Vec3{3.0f, 0.0f, 1.5f}, Vec3{}, counted));
        assert(rays == 2);

        rays = 0;
        const MirrorProjection countedBehind = [&, mirror = flatMirror(receiver, 2.0f, 2.0f)](const Vec3& towards) {
            ++rays;
            return mirror(towards);
        };
        assert(!solveMirrorPath(receiver, Vec3{0.0f, 0.0f, -2.0f}, Vec3{}, countedBehind));
        assert(rays == 1);
    }

    /*
    * A chrome ball seen from aside: the search converges within its steps to a point obeying the law of reflection, and
    * there the Jacobian matches the constraint's change measured by moving the point a little over the ball.
    */
    void testBallReflection() {
        const Vec3 centre{0.0f, 1.0f, 0.0f};
        const float radius = 0.5f;
        const Vec3 receiver{2.0f, 0.2f, 1.5f};
        const Vec3 light{-1.0f, 2.0f, 2.5f};
        const std::optional<MirrorPath> path = solveMirrorPath(receiver, light, centre, convexBall(receiver, centre, radius));
        assert(path);
        assert(path->steps >= 1 && path->steps <= MIRROR_MAX_STEPS);
        assert(near((path->surface.position - centre).length(), radius, 1e-4f));
        assert(obeysReflection(receiver, light, path->surface, 2e-3f));

        const MirrorSurface& at = path->surface;
        const TangentMatrix jacobian = mirrorJacobian(receiver, light, at);
        const float step = 1e-3f;

        for (int axis = 0; axis < 2; ++axis) {
            const Vec3 along = axis == 0 ? at.frame.tangent : at.frame.bitangent;
            const Vec3 moved = centre + unit(at.position + along * step - centre) * radius;
            MirrorSurface shifted{moved, tangentFrame(unit(moved - centre)), at.curvature};
            // Keep the moved frame's tangents as close to the old ones as the tilt allows, so only the tilt counts.
            shifted.frame.tangent = unit(at.frame.tangent - shifted.frame.normal * at.frame.tangent.dot(shifted.frame.normal));
            shifted.frame.bitangent = shifted.frame.normal.cross(shifted.frame.tangent);

            const Vec2 change = (mirrorConstraint(receiver, light, shifted) - mirrorConstraint(receiver, light, at)) * (1.0f / step);
            const float expectedX = axis == 0 ? jacobian.xx : jacobian.xy;
            const float expectedY = axis == 0 ? jacobian.yx : jacobian.yy;
            const float scale = std::abs(jacobian.xx) + std::abs(jacobian.yy);
            assert(near(change.x, expectedX, 0.02f * scale));
            assert(near(change.y, expectedY, 0.02f * scale));
        }
    }

    // Head-on, the ball adds twice its curvature to the wavefront's (the mirror equation): the spread area is
    // (dx + dL + 2 dx dL / R)^2, far more than a flat mirror's (dx + dL)^2.
    void testBallSpreadHeadOn() {
        const Vec3 centre{0.0f, 0.0f, 0.0f};
        const float radius = 0.5f;
        const Vec3 receiver{0.0f, 0.0f, 3.0f};
        const Vec3 light{0.0f, 0.0f, 1.5f};
        const MirrorSurface surface{Vec3{0.0f, 0.0f, radius}, tangentFrame(Vec3{0.0f, 0.0f, 1.0f}), TangentMatrix{2.0f, 0.0f, 0.0f, 2.0f}};
        const float toReceiver = 2.5f;
        const float toLight = 1.0f;
        const float expected = std::pow(toReceiver + toLight + 2.0f * toReceiver * toLight / radius, 2.0f);
        assert(near(mirrorSpreadArea(receiver, light, surface), expected, 1e-3f * expected));
        assert(mirrorConstraint(receiver, light, surface).length() < 1e-6f);
        assert(near(mirrorIncidence(receiver, surface), 1.0f, 1e-6f));
    }

    /*
    * The spread area checked against the light itself: rays from the light spread evenly over the solid angle the ball
    * fills (a grid in the cosine and the turn round its axis), reflected off it, counted where they cross a small square
    * round the receiver facing the way the light arrives. The irradiance there is the intensity times the solid angle
    * per ray times the rays landing, over the square's area: it matches intensity / spread area.
    */
    void testBallSpreadAgainstRays() {
        const Vec3 centre{0.0f, 1.0f, 0.0f};
        const float radius = 0.5f;
        const Vec3 receiver{1.2f, 0.6f, 1.1f};
        const Vec3 light{-0.6f, 1.6f, 1.4f};
        const std::optional<MirrorPath> path = solveMirrorPath(receiver, light, centre, convexBall(receiver, centre, radius));
        assert(path);
        const float expected = 1.0f / mirrorSpreadArea(receiver, light, path->surface);

        const Vec3 arriving = unit(receiver - path->surface.position);
        const TangentFrame across = tangentFrame(arriving);
        const float halfSide = 0.06f;

        const Vec3 axis = unit(centre - light);
        const TangentFrame round = tangentFrame(axis);
        const float distance = (centre - light).length();
        const float cosEdge = std::sqrt(1.0f - (radius / distance) * (radius / distance));
        const float solidAngle = 2.0f * std::numbers::pi_v<float> * (1.0f - cosEdge);
        const int rings = 1500;
        const int turns = 1500;
        int landed = 0;

        for (int ring = 0; ring < rings; ++ring) {
            const float cosine = 1.0f - (1.0f - cosEdge) * (static_cast<float>(ring) + 0.5f) / static_cast<float>(rings);
            const float sine = std::sqrt(std::max(1.0f - cosine * cosine, 0.0f));

            for (int turn = 0; turn < turns; ++turn) {
                const float angle = 2.0f * std::numbers::pi_v<float> * (static_cast<float>(turn) + 0.5f) / static_cast<float>(turns);
                const Vec3 direction = axis * cosine + round.tangent * (sine * std::cos(angle)) + round.bitangent * (sine * std::sin(angle));
                const std::optional<float> t = sphereEntry(light, direction, centre, radius);

                if (!t) {
                    continue;
                }

                const Vec3 hit = light + direction * *t;
                const Vec3 normal = unit(hit - centre);
                const Vec3 reflected = direction - normal * (2.0f * direction.dot(normal));
                const float along = reflected.dot(arriving);

                if (along <= 0.0f) {
                    continue;
                }

                const Vec3 crossing = hit + reflected * ((receiver - hit).dot(arriving) / along);
                const Vec3 offset = crossing - receiver;

                if (std::abs(offset.dot(across.tangent)) <= halfSide && std::abs(offset.dot(across.bitangent)) <= halfSide) {
                    ++landed;
                }
            }
        }

        const float measured = static_cast<float>(landed) * solidAngle / static_cast<float>(rings * turns) / (4.0f * halfSide * halfSide);
        assert(landed > 1000);
        assert(near(measured, expected, 0.05f * expected));
    }

    // At a focus (spread area 0) the light stays finite: as bright as the source filling the sky, intensity / radius^2.
    void testFocusStaysFinite() {
        const Vec3 intensity{4.0f, 4.0f, 4.0f};
        const Vec3 reflectance{1.0f, 1.0f, 1.0f};
        const Vec3 focused = mirroredIrradiance(intensity, reflectance, 0.0f, 1.0f, 100.0f, 0.1f);
        const float window = rangeWindow(1.0f, 100.0f);
        assert(nearVec(focused, intensity * (window * window / 0.01f), 1e-2f));
        assert(nearVec(mirroredIrradiance(intensity, reflectance, 4.0f, 200.0f, 100.0f, 0.1f), Vec3{}, 0.0f));
    }

    void testPairMayReach() {
        const Vec3 centre{0.0f, 0.0f, 0.0f};
        assert(mirrorPairMayReach(Vec3{3.0f, 0.0f, 0.0f}, centre, 1.0f, Vec3{0.0f, 4.0f, 0.0f}, 6.0f));
        assert(!mirrorPairMayReach(Vec3{3.0f, 0.0f, 0.0f}, centre, 1.0f, Vec3{0.0f, 4.0f, 0.0f}, 4.9f));
    }

    DrawItem shinyDraw(const Vec3& centre, float radius, float roughness) {
        DrawItem draw;
        draw.material.metallic = 1.0f;
        draw.material.roughness = roughness;
        draw.bounds = BoundingSphere{centre, radius};
        return draw;
    }

    GpuSpotLight spotAt(const Vec3& position, const Vec3& direction, float range, float cosOuter) {
        GpuSpotLight spot;
        spot.positionRange[0] = position.x;
        spot.positionRange[1] = position.y;
        spot.positionRange[2] = position.z;
        spot.positionRange[3] = range;
        spot.directionCosOuter[0] = direction.x;
        spot.directionCosOuter[1] = direction.y;
        spot.directionCosOuter[2] = direction.z;
        spot.directionCosOuter[3] = cosOuter;
        return spot;
    }

    GpuPointLight pointAt(const Vec3& position, float range) {
        GpuPointLight point;
        point.positionRange[0] = position.x;
        point.positionRange[1] = position.y;
        point.positionRange[2] = position.z;
        point.positionRange[3] = range;
        return point;
    }

    /*
    * Only fast lights, only smooth shadow casters, only within a light's range and a spotlight's cone; nearest first.
    * The lights are measured from the camera, the draws in the world.
    */
    void testChoosePairs() {
        const Vec3 cameraOrigin{100.0f, 0.0f, 0.0f};
        GpuLights lights;
        lights.spots = {spotAt(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, 20.0f, 0.9f),  // the flashlight, fast
                        spotAt(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, 20.0f, 0.9f)}; // a still lamp
        lights.fastSpots = {true, false};
        lights.points = {pointAt(Vec3{0.0f, 5.0f, 0.0f}, 3.0f)};
        lights.fastPoints = {true};

        std::vector<DrawItem> draws{
            shinyDraw(Vec3{100.0f, 0.0f, -10.0f}, 1.0f, 0.0f),  // 0: a mirror ahead
            shinyDraw(Vec3{100.0f, 0.0f, -4.0f}, 1.0f, 0.05f),  // 1: a nearer chrome ball ahead
            shinyDraw(Vec3{100.0f, 0.0f, -6.0f}, 1.0f, 0.7f),   // 2: rough
            shinyDraw(Vec3{100.0f, 0.0f, 10.0f}, 1.0f, 0.0f),   // 3: behind the beam
            shinyDraw(Vec3{100.0f, 0.0f, -40.0f}, 1.0f, 0.0f),  // 4: beyond the range
            shinyDraw(Vec3{100.0f, 0.0f, -8.0f}, 1.0f, 0.0f),   // 5: no shadow
            shinyDraw(Vec3{100.0f, 6.0f, 0.0f}, 0.5f, 0.0f),    // 6: beside the point light
        };
        draws[5].castsShadows = false;

        const std::vector<MirrorPair> pairs = chooseMirrorPairs(lights, draws, cameraOrigin, MirroredLightSettings{});
        assert(pairs.size() == 3);
        assert(pairs[0].lightKind == MIRROR_POINT_LIGHT && pairs[0].slot == 0 && pairs[0].draw == 6);
        assert(pairs[1].lightKind == MIRROR_SPOT_LIGHT && pairs[1].slot == 0 && pairs[1].draw == 1);
        assert(pairs[2].lightKind == MIRROR_SPOT_LIGHT && pairs[2].slot == 0 && pairs[2].draw == 0);
        assert(nearVec(pairs[2].centre, Vec3{0.0f, 0.0f, -10.0f}, 1e-5f));
        assert(pairs[2].radius == 1.0f);

        MirroredLightSettings rougher;
        rougher.mirrorRoughness = 0.8f;
        assert(chooseMirrorPairs(lights, draws, cameraOrigin, rougher).size() == 4);

        // More than the pass holds: the nearest are kept.
        std::vector<DrawItem> many;

        for (int i = 0; i < MIRROR_MAX_PAIRS + 5; ++i) {
            many.push_back(shinyDraw(Vec3{100.0f, 0.0f, -2.0f - static_cast<float>(MIRROR_MAX_PAIRS + 5 - i) * 0.5f}, 0.2f, 0.0f));
        }

        const std::vector<MirrorPair> kept = chooseMirrorPairs(lights, many, cameraOrigin, MirroredLightSettings{});
        assert(kept.size() == MIRROR_MAX_PAIRS);
        assert(kept.front().draw == MIRROR_MAX_PAIRS + 4);
        assert(kept.back().draw == 5);
    }

    void testPackPass() {
        const std::vector<MirrorPair> pairs{MirrorPair{MIRROR_SPOT_LIGHT, 3, 7, Vec3{1.0f, 2.0f, 3.0f}, 0.5f}};
        const Mat4 inverse = Mat4::translation(1.0f, 2.0f, 3.0f);
        const GpuMirroredLightPass pass = packMirroredLightPass(inverse, RenderSize{200, 100}, pairs);
        assert(pass.counts[0] == 1);
        assert(pass.size[0] == 200.0f && pass.size[1] == 100.0f && pass.size[2] == 1.0f / 200.0f && pass.size[3] == 1.0f / 100.0f);
        assert(pass.inverseViewProjection[12] == 1.0f && pass.inverseViewProjection[13] == 2.0f); // column-major: translation last
        assert(pass.pairs[0].light[0] == MIRROR_SPOT_LIGHT && pass.pairs[0].light[1] == 3 && pass.pairs[0].light[2] == 7);
        assert(pass.pairs[0].bounds[0] == 1.0f && pass.pairs[0].bounds[3] == 0.5f);

        const std::vector<MirrorPair> tooMany(MIRROR_MAX_PAIRS + 1);
        bool threw = false;

        try {
            packMirroredLightPass(inverse, RenderSize{200, 100}, tooMany);
        } catch (const std::length_error&) {
            threw = true;
        }

        assert(threw);
    }
}

void testMirroredLight() {
    testSettings();
    testTangentFrame();
    testTriangleCurvature();
    testPlaneMirrorPoint();
    testFlatMirrorIsImageLight();
    testFlatMirrorMisses();
    testBallReflection();
    testBallSpreadHeadOn();
    testBallSpreadAgainstRays();
    testFocusStaysFinite();
    testPairMayReach();
    testChoosePairs();
    testPackPass();
}
