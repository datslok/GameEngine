#include "camera.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

/*
* Initialise the camera's position, direction, and projection so it can transform and view objects in the scene.
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
    projection(Mat4::perspective(
        verticalFovRadians,
        aspectRatio,
        nearPlane,
        farPlane
    )){
    // Reuse lookAt's checks for invalid camera directions.
    Mat4::lookAt(position, target, up);
}

/*
* Move the camera by adding the given displacement to its current position, allowing it to move in any direction.
*/
void Camera::move(const Vec3& displacement){
    position = position + displacement;
}

void Camera::rotate(float yawRadians, float pitchRadians) {
    // Find the current elevation relative to the camera's fixed up axis.
    const float verticalComponent =
        std::clamp(forward.dot(up), -1.0f, 1.0f);

    const float currentPitch = std::asin(verticalComponent);

    // Stop just short of looking straight up or down.
    const float pitchLimit =
        89.0f * std::numbers::pi_v<float> / 180.0f;

    const float newPitch = std::clamp(
        currentPitch + pitchRadians,
        -pitchLimit,
        pitchLimit
    );

    // Remove the vertical component to get the horizontal heading.
    const Vec3 horizontal =
        (forward - up * verticalComponent).normalized();

    // Rotate around the fixed up axis.
    // The minus sign makes positive yaw turn toward the right.
    const Vec3 newHorizontal =
        horizontal * std::cos(yawRadians) -
        up.cross(horizontal) * std::sin(yawRadians);

    // Rebuild a unit direction using the new heading and elevation.
    forward = (
        newHorizontal * std::cos(newPitch) +
        up * std::sin(newPitch)
    ).normalized();
}

/*
* Return the camera's forward direction so it can be used for movement and viewing.
*/
Vec3 Camera::getForward() const{
    return forward;
}

/*
* Calculate the camera's right direction from its forward and up vectors.
*/
Vec3 Camera::getRight() const{
    return forward.cross(up).normalized();
}

/*
* Return the camera's up direction to maintain a consistent orientation and prevent rolling.
*/
Vec3 Camera::getUp() const{
    return up;
}

/*
* Create a view matrix to transform world coordinates into camera space.
*/
Mat4 Camera::getViewMatrix() const{
    return Mat4::lookAt(position, position + forward, up);
}

/*
* Return the projection matrix used to transform camera coordinates for rendering.
*/
Mat4 Camera::getProjectionMatrix() const{
    return projection;
}

void Camera::setAspectRatio(float aspectRatio) {
    if (!std::isfinite(aspectRatio) || aspectRatio <= 0.0f) {
        throw std::invalid_argument(
            "Camera aspect ratio must be finite and positive"
        );
    }

    // Adjust horizontal projection while preserving vertical FOV.
    projection.values[0][0] =
        projection.values[1][1] / aspectRatio;
}