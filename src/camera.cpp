#include "camera.h"

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