#include "render/gpu/planar_reflection.h"
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

/*
* The plane's signed distance pi(x) = n.x - d is a row like the matrix's own. Where it is above 0, x is in front of the
* mirror. With z = w - k pi, the GPU's z <= w is pi >= 0, the mirror for a near plane; its z >= 0 is k pi <= w, so k is
* what brings the largest pi / w over the view's far corners down to 1 (a ratio of homogeneous values, so no divide by the
* corners' own w is needed).
*/
Mat4 planarViewProjection(const Mat4& viewProjection, const MirrorPlane& plane) {
    Mat4 mirrored = viewProjection * planeReflection(plane);
    const std::array<float, 4> distance{plane.normal.x, plane.normal.y, plane.normal.z, -(plane.offset + static_cast<float>(PLANAR_CLIP_LIFT))};
    float largest = 0.0f;

    if (const std::optional<Mat4> corners = inverse(mirrored)) {
        for (const float x : {-1.0f, 1.0f}) {
            for (const float y : {-1.0f, 1.0f}) {
                const Vec4 corner = *corners * Vec4{x, y, 0.0f, 1.0f};
                const std::array<float, 4> point{corner.x, corner.y, corner.z, corner.w};
                float pi = 0.0f;
                float w = 0.0f;

                for (std::size_t column = 0; column < point.size(); ++column) {
                    pi += distance[column] * point[column];
                    w += mirrored.values[3][column] * point[column];
                }

                if (w != 0.0f) {
                    largest = std::max(largest, pi / w);
                }
            }
        }
    }

    const float scale = largest > 0.0f ? 1.0f / largest : 1.0f;

    for (std::size_t column = 0; column < distance.size(); ++column) {
        mirrored.values[2][column] = mirrored.values[3][column] - scale * distance[column];
    }

    return mirrored;
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
