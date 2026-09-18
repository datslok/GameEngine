#include "clipper.h"

namespace{
    bool clipAgainstPlane(
        Vec4& start,
        Vec4& end,
        float startDistance,
        float endDistance
    ){
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
}

bool clipLine(Vec4& start, Vec4& end){
    // Left plane:  x + w >= 0
    if (!clipAgainstPlane(
            start, end,
            start.x + start.w,
            end.x + end.w)){
        return false;
    }

    // Right plane: w - x >= 0
    if (!clipAgainstPlane(
            start, end,
            start.w - start.x,
            end.w - end.x)){
        return false;
    }

    // Bottom plane: y + w >= 0
    if (!clipAgainstPlane(
            start, end,
            start.y + start.w,
            end.y + end.w)){
        return false;
    }

    // Top plane: w - y >= 0
    if (!clipAgainstPlane(
            start, end,
            start.w - start.y,
            end.w - end.y)){
        return false;
    }

    // Near plane: z + w >= 0
    if (!clipAgainstPlane(
            start, end,
            start.z + start.w,
            end.z + end.w)){
        return false;
    }

    // Far plane: w - z >= 0
    if (!clipAgainstPlane(
            start, end,
            start.w - start.z,
            end.w - end.z)){
        return false;
    }

    return true;
}