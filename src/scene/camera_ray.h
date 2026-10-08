#pragma once

#include "math/ray.h"
#include "scene/camera.h"

// Build a world-space ray from the camera through a point on the screen, for mouse picking.
// Mouse coordinates are normalized:
// (0, 0) = top-left, (1, 1) = bottom-right.
Ray makeCameraRay(
    const Camera& camera,
    float mouseX,
    float mouseY,
    float aspectRatio
);
