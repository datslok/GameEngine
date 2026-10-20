#include "render/gpu/atmosphere.h"
#include "render/gpu/ddgi.h"

#include <algorithm>
#include <cmath>

namespace {
    constexpr float pi = 3.14159265358979f;

    Vec3 exponential(const Vec3& value) {
        return Vec3{std::exp(value.x), std::exp(value.y), std::exp(value.z)};
    }

    Vec3 multiply(const Vec3& a, const Vec3& b) {
        return Vec3{a.x * b.x, a.y * b.y, a.z * b.z};
    }

    // Component by component, 0 where the divisor is 0 (no extinction: nothing scatters either).
    Vec3 divide(const Vec3& a, const Vec3& b) {
        return Vec3{b.x > 0.0f ? a.x / b.x : 0.0f, b.y > 0.0f ? a.y / b.y : 0.0f, b.z > 0.0f ? a.z / b.z : 0.0f};
    }

    // A direction at cosZenith to the up, leaning along +x.
    Vec3 directionAt(float cosZenith) {
        return Vec3{std::sqrt(std::max(0.0f, 1.0f - cosZenith * cosZenith)), cosZenith, 0.0f};
    }

    // The distance to the horizon from radius: sqrt(r^2 - b^2), written so the two near-equal squares do not cancel.
    float horizonDistance(float radius, float bottom) {
        return std::sqrt(std::max(0.0f, (radius - bottom) * (radius + bottom)));
    }

    // A unit share onto texel centres from the first to the last (half a texel in from each edge), and back.
    float unitToTexelCentres(float unit, int size) {
        const float texels = static_cast<float>(size);
        return unit * (texels - 1.0f) / texels + 0.5f / texels;
    }

    float texelCentresToUnit(float uv, int size) {
        const float texels = static_cast<float>(size);
        return (uv - 0.5f / texels) * texels / (texels - 1.0f);
    }
}

AtmosphereMedium atmosphereMedium(const Atmosphere& atmosphere, float altitude) {
    const float height = std::max(altitude, 0.0f);
    const float rayleighDensity = std::exp(-height / atmosphere.rayleighScaleHeight);
    const float mieDensity = std::exp(-height / atmosphere.mieScaleHeight);
    const float ozoneDensity = std::max(0.0f, 1.0f - std::abs(height - atmosphere.ozoneCentre) / atmosphere.ozoneHalfWidth);

    AtmosphereMedium medium;
    medium.rayleighScattering = atmosphere.rayleighScattering * rayleighDensity;
    medium.mieScattering = atmosphere.mieScattering * mieDensity;
    medium.scattering = medium.rayleighScattering + Vec3{medium.mieScattering, medium.mieScattering, medium.mieScattering};
    const float mieAbsorption = atmosphere.mieAbsorption * mieDensity;
    medium.extinction = medium.scattering + Vec3{mieAbsorption, mieAbsorption, mieAbsorption} + atmosphere.ozoneAbsorption * ozoneDensity;
    return medium;
}

float rayleighPhase(float cosTheta) {
    return 3.0f / (16.0f * pi) * (1.0f + cosTheta * cosTheta);
}

float henyeyGreensteinPhase(float g, float cosTheta) {
    const float denominator = 1.0f + g * g - 2.0f * g * cosTheta;
    return (1.0f - g * g) / (4.0f * pi * denominator * std::sqrt(denominator));
}

/*
* From a point at radius along a direction at cosZenith to its up, |o + t d|^2 = R^2 is t^2 + 2 b t + c = 0 with
* b = radius cosZenith and c = radius^2 - R^2, written (radius - R)(radius + R): at the planet's radius in kilometres the
* squares are about 4e7, whose float rounding alone is several square kilometres. Working from the radius rather than a
* position also spares the rounding of its length (half a metre at 6,360 km). The root near 0 (a ray grazing the ground
* from just above it) is found from the other, which loses no digits: their product is c.
*/
float rayDistanceToSphere(float radius, float cosZenith, float sphereRadius) {
    const float b = radius * cosZenith;
    const float c = (radius - sphereRadius) * (radius + sphereRadius);
    const float discriminant = b * b - c;

    if (discriminant < 0.0f) {
        return -1.0f;
    }

    const float root = std::sqrt(discriminant);
    const float larger = b <= 0.0f ? -b + root : -b - root;
    const float other = larger != 0.0f ? c / larger : 0.0f;
    const float near = std::min(larger, other);
    const float far = std::max(larger, other);

    if (near >= 0.0f) {
        return near;
    }

    return far >= 0.0f ? far : -1.0f;
}

bool aboveHorizon(const Atmosphere& atmosphere, float radius, float cosZenith) {
    if (radius <= atmosphere.bottomRadius) {
        return cosZenith >= 0.0f;
    }

    return cosZenith >= -horizonDistance(radius, atmosphere.bottomRadius) / radius;
}

MarchSample marchSample(int step, int steps, float rayLength) {
    const float start = static_cast<float>(step) / static_cast<float>(steps);
    const float end = static_cast<float>(step + 1) / static_cast<float>(steps);
    const float near = rayLength * start * start;
    const float far = rayLength * end * end;
    return MarchSample{near + (far - near) * atmosphereSampleOffset, far - near};
}

float atmosphereMarchLength(const Atmosphere& atmosphere, float radius, float cosZenith) {
    const float ground = rayDistanceToSphere(radius, cosZenith, atmosphere.bottomRadius);

    if (ground >= 0.0f) {
        return ground;
    }

    return std::max(rayDistanceToSphere(radius, cosZenith, atmosphere.topRadius), 0.0f);
}

Vec3 atmosphereTransmittance(const Atmosphere& atmosphere, float radius, float cosZenith) {
    if (radius >= atmosphere.topRadius) {
        return Vec3{1.0f, 1.0f, 1.0f};
    }

    if (!aboveHorizon(atmosphere, radius, cosZenith)) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }

    const Vec3 origin{0.0f, radius, 0.0f};
    const Vec3 direction = directionAt(cosZenith);
    const float length = std::max(rayDistanceToSphere(radius, cosZenith, atmosphere.topRadius), 0.0f);
    Vec3 depth{0.0f, 0.0f, 0.0f};

    for (int step = 0; step < transmittanceSteps; ++step) {
        const MarchSample sample = marchSample(step, transmittanceSteps, length);
        const float altitude = (origin + direction * sample.distance).length() - atmosphere.bottomRadius;
        depth = depth + atmosphereMedium(atmosphere, altitude).extinction * sample.stepLength;
    }

    return exponential(depth * -1.0f);
}

/*
* Across: the distance d to space between its least (straight up, top - r) and most (to the horizon and on, rho + H);
* down: rho / H, rho the distance to the horizon from r and H from the top.
*/
Vec2 transmittanceLutUv(const Atmosphere& atmosphere, float radius, float cosZenith) {
    const float topHorizon = horizonDistance(atmosphere.topRadius, atmosphere.bottomRadius);
    const float rho = horizonDistance(radius, atmosphere.bottomRadius);
    const float discriminant = radius * radius * (cosZenith * cosZenith - 1.0f) + atmosphere.topRadius * atmosphere.topRadius;
    const float distance = std::max(0.0f, -radius * cosZenith + std::sqrt(std::max(discriminant, 0.0f)));
    const float least = atmosphere.topRadius - radius;
    const float most = rho + topHorizon;
    return Vec2{(distance - least) / (most - least), rho / topHorizon};
}

AtmosphereRay transmittanceLutRay(const Atmosphere& atmosphere, const Vec2& uv) {
    const float topHorizon = horizonDistance(atmosphere.topRadius, atmosphere.bottomRadius);
    const float rho = topHorizon * uv.y;
    const float radius = std::sqrt(rho * rho + atmosphere.bottomRadius * atmosphere.bottomRadius);
    const float least = atmosphere.topRadius - radius;
    const float most = rho + topHorizon;
    const float distance = least + uv.x * (most - least);
    const float cosZenith = distance == 0.0f ? 1.0f : (topHorizon * topHorizon - rho * rho - distance * distance) / (2.0f * radius * distance);
    return AtmosphereRay{radius, std::clamp(cosZenith, -1.0f, 1.0f)};
}

Vec2 multipleScatteringLutUv(const Atmosphere& atmosphere, float radius, float cosSunZenith) {
    const float altitude = std::clamp((radius - atmosphere.bottomRadius) / (atmosphere.topRadius - atmosphere.bottomRadius), 0.0f, 1.0f);
    const float sun = std::clamp(cosSunZenith * 0.5f + 0.5f, 0.0f, 1.0f);
    return Vec2{unitToTexelCentres(sun, multipleScatteringLutSize), unitToTexelCentres(altitude, multipleScatteringLutSize)};
}

AtmosphereRay multipleScatteringLutRay(const Atmosphere& atmosphere, const Vec2& uv) {
    const float sun = texelCentresToUnit(uv.x, multipleScatteringLutSize);
    const float altitude = texelCentresToUnit(uv.y, multipleScatteringLutSize);
    const float radius = atmosphere.bottomRadius + std::max(altitude * (atmosphere.topRadius - atmosphere.bottomRadius), atmosphereMinAltitude);
    return AtmosphereRay{radius, std::clamp(sun * 2.0f - 1.0f, -1.0f, 1.0f)};
}

/*
* beta is the horizon's angle below the horizontal plane, so the sky runs from the zenith to pi - beta and the ground
* from there to the nadir.
*/
Vec2 skyViewLutUv(const Atmosphere& atmosphere, float viewRadius, const SkyViewDirection& direction) {
    const float beta = std::acos(std::clamp(horizonDistance(viewRadius, atmosphere.bottomRadius) / viewRadius, -1.0f, 1.0f));
    const float horizonZenith = pi - beta;
    const float zenith = std::acos(std::clamp(direction.cosViewZenith, -1.0f, 1.0f));
    float v = 0.0f;

    if (!direction.meetsGround) {
        const float share = std::clamp(zenith / horizonZenith, 0.0f, 1.0f);
        v = (1.0f - std::sqrt(1.0f - share)) * 0.5f;
    } else {
        const float share = std::clamp((zenith - horizonZenith) / beta, 0.0f, 1.0f);
        v = std::sqrt(share) * 0.5f + 0.5f;
    }

    const float u = std::sqrt(std::clamp(-direction.cosLightView * 0.5f + 0.5f, 0.0f, 1.0f));
    return Vec2{unitToTexelCentres(u, skyViewLutWidth), unitToTexelCentres(v, skyViewLutHeight)};
}

SkyViewDirection skyViewLutDirection(const Atmosphere& atmosphere, float viewRadius, const Vec2& uv) {
    const float u = std::clamp(texelCentresToUnit(uv.x, skyViewLutWidth), 0.0f, 1.0f);
    const float v = std::clamp(texelCentresToUnit(uv.y, skyViewLutHeight), 0.0f, 1.0f);
    const float beta = std::acos(std::clamp(horizonDistance(viewRadius, atmosphere.bottomRadius) / viewRadius, -1.0f, 1.0f));
    const float horizonZenith = pi - beta;

    SkyViewDirection direction;

    if (v < 0.5f) {
        const float coord = 1.0f - 2.0f * v;
        direction.cosViewZenith = std::cos(horizonZenith * (1.0f - coord * coord));
        direction.meetsGround = false;
    } else {
        const float coord = 2.0f * v - 1.0f;
        direction.cosViewZenith = std::cos(horizonZenith + beta * coord * coord);
        direction.meetsGround = true;
    }

    direction.cosLightView = -(u * u * 2.0f - 1.0f);
    return direction;
}

SkyViewDirection skyViewLookup(const Atmosphere& atmosphere, float viewRadius, const Vec3& direction, const Vec3& towardsSun) {
    SkyViewDirection lookup;
    lookup.cosViewZenith = direction.y;

    const Vec2 view{direction.x, direction.z};
    const Vec2 sun{towardsSun.x, towardsSun.z};
    const float viewLength = std::sqrt(view.x * view.x + view.y * view.y);
    const float sunLength = std::sqrt(sun.x * sun.x + sun.y * sun.y);
    lookup.cosLightView = viewLength > 1e-6f && sunLength > 1e-6f ? std::clamp((view.x * sun.x + view.y * sun.y) / (viewLength * sunLength), -1.0f, 1.0f)
                                                                  : 1.0f;
    lookup.meetsGround = rayDistanceToSphere(viewRadius, direction.y, atmosphere.bottomRadius) >= 0.0f;
    return lookup;
}

Vec3 skyViewTexelDirection(const SkyViewDirection& direction) {
    const float sinZenith = std::sqrt(std::max(0.0f, 1.0f - direction.cosViewZenith * direction.cosViewZenith));
    const float sinLight = std::sqrt(std::max(0.0f, 1.0f - direction.cosLightView * direction.cosLightView));
    return Vec3{sinZenith * direction.cosLightView, direction.cosViewZenith, sinZenith * sinLight};
}

Vec3 skyViewSunDirection(float cosSunZenith) {
    return directionAt(cosSunZenith);
}

float oneMinusExp(float depth) {
    if (depth < 1e-3f) {
        return depth * (1.0f - depth * (0.5f - depth / 6.0f));
    }

    return 1.0f - std::exp(-depth);
}

void scatteringMarchStep(ScatteringMarch& march, const AtmosphereMedium& medium, const Vec3& source, float stepLength) {
    const Vec3 kept = exponential(medium.extinction * -stepLength);
    const Vec3 lost{oneMinusExp(medium.extinction.x * stepLength), oneMinusExp(medium.extinction.y * stepLength),
                    oneMinusExp(medium.extinction.z * stepLength)};
    const Vec3 scatteredIn = divide(multiply(source, lost), medium.extinction);
    const Vec3 scatteredOnce = divide(multiply(medium.scattering, lost), medium.extinction);
    march.luminance = march.luminance + multiply(march.throughput, scatteredIn);
    march.multipleScatteringAs1 = march.multipleScatteringAs1 + multiply(march.throughput, scatteredOnce);
    march.throughput = multiply(march.throughput, kept);
}

ScatteringMarch multipleScatteringDirection(const Atmosphere& atmosphere, float radius, float cosSunZenith, int index) {
    const Vec3 origin{0.0f, radius, 0.0f};
    const Vec3 sun = directionAt(cosSunZenith);
    const float isotropicPhase = 1.0f / (4.0f * pi);
    const Vec3 direction = sphericalFibonacci(index, multipleScatteringDirections);
    const float ground = rayDistanceToSphere(radius, direction.y, atmosphere.bottomRadius);
    const float length = atmosphereMarchLength(atmosphere, radius, direction.y);
    ScatteringMarch march;

    for (int step = 0; step < multipleScatteringSteps; ++step) {
        const MarchSample sample = marchSample(step, multipleScatteringSteps, length);
        const Vec3 point = origin + direction * sample.distance;
        const float pointRadius = point.length();
        const AtmosphereMedium medium = atmosphereMedium(atmosphere, pointRadius - atmosphere.bottomRadius);
        const Vec3 sunlight = atmosphereTransmittance(atmosphere, pointRadius, (point * (1.0f / pointRadius)).dot(sun));
        scatteringMarchStep(march, medium, multiply(sunlight, medium.scattering) * isotropicPhase, sample.stepLength);
    }

    // The ground the ray ends on sends back the sunlight on it, as a matte surface.
    if (ground >= 0.0f) {
        const Vec3 point = origin + direction * ground;
        const float pointRadius = point.length();
        const float facing = (point * (1.0f / pointRadius)).dot(sun);
        march.luminance = march.luminance + multiply(march.throughput, groundRadiance(atmosphere, atmosphereTransmittance(atmosphere, pointRadius, facing), facing));
    }

    return march;
}

Vec3 multipleScatteringFromSums(const Vec3& luminanceSum, const Vec3& scatteredOnceSum) {
    const float share = 1.0f / static_cast<float>(multipleScatteringDirections);
    return divide(luminanceSum * share, Vec3{1.0f, 1.0f, 1.0f} - scatteredOnceSum * share);
}

Vec3 multipleScatteringTexel(const Atmosphere& atmosphere, float radius, float cosSunZenith) {
    Vec3 luminance{0.0f, 0.0f, 0.0f};
    Vec3 scatteredOnce{0.0f, 0.0f, 0.0f};

    for (int index = 0; index < multipleScatteringDirections; ++index) {
        const ScatteringMarch march = multipleScatteringDirection(atmosphere, radius, cosSunZenith, index);
        luminance = luminance + march.luminance;
        scatteredOnce = scatteredOnce + march.multipleScatteringAs1;
    }

    return multipleScatteringFromSums(luminance, scatteredOnce);
}

AtmosphereLookups exactAtmosphereLookups(const Atmosphere& atmosphere) {
    return AtmosphereLookups{
        [atmosphere](float radius, float cosZenith) { return atmosphereTransmittance(atmosphere, radius, cosZenith); },
        [atmosphere](float radius, float cosSunZenith) { return multipleScatteringTexel(atmosphere, radius, cosSunZenith); }};
}

Vec3 skyStepSource(const Atmosphere& atmosphere, const AtmosphereMedium& medium, const Vec3& sunlight, const Vec3& multipleScattering, float cosTheta,
                   const Vec3& sunIlluminance) {
    const Vec3 phaseScattering = medium.rayleighScattering * rayleighPhase(cosTheta) +
                                 Vec3{1.0f, 1.0f, 1.0f} * (medium.mieScattering * henyeyGreensteinPhase(atmosphere.mieAnisotropy, cosTheta));
    return multiply(sunIlluminance, multiply(sunlight, phaseScattering) + multiply(multipleScattering, medium.scattering));
}

Vec3 groundRadiance(const Atmosphere& atmosphere, const Vec3& sunlight, float facing) {
    return multiply(sunlight, atmosphere.groundAlbedo) * (std::max(facing, 0.0f) / pi);
}

Vec3 skyAmbientDirection(int index) {
    const Vec3 point = sphericalFibonacci(index, skyAmbientDirections);
    return Vec3{point.x, point.z, point.y};
}

Vec3 skyRadiance(const Atmosphere& atmosphere, float viewRadius, const Vec3& direction, const Vec3& towardsSun, const Vec3& sunIlluminance,
                 const AtmosphereLookups& lookups) {
    const Vec3 origin{0.0f, viewRadius, 0.0f};
    const float length = atmosphereMarchLength(atmosphere, viewRadius, direction.y);
    const float cosTheta = direction.dot(towardsSun);
    ScatteringMarch march;

    for (int step = 0; step < skyViewSteps; ++step) {
        const MarchSample sample = marchSample(step, skyViewSteps, length);
        const Vec3 point = origin + direction * sample.distance;
        const float pointRadius = point.length();
        const AtmosphereMedium medium = atmosphereMedium(atmosphere, pointRadius - atmosphere.bottomRadius);
        const float cosSun = (point * (1.0f / pointRadius)).dot(towardsSun);
        const Vec3 source = skyStepSource(atmosphere, medium, lookups.transmittance(pointRadius, cosSun), lookups.multipleScattering(pointRadius, cosSun),
                                          cosTheta, sunIlluminance);
        scatteringMarchStep(march, medium, source, sample.stepLength);
    }

    if (rayDistanceToSphere(viewRadius, direction.y, atmosphere.bottomRadius) >= 0.0f) {
        const Vec3 point = origin + direction * length;
        const float pointRadius = point.length();
        const float facing = (point * (1.0f / pointRadius)).dot(towardsSun);
        const Vec3 sunlight = multiply(sunIlluminance, lookups.transmittance(pointRadius, facing));
        march.luminance = march.luminance + multiply(march.throughput, groundRadiance(atmosphere, sunlight, facing));
    }

    return march.luminance;
}

Vec3 skyAmbient(const std::function<Vec3(const Vec3& direction)>& sky) {
    Vec3 sum{0.0f, 0.0f, 0.0f};

    for (int index = 0; index < skyAmbientDirections; ++index) {
        const Vec3 direction = skyAmbientDirection(index);

        if (direction.y > 0.0f) {
            sum = sum + sky(direction) * direction.y;
        }
    }

    return sum * (4.0f / static_cast<float>(skyAmbientDirections));
}

float atmosphereViewRadius(const Atmosphere& atmosphere, float cameraHeightMetres) {
    return atmosphere.bottomRadius + std::max(atmosphere.groundAltitude + cameraHeightMetres / 1000.0f, atmosphereMinAltitude);
}

DirectionalLight throughAtmosphere(const DirectionalLight& light, const Atmosphere& atmosphere, float viewRadius) {
    DirectionalLight through = light;
    const Vec3 towards = light.direction.normalized() * -1.0f;
    through.colour = multiply(light.colour, atmosphereTransmittance(atmosphere, viewRadius, towards.y));
    return through;
}

int atmosphereSun(std::span<const DirectionalLight> lights) {
    int brightest = -1;
    float strongest = -1.0f;

    for (std::size_t index = 0; index < lights.size(); ++index) {
        const DirectionalLight& light = lights[index];
        const float strength = light.intensity * std::max({light.colour.x, light.colour.y, light.colour.z});

        if (strength > strongest) {
            strongest = strength;
            brightest = static_cast<int>(index);
        }
    }

    return brightest;
}

GpuAtmosphere packAtmosphere(const Atmosphere& atmosphere, const AtmosphereFrame& frame) {
    const Vec3 illuminance = frame.sunLight * pi;
    return GpuAtmosphere{
        {atmosphere.rayleighScattering.x, atmosphere.rayleighScattering.y, atmosphere.rayleighScattering.z, atmosphere.rayleighScaleHeight},
        {atmosphere.mieScattering, atmosphere.mieAbsorption, atmosphere.mieScaleHeight, atmosphere.mieAnisotropy},
        {atmosphere.ozoneAbsorption.x, atmosphere.ozoneAbsorption.y, atmosphere.ozoneAbsorption.z, atmosphere.ozoneCentre},
        {atmosphere.bottomRadius, atmosphere.topRadius, atmosphere.ozoneHalfWidth, frame.viewRadius},
        {atmosphere.groundAlbedo.x, atmosphere.groundAlbedo.y, atmosphere.groundAlbedo.z, 1.0f},
        {frame.towardsSun.x, frame.towardsSun.y, frame.towardsSun.z, std::cos(frame.discAngularRadius)},
        {illuminance.x, illuminance.y, illuminance.z, 0.0f},
        {frame.sunDisc.x, frame.sunDisc.y, frame.sunDisc.z, 0.0f},
        {frame.ambient.x, frame.ambient.y, frame.ambient.z, 0.0f}};
}
