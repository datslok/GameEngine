#include "gameplay/camera/moba_camera_controller.h"
#include "gameplay/edge_pan.h"

MobaCameraController::MobaCameraController(const Vec3& offset, float panSpeed, float edgeMarginPixels):
    offset(offset),
    panSpeed(panSpeed),
    edgeMarginPixels(edgeMarginPixels)
{
}

void MobaCameraController::centreOn(Camera& camera, const Vec3& point) const {
    camera.setPose(point + offset, point, Vec3{0.0f, 1.0f, 0.0f});
}

/*
* The camera only ever moves by whole offsets from a point and pans without turning, so the point is always one offset away.
*/
Vec3 MobaCameraController::getLookPoint(const Camera& camera) const {
    return camera.getPosition() - offset;
}

/*
* Panning moves the camera without turning it, so the viewing angle set by the offset never changes.
*/
void MobaCameraController::update(Camera& camera, const Input& input, float frameSeconds,
                                  const std::optional<Vec3>& followTarget) const {
    if (followTarget) {
        centreOn(camera, *followTarget);
        return;
    }

    // Only pan while the cursor is confined to a focused window, so moving to another monitor does not scroll the map.
    if (!input.isCursorConfined() || !input.hasKeyboardFocus()) {
        return;
    }

    const Vec2 edge = getEdgePanDirection(input, edgeMarginPixels);

    // Screen left/right maps to world X; screen top/bottom maps to world -Z/+Z.
    const Vec3 movement{edge.x, 0.0f, edge.y};

    if (movement.lengthSquared() > 0.0f) {
        camera.setPosition(camera.getPosition() + movement.normalized() * (panSpeed * frameSeconds));
    }
}
