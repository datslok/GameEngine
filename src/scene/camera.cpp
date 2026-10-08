#include "scene/camera.h"

#include <cmath>
#include <stdexcept>

/*
* Initialise the camera's position, direction and lens. Building the matrices once here reuses their validation, so a bad camera fails immediately.
*/
Camera::Camera(
    const Vec3& position,
    const Vec3& target,
    const Vec3& up,
    float verticalFovRadians,
    float aspectRatio,
    float nearPlane,
    float farPlane
):
    position(position),
    forward((target - position).normalized()),
    up(up.normalized()),
    verticalFov(verticalFovRadians),
    aspectRatio(aspectRatio),
    nearPlane(nearPlane),
    farPlane(farPlane)
{
    Mat4::lookAt(position, target, up);
    Mat4::perspective(verticalFov, aspectRatio, nearPlane, farPlane);
}

Vec3 Camera::getPosition() const {
    return position;
}

/*
* Return the camera's forward direction so it can be used for movement and viewing.
*/
Vec3 Camera::getForward() const {
    return forward;
}

/*
* Calculate the camera's right direction from its forward and up vectors.
*/
Vec3 Camera::getRight() const {
    return forward.cross(up).normalized();
}

/*
* Return the camera's up direction to maintain a consistent orientation and prevent rolling.
*/
Vec3 Camera::getUp() const {
    return up;
}

void Camera::setPosition(const Vec3& newPosition) {
    position = newPosition;
}

/*
* Validate with lookAt before changing anything, so a direction parallel to up cannot leave the camera unusable.
*/
void Camera::setForward(const Vec3& direction) {
    Mat4::lookAt(position, position + direction, up);
    forward = direction.normalized();
}

void Camera::setPose(const Vec3& newPosition, const Vec3& target, const Vec3& upDirection) {
    // Validate before changing the camera.
    Mat4::lookAt(newPosition, target, upDirection);

    position = newPosition;
    forward = (target - newPosition).normalized();
    up = upDirection.normalized();
}

/*
* Create a view matrix to transform world coordinates into camera space.
*/
Mat4 Camera::getViewMatrix() const {
    return Mat4::lookAt(position, position + forward, up);
}

/*
* Rebuilt from the lens parameters each time, so changing the field of view or aspect ratio is just changing a number.
*/
Mat4 Camera::getProjectionMatrix() const {
    return Mat4::perspective(verticalFov, aspectRatio, nearPlane, farPlane);
}

float Camera::getAspectRatio() const {
    return aspectRatio;
}

void Camera::setAspectRatio(float newAspectRatio) {
    if (!std::isfinite(newAspectRatio) || newAspectRatio <= 0.0f) {
        throw std::invalid_argument(
            "Camera aspect ratio must be finite and positive"
        );
    }

    aspectRatio = newAspectRatio;
}

float Camera::getVerticalFov() const {
    return verticalFov;
}

void Camera::setVerticalFov(float verticalFovRadians) {
    // Validate with the same rules as the projection matrix.
    Mat4::perspective(verticalFovRadians, aspectRatio, nearPlane, farPlane);
    verticalFov = verticalFovRadians;
}
