#pragma once

#include "mat4.h"
#include "vec3.h"

/*
* Represents a camera that controls its position, orientation, and projection to transform the scene from world space into viewable screen space.
*/
class Camera {
    public:
        Camera(
            const Vec3& position,
            const Vec3& target,
            const Vec3& up,
            float verticalFovRadians,
            float aspectRatio,
            float nearPlane,
            float farPlane
        );

        void move(const Vec3& displacement);
        // Positive yaw turns right; positive pitch looks up.
        void rotate(float yawRadians, float pitchRadians);

        Vec3 getForward() const;
        Vec3 getRight() const;
        Vec3 getUp() const;

        Mat4 getViewMatrix() const;
        Mat4 getProjectionMatrix() const;

    private:
        Vec3 position;
        Vec3 forward;
        Vec3 up;
        Mat4 projection;
};