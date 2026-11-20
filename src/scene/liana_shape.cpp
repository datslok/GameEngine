#include "scene/liana_shape.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
    // Holds closer than this across hang straight (catenaryStrand's limit too).
    constexpr double straightAcross = 1e-3;

    // The shares sampled for a liana's deepest sag below its chord.
    constexpr int sagSamples = 64;

    Vec3 fromOf(const LianaShape& shape) {
        return Vec3{shape.from[0], shape.from[1], shape.from[2]};
    }

    Vec3 toOf(const LianaShape& shape) {
        return Vec3{shape.to[0], shape.to[1], shape.to[2]};
    }

    // How far a centre point hangs below the chord from the first hold to the far end (hangBetween's arm).
    float armBelowChord(const Vec3& offset, const Vec3& chord) {
        const float chordSquared = chord.lengthSquared();
        const float onChord = chordSquared > 0.0f ? std::clamp(offset.dot(chord) / chordSquared, 0.0f, 1.0f) : 0.0f;
        return (offset - chord * onChord).length();
    }

    void setFour(float values[4], const Vec3& xyz, float w) {
        values[0] = xyz.x;
        values[1] = xyz.y;
        values[2] = xyz.z;
        values[3] = w;
    }
}

CatenaryFit fitCatenary(double d, double h, double total) {
    const double vertical = std::sqrt(total * total - h * h);
    double low = std::log(d / 1400.0);
    double high = std::log(d * 1e6);

    for (int halving = 0; halving < 200; ++halving) {
        const double middle = 0.5 * (low + high);
        const double a = std::exp(middle);
        (2.0 * a * std::sinh(d / (2.0 * a)) > vertical ? low : high) = middle;
    }

    const double a = std::exp(0.5 * (low + high));
    return CatenaryFit{a, 0.5 * d - a * std::atanh(h / total)};
}

/*
* The slope at the first hold, u0 = sinh(-x0 / a), is all the shaders need beside a: the run and rise from it in
* lianaCentre. Its deepest sag is sampled from the curve as the shaders place it.
*/
LianaShape makeLianaShape(const Vec3& from, const Vec3& to, float length, float radius, float barkTileMetres, const LianaTree& first,
                          const LianaTree* second, float bend) {
    LianaShape shape;
    const Vec3 offset = to - from;
    const double d = Vec3{offset.x, 0.0f, offset.z}.length();
    const double straight = offset.length();
    const bool hangs = d >= straightAcross && length > straight * (1.0 + 1e-6);
    setFour(shape.from, from, radius);
    setFour(shape.to, to, hangs ? length : static_cast<float>(straight));

    if (hangs) {
        const CatenaryFit fit = fitCatenary(d, offset.y, length);
        shape.curve[0] = static_cast<float>(fit.a);
        shape.curve[1] = static_cast<float>(std::sinh(-fit.x0 / fit.a));
    }

    shape.curve[3] = barkTileMetres;
    setFour(shape.firstTree, first.foot, first.height);
    setFour(shape.secondTree, second != nullptr ? second->foot : to, second != nullptr ? second->height : 1.0f);
    shape.trees[0] = first.scale;
    shape.trees[1] = second != nullptr ? second->scale : 0.0f;
    shape.trees[2] = bend;

    float deepest = 0.0f;

    for (int sample = 0; sample <= sagSamples; ++sample) {
        deepest = std::max(deepest, armBelowChord(lianaCentre(shape, static_cast<float>(sample) / sagSamples).offset, offset));
    }

    shape.curve[2] = deepest;
    return shape;
}

Vec3 lianaRun(const LianaShape& shape) {
    const Vec3 offset = toOf(shape) - fromOf(shape);
    const Vec3 across{offset.x, 0.0f, offset.z};
    const float d = across.length();
    return d >= static_cast<float>(straightAcross) ? across * (1.0f / d) : Vec3{1.0f, 0.0f, 0.0f};
}

LianaPoint lianaCentre(const LianaShape& shape, float share) {
    const Vec3 chord = toOf(shape) - fromOf(shape);
    const float total = shape.to[3];
    const float travelled = share * total;
    const float a = shape.curve[0];

    if (a == 0.0f) {
        const float chordLength = chord.length();
        return LianaPoint{chord * share, chordLength > 0.0f ? chord * (1.0f / chordLength) : Vec3{0.0f, -1.0f, 0.0f}, travelled};
    }

    const Vec3 run = lianaRun(shape);
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const float u0 = shape.curve[1];
    const float c0 = std::sqrt(1.0f + u0 * u0);
    const float step = travelled / a;
    const float u = u0 + step;
    const float c = std::sqrt(1.0f + u * u);
    const float rise = travelled * (u + u0) / (c + c0);
    const float across = a * std::asinh(step * (c0 - u0 * (u + u0) / (c + c0)));
    return LianaPoint{run * across + up * rise, (run + up * u) * (1.0f / c), travelled};
}

LianaVertex lianaVertex(const LianaShape& shape, float share, float around, float acrossCos, float acrossSin) {
    const LianaPoint centre = lianaCentre(shape, share);
    const Vec3 run = lianaRun(shape);
    const Vec3 side{-run.z, 0.0f, run.x};
    const Vec3 below = centre.along.cross(side);
    const Vec3 normal = side * acrossCos + below * acrossSin;
    LianaVertex vertex;
    vertex.offset = centre.offset + normal * shape.from[3];
    vertex.normal = normal;
    vertex.tangent = below * acrossCos - side * acrossSin;
    vertex.uv = Vec2{around, centre.travelled / shape.curve[3]};
    vertex.along = share;
    vertex.arm = armBelowChord(centre.offset, toOf(shape) - fromOf(shape));
    return vertex;
}

IndexedMesh lianaTemplateMesh(int rings, int sides) {
    IndexedMesh mesh;
    const std::uint32_t columns = static_cast<std::uint32_t>(sides) + 1;

    for (int ring = 0; ring <= rings; ++ring) {
        const float share = static_cast<float>(ring) / static_cast<float>(rings);

        for (int side = 0; side <= sides; ++side) {
            const float around = static_cast<float>(side) / static_cast<float>(sides);
            const float angle = around * 2.0f * std::numbers::pi_v<float>;
            const float cosine = side == sides ? 1.0f : std::cos(angle);
            const float sine = side == sides ? 0.0f : std::sin(angle);
            mesh.vertices.push_back(MeshVertex{Vec3{share, cosine, sine}, Vec3{0.0f, cosine, sine}, Vec2{around, share}, Vec4{0.0f, -sine, cosine, 1.0f}});
        }
    }

    for (std::uint32_t ring = 0; ring < static_cast<std::uint32_t>(rings); ++ring) {
        for (std::uint32_t side = 0; side < static_cast<std::uint32_t>(sides); ++side) {
            const std::uint32_t first = ring * columns + side;
            const std::uint32_t next = first + columns;
            mesh.indices.insert(mesh.indices.end(), {first, first + 1, next, first + 1, next + 1, next});
        }
    }

    return mesh;
}

/*
* The box: along the run from a radius before the first hold to a radius past the far end, up from a radius below the
* curve's lowest point (where its slope passes 0, else the lower hold) to a radius above the higher hold (a catenary is
* highest at an end), a radius either side.
*/
Transform lianaPose(const LianaShape& shape) {
    const Vec3 from = fromOf(shape);
    const Vec3 offset = toOf(shape) - from;
    const Vec3 run = lianaRun(shape);
    const float radius = shape.from[3];
    const float across = Vec3{offset.x, 0.0f, offset.z}.length();
    const float highest = std::max(0.0f, offset.y);
    float lowest = std::min(0.0f, offset.y);
    const float a = shape.curve[0];

    if (a != 0.0f) {
        const float u0 = shape.curve[1];
        const float uEnd = u0 + shape.to[3] / a;

        if (u0 < 0.0f && uEnd > 0.0f) {
            lowest = std::min(lowest, -a * u0 * u0 / (1.0f + std::sqrt(1.0f + u0 * u0)));
        }
    }

    const float middle = 0.5f * (lowest + highest);
    const float half = 0.5f * (highest - lowest) + radius;
    Transform pose;
    pose.position = from + run * -radius + Vec3{0.0f, middle, 0.0f};
    pose.rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, std::atan2(-run.z, run.x));
    pose.scale = Vec3{across + 2.0f * radius, half, radius};
    return pose;
}
