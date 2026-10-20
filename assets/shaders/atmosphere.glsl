#ifndef ATMOSPHERE_GLSL
#define ATMOSPHERE_GLSL

// The sky and atmosphere (Hillaire 2020). Pure functions, mirrored by render/gpu/atmosphere.h, which explains each; the
// GPU tests check them against it. The table reads live in the passes (atmosphere_*.comp, sky.frag) and in
// scene_sky.glsl.

#include "render/gpu/atmosphere_shared.h"
#include "fibonacci.glsl"

const float ATMOSPHERE_PI = 3.14159265358979;

// Must match GpuAtmosphere in render/gpu/atmosphere.h.
struct Atmosphere {
    vec4 rayleigh;       // rgb scattering per km, w scale height
    vec4 mie;            // x scattering, y absorption, z scale height, w the phase function's g
    vec4 ozone;          // rgb absorption per km, w the layer's centre
    vec4 radii;          // x the planet's, y the top's, z the ozone layer's half width, w the camera's
    vec4 ground;         // rgb albedo, w 1 when there is an atmosphere
    vec4 sun;            // xyz towards the sun, w the cosine of the drawn disc's angular radius
    vec4 sunIlluminance; // rgb pi times the sun's value above the air
    vec4 sunDisc;        // rgb the disc's radiance as drawn
    vec4 ambient;        // rgb the frame's own flat ambient
    vec4 moon;           // xyz towards the moon, w the cosine of the drawn disc's angular radius
    vec4 moonIlluminance; // rgb pi times the moon's value above the air, w the sine of the drawn disc's angular radius
    vec4 moonDisc;       // rgb the disc's radiance as drawn where lommelSeeliger is 1
};

// The lights the sky view table's skies are lit by, in its order (ATMOSPHERE_SKY_VIEW_LIGHTS): the sun, the moon.
vec3 atmosphereLightDirection(Atmosphere atmosphere, int light) {
    return light == 0 ? atmosphere.sun.xyz : atmosphere.moon.xyz;
}

vec3 atmosphereLightIlluminance(Atmosphere atmosphere, int light) {
    return light == 0 ? atmosphere.sunIlluminance.rgb : atmosphere.moonIlluminance.rgb;
}

struct AtmosphereMedium {
    vec3 rayleighScattering;
    float mieScattering;
    vec3 scattering;
    vec3 extinction;
};

AtmosphereMedium atmosphereMedium(Atmosphere atmosphere, float altitude) {
    float height = max(altitude, 0.0);
    float rayleighDensity = exp(-height / atmosphere.rayleigh.w);
    float mieDensity = exp(-height / atmosphere.mie.z);
    float ozoneDensity = max(0.0, 1.0 - abs(height - atmosphere.ozone.w) / atmosphere.radii.z);

    AtmosphereMedium medium;
    medium.rayleighScattering = atmosphere.rayleigh.rgb * rayleighDensity;
    medium.mieScattering = atmosphere.mie.x * mieDensity;
    medium.scattering = medium.rayleighScattering + vec3(medium.mieScattering);
    medium.extinction = medium.scattering + vec3(atmosphere.mie.y * mieDensity) + atmosphere.ozone.rgb * ozoneDensity;
    return medium;
}

float rayleighPhase(float cosTheta) {
    return 3.0 / (16.0 * ATMOSPHERE_PI) * (1.0 + cosTheta * cosTheta);
}

float henyeyGreensteinPhase(float g, float cosTheta) {
    float denominator = 1.0 + g * g - 2.0 * g * cosTheta;
    return (1.0 - g * g) / (4.0 * ATMOSPHERE_PI * denominator * sqrt(denominator));
}

float rayDistanceToSphere(float radius, float cosZenith, float sphereRadius) {
    float b = radius * cosZenith;
    float c = (radius - sphereRadius) * (radius + sphereRadius);
    float discriminant = b * b - c;

    if (discriminant < 0.0) {
        return -1.0;
    }

    float root = sqrt(discriminant);
    float larger = b <= 0.0 ? -b + root : -b - root;
    float other = larger != 0.0 ? c / larger : 0.0;
    float near = min(larger, other);
    float far = max(larger, other);

    if (near >= 0.0) {
        return near;
    }

    return far >= 0.0 ? far : -1.0;
}

float atmosphereHorizonDistance(float radius, float bottom) {
    return sqrt(max(0.0, (radius - bottom) * (radius + bottom)));
}

bool aboveHorizon(Atmosphere atmosphere, float radius, float cosZenith) {
    if (radius <= atmosphere.radii.x) {
        return cosZenith >= 0.0;
    }

    return cosZenith >= -atmosphereHorizonDistance(radius, atmosphere.radii.x) / radius;
}

vec3 atmosphereDirectionAt(float cosZenith) {
    return vec3(sqrt(max(0.0, 1.0 - cosZenith * cosZenith)), cosZenith, 0.0);
}

struct MarchSample {
    float distance;
    float stepLength;
};

MarchSample marchSample(int step, int steps, float rayLength) {
    float start = float(step) / float(steps);
    float end = float(step + 1) / float(steps);
    float near = rayLength * start * start;
    float far = rayLength * end * end;
    return MarchSample(near + (far - near) * ATMOSPHERE_SAMPLE_OFFSET, far - near);
}

float atmosphereMarchLength(Atmosphere atmosphere, float radius, float cosZenith) {
    float ground = rayDistanceToSphere(radius, cosZenith, atmosphere.radii.x);

    if (ground >= 0.0) {
        return ground;
    }

    return max(rayDistanceToSphere(radius, cosZenith, atmosphere.radii.y), 0.0);
}

vec3 atmosphereTransmittance(Atmosphere atmosphere, float radius, float cosZenith) {
    if (radius >= atmosphere.radii.y) {
        return vec3(1.0);
    }

    if (!aboveHorizon(atmosphere, radius, cosZenith)) {
        return vec3(0.0);
    }

    vec3 origin = vec3(0.0, radius, 0.0);
    vec3 direction = atmosphereDirectionAt(cosZenith);
    float rayLength = max(rayDistanceToSphere(radius, cosZenith, atmosphere.radii.y), 0.0);
    vec3 depth = vec3(0.0);

    for (int step = 0; step < ATMOSPHERE_TRANSMITTANCE_STEPS; ++step) {
        MarchSample sampled = marchSample(step, ATMOSPHERE_TRANSMITTANCE_STEPS, rayLength);
        float altitude = length(origin + direction * sampled.distance) - atmosphere.radii.x;
        depth += atmosphereMedium(atmosphere, altitude).extinction * sampled.stepLength;
    }

    return exp(-depth);
}

struct AtmosphereRay {
    float radius;
    float cosZenith;
};

vec2 transmittanceLutUv(Atmosphere atmosphere, float radius, float cosZenith) {
    float topHorizon = atmosphereHorizonDistance(atmosphere.radii.y, atmosphere.radii.x);
    float rho = atmosphereHorizonDistance(radius, atmosphere.radii.x);
    float discriminant = radius * radius * (cosZenith * cosZenith - 1.0) + atmosphere.radii.y * atmosphere.radii.y;
    float distance = max(0.0, -radius * cosZenith + sqrt(max(discriminant, 0.0)));
    float least = atmosphere.radii.y - radius;
    float most = rho + topHorizon;
    return vec2((distance - least) / (most - least), rho / topHorizon);
}

AtmosphereRay transmittanceLutRay(Atmosphere atmosphere, vec2 uv) {
    float topHorizon = atmosphereHorizonDistance(atmosphere.radii.y, atmosphere.radii.x);
    float rho = topHorizon * uv.y;
    float radius = sqrt(rho * rho + atmosphere.radii.x * atmosphere.radii.x);
    float least = atmosphere.radii.y - radius;
    float most = rho + topHorizon;
    float distance = least + uv.x * (most - least);
    float cosZenith = distance == 0.0 ? 1.0 : (topHorizon * topHorizon - rho * rho - distance * distance) / (2.0 * radius * distance);
    return AtmosphereRay(radius, clamp(cosZenith, -1.0, 1.0));
}

float unitToTexelCentres(float unit, int size) {
    float texels = float(size);
    return unit * (texels - 1.0) / texels + 0.5 / texels;
}

float texelCentresToUnit(float uv, int size) {
    float texels = float(size);
    return (uv - 0.5 / texels) * texels / (texels - 1.0);
}

vec2 multipleScatteringLutUv(Atmosphere atmosphere, float radius, float cosSunZenith) {
    float altitude = clamp((radius - atmosphere.radii.x) / (atmosphere.radii.y - atmosphere.radii.x), 0.0, 1.0);
    float sun = clamp(cosSunZenith * 0.5 + 0.5, 0.0, 1.0);
    return vec2(unitToTexelCentres(sun, ATMOSPHERE_MULTIPLE_SCATTERING_SIZE), unitToTexelCentres(altitude, ATMOSPHERE_MULTIPLE_SCATTERING_SIZE));
}

AtmosphereRay multipleScatteringLutRay(Atmosphere atmosphere, vec2 uv) {
    float sun = texelCentresToUnit(uv.x, ATMOSPHERE_MULTIPLE_SCATTERING_SIZE);
    float altitude = texelCentresToUnit(uv.y, ATMOSPHERE_MULTIPLE_SCATTERING_SIZE);
    float radius = atmosphere.radii.x + max(altitude * (atmosphere.radii.y - atmosphere.radii.x), ATMOSPHERE_MIN_ALTITUDE);
    return AtmosphereRay(radius, clamp(sun * 2.0 - 1.0, -1.0, 1.0));
}

struct SkyViewDirection {
    float cosViewZenith;
    float cosLightView;
    bool meetsGround;
};

vec2 skyViewLutUv(Atmosphere atmosphere, float viewRadius, SkyViewDirection direction) {
    float beta = acos(clamp(atmosphereHorizonDistance(viewRadius, atmosphere.radii.x) / viewRadius, -1.0, 1.0));
    float horizonZenith = ATMOSPHERE_PI - beta;
    float zenith = acos(clamp(direction.cosViewZenith, -1.0, 1.0));
    float v;

    if (!direction.meetsGround) {
        float share = clamp(zenith / horizonZenith, 0.0, 1.0);
        v = (1.0 - sqrt(1.0 - share)) * 0.5;
    } else {
        float share = clamp((zenith - horizonZenith) / beta, 0.0, 1.0);
        v = sqrt(share) * 0.5 + 0.5;
    }

    float u = sqrt(clamp(-direction.cosLightView * 0.5 + 0.5, 0.0, 1.0));
    return vec2(unitToTexelCentres(u, ATMOSPHERE_SKY_VIEW_WIDTH), unitToTexelCentres(v, ATMOSPHERE_SKY_VIEW_HEIGHT));
}

SkyViewDirection skyViewLutDirection(Atmosphere atmosphere, float viewRadius, vec2 uv) {
    float u = clamp(texelCentresToUnit(uv.x, ATMOSPHERE_SKY_VIEW_WIDTH), 0.0, 1.0);
    float v = clamp(texelCentresToUnit(uv.y, ATMOSPHERE_SKY_VIEW_HEIGHT), 0.0, 1.0);
    float beta = acos(clamp(atmosphereHorizonDistance(viewRadius, atmosphere.radii.x) / viewRadius, -1.0, 1.0));
    float horizonZenith = ATMOSPHERE_PI - beta;

    SkyViewDirection direction;

    if (v < 0.5) {
        float coord = 1.0 - 2.0 * v;
        direction.cosViewZenith = cos(horizonZenith * (1.0 - coord * coord));
        direction.meetsGround = false;
    } else {
        float coord = 2.0 * v - 1.0;
        direction.cosViewZenith = cos(horizonZenith + beta * coord * coord);
        direction.meetsGround = true;
    }

    direction.cosLightView = -(u * u * 2.0 - 1.0);
    return direction;
}

SkyViewDirection skyViewLookup(Atmosphere atmosphere, float viewRadius, vec3 direction, vec3 towardsSun) {
    SkyViewDirection lookup;
    lookup.cosViewZenith = direction.y;

    vec2 view = direction.xz;
    vec2 sun = towardsSun.xz;
    float viewLength = length(view);
    float sunLength = length(sun);
    lookup.cosLightView = viewLength > 1e-6 && sunLength > 1e-6 ? clamp(dot(view, sun) / (viewLength * sunLength), -1.0, 1.0) : 1.0;
    lookup.meetsGround = rayDistanceToSphere(viewRadius, direction.y, atmosphere.radii.x) >= 0.0;
    return lookup;
}

vec3 skyViewTexelDirection(SkyViewDirection direction) {
    float sinZenith = sqrt(max(0.0, 1.0 - direction.cosViewZenith * direction.cosViewZenith));
    float sinLight = sqrt(max(0.0, 1.0 - direction.cosLightView * direction.cosLightView));
    return vec3(sinZenith * direction.cosLightView, direction.cosViewZenith, sinZenith * sinLight);
}

vec3 skyViewSunDirection(float cosSunZenith) {
    return atmosphereDirectionAt(cosSunZenith);
}

vec2 skyViewAtlasUv(vec2 uv, int light) {
    return vec2(uv.x, (uv.y + float(light)) / float(ATMOSPHERE_SKY_VIEW_LIGHTS));
}

float lommelSeeliger(vec3 direction, vec3 towardsBody, float sinRadius, vec3 towardsLight) {
    float along = dot(direction, towardsBody);

    if (sinRadius <= 0.0 || along <= 0.0) {
        return 0.0;
    }

    vec3 across = (direction - towardsBody * along) * (1.0 / sinRadius);
    float acrossSquared = dot(across, across);

    if (acrossSquared > 1.0) {
        return 0.0;
    }

    float viewCosine = sqrt(1.0 - acrossSquared);
    vec3 normal = across - towardsBody * viewCosine;
    float lightCosine = dot(normal, towardsLight);

    if (lightCosine <= 0.0 || lightCosine + viewCosine <= 0.0) {
        return 0.0;
    }

    return 2.0 * lightCosine / (lightCosine + viewCosine);
}

struct ScatteringMarch {
    vec3 luminance;
    vec3 throughput;
    vec3 multipleScatteringAs1;
};

ScatteringMarch emptyScatteringMarch() {
    return ScatteringMarch(vec3(0.0), vec3(1.0), vec3(0.0));
}

// Component by component, 0 where the divisor is 0.
vec3 atmosphereDivide(vec3 a, vec3 b) {
    return vec3(b.x > 0.0 ? a.x / b.x : 0.0, b.y > 0.0 ? a.y / b.y : 0.0, b.z > 0.0 ? a.z / b.z : 0.0);
}

float oneMinusExp(float depth) {
    if (depth < 1e-3) {
        return depth * (1.0 - depth * (0.5 - depth / 6.0));
    }

    return 1.0 - exp(-depth);
}

void scatteringMarchStep(inout ScatteringMarch march, AtmosphereMedium medium, vec3 source, float stepLength) {
    vec3 kept = exp(-medium.extinction * stepLength);
    vec3 lost = vec3(oneMinusExp(medium.extinction.x * stepLength), oneMinusExp(medium.extinction.y * stepLength),
                     oneMinusExp(medium.extinction.z * stepLength));
    vec3 scatteredIn = atmosphereDivide(source * lost, medium.extinction);
    vec3 scatteredOnce = atmosphereDivide(medium.scattering * lost, medium.extinction);
    march.luminance += march.throughput * scatteredIn;
    march.multipleScatteringAs1 += march.throughput * scatteredOnce;
    march.throughput *= kept;
}

vec3 groundRadiance(Atmosphere atmosphere, vec3 sunlight, float facing) {
    return sunlight * atmosphere.ground.rgb * (max(facing, 0.0) / ATMOSPHERE_PI);
}

ScatteringMarch multipleScatteringDirection(Atmosphere atmosphere, float radius, float cosSunZenith, int index) {
    vec3 origin = vec3(0.0, radius, 0.0);
    vec3 sun = atmosphereDirectionAt(cosSunZenith);
    float isotropicPhase = 1.0 / (4.0 * ATMOSPHERE_PI);
    vec3 direction = sphericalFibonacci(index, ATMOSPHERE_MULTIPLE_SCATTERING_DIRECTIONS);
    float ground = rayDistanceToSphere(radius, direction.y, atmosphere.radii.x);
    float rayLength = atmosphereMarchLength(atmosphere, radius, direction.y);
    ScatteringMarch march = emptyScatteringMarch();

    for (int step = 0; step < ATMOSPHERE_MULTIPLE_SCATTERING_STEPS; ++step) {
        MarchSample sampled = marchSample(step, ATMOSPHERE_MULTIPLE_SCATTERING_STEPS, rayLength);
        vec3 point = origin + direction * sampled.distance;
        float pointRadius = length(point);
        AtmosphereMedium medium = atmosphereMedium(atmosphere, pointRadius - atmosphere.radii.x);
        vec3 sunlight = atmosphereTransmittance(atmosphere, pointRadius, dot(point / pointRadius, sun));
        scatteringMarchStep(march, medium, sunlight * medium.scattering * isotropicPhase, sampled.stepLength);
    }

    if (ground >= 0.0) {
        vec3 point = origin + direction * ground;
        float pointRadius = length(point);
        float facing = dot(point / pointRadius, sun);
        march.luminance += march.throughput * groundRadiance(atmosphere, atmosphereTransmittance(atmosphere, pointRadius, facing), facing);
    }

    return march;
}

vec3 multipleScatteringFromSums(vec3 luminanceSum, vec3 scatteredOnceSum) {
    float share = 1.0 / float(ATMOSPHERE_MULTIPLE_SCATTERING_DIRECTIONS);
    return atmosphereDivide(luminanceSum * share, vec3(1.0) - scatteredOnceSum * share);
}

// The light a sky step gathers per kilometre: the light's through the air, by the phase functions, and the multiple
// scattering (both per unit illuminance), times the light's illuminance.
vec3 skyStepSource(Atmosphere atmosphere, AtmosphereMedium medium, vec3 sunlight, vec3 multipleScattering, float cosTheta, vec3 illuminance) {
    vec3 phaseScattering = medium.rayleighScattering * rayleighPhase(cosTheta) + vec3(medium.mieScattering * henyeyGreensteinPhase(atmosphere.mie.w, cosTheta));
    return illuminance * (sunlight * phaseScattering + multipleScattering * medium.scattering);
}

// The direction number index of the flat ambient's average: the spiral's z, stratified in even steps, as the up.
vec3 skyAmbientDirection(int index) {
    vec3 point = sphericalFibonacci(index, ATMOSPHERE_AMBIENT_DIRECTIONS);
    return vec3(point.x, point.z, point.y);
}

#endif
