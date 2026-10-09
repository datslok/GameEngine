#pragma once

#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/model.h"

#include <vector>

// One straight line to draw over the scene. Colours are linear 0..1, like light colours.
struct DebugLine {
    Vec3 from;
    Vec3 to;
    Vec3 colour;
};

/*
* Collects wireframe shapes to draw over the scene for one frame: lines, boxes, spheres and capsules, all as plain lines.
* It is for seeing what the engine knows but does not show (bounding boxes, colliders, light positions, paths), so the
* lines are drawn on top of everything, never hidden behind walls. The engine clears it every frame, so shapes are
* added again each frame by whatever wants them seen, like immediate-mode drawing.
*/
class DebugDraw {
public:
    void line(const Vec3& from, const Vec3& to, const Vec3& colour);

    // The 12 edges of a box: the axis-aligned bounds, then the transform (so it can follow a rotated, scaled object).
    void box(const ModelBounds& bounds, const Mat4& transform, const Vec3& colour);

    // Three circles, around the x, y and z axes, each made of `segments` lines.
    void sphere(const Vec3& centre, float radius, const Vec3& colour, int segments = 24);

    // Every point within radius of the segment from a to b, the usual shape of a character's collider:
    // a circle at each end, four lines along the sides, and half circles over the ends.
    void capsule(const Vec3& a, const Vec3& b, float radius, const Vec3& colour, int segments = 24);

    // A line on the screen itself, in window pixels from the top-left corner, for overlays such as text. Not affected by the camera.
    void screenLine(const Vec2& from, const Vec2& to, const Vec3& colour);

    void clear();
    const std::vector<DebugLine>& getLines() const;
    const std::vector<DebugLine>& getScreenLines() const; // z is always 0

private:
    std::vector<DebugLine> lines;
    std::vector<DebugLine> screenLines;

    // A circle around centre in the plane spanned by two perpendicular unit vectors, from startAngle sweeping sweepAngle.
    void arc(const Vec3& centre, const Vec3& axisA, const Vec3& axisB, float radius, float startAngle, float sweepAngle, const Vec3& colour, int segments);
};