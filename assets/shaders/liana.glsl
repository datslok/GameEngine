// A liana's tube bent onto its catenary (scene/liana_shape.h mirrors every function and is tested against it): its
// centre line at a share of its length, a template vertex placed round it, and its ivy's cards. Pure maths, read by the
// scene's vertex shaders through liana_data.glsl.
#ifndef LIANA_GLSL
#define LIANA_GLSL

#include "hash.glsl"
#include "wind.glsl"
#include "scene/liana_shared.h"

// One liana. Must match LianaShape (scene/liana_shape.h).
struct LianaShape {
    vec4 from;       // xyz the first hold, w the radius
    vec4 to;         // xyz the far end, w the length along it
    vec4 curve;      // x a (0 straight), y u0 (the slope at the first hold), z its deepest sag below its chord, w the bark's tile in metres
    vec4 firstTree;  // xyz the first tree's foot, w its height in its mesh's units
    vec4 secondTree; // xyz the second tree's foot, w its height
    vec4 trees;      // x the first tree's scale, y the second's (0: fixed), z the trees' bend
    vec4 leaves;     // x the metres from the first hold inside its crown, y from the far end inside the second's, z the ivy's spacing there, w along the rest
    vec4 leafLook;   // x a leaf's length (0: no ivy), y the distance its cards start thinning, z the distance they are gone by, w its seed
};

// The level direction across the upright plane from the first hold (x when the holds are one above the other) (lianaRun).
vec3 lianaRun(LianaShape shape) {
    vec3 offset = shape.to.xyz - shape.from.xyz;
    vec3 across = vec3(offset.x, 0.0, offset.z);
    float d = length(across);
    return d >= 1e-3 ? across * (1.0 / d) : vec3(1.0, 0.0, 0.0);
}

// A point of the centre line: its offset from the first hold, its unit direction along, the metres along to it.
struct LianaPoint {
    vec3 offset;
    vec3 along;
    float travelled;
};

/*
* The centre line at share (0..1) of the length (lianaCentre): with u the slope there (u0 + s / a), the rise
* s (u + u0) / (c + c0) and the run a asinh(s / a (c0 - u0 (u + u0) / (c + c0))), c = sqrt(1 + u^2), free of
* differences of large numbers; a straight line where a is 0.
*/
LianaPoint lianaCentre(LianaShape shape, float share) {
    vec3 chord = shape.to.xyz - shape.from.xyz;
    float travelled = share * shape.to.w;
    float a = shape.curve.x;
    LianaPoint point;
    point.travelled = travelled;

    if (a == 0.0) {
        float chordLength = length(chord);
        point.offset = chord * share;
        point.along = chordLength > 0.0 ? chord * (1.0 / chordLength) : vec3(0.0, -1.0, 0.0);
        return point;
    }

    vec3 run = lianaRun(shape);
    float u0 = shape.curve.y;
    float c0 = sqrt(1.0 + u0 * u0);
    float step = travelled / a;
    float u = u0 + step;
    float c = sqrt(1.0 + u * u);
    float rise = travelled * (u + u0) / (c + c0);
    float across = a * asinh(step * (c0 - u0 * (u + u0) / (c + c0)));
    point.offset = run * across + vec3(0.0, rise, 0.0);
    point.along = (run + vec3(0.0, u, 0.0)) * (1.0 / c);
    return point;
}

// How far a centre point hangs below the chord from the first hold to the far end (hangBetween's arm).
float lianaArmBelowChord(vec3 offset, vec3 chord) {
    float chordSquared = dot(chord, chord);
    float onChord = chordSquared > 0.0 ? clamp(dot(offset, chord) / chordSquared, 0.0, 1.0) : 0.0;
    return length(offset - chord * onChord);
}

// A vertex of the tube: offset from the first hold, outward normal, tangent round it (+u), texture place, share along
// and arm below the chord.
struct LianaVertex {
    vec3 offset;
    vec3 normal;
    vec3 tangent;
    vec2 uv;
    float along;
    float arm;
};

// A template vertex placed (lianaVertex): its ring square to the line, axes the level side and the side cross the line.
LianaVertex lianaVertex(LianaShape shape, float share, float around, float acrossCos, float acrossSin) {
    LianaPoint centre = lianaCentre(shape, share);
    vec3 run = lianaRun(shape);
    vec3 side = vec3(-run.z, 0.0, run.x);
    vec3 below = cross(centre.along, side);
    vec3 normal = side * acrossCos + below * acrossSin;
    LianaVertex vertex;
    vertex.offset = centre.offset + normal * shape.from.w;
    vertex.normal = normal;
    vertex.tangent = below * acrossCos - side * acrossSin;
    vertex.uv = vec2(around, centre.travelled / shape.curve.w);
    vertex.along = share;
    vertex.arm = lianaArmBelowChord(centre.offset, shape.to.xyz - shape.from.xyz);
    return vertex;
}

// The leaves its ivy holds (lianaLeafCount): the crowns' metres over their spacing plus the rest over its own; 0 without.
float lianaLeafCount(LianaShape shape) {
    if (shape.leafLook.x <= 0.0 || shape.leaves.z <= 0.0 || shape.leaves.w <= 0.0) {
        return 0.0;
    }

    float crowns = shape.leaves.x + shape.leaves.y;
    float rest = max(shape.to.w - crowns, 0.0);
    return crowns / shape.leaves.z + rest / shape.leaves.w;
}

// The metres along it of a card, jitter (0..1) its stray: the inverse of the leaves' count along it (lianaLeafPlace).
float lianaLeafPlace(LianaShape shape, float card, float jitter) {
    float count = lianaLeafCount(shape);
    float cards = ceil(count);

    if (cards <= 0.0) {
        return 0.0;
    }

    float t = (card + 0.5 + float(LIANA_LEAF_JITTER) * (jitter - 0.5)) * (count / cards);
    float firstLeaves = shape.leaves.x / shape.leaves.z;
    float rest = max(shape.to.w - shape.leaves.x - shape.leaves.y, 0.0);
    float restLeaves = rest / shape.leaves.w;
    float metres = 0.0;

    if (t < firstLeaves) {
        metres = t * shape.leaves.z;
    } else if (t < firstLeaves + restLeaves) {
        metres = shape.leaves.x + (t - firstLeaves) * shape.leaves.w;
    } else {
        metres = shape.leaves.x + rest + (t - firstLeaves - restLeaves) * shape.leaves.z;
    }

    return clamp(metres, 0.0, shape.to.w);
}

// How much of its length a card shows with its stem distance metres from the eye (lianaLeafGrowth): every other card
// gone and the rest sqrt 2 long past the thinning distance, all gone by the last; none past the count.
float lianaLeafGrowth(LianaShape shape, float card, float distance) {
    if (card >= ceil(lianaLeafCount(shape))) {
        return 0.0;
    }

    float band = float(LIANA_LEAF_FADE_BAND);
    float growth = 1.0;
    float thinFrom = shape.leafLook.y;

    if (thinFrom > 0.0) {
        float thinned = clamp((distance - thinFrom) / (band * thinFrom), 0.0, 1.0);
        bool kept = mod(card, 2.0) < 1.0;
        growth = kept ? 1.0 + (1.41421356 - 1.0) * thinned : 1.0 - thinned;
    }

    float goneBy = shape.leafLook.z;

    if (goneBy > 0.0) {
        float gone = clamp((distance - (1.0 - band) * goneBy) / (band * goneBy), 0.0, 1.0);
        growth *= 1.0 - gone;
    }

    return growth;
}

// A hashed word as a share 0..1: its top 24 bits.
float lianaUnitOf(uint word) {
    return float(word >> 8u) * (1.0 / 16777216.0);
}

// A corner of an ivy card: offset from the first hold, its stem's, normal, tangent across (+u), atlas place, share along
// and arm below the chord at its stem.
struct LianaLeafVertex {
    vec3 offset;
    vec3 stem;
    vec3 normal;
    vec3 tangent;
    vec2 uv;
    float along;
    float arm;
};

// A corner of an ivy card placed (lianaLeafVertex): across and up the card, grown at its stem's distance from eye.
LianaLeafVertex lianaLeafVertex(LianaShape shape, float card, float across, float up, vec3 eye) {
    uvec3 hash = pcg3d(uvec3(uint(shape.leafLook.w), uint(card), LIANA_LEAF_HASH_USE));
    float len = shape.to.w;
    float metres = lianaLeafPlace(shape, card, lianaUnitOf(hash.x));
    float share = len > 0.0 ? metres / len : 0.0;
    LianaPoint centre = lianaCentre(shape, share);
    vec3 run = lianaRun(shape);
    vec3 side = vec3(-run.z, 0.0, run.x);
    vec3 below = cross(centre.along, side);
    float turn = 6.28318531 * lianaUnitOf(hash.y);
    vec3 radial = side * cos(turn) + below * sin(turn);
    vec3 outward = normalize(radial + vec3(0.0, -float(LIANA_LEAF_DROOP), 0.0));
    vec3 crossing = cross(centre.along, outward);
    vec3 cardSide = dot(crossing, crossing) > 1e-12 ? normalize(crossing) : side;
    vec3 stem = centre.offset + radial * shape.from.w;
    float growth = lianaLeafGrowth(shape, card, length(shape.from.xyz + stem - eye));
    float size = shape.leafLook.x * growth;
    float width = float(LIANA_LEAF_WIDTH_SHARE) * size;
    int cells = LIANA_LEAF_COLUMNS * LIANA_LEAF_ROWS;
    int cell = min(int(lianaUnitOf(hash.z) * float(cells)), cells - 1);
    LianaLeafVertex vertex;
    vertex.offset = stem + cardSide * (width * (across - 0.5)) + outward * (size * up);
    vertex.stem = stem;
    vertex.normal = cross(cardSide, outward);
    vertex.tangent = cardSide;
    vertex.uv = vec2((float(cell % LIANA_LEAF_COLUMNS) + across) / float(LIANA_LEAF_COLUMNS), (float(cell / LIANA_LEAF_COLUMNS) + up) / float(LIANA_LEAF_ROWS));
    vertex.along = share;
    vertex.arm = lianaArmBelowChord(centre.offset, shape.to.xyz - shape.from.xyz);
    return vertex;
}

// A template ivy corner's card and place across and up it, from its uv (across + 2 card, up; lianaLeafTemplateMesh).
LianaLeafVertex lianaLeafTemplateVertex(LianaShape shape, vec2 uv, vec3 eye) {
    float card = floor(uv.x * 0.5);
    return lianaLeafVertex(shape, card, uv.x - 2.0 * card, uv.y, eye);
}

// The liana's anchor in its first tree's crown, or (far) in its second's at its far end (lianaWindAnchor).
WindAnchorRecord lianaWindAnchor(LianaShape shape, bool far) {
    vec4 tree = far ? shape.secondTree : shape.firstTree;
    vec3 point = far ? shape.to.xyz : shape.from.xyz;
    WindAnchorRecord anchor;
    anchor.foot = tree;
    anchor.point = vec4(point, far ? shape.trees.y : shape.trees.x);
    anchor.bend = vec4(shape.trees.z, 0.0, 0.0, 0.0);
    return anchor;
}

#endif
