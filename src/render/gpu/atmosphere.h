#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/atmosphere_shared.h"
#include "scene/atmosphere.h"
#include "scene/light.h"

#include <functional>
#include <span>

/*
* The sky and atmosphere (Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique"; the
* physics in docs/ENGINE_GUIDE.md). Light crossing the air is dimmed by Beer-Lambert's law (transmittance, exp of
* minus the summed extinction along the way) and scattered by air molecules (Rayleigh: blue far more than red) and
* aerosols (Mie: every colour alike, mostly forward). The sky's light along a view ray is the sunlight scattered towards
* the camera at every point on it, dimmed on its way in and on its way out; light scattered more than once is the
* paper's geometric series, each order a fixed share of the last. Three tables hold the work: the transmittance to space
* (made once), the multiple scattering (made once) and the sky in every direction round the camera (each frame).
*
* Positions are in the planet's frame, in kilometres, from its centre, y up (the engine's axes): the camera stands at
* (0, viewRadius, 0) and world directions are used as they are. This is the API-free part: the medium, the phase
* functions, the tables' coordinates and the march's step, mirrored by assets/shaders/atmosphere.glsl and checked
* against it by the GPU tests, plus whole marches on the CPU as the passes' references.
*/

inline constexpr int transmittanceLutWidth = ATMOSPHERE_TRANSMITTANCE_WIDTH;
inline constexpr int transmittanceLutHeight = ATMOSPHERE_TRANSMITTANCE_HEIGHT;
inline constexpr int transmittanceSteps = ATMOSPHERE_TRANSMITTANCE_STEPS;
inline constexpr int multipleScatteringLutSize = ATMOSPHERE_MULTIPLE_SCATTERING_SIZE;
inline constexpr int multipleScatteringDirections = ATMOSPHERE_MULTIPLE_SCATTERING_DIRECTIONS;
inline constexpr int multipleScatteringSteps = ATMOSPHERE_MULTIPLE_SCATTERING_STEPS;
inline constexpr int skyViewLutWidth = ATMOSPHERE_SKY_VIEW_WIDTH;
inline constexpr int skyViewLutHeight = ATMOSPHERE_SKY_VIEW_HEIGHT;
inline constexpr int skyViewSteps = ATMOSPHERE_SKY_VIEW_STEPS;
inline constexpr float atmosphereSampleOffset = static_cast<float>(ATMOSPHERE_SAMPLE_OFFSET);
inline constexpr int skyAmbientDirections = ATMOSPHERE_AMBIENT_DIRECTIONS;
inline constexpr float atmosphereMinAltitude = static_cast<float>(ATMOSPHERE_MIN_ALTITUDE);

// What the air does at one altitude, per kilometre: the scattering by molecules and by aerosols, both together, and
// the extinction (scattering plus absorption: aerosols and ozone absorb).
struct AtmosphereMedium {
    Vec3 rayleighScattering{0.0f, 0.0f, 0.0f};
    float mieScattering = 0.0f;
    Vec3 scattering{0.0f, 0.0f, 0.0f};
    Vec3 extinction{0.0f, 0.0f, 0.0f};
};

AtmosphereMedium atmosphereMedium(const Atmosphere& atmosphere, float altitude);

// Where a scattered photon goes, per steradian, by the angle from its old direction: Rayleigh's 3 / (16 pi) (1 + cos^2),
// and Henyey-Greenstein's (1 - g^2) / (4 pi (1 + g^2 - 2 g cos)^1.5) for the aerosols' forward lobe.
float rayleighPhase(float cosTheta);
float henyeyGreensteinPhase(float g, float cosTheta);

// The distance from a point at radius from the planet's centre, along a direction at cosZenith to its up, to a sphere
// of sphereRadius round the centre: the way out from inside, the near side from outside, -1 when the ray misses it (or
// it lies behind).
float rayDistanceToSphere(float radius, float cosZenith, float sphereRadius);

// Whether the sun (or anything along cosZenith) is above the planet's horizon seen from radius.
bool aboveHorizon(const Atmosphere& atmosphere, float radius, float cosZenith);

/*
* The light kept along a ray from radius at cosZenith (to the local up) out to space, marched in transmittanceSteps
* steps: 0 when the ray meets the planet, 1 from above the air.
*/
Vec3 atmosphereTransmittance(const Atmosphere& atmosphere, float radius, float cosZenith);

// A ray from inside the air, as the tables' coordinates name it: its radius and the cosine of its angle to the up.
struct AtmosphereRay {
    float radius = 0.0f;
    float cosZenith = 1.0f;
};

/*
* The transmittance table's coordinates (Bruneton and Neyret 2008): across, how far the ray travels to space between
* its shortest and longest possible; down, the radius as the share of the way to the horizon's distance from the top.
* Only rays that leave without meeting the planet.
*/
Vec2 transmittanceLutUv(const Atmosphere& atmosphere, float radius, float cosZenith);
AtmosphereRay transmittanceLutRay(const Atmosphere& atmosphere, const Vec2& uv);

// The multiple scattering table's: across, the sun's cosine to the up (-1 to 1), down, the altitude; both kept off the
// table's edge by half a texel, so its texel centres hold the ends.
Vec2 multipleScatteringLutUv(const Atmosphere& atmosphere, float radius, float cosSunZenith);
AtmosphereRay multipleScatteringLutRay(const Atmosphere& atmosphere, const Vec2& uv);

/*
* A direction as the sky view table names it: the cosine of its angle to the up, the cosine of its bearing from the
* sun's seen from above (the sky is mirror symmetric about the sun's vertical plane, so that is enough), and whether it
* meets the ground.
*/
struct SkyViewDirection {
    float cosViewZenith = 1.0f;
    float cosLightView = 1.0f;
    bool meetsGround = false;
};

/*
* The sky view table's coordinates: down, the zenith angle, the upper half the sky from the zenith to the horizon and
* the lower half below it, each squared towards the horizon (where the colour changes fastest); across, the bearing,
* square-rooted towards the sun. Kept off the edges by half a texel.
*/
Vec2 skyViewLutUv(const Atmosphere& atmosphere, float viewRadius, const SkyViewDirection& direction);
SkyViewDirection skyViewLutDirection(const Atmosphere& atmosphere, float viewRadius, const Vec2& uv);

// A world direction's place in the table, for the camera at viewRadius and the sun towards towardsSun.
SkyViewDirection skyViewLookup(const Atmosphere& atmosphere, float viewRadius, const Vec3& direction, const Vec3& towardsSun);

// The direction a table texel stands for, and the sun's, in the frame where the sun's bearing is +x.
Vec3 skyViewTexelDirection(const SkyViewDirection& direction);
Vec3 skyViewSunDirection(float cosSunZenith);

/*
* The march along a ray: the light gathered so far, how much of what lies further on still reaches the start, and the
* share of light scattered once more (for the multiple scattering table's series).
*/
struct ScatteringMarch {
    Vec3 luminance{0.0f, 0.0f, 0.0f};
    Vec3 throughput{1.0f, 1.0f, 1.0f};
    Vec3 multipleScatteringAs1{0.0f, 0.0f, 0.0f};
};

/*
* One step of stepLength through a medium lit by source (light scattered towards the ray per kilometre): the paper's
* energy-conserving form, which integrates source exp(-extinction t) over the step exactly, (source - source T) /
* extinction with T = exp(-extinction stepLength), so long steps lose no light.
*/
void scatteringMarchStep(ScatteringMarch& march, const AtmosphereMedium& medium, const Vec3& source, float stepLength);

/*
* 1 - exp(-depth), the share of light a step stops, without the cancellation of subtracting from 1 a number that rounds
* to 1: in the thin air near the top a step's depth is about 1e-7, and the GPU's exp returned exactly 1 there, so the
* step gathered nothing. Below 1e-3 the series depth - depth^2 / 2 + depth^3 / 6, exact to the float's last digit.
*/
float oneMinusExp(float depth);

/*
* Where the step-th of steps along a ray of length samples the air, and the step's own length: steps grow as the square
* of their number (the paper's variable sampling), short where the ray starts, so the dense air near the ground is cut
* finely, and each is sampled atmosphereSampleOffset of the way in (the midpoint). With even steps of 2.5 km the 1.2
* km aerosol layer fell within the first, and its optical depth came out a quarter too high.
*/
struct MarchSample {
    float distance = 0.0f;
    float stepLength = 0.0f;
};

MarchSample marchSample(int step, int steps, float rayLength);

// How far a ray from radius at cosZenith runs through the air: to the ground if it meets it, else to the top (0 from
// above the air looking away).
float atmosphereMarchLength(const Atmosphere& atmosphere, float radius, float cosZenith);

/*
* The light a sky step gathers per kilometre: sunIlluminance times the sun's light through the air (sunlight, per unit
* illuminance) by the phase functions at the angle from the sun (cosTheta), plus the multiple scattering times the
* scattering.
*/
Vec3 skyStepSource(const Atmosphere& atmosphere, const AtmosphereMedium& medium, const Vec3& sunlight, const Vec3& multipleScattering, float cosTheta,
                   const Vec3& sunIlluminance);

/*
* The light the planet's ground sends back up, as a matte surface of the atmosphere's albedo: sunlight (the sun's light
* reaching it, per unit illuminance or times it) times the cosine of the sun from its up (facing, none from below), over
* pi. What a ray that ends on the ground sees there.
*/
Vec3 groundRadiance(const Atmosphere& atmosphere, const Vec3& sunlight, float facing);

// The direction number index of the flat ambient's average (skyAmbient): the spiral's z, stratified in even steps, as up.
Vec3 skyAmbientDirection(int index);

// How the CPU's marches read the tables: the transmittance to space and the multiple scattering at a radius and
// cosine, per unit sun illuminance.
struct AtmosphereLookups {
    std::function<Vec3(float radius, float cosZenith)> transmittance;
    std::function<Vec3(float radius, float cosSunZenith)> multipleScattering;
};

/*
* One of the multiple scattering table's directions (index of multipleScatteringDirections, a Fibonacci sphere) from
* radius with the sun at cosSunZenith: the light scattered once towards the point along it, isotropically (1 / 4 pi),
* per unit sun illuminance, plus the ground it ends on as a matte surface lit by the sun; and its share scattered once.
* The sun's light along the way is the exact transmittance. One invocation of the table's pass each.
*/
ScatteringMarch multipleScatteringDirection(const Atmosphere& atmosphere, float radius, float cosSunZenith, int index);

// The texel from its directions' sums: their mean light times 1 / (1 - f), f their mean share scattered once more.
Vec3 multipleScatteringFromSums(const Vec3& luminanceSum, const Vec3& scatteredOnceSum);

/*
* The light scattered twice or more at a point, per unit sun illuminance (the multiple scattering table's texel; the
* paper's Psi_ms): the light scattered once from the air and the ground all round it (multipleScatteringDirections
* directions, isotropic phase), times 1 / (1 - f), f the share of that light scattered once more: the sum of every
* further order, a geometric series. The sun's light along the way is the exact transmittance.
*/
Vec3 multipleScatteringTexel(const Atmosphere& atmosphere, float radius, float cosSunZenith);

// Lookups computed exactly, without tables (the CPU references; slow for the multiple scattering).
AtmosphereLookups exactAtmosphereLookups(const Atmosphere& atmosphere);

/*
* The sky's light along a world direction from viewRadius (the sky view table's texel, in the engine's pixel units):
* skyViewSteps steps, each lit by sunIlluminance (pi times the sun's value above the air) through the transmittance to
* the sun, the phase functions and the multiple scattering, and where it ends on the ground the sunlit ground
* (groundRadiance), dimmed by the air in front of it: without it the planet's surface beyond a level's own ground showed
* as a dark band below the horizon. The sun's disc is not in it.
*/
Vec3 skyRadiance(const Atmosphere& atmosphere, float viewRadius, const Vec3& direction, const Vec3& towardsSun, const Vec3& sunIlluminance,
                 const AtmosphereLookups& lookups);

/*
* The flat ambient light a sky gives (the engine's units: a uniform sky drawn at B gives B): the cosine-weighted mean
* over the sky above, from skyAmbientDirections Fibonacci directions, 4 / N times the sum of sky times the up component
* over those that point up.
*/
Vec3 skyAmbient(const std::function<Vec3(const Vec3& direction)>& sky);

// The camera's radius in the planet's frame for its world height in metres (atmosphereMinAltitude at least).
float atmosphereViewRadius(const Atmosphere& atmosphere, float cameraHeightMetres);

// A directional light as it reaches the camera: its value above the air times what gets through on the way down.
DirectionalLight throughAtmosphere(const DirectionalLight& light, const Atmosphere& atmosphere, float viewRadius);

// The light the sky is lit by: the brightest directional light's index, -1 without one.
int atmosphereSun(std::span<const DirectionalLight> lights);

// This frame's sky as the shaders take it beside the atmosphere's own numbers.
struct AtmosphereFrame {
    float viewRadius = 0.0f;
    Vec3 towardsSun{0.0f, 1.0f, 0.0f};
    Vec3 sunLight{0.0f, 0.0f, 0.0f};  // the sun's value above the air (colour times intensity)
    float discAngularRadius = 0.0f;   // as drawn
    Vec3 sunDisc{0.0f, 0.0f, 0.0f};   // the disc's radiance as drawn, through the air (sky_object.h, discRadiance)
    Vec3 ambient{0.0f, 0.0f, 0.0f};   // the frame's own flat ambient, to which the sky's is added
};

/*
* What every atmosphere pass reads (must match Atmosphere in assets/shaders/atmosphere.glsl; a storage buffer, std430).
*/
struct GpuAtmosphere {
    float rayleigh[4];       // rgb scattering per km, w scale height
    float mie[4];            // x scattering, y absorption, z scale height, w the phase function's g
    float ozone[4];          // rgb absorption per km, w the layer's centre
    float radii[4];          // x the planet's, y the top's, z the ozone layer's half width, w the camera's
    float ground[4];         // rgb albedo, w 1 when there is an atmosphere
    float sun[4];            // xyz towards the sun, w the cosine of the drawn disc's angular radius
    float sunIlluminance[4]; // rgb pi times the sun's value above the air
    float sunDisc[4];        // rgb the disc's radiance as drawn
    float ambient[4];        // rgb the frame's own flat ambient
};

static_assert(sizeof(GpuAtmosphere) == 144, "GpuAtmosphere must match Atmosphere in atmosphere.glsl");

GpuAtmosphere packAtmosphere(const Atmosphere& atmosphere, const AtmosphereFrame& frame);
