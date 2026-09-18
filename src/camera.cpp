#include "camera.h"

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

void Camera::move(const Vec3& displacement){
    position = position + displacement;
}

Vec3 Camera::getForward() const{
    return forward;
}

Vec3 Camera::getRight() const{
    return forward.cross(up).normalized();
}

Vec3 Camera::getUp() const{
    return up;
}

Mat4 Camera::getViewMatrix() const{
    return Mat4::lookAt(position, position + forward, up);
}

Mat4 Camera::getProjectionMatrix() const{
    return projection;
}