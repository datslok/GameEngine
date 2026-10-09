#include "render/gpu/light_uniforms.h"

#include <algorithm>
#include <cmath>

namespace {
    void writeVector(float (&target)[4], const Vec3& value, float w) {
        target[0] = value.x;
        target[1] = value.y;
        target[2] = value.z;
        target[3] = w;
    }
}

/*
* Work that is the same for every pixel is done here once per frame: colour times intensity, and normalising and flipping the
* direction (the shader's N dot L needs the direction towards the light).
* A zero direction cannot be normalised, and a point light without a positive range would make the shader divide by zero, so both are skipped.
* The first lights collected win when there are more than the shader has room for.
*/
LightUniformData packLighting(const FrameLighting& lighting, const Vec3& cameraPosition) {
    LightUniformData data{};

    writeVector(data.ambient, lighting.ambient, 0.0f);
    writeVector(data.cameraPosition, cameraPosition, 0.0f);

    int directionalCount = 0;

    for (const DirectionalLight& light : lighting.directionalLights) {
        if (directionalCount == maxDirectionalLights) {
            break;
        }

        if (light.direction.lengthSquared() < 1e-12f) {
            continue;
        }

        DirectionalLightUniform& slot = data.directional[directionalCount];
        writeVector(slot.toLight, light.direction.normalized() * -1.0f, 0.0f);
        writeVector(slot.radiance, light.colour * light.intensity, 0.0f);
        ++directionalCount;
    }

    int pointCount = 0;
    int shadowedPoint = -1;

    for (const PlacedPointLight& placed : lighting.pointLights) {
        if (pointCount == maxPointLights) {
            break;
        }

        if (placed.light.range <= 0.0f) {
            continue;
        }

        PointLightUniform& slot = data.points[pointCount];
        writeVector(slot.positionRange, placed.position, placed.light.range);
        // The falloff divides by distance^2 + radius^2, so a radius that is not positive falls back to 1.
        const float sourceRadius = placed.light.sourceRadius > 0.0f ? placed.light.sourceRadius : 1.0f;
        writeVector(slot.radiance, placed.light.colour * placed.light.intensity, sourceRadius);

        // There is one shadow map, so the first shadow-casting light gets it.
        if (placed.light.castsShadows && shadowedPoint < 0) {
            shadowedPoint = pointCount;
        }

        ++pointCount;
    }

    int spotCount = 0;

    for (const PlacedSpotLight& placed : lighting.spotLights) {
        if (spotCount == maxSpotLights) {
            break;
        }

        const SpotLight& light = placed.light;

        if (light.range <= 0.0f || light.direction.lengthSquared() < 1e-12f) {
            continue;
        }

        // The shader compares cosines (bigger cosine = closer to the axis), so it needs no acos per pixel.
        // An inner cone wider than the outer one would make the soft edge run backwards, so it is clamped.
        const float cosOuter = std::cos(light.outerAngle);
        const float cosInner = std::cos(std::min(light.innerAngle, light.outerAngle));

        SpotLightUniform& slot = data.spots[spotCount];
        writeVector(slot.positionRange, placed.position, light.range);
        writeVector(slot.directionCosOuter, light.direction.normalized(), cosOuter);
        writeVector(slot.radianceCosInner, light.colour * light.intensity, cosInner);

        // The falloff divides by distance^2 + radius^2, so a radius that is not positive would allow a division by zero at the light.
        slot.sourceRadius[0] = light.sourceRadius > 0.0f ? light.sourceRadius : 1.0f;
        ++spotCount;
    }

    data.counts[0] = directionalCount;
    data.counts[1] = pointCount;
    data.counts[2] = spotCount;
    data.counts[3] = shadowedPoint;

    return data;
}
