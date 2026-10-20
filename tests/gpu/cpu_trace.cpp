#include "cpu_trace.h"

#include "physics/raycast.h"
#include "render/gpu/ray_tracing.h"
#include "scene/frame_description.h"

#include <cmath>

Vec3 cornerPosition(const IndexedMesh& mesh, std::uint32_t corner, const Mat4& model) {
    const MeshVertex& vertex = mesh.vertices[mesh.indices[corner]];
    return transformPoint(model, vertex.position);
}

std::optional<CpuTraceHit> traceOnCpu(std::span<const CpuTraceInstance> instances, const Ray& ray, float maxDistance, std::uint32_t mask) {
    std::optional<CpuTraceHit> nearest;

    for (std::uint32_t instance = 0; instance < instances.size(); ++instance) {
        const CpuTraceInstance& placed = instances[instance];
        DrawItem draw;
        draw.castsShadows = placed.castsShadows;
        draw.isStatic = placed.isStatic;

        if ((rayMaskFor(draw) & mask) == 0) {
            continue;
        }

        const IndexedMesh& mesh = *placed.mesh;

        for (std::uint32_t triangle = 0; triangle * 3 < mesh.indices.size(); ++triangle) {
            const Vec3 a = cornerPosition(mesh, triangle * 3, placed.model);
            const Vec3 b = cornerPosition(mesh, triangle * 3 + 1, placed.model);
            const Vec3 c = cornerPosition(mesh, triangle * 3 + 2, placed.model);
            const float reach = nearest ? nearest->distance : maxDistance;
            const std::optional<RayHit> hit = raycastTriangle(ray, a, b, c, reach);

            if (!hit || (nearest && hit->distance >= nearest->distance)) {
                continue;
            }

            // The hit's barycentric weights, from areas (an affine map keeps them), give the interpolated normal.
            const Vec3 normal = (b - a).cross(c - a);
            const float area = normal.lengthSquared();
            const float second = (hit->point - a).cross(c - a).dot(normal) / area;
            const float third = (b - a).cross(hit->point - a).dot(normal) / area;
            const Vec3 weights = hitCornerWeights(second, third);
            const MeshVertex& first = mesh.vertices[mesh.indices[triangle * 3]];
            const MeshVertex& secondCorner = mesh.vertices[mesh.indices[triangle * 3 + 1]];
            const MeshVertex& thirdCorner = mesh.vertices[mesh.indices[triangle * 3 + 2]];

            nearest = CpuTraceHit{hit->distance, instance, triangle, hitsFrontFace(a, b, c, ray.direction, handedness(placed.model)),
                                  first.normal * weights.x + secondCorner.normal * weights.y + thirdCorner.normal * weights.z,
                                  std::abs(normal.normalized().dot(ray.direction))};
        }
    }

    return nearest;
}
