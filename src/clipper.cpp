#include "clipper.h"

#include <vector>

/*
* Clip a line segment against a plane to remove points outside the view volume.
*/
namespace {
    bool clipAgainstPlane(
        Vec4& start,
        Vec4& end,
        float startDistance,
        float endDistance
    ) {
        const bool startInside = startDistance >= 0.0f;
        const bool endInside = endDistance >= 0.0f;

        if (!startInside && !endInside){
            return false;
        }

        if (startInside && endInside){
            return true;
        }

        const float t = startDistance / (startDistance - endDistance);

        const Vec4 intersection = start + (end - start) * t;

        if (!startInside){
            start = intersection;
        } else {
            end = intersection;
        }

        return true;
    }

    float planeDistance(const Vec4& point, int plane){
        switch (plane){
            case 0:
                return point.x + point.w; // left

            case 1:
                return point.w - point.x; // right

            case 2:
                return point.y + point.w; // bottom

            case 3:
                return point.w - point.y; // top

            case 4:
                return point.z + point.w; // near

            case 5:
                return point.w - point.z; // far
        }

        return 0.0f;
    }

    std::vector<Vec4> clipPolygonAgainstPlane(
        const std::vector<Vec4>& polygon,
        int plane
    ){
        std::vector<Vec4> result;

        if (polygon.empty()){
            return result;
        }

        const Vec4* previous = &polygon.back();
        float previousDistance = planeDistance(*previous, plane);
        bool previousInside = previousDistance >= 0.0f;

        for (const Vec4& current : polygon){
            const float currentDistance = planeDistance(current, plane);

            const bool currentInside = currentDistance >= 0.0f;

            if (currentInside != previousInside){
                const float t =
                    previousDistance / (previousDistance - currentDistance);

                result.push_back(
                    *previous + (current - *previous) * t
                );
            }

            if (currentInside) {
                result.push_back(current);
            }

            previous = &current;
            previousDistance = currentDistance;
            previousInside = currentInside;
        }

        return result;
    }
}

/*
* Clip the line against each boundary of the view volume so only the visible portion of the line remains.
*/
bool clipLine(Vec4& start, Vec4& end){
    if (!clipAgainstPlane(
            start, end,
            start.x + start.w,
            end.x + end.w)){
        return false;
    }

    if (!clipAgainstPlane(
            start, end,
            start.w - start.x,
            end.w - end.x)){
        return false;
    }

    if (!clipAgainstPlane(
            start, end,
            start.y + start.w,
            end.y + end.w)){
        return false;
    }

    if (!clipAgainstPlane(
            start, end,
            start.w - start.y,
            end.w - end.y)){
        return false;
    }

    if (!clipAgainstPlane(
            start, end,
            start.z + start.w,
            end.z + end.w)){
        return false;
    }

    if (!clipAgainstPlane(
            start, end,
            start.w - start.z,
            end.w - end.z)){
        return false;
    }

    return true;
}

std::vector<Vec4> clipTriangle(
    const Vec4& first,
    const Vec4& second,
    const Vec4& third
) {
    std::vector<Vec4> polygon{
        first,
        second,
        third
    };

    for (int plane = 0; plane < 6; ++plane){
        polygon = clipPolygonAgainstPlane(polygon, plane);

        if (polygon.empty()) {
            break;
        }
    }

    return polygon;
}