#include "scene/liana_shape.h"

#include "core/hash.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
    // Holds closer than this across hang straight (catenaryStrand's limit too).
    constexpr double straightAcross = 1e-3;

    // The ivy's numbers (scene/liana_shared.h).
    constexpr int leafColumns = LIANA_LEAF_COLUMNS;
    constexpr int leafRows = LIANA_LEAF_ROWS;
    constexpr float leafWidthShare = static_cast<float>(LIANA_LEAF_WIDTH_SHARE);
    constexpr float leafDroop = static_cast<float>(LIANA_LEAF_DROOP);
    constexpr float leafJitter = static_cast<float>(LIANA_LEAF_JITTER);
    constexpr float leafFadeBand = static_cast<float>(LIANA_LEAF_FADE_BAND);
    constexpr std::uint32_t ivyBit = LIANA_IVY_BIT;
    constexpr std::uint32_t leafHashUse = LIANA_LEAF_HASH_USE;

    // A hashed word as a share 0..1: its top 24 bits, which a float holds exactly.
    float unitOf(std::uint32_t word) {
        return static_cast<float>(word >> 8u) * (1.0f / 16777216.0f);
    }

    // A card's three random words: its stray, its turn round the liana, its leaf.
    std::array<std::uint32_t, 3> leafHash(const LianaShape& shape, float card) {
        return pcg3d({static_cast<std::uint32_t>(shape.leafLook[3]), static_cast<std::uint32_t>(card), leafHashUse});
    }

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
                          const LianaTree* second) {
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
    setFour(shape.firstTree, first.foot, 0.0f);
    setFour(shape.secondTree, second != nullptr ? second->foot : to, 0.0f);
    const auto anchorOf = [](const LianaTree* tree) {
        return tree != nullptr && tree->layer != LIANA_NO_TREE ? packSwayBinding(tree->binding) : swayUnbound;
    };
    shape.anchors[0] = anchorOf(&first);
    shape.anchors[1] = anchorOf(second);
    shape.anchors[2] = first.layer;
    shape.anchors[3] = second != nullptr ? second->layer : LIANA_NO_TREE;

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

namespace {
    /*
    * The box: along the run from reach before the first hold to reach past the far end, up from reach below the curve's
    * lowest point (where its slope passes 0, else the lower hold) to reach above the higher hold (a catenary is highest
    * at an end), reach either side.
    */
    Transform poseReaching(const LianaShape& shape, float radius) {
        const Vec3 from = fromOf(shape);
        const Vec3 offset = toOf(shape) - from;
        const Vec3 run = lianaRun(shape);
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
}

// The tube reaches a radius out from its centre line.
Transform lianaPose(const LianaShape& shape) {
    return poseReaching(shape, shape.from[3]);
}

// A card's corner lies at most its grown length (sqrt 2 leaves) along and half its width across from its stem, which is
// a radius out.
Transform lianaLeafPose(const LianaShape& shape) {
    const float longest = shape.leafLook[0] * std::numbers::sqrt2_v<float>;
    return poseReaching(shape, shape.from[3] + longest * std::hypot(1.0f, 0.5f * leafWidthShare));
}

void setLianaLeaves(LianaShape& shape, const LianaLeaves& leaves) {
    const float length = shape.to[3];
    const float first = std::clamp(leaves.firstCrownMetres, 0.0f, length);
    shape.leaves[0] = first;
    shape.leaves[1] = std::clamp(leaves.lastCrownMetres, 0.0f, length - first);
    shape.leaves[2] = leaves.spacing;
    shape.leaves[3] = leaves.sparseSpacing;
    shape.leafLook[0] = leaves.size;
    shape.leafLook[1] = leaves.thinFrom;
    shape.leafLook[2] = leaves.goneBy;
    shape.leafLook[3] = static_cast<float>(leaves.seed & 0xffffffu);
}

float lianaLeafCount(const LianaShape& shape) {
    if (shape.leafLook[0] <= 0.0f || shape.leaves[2] <= 0.0f || shape.leaves[3] <= 0.0f) {
        return 0.0f;
    }

    const float crowns = shape.leaves[0] + shape.leaves[1];
    const float rest = std::max(shape.to[3] - crowns, 0.0f);
    return crowns / shape.leaves[2] + rest / shape.leaves[3];
}

// C(s) rises 1 / spacing a metre through the first crown, 1 / sparse spacing along the rest, 1 / spacing through the
// last; t is turned back into metres on the piece it falls in.
float lianaLeafPlace(const LianaShape& shape, float card, float jitter) {
    const float count = lianaLeafCount(shape);
    const float cards = std::ceil(count);

    if (cards <= 0.0f) {
        return 0.0f;
    }

    const float t = (card + 0.5f + leafJitter * (jitter - 0.5f)) * (count / cards);
    const float firstLeaves = shape.leaves[0] / shape.leaves[2];
    const float rest = std::max(shape.to[3] - shape.leaves[0] - shape.leaves[1], 0.0f);
    const float restLeaves = rest / shape.leaves[3];
    float metres = 0.0f;

    if (t < firstLeaves) {
        metres = t * shape.leaves[2];
    } else if (t < firstLeaves + restLeaves) {
        metres = shape.leaves[0] + (t - firstLeaves) * shape.leaves[3];
    } else {
        metres = shape.leaves[0] + rest + (t - firstLeaves - restLeaves) * shape.leaves[2];
    }

    return std::clamp(metres, 0.0f, shape.to[3]);
}

float lianaLeafGrowth(const LianaShape& shape, float card, float distance) {
    if (card >= std::ceil(lianaLeafCount(shape))) {
        return 0.0f;
    }

    float growth = 1.0f;
    const float thinFrom = shape.leafLook[1];

    if (thinFrom > 0.0f) {
        const float thinned = std::clamp((distance - thinFrom) / (leafFadeBand * thinFrom), 0.0f, 1.0f);
        const bool kept = std::fmod(card, 2.0f) < 1.0f;
        growth = kept ? 1.0f + (std::numbers::sqrt2_v<float> - 1.0f) * thinned : 1.0f - thinned;
    }

    const float goneBy = shape.leafLook[2];

    if (goneBy > 0.0f) {
        const float gone = std::clamp((distance - (1.0f - leafFadeBand) * goneBy) / (leafFadeBand * goneBy), 0.0f, 1.0f);
        growth *= 1.0f - gone;
    }

    return growth;
}

/*
* Its stem a radius out from the line at a random turn round it, in the ring's frame (the level side and the side cross
* the line, as lianaVertex's); the card from there out along the turn, dropped by the droop; its side square to the line
* and to that, so it lies along the vine; its normal side x out, so the corners in the template's order turn
* counterclockwise seen from the front, as appendLeafCard's.
*/
LianaLeafVertex lianaLeafVertex(const LianaShape& shape, float card, float across, float up, const Vec3& eye) {
    const std::array<std::uint32_t, 3> hash = leafHash(shape, card);
    const float length = shape.to[3];
    const float metres = lianaLeafPlace(shape, card, unitOf(hash[0]));
    const float share = length > 0.0f ? metres / length : 0.0f;
    const LianaPoint centre = lianaCentre(shape, share);
    const Vec3 run = lianaRun(shape);
    const Vec3 side{-run.z, 0.0f, run.x};
    const Vec3 below = centre.along.cross(side);
    const float turn = 2.0f * std::numbers::pi_v<float> * unitOf(hash[1]);
    const Vec3 radial = side * std::cos(turn) + below * std::sin(turn);
    const Vec3 out = (radial + Vec3{0.0f, -leafDroop, 0.0f}).normalized();
    const Vec3 crossing = centre.along.cross(out);
    const Vec3 cardSide = crossing.lengthSquared() > 1e-12f ? crossing.normalized() : side;
    const Vec3 stem = centre.offset + radial * shape.from[3];
    const float growth = lianaLeafGrowth(shape, card, (fromOf(shape) + stem - eye).length());
    const float size = shape.leafLook[0] * growth;
    const float width = leafWidthShare * size;
    const int cells = leafColumns * leafRows;
    const int cell = std::min(static_cast<int>(unitOf(hash[2]) * static_cast<float>(cells)), cells - 1);
    LianaLeafVertex vertex;
    vertex.offset = stem + cardSide * (width * (across - 0.5f)) + out * (size * up);
    vertex.stem = stem;
    vertex.normal = cardSide.cross(out);
    vertex.tangent = cardSide;
    vertex.uv = Vec2{(static_cast<float>(cell % leafColumns) + across) / static_cast<float>(leafColumns),
                     (static_cast<float>(cell / leafColumns) + up) / static_cast<float>(leafRows)};
    vertex.along = share;
    vertex.arm = armBelowChord(centre.offset, toOf(shape) - fromOf(shape));
    return vertex;
}

IndexedMesh lianaLeafTemplateMesh(int cards) {
    IndexedMesh mesh;
    const Vec2 corners[4] = {Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{1.0f, 1.0f}, Vec2{0.0f, 1.0f}};

    for (int card = 0; card < cards; ++card) {
        const float along = cards > 1 ? static_cast<float>(card) / static_cast<float>(cards - 1) : 0.5f;
        const std::uint32_t first = static_cast<std::uint32_t>(mesh.vertices.size());

        for (const Vec2& corner : corners) {
            mesh.vertices.push_back(MeshVertex{Vec3{along, 2.0f * corner.x - 1.0f, 2.0f * corner.y - 1.0f}, Vec3{1.0f, 0.0f, 0.0f},
                                               Vec2{corner.x + 2.0f * static_cast<float>(card), corner.y}, Vec4{0.0f, 1.0f, 0.0f, 1.0f}});
        }

        mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
    }

    return mesh;
}

std::size_t lianaLeafTemplate(float count) {
    for (std::size_t size = 0; size < lianaLeafTemplateCards.size(); ++size) {
        if (count <= static_cast<float>(lianaLeafTemplateCards[size])) {
            return size;
        }
    }

    return lianaLeafTemplateCards.size() - 1;
}

std::uint32_t lianaRecordOf(std::uint32_t named) {
    return (named & ~ivyBit) - 1u;
}

bool lianaDrawsIvy(std::uint32_t named) {
    return (named & ivyBit) != 0u;
}
