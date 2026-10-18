#include "engine/debug_views.h"
#include "math/transform.h"
#include "physics/collider.h"
#include "render/gpu/debug_line_vertices.h"
#include "scene/debug_draw.h"
#include "scene/frame_description.h"
#include "scene/mesh.h"
#include "scene/model.h"

#include <cassert>
#include <cmath>
#include <initializer_list>

namespace {
    bool nearlyEqual(float actual, float expected, float tolerance = 0.0001f) {
        return std::abs(actual - expected) < tolerance;
    }

    bool nearlyEqual(const Vec3& actual, const Vec3& expected) {
        return nearlyEqual(actual.x, expected.x) && nearlyEqual(actual.y, expected.y) && nearlyEqual(actual.z, expected.z);
    }

    // Distance from a point to the segment from a to b.
    float distanceToSegment(const Vec3& point, const Vec3& a, const Vec3& b) {
        const Vec3 along = b - a;
        float t = (point - a).dot(along) / along.lengthSquared();
        t = std::fmax(0.0f, std::fmin(1.0f, t));
        return (point - (a + along * t)).length();
    }

    const Vec3 green{0.0f, 1.0f, 0.0f};

    void testLineIsStored() {
        DebugDraw debug;
        debug.line(Vec3{1.0f, 2.0f, 3.0f}, Vec3{4.0f, 5.0f, 6.0f}, green);

        assert(debug.getLines().size() == 1);
        assert(nearlyEqual(debug.getLines()[0].from, Vec3{1.0f, 2.0f, 3.0f}));
        assert(nearlyEqual(debug.getLines()[0].to, Vec3{4.0f, 5.0f, 6.0f}));
        assert(nearlyEqual(debug.getLines()[0].colour, green));

        // Shapes are collected per frame, so the engine clears them before the next one.
        debug.clear();
        assert(debug.getLines().empty());
    }

    // A box is its 12 edges. Its corners go through the transform, so it can follow a rotated, scaled object.
    void testBoxHasTwelveEdgesThroughItsCorners() {
        DebugDraw debug;
        const Aabb bounds{Vec3{-1.0f, 0.0f, -2.0f}, Vec3{1.0f, 3.0f, 2.0f}};
        debug.box(bounds, Mat4::translation(10.0f, 0.0f, 0.0f), green);

        assert(debug.getLines().size() == 12);

        float totalLength = 0.0f;

        for (const DebugLine& edge : debug.getLines()) {
            for (const Vec3& end : {edge.from, edge.to}) {
                assert(nearlyEqual(std::abs(end.x - 10.0f), 1.0f));
                assert(nearlyEqual(end.y, 0.0f) || nearlyEqual(end.y, 3.0f));
                assert(nearlyEqual(std::abs(end.z), 2.0f));
            }

            totalLength += (edge.to - edge.from).length();
        }

        // Four edges along each axis: 4 * (2 + 3 + 4).
        assert(nearlyEqual(totalLength, 36.0f, 0.001f));
    }

    // A wire sphere is three circles (around x, y and z), every point exactly one radius from the centre.
    void testSphereIsThreeCircles() {
        DebugDraw debug;
        const Vec3 centre{1.0f, 2.0f, 3.0f};
        debug.sphere(centre, 0.5f, green, 16);

        assert(debug.getLines().size() == 3 * 16);

        for (const DebugLine& line : debug.getLines()) {
            assert(nearlyEqual((line.from - centre).length(), 0.5f));
            assert(nearlyEqual((line.to - centre).length(), 0.5f));
        }
    }

    // A capsule (the usual character collider) is every point within a radius of a segment: its outline stays on that
    // surface, and it reaches one radius past each end.
    void testCapsuleOutlinesItsSurface() {
        DebugDraw debug;
        const Vec3 bottom{0.0f, 0.5f, 0.0f};
        const Vec3 top{0.0f, 1.5f, 0.0f};
        debug.capsule(bottom, top, 0.5f, green, 16);

        assert(!debug.getLines().empty());

        float highest = -1.0f;
        float lowest = 10.0f;

        for (const DebugLine& line : debug.getLines()) {
            for (const Vec3& end : {line.from, line.to}) {
                assert(nearlyEqual(distanceToSegment(end, bottom, top), 0.5f, 0.001f));
                highest = std::fmax(highest, end.y);
                lowest = std::fmin(lowest, end.y);
            }
        }

        assert(nearlyEqual(highest, 2.0f) && nearlyEqual(lowest, 0.0f));

        // With both ends in the same place it is just a sphere, not a crash.
        DebugDraw ball;
        ball.capsule(top, top, 0.5f, green, 16);
        assert(!ball.getLines().empty());
    }

    // A mesh's bounding box: the smallest axis-aligned box around its vertices.
    void testMeshBounds() {
        Mesh mesh;
        mesh.vertices = {Vec4{1.0f, -2.0f, 0.5f, 1.0f}, Vec4{-3.0f, 4.0f, 0.0f, 1.0f}, Vec4{0.0f, 0.0f, -1.0f, 1.0f}};

        const Aabb bounds = meshBounds(mesh);

        assert(nearlyEqual(bounds.minimum, Vec3{-3.0f, -2.0f, -1.0f}));
        assert(nearlyEqual(bounds.maximum, Vec3{1.0f, 4.0f, 0.5f}));
        assert(nearlyEqual(meshBounds(Mesh{}).size(), Vec3{0.0f, 0.0f, 0.0f}));
    }

    // The bounding-box view draws a box around every draw in the frame, through the same matrix the mesh is drawn with.
    void testBoundingBoxView() {
        std::vector<DrawItem> draws(2);
        draws[0].mesh = MeshHandle{0};
        draws[0].model = Mat4::translation(5.0f, 0.0f, 0.0f);
        draws[1].mesh = MeshHandle{1};

        const std::vector<Aabb> bounds = {
            Aabb{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}},
            Aabb{Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 2.0f, 2.0f}}
        };

        DebugDraw debug;
        drawBoundingBoxes(draws, bounds, debug);

        assert(debug.getLines().size() == 24);
        assert(nearlyEqual(std::abs(debug.getLines()[0].from.x - 5.0f), 1.0f));

        // A mesh whose bounds are not known (yet) is skipped rather than crashing.
        std::vector<DrawItem> unknown(1);
        unknown[0].mesh = MeshHandle{7};
        DebugDraw none;
        drawBoundingBoxes(unknown, bounds, none);
        assert(none.getLines().empty());
    }

    bool sameLines(const DebugDraw& actual, const DebugDraw& expected) {
        if (actual.getLines().size() != expected.getLines().size()) {
            return false;
        }
        for (std::size_t i = 0; i < actual.getLines().size(); ++i) {
            const DebugLine& a = actual.getLines()[i];
            const DebugLine& b = expected.getLines()[i];
            if (!nearlyEqual(a.from, b.from) || !nearlyEqual(a.to, b.to) || !nearlyEqual(a.colour, b.colour)) {
                return false;
            }
        }
        return true;
    }

    // Any collision shape can be drawn through one call, which draws exactly what the matching specific call draws.
    void testShapeMatchesDirectCalls() {
        const Sphere ball{Vec3{1.0f, 2.0f, 3.0f}, 0.5f};
        DebugDraw viaShape;
        DebugDraw direct;
        viaShape.shape(ball, green);
        direct.sphere(ball.centre, ball.radius, green);
        assert(sameLines(viaShape, direct));

        const Capsule pill{Vec3{0.0f, 0.5f, 0.0f}, Vec3{0.0f, 1.5f, 0.0f}, 0.5f};
        viaShape.clear();
        direct.clear();
        viaShape.shape(pill, green);
        direct.capsule(pill.a, pill.b, pill.radius, green);
        assert(sameLines(viaShape, direct));

        const Aabb crate{Vec3{-1.0f, 0.0f, -1.0f}, Vec3{1.0f, 2.0f, 1.0f}};
        viaShape.clear();
        direct.clear();
        viaShape.shape(crate, green);
        direct.box(crate, Mat4::identity(), green);
        assert(sameLines(viaShape, direct));
    }

    // The engine's collider view draws each collider where its entity is, as the shape a query would test.
    void testColliderView() {
        World world;
        const Entity entity = world.create();
        Transform placement;
        placement.position = Vec3{4.0f, 0.0f, 0.0f};
        world.add(entity, placement);
        world.add(entity, Collider{Sphere{Vec3{0.0f, 1.0f, 0.0f}, 0.5f}, nullptr});

        // A collider with no Transform is nowhere, so it is not drawn.
        world.add(world.create(), Collider{Sphere{Vec3{0.0f, 0.0f, 0.0f}, 1.0f}, nullptr});

        DebugDraw view;
        drawColliders(world, 1.0f, view);

        DebugDraw expected;
        expected.shape(Sphere{Vec3{4.0f, 1.0f, 0.0f}, 0.5f}, colliderDebugColour);
        assert(sameLines(view, expected));
    }

    // The readout's lines are spaced evenly down from the top-left margin, so games can add lines below the engine's.
    void testReadoutLines() {
        const Vec2 first = debugReadoutLine(0);
        const Vec2 second = debugReadoutLine(1);
        const Vec2 fifth = debugReadoutLine(4);
        assert(first.x > 0.0f && first.y > 0.0f);
        assert(nearlyEqual(second.x, first.x));
        assert(second.y - first.y > debugReadoutTextHeight);
        assert(nearlyEqual(fifth.y - first.y, 4.0f * (second.y - first.y)));
    }

    // The GPU gets two vertices per line, each a position and a colour.
    void testLinesBecomeVertices() {
        const std::vector<DebugLine> lines = {DebugLine{Vec3{1.0f, 2.0f, 3.0f}, Vec3{4.0f, 5.0f, 6.0f}, Vec3{0.1f, 0.2f, 0.3f}}};
        const std::vector<DebugLineVertex> vertices = buildDebugLineVertices(lines);

        assert(vertices.size() == 2);
        assert(nearlyEqual(vertices[0].position[0], 1.0f) && nearlyEqual(vertices[1].position[2], 6.0f));
        assert(nearlyEqual(vertices[0].colour[1], 0.2f) && nearlyEqual(vertices[1].colour[2], 0.3f));

        // A frame starts with no debug lines.
        const Camera camera{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.0f, 0.1f, 10.0f};
        World world;
        assert(buildFrame(world, camera, 0.0f).debugLines.empty());
    }
}

void testDebugDraw() {
    testLineIsStored();
    testBoxHasTwelveEdgesThroughItsCorners();
    testSphereIsThreeCircles();
    testCapsuleOutlinesItsSurface();
    testMeshBounds();
    testBoundingBoxView();
    testShapeMatchesDirectCalls();
    testColliderView();
    testReadoutLines();
    testLinesBecomeVertices();
}
