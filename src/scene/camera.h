#pragma once

#include "math/mat4.h"
#include "math/vec3.h"

/*
* A lens in the world: where it is, which way it looks, and its field of view.
* It has no behaviour of its own. Camera controllers (gameplay/camera/) decide how it moves.
* The projection is rebuilt from its parameters when asked, so the field of view can change (for example when aiming down sights).
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

        Vec3 getPosition() const;
        Vec3 getForward() const;
        Vec3 getRight() const;
        Vec3 getUp() const;

        void setPosition(const Vec3& newPosition);

        // Look along a direction, keeping the current up axis. The direction must not be parallel to it.
        void setForward(const Vec3& direction);

        // Place the camera and aim it at a target.
        void setPose(const Vec3& newPosition, const Vec3& target, const Vec3& upDirection);

        Mat4 getViewMatrix() const;
        Mat4 getProjectionMatrix() const;

        float getAspectRatio() const;
        void setAspectRatio(float aspectRatio);

        float getVerticalFov() const;
        void setVerticalFov(float verticalFovRadians);

    private:
        Vec3 position;
        Vec3 forward;
        Vec3 up;

        float verticalFov;
        float aspectRatio;
        float nearPlane;
        float farPlane;
};
