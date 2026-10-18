#include "scene/tangents.h"

#include "mikktspace/mikktspace.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <stdexcept>

namespace {
    // What MikkTSpace's callbacks read and write, passed through its context's user pointer.
    struct TangentJob {
        std::span<MeshVertex> corners;
        UvOrigin origin;
    };

    TangentJob& jobOf(const SMikkTSpaceContext* context) {
        return *static_cast<TangentJob*>(context->m_pUserData);
    }

    MeshVertex& cornerOf(const SMikkTSpaceContext* context, int face, int vertex) {
        return jobOf(context).corners[static_cast<std::size_t>(face) * 3 + static_cast<std::size_t>(vertex)];
    }

    int faceCount(const SMikkTSpaceContext* context) {
        return static_cast<int>(jobOf(context).corners.size() / 3);
    }

    int cornersPerFace(const SMikkTSpaceContext*, int) {
        return 3;
    }

    void readPosition(const SMikkTSpaceContext* context, float output[], int face, int vertex) {
        const Vec3& position = cornerOf(context, face, vertex).position;
        output[0] = position.x;
        output[1] = position.y;
        output[2] = position.z;
    }

    void readNormal(const SMikkTSpaceContext* context, float output[], int face, int vertex) {
        const Vec3& normal = cornerOf(context, face, vertex).normal;
        output[0] = normal.x;
        output[1] = normal.y;
        output[2] = normal.z;
    }

    /*
    * MikkTSpace's bitangent points towards increasing v. A normal map's green channel means "up the picture", so v is
    * handed over counting up from the bottom row: as it is for OBJ, flipped for glTF (whose v counts down from the top).
    * Blender's glTF exporter makes its tangents the same way.
    */
    void readUv(const SMikkTSpaceContext* context, float output[], int face, int vertex) {
        const Vec2& uv = cornerOf(context, face, vertex).uv;
        output[0] = uv.x;
        output[1] = jobOf(context).origin == UvOrigin::TopLeft ? 1.0f - uv.y : uv.y;
    }

    void writeTangent(const SMikkTSpaceContext* context, const float tangent[], float sign, int face, int vertex) {
        cornerOf(context, face, vertex).tangent = Vec4{tangent[0], tangent[1], tangent[2], sign < 0.0f ? -1.0f : 1.0f};
    }

    // The part of a direction across the normal, unit length (one Gram-Schmidt step), or nothing if it lies along the normal.
    std::optional<Vec3> acrossNormal(const Vec3& direction, const Vec3& normal) {
        const Vec3 across = direction - normal * normal.dot(direction);
        const float length = across.length();

        if (!std::isfinite(length) || length < 0.001f) {
            return std::nullopt;
        }

        return across * (1.0f / length);
    }

    // Any unit vector across the normal: whichever of two axes is not along it.
    Vec3 anyTangentAcross(const Vec3& normal) {
        return acrossNormal(Vec3{1.0f, 0.0f, 0.0f}, normal)
            .value_or(acrossNormal(Vec3{0.0f, 1.0f, 0.0f}, normal).value_or(Vec3{1.0f, 0.0f, 0.0f}));
    }

    // A picture needs at least two different UVs; a mesh without UVs has them all at 0, 0.
    bool hasPicture(std::span<const MeshVertex> corners) {
        return std::any_of(corners.begin(), corners.end(), [&](const MeshVertex& corner) {
            return corner.uv.x != corners.front().uv.x || corner.uv.y != corners.front().uv.y;
        });
    }
}

void generateTangents(std::span<MeshVertex> corners, UvOrigin origin) {
    if (corners.size() % 3 != 0) {
        throw std::invalid_argument("generateTangents needs whole triangles");
    }

    if (corners.empty()) {
        return;
    }

    if (!hasPicture(corners)) {
        for (MeshVertex& corner : corners) {
            const Vec3 tangent = anyTangentAcross(corner.normal);
            corner.tangent = Vec4{tangent.x, tangent.y, tangent.z, 1.0f};
        }

        return;
    }

    TangentJob job{corners, origin};

    SMikkTSpaceInterface callbacks{};
    callbacks.m_getNumFaces = faceCount;
    callbacks.m_getNumVerticesOfFace = cornersPerFace;
    callbacks.m_getPosition = readPosition;
    callbacks.m_getNormal = readNormal;
    callbacks.m_getTexCoord = readUv;
    callbacks.m_setTSpaceBasic = writeTangent;

    const SMikkTSpaceContext context{&callbacks, &job};

    if (!genTangSpaceDefault(&context)) {
        throw std::runtime_error("MikkTSpace could not generate tangents");
    }

    // MikkTSpace's tangents lie across the normal already. Where the UVs give it no direction (a patch squashed to a line)
    // it hands back a placeholder, which is made to lie across the normal too: the shader needs a frame everywhere.
    for (MeshVertex& corner : corners) {
        const Vec3 tangent = acrossNormal(Vec3{corner.tangent.x, corner.tangent.y, corner.tangent.z}, corner.normal)
            .value_or(anyTangentAcross(corner.normal));
        corner.tangent = Vec4{tangent.x, tangent.y, tangent.z, corner.tangent.w};
    }
}
