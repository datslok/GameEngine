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

bool canBeDrawn(const DirectionalLight& light) {
    return light.direction.lengthSquared() >= 1e-12f;
}

bool canBeDrawn(const PlacedPointLight& placed) {
    return placed.light.range > 0.0f;
}

bool canBeDrawn(const PlacedSpotLight& placed) {
    return placed.light.range > 0.0f && placed.light.direction.lengthSquared() >= 1e-12f;
}

/*
* Lights the shader cannot use are skipped, and the first lights in the lists win when there are more than it has room for.
* The renderer ranks the lists first (prioritizeLights), so by the time they arrive here they fit and this keeps their order.
*/
FrameLighting selectDrawableLights(const FrameLighting& lighting) {
    FrameLighting selected;
    selected.ambient = lighting.ambient;

    for (const DirectionalLight& light : lighting.directionalLights) {
        if (static_cast<int>(selected.directionalLights.size()) == maxDirectionalLights) {
            break;
        }

        if (canBeDrawn(light)) {
            selected.directionalLights.push_back(light);
        }
    }

    for (const PlacedPointLight& placed : lighting.pointLights) {
        if (static_cast<int>(selected.pointLights.size()) == maxPointLights) {
            break;
        }

        if (canBeDrawn(placed)) {
            selected.pointLights.push_back(placed);
        }
    }

    for (const PlacedSpotLight& placed : lighting.spotLights) {
        if (static_cast<int>(selected.spotLights.size()) == maxSpotLights) {
            break;
        }

        if (canBeDrawn(placed)) {
            selected.spotLights.push_back(placed);
        }
    }

    return selected;
}

/*
* Work that is the same for every pixel is done here once per frame: colour times intensity, and normalising and flipping the
* direction (the shader's N dot L needs the direction towards the light).
*/
LightUniformData packLighting(const FrameLighting& lighting, const Vec3& cameraPosition) {
    const FrameLighting selected = selectDrawableLights(lighting);
    LightUniformData data{};

    writeVector(data.ambient, selected.ambient, 0.0f);
    writeVector(data.cameraPosition, cameraPosition, 0.0f);

    for (std::size_t i = 0; i < selected.directionalLights.size(); ++i) {
        const DirectionalLight& light = selected.directionalLights[i];
        DirectionalLightUniform& slot = data.directional[i];
        writeVector(slot.toLight, light.direction.normalized() * -1.0f, 0.0f);
        writeVector(slot.radiance, light.colour * light.intensity, 0.0f);
    }

    for (std::size_t i = 0; i < selected.pointLights.size(); ++i) {
        const PlacedPointLight& placed = selected.pointLights[i];
        PointLightUniform& slot = data.points[i];
        writeVector(slot.positionRange, placed.position, placed.light.range);

        // The falloff divides by distance^2 + radius^2, so a radius that is not positive falls back to 1.
        const float sourceRadius = placed.light.sourceRadius > 0.0f ? placed.light.sourceRadius : 1.0f;
        writeVector(slot.radiance, placed.light.colour * placed.light.intensity, sourceRadius);
    }

    for (std::size_t i = 0; i < selected.spotLights.size(); ++i) {
        const SpotLight& light = selected.spotLights[i].light;

        // The shader compares cosines (bigger cosine = closer to the axis), so it needs no acos per pixel.
        // An inner cone wider than the outer one would make the soft edge run backwards, so it is clamped.
        const float cosOuter = std::cos(light.outerAngle);
        const float cosInner = std::cos(std::min(light.innerAngle, light.outerAngle));

        SpotLightUniform& slot = data.spots[i];
        writeVector(slot.positionRange, selected.spotLights[i].position, light.range);
        writeVector(slot.directionCosOuter, light.direction.normalized(), cosOuter);
        writeVector(slot.radianceCosInner, light.colour * light.intensity, cosInner);

        // The falloff divides by distance^2 + radius^2, so a radius that is not positive would allow a division by zero at the light.
        slot.sourceRadius[0] = light.sourceRadius > 0.0f ? light.sourceRadius : 1.0f;
    }

    data.counts[0] = static_cast<std::int32_t>(selected.directionalLights.size());
    data.counts[1] = static_cast<std::int32_t>(selected.pointLights.size());
    data.counts[2] = static_cast<std::int32_t>(selected.spotLights.size());

    return data;
}
