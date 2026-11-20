// A liana's tube bent onto its catenary (scene/liana_shape.h mirrors every function and is tested against it): its
// centre line at a share of its length, and a template vertex placed round it. Pure maths, read by the scene's vertex
// shaders through liana_data.glsl.
#ifndef LIANA_GLSL
#define LIANA_GLSL

#include "wind.glsl"

// One liana. Must match LianaShape (scene/liana_shape.h).
struct LianaShape {
    vec4 from;       // xyz the first hold, w the radius
    vec4 to;         // xyz the far end, w the length along it
    vec4 curve;      // x a (0 straight), y u0 (the slope at the first hold), z its deepest sag below its chord, w the bark's tile in metres
    vec4 firstTree;  // xyz the first tree's foot, w its height in its mesh's units
    vec4 secondTree; // xyz the second tree's foot, w its height
    vec4 trees;      // x the first tree's scale, y the second's (0: fixed), z the trees' bend
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
