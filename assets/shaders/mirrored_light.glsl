// Mirrored lights (render/gpu/mirrored_light.h): light reaching a point by way of one reflection off a smooth shiny
// surface, found by manifold next event estimation. Pure functions, mirrored by render/gpu/mirrored_light.cpp and
// checked against it by the GPU tests; the search's loop and its rays are in mirrored_light.comp. 2 x 2 matrices in a
// tangent frame are vec4s (xx, xy, yx, yy): row tangent, column bitangent, as TangentMatrix.

#ifndef MIRRORED_LIGHT_GLSL
#define MIRRORED_LIGHT_GLSL

#include "render/gpu/mirrored_light_shared.h"
#include "lighting.glsl"

struct TangentFrame {
    vec3 normal;
    vec3 tangent;
    vec3 bitangent;
};

// Duff et al. 2017: a frame round a unit normal with no branch on it and no break near any axis.
TangentFrame tangentFrame(vec3 normal) {
    float sign = normal.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + normal.z);
    float b = normal.x * normal.y * a;
    vec3 tangent = vec3(1.0 + sign * normal.x * normal.x * a, sign * b, -sign * normal.x);
    vec3 bitangent = vec3(b, sign + normal.y * normal.y * a, -normal.y);
    return TangentFrame(normal, tangent, bitangent);
}

float tangentDeterminant(vec4 matrix) {
    return matrix.x * matrix.w - matrix.y * matrix.z;
}

// How the smoothed triangle's unit normal turns as a point moves over it, in the frame's tangents (triangleCurvature).
vec4 triangleCurvature(vec3 edge1, vec3 edge2, vec3 normalChange1, vec3 normalChange2, float normalLength, TangentFrame frame) {
    float e11 = dot(edge1, edge1);
    float e12 = dot(edge1, edge2);
    float e22 = dot(edge2, edge2);
    float gram = e11 * e22 - e12 * e12;

    if (!(gram > 0.0 && normalLength > 0.0)) {
        return vec4(0.0);
    }

    float tangent1 = dot(edge1, frame.tangent);
    float tangent2 = dot(edge2, frame.tangent);
    vec3 alongTangent = (normalChange1 * ((e22 * tangent1 - e12 * tangent2) / gram) + normalChange2 * ((e11 * tangent2 - e12 * tangent1) / gram)) *
                        (1.0 / normalLength);
    float bitangent1 = dot(edge1, frame.bitangent);
    float bitangent2 = dot(edge2, frame.bitangent);
    vec3 alongBitangent = (normalChange1 * ((e22 * bitangent1 - e12 * bitangent2) / gram) + normalChange2 * ((e11 * bitangent2 - e12 * bitangent1) / gram)) *
                          (1.0 / normalLength);
    float twist = 0.5 * (dot(frame.tangent, alongBitangent) + dot(frame.bitangent, alongTangent));
    return vec4(dot(frame.tangent, alongTangent), twist, twist, dot(frame.bitangent, alongBitangent));
}

// A point on a shiny surface: where it is, its frame, and how it curves there.
struct MirrorSurface {
    vec3 position;
    TangentFrame frame;
    vec4 curvature;
};

// The two unit vectors from the point of reflection and how far each end is.
struct MirrorLegs {
    vec3 toReceiver;
    vec3 toLight;
    float receiverDistance;
    float lightDistance;
};

MirrorLegs mirrorLegs(vec3 receiver, vec3 light, vec3 position) {
    vec3 toReceiver = receiver - position;
    vec3 toLight = light - position;
    float receiverDistance = length(toReceiver);
    float lightDistance = length(toLight);
    return MirrorLegs(toReceiver * (1.0 / receiverDistance), toLight * (1.0 / lightDistance), receiverDistance, lightDistance);
}

// u . P v, P = (I - wx wx^T) / dx + (I - wL wL^T) / dL.
float legsProjection(MirrorLegs legs, vec3 u, vec3 v) {
    float alongBoth = dot(u, v);
    return (alongBoth - dot(legs.toReceiver, u) * dot(legs.toReceiver, v)) / legs.receiverDistance +
           (alongBoth - dot(legs.toLight, u) * dot(legs.toLight, v)) / legs.lightDistance;
}

// The half vector's components along the tangents: zero where the law of reflection holds.
vec2 mirrorConstraint(vec3 receiver, vec3 light, MirrorSurface surface) {
    MirrorLegs legs = mirrorLegs(receiver, light, surface.position);
    vec3 halfway = legs.toReceiver + legs.toLight;
    return vec2(dot(halfway, surface.frame.tangent), dot(halfway, surface.frame.bitangent));
}

// The constraint's change as the point moves in its tangents (mirrorJacobian).
vec4 mirrorJacobian(vec3 receiver, vec3 light, MirrorSurface surface) {
    MirrorLegs legs = mirrorLegs(receiver, light, surface.position);
    float alongNormal = dot(legs.toReceiver + legs.toLight, surface.frame.normal);
    vec3 t = surface.frame.tangent;
    vec3 b = surface.frame.bitangent;
    vec4 s = surface.curvature;
    return vec4(-legsProjection(legs, t, t) - alongNormal * s.x, -legsProjection(legs, t, b) - alongNormal * s.y,
                -legsProjection(legs, b, t) - alongNormal * s.z, -legsProjection(legs, b, b) - alongNormal * s.w);
}

// Newton's next guess; false where the Jacobian is singular (a focus).
bool mirrorNewtonTarget(vec3 receiver, vec3 light, MirrorSurface surface, out vec3 target) {
    target = surface.position;
    vec4 jacobian = mirrorJacobian(receiver, light, surface);
    float det = tangentDeterminant(jacobian);

    if (!(abs(det) > 0.0)) {
        return false;
    }

    vec2 constraint = mirrorConstraint(receiver, light, surface);
    float stepTangent = -(jacobian.w * constraint.x - jacobian.y * constraint.y) / det;
    float stepBitangent = -(jacobian.x * constraint.y - jacobian.z * constraint.x) / det;

    if (isinf(stepTangent) || isnan(stepTangent) || isinf(stepBitangent) || isnan(stepBitangent)) {
        return false;
    }

    target = surface.position + surface.frame.tangent * stepTangent + surface.frame.bitangent * stepBitangent;
    return true;
}

// The point of reflection off the plane through point with this normal; false unless both ends are in front of it.
bool planeMirrorPoint(vec3 receiver, vec3 light, vec3 point, vec3 normal, out vec3 mirrorPoint) {
    float receiverHeight = dot(receiver - point, normal);
    float lightHeight = dot(light - point, normal);
    mirrorPoint = point;

    if (!(receiverHeight > 0.0 && lightHeight > 0.0)) {
        return false;
    }

    vec3 image = light - normal * (2.0 * lightHeight);
    mirrorPoint = receiver + (image - receiver) * (receiverHeight / (receiverHeight + lightHeight));
    return true;
}

// dx^2 dL^2 |det J| / cos^2 theta: what the squared distance is for a direct light.
float mirrorSpreadArea(vec3 receiver, vec3 light, MirrorSurface surface) {
    MirrorLegs legs = mirrorLegs(receiver, light, surface.position);
    float incidence = dot(legs.toReceiver, surface.frame.normal);
    float squared = legs.receiverDistance * legs.lightDistance;
    return squared * squared * abs(tangentDeterminant(mirrorJacobian(receiver, light, surface))) / (incidence * incidence);
}

// Whether a surface curves less than MIRROR_FLAT_CURVATURE every way there, so the flat mirror step is exact.
bool isFlatMirror(vec4 curvature) {
    return all(lessThan(abs(curvature), vec4(float(MIRROR_FLAT_CURVATURE))));
}

// The cosine of the angle of incidence: negative with the receiver behind the surface.
float mirrorIncidence(vec3 receiver, MirrorSurface surface) {
    return dot(normalize(receiver - surface.position), surface.frame.normal);
}

// The intensity times the reflectance, the range's window on the unfolded path squared, over the spread area plus the
// source's size squared.
vec3 mirroredIrradiance(vec3 intensity, vec3 reflectance, float spreadArea, float pathLength, float range, float sourceRadius) {
    float window = rangeWindow(pathLength, range);
    return intensity * reflectance * (window * window / (spreadArea + sourceRadius * sourceRadius));
}

// Whether the shortest path from the light to the receiver by way of the sphere is within the light's range.
bool mirrorPairMayReach(vec3 receiver, vec3 centre, float radius, vec3 light, float range) {
    float shortest = max(length(receiver - centre) - radius, 0.0) + max(length(light - centre) - radius, 0.0);
    return shortest < range;
}

// The share of a surface's mirrored light kept at its roughness: all up to limit, fading out over MIRROR_ROUGHNESS_FADE.
float mirrorRoughnessFade(float roughness, float limit) {
    return 1.0 - smoothstep(limit, limit + float(MIRROR_ROUGHNESS_FADE), roughness);
}

// Whether a surface this rough reflects a light from a footprint rather than its one point of reflection.
bool usesMirrorFootprint(float roughness) {
    return roughness > float(MIRROR_FOOTPRINT_ROUGHNESS);
}

// Point index of count of a standard 2D normal for a frame's turn and stratum (gaussianSamplePoint): stratified radii,
// angles spread evenly round.
vec2 gaussianSamplePoint(int index, int count, float turn, float stratum) {
    float share = (float(index) + stratum) / float(count);
    float radius = sqrt(-2.0 * log(1.0 - share));
    float angle = 6.2831853 * (turn + float(index) / float(count));
    return vec2(radius * cos(angle), radius * sin(angle));
}

// A rough mirror's footprint round a point of reflection (MirrorFootprint): offsetMatrix is (xx, xy, yy), the upper
// triangular square root of its covariance; a step's weight is weightScale exp(|d|^2 inverseSizeSquared / 2).
struct MirrorFootprint {
    vec3 offsetMatrix;
    float weightScale;
    float inverseSizeSquared;
};

// The precision J^T J / s^2 + I / R^2 and its Cholesky factor; the determinant from det(A + cI) = det A + c tr A + c^2,
// so a near-singular J does not cancel its digits away (mirrorFootprint).
MirrorFootprint mirrorFootprint(vec3 receiver, vec3 light, MirrorSurface surface, float alpha, float emitterRadius, float mirrorSize) {
    MirrorLegs legs = mirrorLegs(receiver, light, surface.position);
    float along = dot(legs.toReceiver + legs.toLight, surface.frame.normal);
    float sourceSpread = emitterRadius / legs.lightDistance;
    float spread = along * along * alpha * alpha * 0.5 + 0.25 * sourceSpread * sourceSpread;
    float inverseSizeSquared = 1.0 / (mirrorSize * mirrorSize);

    vec4 j = mirrorJacobian(receiver, light, surface);
    float squaredXX = (j.x * j.x + j.z * j.z) / spread;
    float squaredXY = (j.x * j.y + j.z * j.w) / spread;
    float squaredYY = (j.y * j.y + j.w * j.w) / spread;
    float det = tangentDeterminant(j);
    float precisionXX = squaredXX + inverseSizeSquared;
    float precisionDet = det * det / (spread * spread) + inverseSizeSquared * (squaredXX + squaredYY) + inverseSizeSquared * inverseSizeSquared;

    float l11 = sqrt(precisionXX);
    float l21 = squaredXY / l11;
    float l22 = sqrt(precisionDet / precisionXX);
    return MirrorFootprint(vec3(1.0 / l11, -l21 / (l11 * l22), 1.0 / l22), 1.0 / (spread * sqrt(precisionDet)), inverseSizeSquared);
}

// A standard normal pair's step from the point of reflection along the surface's tangent and bitangent.
vec2 footprintOffset(MirrorFootprint footprint, vec2 standardNormal) {
    return vec2(footprint.offsetMatrix.x * standardNormal.x + footprint.offsetMatrix.y * standardNormal.y, footprint.offsetMatrix.z * standardNormal.y);
}

float footprintWeight(MirrorFootprint footprint, vec2 offset) {
    return footprint.weightScale * exp(0.5 * dot(offset, offset) * footprint.inverseSizeSquared);
}

// A rough mirror's light: intensity times reflectance and the range's window, times (h.n)^2 / (4 dx^2 dL^2) times the
// footprint's mean weight (roughMirroredIrradiance).
vec3 roughMirroredIrradiance(vec3 intensity, vec3 reflectance, vec3 receiver, vec3 light, MirrorSurface surface, float meanWeight, float range) {
    MirrorLegs legs = mirrorLegs(receiver, light, surface.position);
    float along = dot(legs.toReceiver + legs.toLight, surface.frame.normal);
    float window = rangeWindow(legs.receiverDistance + legs.lightDistance, range);
    float distances = legs.receiverDistance * legs.lightDistance;
    return intensity * reflectance * (window * window * along * along * meanWeight / (4.0 * distances * distances));
}

#endif
