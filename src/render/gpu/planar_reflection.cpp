#include "render/gpu/planar_reflection.h"
#include "render/gpu/cascades.h"
#include "render/gpu/transparency.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
    // A face turned this far from the camera (the cosine) shows too little to be worth a second draw of the scene.
    constexpr float leastFacing = 0.05f;

    // How much better another mirror must be to take last frame's place.
    constexpr float keepShare = 1.5f;

    bool samePlane(const MirrorPlane& a, const MirrorPlane& b) {
        return a.normal.dot(b.normal) > 0.999f && std::abs(a.offset - b.offset) < 0.01f;
    }

    // The world area of a face, the two largest of its box's sides (the third is its thickness, none for a true face).
    float faceArea(const FlatFace& face, const Mat4& model) {
        const Vec3 size = face.bounds.size();
        const Vec3 x = transformDirection(model, Vec3{size.x, 0.0f, 0.0f});
        const Vec3 y = transformDirection(model, Vec3{0.0f, size.y, 0.0f});
        const Vec3 z = transformDirection(model, Vec3{0.0f, 0.0f, size.z});
        return std::max({x.cross(y).length(), y.cross(z).length(), z.cross(x).length()});
    }
}

MirrorPlane planeRelativeTo(const MirrorPlane& plane, const Vec3& origin) {
    return MirrorPlane{plane.normal, plane.offset - plane.normal.dot(origin)};
}

Vec3 reflectAcrossPlane(const Vec3& point, const MirrorPlane& plane) {
    return point - plane.normal * (2.0f * (plane.normal.dot(point) - plane.offset));
}

Mat4 planeReflection(const MirrorPlane& plane) {
    const std::array<float, 3> n{plane.normal.x, plane.normal.y, plane.normal.z};
    Mat4 reflection = Mat4::identity();

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            reflection.values[row][column] -= 2.0f * n[row] * n[column];
        }

        reflection.values[row][3] = 2.0f * plane.offset * n[row];
    }

    return reflection;
}

Mat4 planarViewProjection(const Mat4& viewProjection, const MirrorPlane& plane) {
    return viewProjection * planeReflection(plane);
}

// The near side is the fifth plane (Frustum::fromClipMatrix); the clip plane's row is already a plane's normal and distance.
Frustum planarFrustum(const Mat4& worldViewProjection, const MirrorPlane& plane) {
    Frustum frustum = Frustum::fromClipMatrix(worldViewProjection, ClipDepth::OneToZero);
    const Vec4 clip = planarClipPlane(plane);
    frustum.planes[4] = Plane{Vec3{clip.x, clip.y, clip.z}, clip.w};
    return frustum;
}

Camera mirroredCamera(const Camera& camera, const MirrorPlane& plane) {
    const Vec3 position = reflectAcrossPlane(camera.getPosition(), plane);
    const Vec3 target = reflectAcrossPlane(camera.getPosition() + camera.getForward(), plane);
    const Vec3 up = camera.getUp() - plane.normal * (2.0f * plane.normal.dot(camera.getUp()));
    return Camera{position, target, up, camera.getVerticalFov(), camera.getAspectRatio(), camera.getNearPlane(), camera.getFarPlane()};
}

/*
* The mirrored camera is as far behind the glass as the camera is in front, so whatever it sees past the glass is at
* least that far from it, and a point that far from it along a ray at an angle a from its forward axis is that times
* cos a along the axis. The widest angle is the view's corner's, whose cosine is 1 / sqrt(1 + tan^2 of the half-height
* + tan^2 of the half-width). Rounded down to the ladder, so it does not change the cascades as the camera creeps.
*/
float mirrorCascadesStart(const Camera& camera, const MirrorPlane& plane) {
    const float behindGlass = std::abs(plane.normal.dot(camera.getPosition()) - plane.offset);
    const float halfHeight = std::tan(camera.getVerticalFov() * 0.5f);
    const float halfWidth = halfHeight * camera.getAspectRatio();
    const float alongAxis = behindGlass / std::sqrt(1.0f + halfHeight * halfHeight + halfWidth * halfWidth);
    return alongAxis > camera.getNearPlane() ? std::max(shadowLadderStepBelow(alongAxis), camera.getNearPlane()) : camera.getNearPlane();
}

std::optional<MirrorPlane> choosePlanarMirror(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& visible, const Vec3& cameraPosition,
                                              float maxRoughness, const std::optional<MirrorPlane>& previous) {
    std::optional<MirrorPlane> best;
    float bestScore = 0.0f;

    for (const std::size_t index : visible) {
        const DrawItem& draw = draws[index];

        if (draw.material.metallic < 0.5f || draw.material.roughness > maxRoughness || isTransparent(draw.material)) {
            continue;
        }

        for (const FlatFace& face : draw.flatFaces) {
            const Vec3 centre = transformPoint(draw.model, face.bounds.centre());
            const Vec3 normal = transformDirection(normalMatrix(draw.model), face.normal).normalized();
            const Vec3 toCamera = cameraPosition - centre;
            const float distance = toCamera.length();

            if (!(distance > 0.0f) || normal.dot(toCamera) < leastFacing * distance) {
                continue;
            }

            const MirrorPlane plane{normal, normal.dot(centre)};
            const bool kept = previous.has_value() && samePlane(plane, *previous);
            const float score = (kept ? keepShare : 1.0f) * faceArea(face, draw.model) * normal.dot(toCamera) / (distance * distance * distance);

            if (score > bestScore) {
                bestScore = score;
                best = plane;
            }
        }
    }

    return best;
}

Vec4 planarClipPlane(const MirrorPlane& relativePlane) {
    return Vec4{relativePlane.normal.x, relativePlane.normal.y, relativePlane.normal.z, -(relativePlane.offset + static_cast<float>(PLANAR_CLIP_LIFT))};
}

Vec4 noClipPlane() {
    return Vec4{0.0f, 0.0f, 0.0f, 1.0f};
}

float planarClipDistance(const Vec4& plane, const Vec3& position) {
    return plane.x * position.x + plane.y * position.y + plane.z * position.z + plane.w;
}

float isOnPlanarMirror(const Vec3& position, const Vec3& normal, const Vec3& planeNormal, float planeOffset) {
    const float tolerance = static_cast<float>(PLANAR_PLANE_TOLERANCE) + static_cast<float>(PLANAR_PLANE_TOLERANCE_PER_METRE) * position.length();
    return std::abs(planeNormal.dot(position) - planeOffset) <= tolerance && normal.dot(planeNormal) >= static_cast<float>(PLANAR_NORMAL_COSINE) ? 1.0f : 0.0f;
}

GpuRayDebugView mirroredRays(const GpuRayDebugView& view, const MirrorPlane& plane) {
    GpuRayDebugView mirrored = view;
    const auto mirrorDirection = [&plane](const float* from, float* to) {
        const Vec3 direction{from[0], from[1], from[2]};
        const Vec3 turned = direction - plane.normal * (2.0f * plane.normal.dot(direction));
        to[0] = turned.x;
        to[1] = turned.y;
        to[2] = turned.z;
    };

    mirrorDirection(view.right, mirrored.right);
    mirrorDirection(view.up, mirrored.up);
    mirrorDirection(view.forward, mirrored.forward);
    const Vec3 origin = reflectAcrossPlane(Vec3{view.originAndView[0], view.originAndView[1], view.originAndView[2]}, plane);
    mirrored.originAndView[0] = origin.x;
    mirrored.originAndView[1] = origin.y;
    mirrored.originAndView[2] = origin.z;
    return mirrored;
}

RenderSize planarRenderSize(RenderSize render, float scale) {
    const auto scaled = [scale](std::uint32_t size) { return std::max(static_cast<std::uint32_t>(std::lround(static_cast<float>(size) * scale)), std::uint32_t{1}); };
    return RenderSize{scaled(render.width), scaled(render.height)};
}
