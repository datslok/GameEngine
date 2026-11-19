#include "render/gpu/wind.h"
#include "core/half_float.h"
#include "core/hash.h"
#include "math/angles.h"
#include "math/smooth_step.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
    constexpr const char* categoryNames[WIND_CATEGORIES] = {"Trees", "Plants", "Grass", "Hanging"};
    constexpr const char* bandNames[WIND_HANGING_BANDS] = {"Short", "Medium", "Long"};

    void visitSpring(WindSpring& spring, SettingsVisitor& visitor) {
        visitor.field("Frequency", spring.frequency, {.min = 0.01f, .max = 20.0f, .sliderMax = 5.0f, .unit = "Hz"});
        visitor.field("Damping", spring.damping, {.min = 0.0f, .max = 4.0f, .sliderMax = 1.5f});
        visitor.field("Response", spring.response, {.min = 0.0f, .max = 1.0f, .sliderMax = 0.1f});
    }

    // A lattice point's value: its hash's top 24 bits over 2^24, exactly as the shader makes it.
    float latticeValue(int x, int y, int z) {
        const std::array<std::uint32_t, 3> hashed = pcg3d({static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), static_cast<std::uint32_t>(z)});
        return static_cast<float>(hashed[0] >> 8u) * (1.0f / 16777216.0f);
    }

    float smoothFraction(float fraction) {
        return fraction * fraction * (3.0f - 2.0f * fraction);
    }

    float mix(float a, float b, float share) {
        return a + (b - a) * share;
    }

    // The floor of a over b for whole numbers, a negative a included.
    int floorDivide(int a, int b) {
        return static_cast<int>(std::floor(static_cast<float>(a) / static_cast<float>(b)));
    }
}

void visit(WindSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Enabled", settings.enabled);
    visitor.field("Near cell", settings.nearCellMetres, {.min = 0.1f, .max = 100.0f, .sliderMax = 8.0f, .unit = "m"});
    visitor.field("Far cell", settings.farCellMetres, {.min = 0.1f, .max = 1000.0f, .sliderMax = 64.0f, .unit = "m"});

    for (int category = 0; category < WIND_CATEGORIES; ++category) {
        // The hanging category's springs are its three bands, each a group of its own.
        visitor.group(categoryNames[category], [&] {
            if (category == WIND_CATEGORY_HANGING) {
                for (int band = 0; band < WIND_HANGING_BANDS; ++band) {
                    visitor.group(bandNames[band], [&] {
                        visitSpring(settings.springs[static_cast<std::size_t>(WIND_SPRING_HANGING_SHORT + band)], visitor);
                    });
                }
            } else {
                visitSpring(settings.springs[static_cast<std::size_t>(category)], visitor);
            }

            WindFlutter& flutter = settings.flutters[static_cast<std::size_t>(category)];
            visitor.field("Flutter", flutter.hertz, {.min = 0.01f, .max = 20.0f, .sliderMax = 6.0f, .unit = "Hz"});
            visitor.field("Flutter wave", flutter.metres, {.min = 0.01f, .max = 1000.0f, .sliderMax = 20.0f, .unit = "m"});
        });
    }

    visitor.field("Show field", settings.showField);
    visitor.field("Sway", settings.sway);
    visitor.field("Sway fade start", settings.swayFadeStart, {.min = 0.0f, .max = 10000.0f, .sliderMax = 300.0f, .unit = "m"});
    visitor.field("Sway fade end", settings.swayFadeEnd, {.min = 0.0f, .max = 10000.0f, .sliderMax = 400.0f, .unit = "m"});
    visitor.field("Swaying shadow cascades", settings.swayShadowCascades, {.min = 0, .max = 8});
    visitor.field("Swaying lamp shadows", settings.swayLocalShadows);
}

void validateWind(const WindSettings& settings) {
    if (!(settings.nearCellMetres > 0.0f) || !(settings.farCellMetres > 0.0f)) {
        throw std::invalid_argument("The wind field's cells must be above 0 m");
    }

    for (const WindSpring& spring : settings.springs) {
        if (!(spring.frequency > 0.0f) || spring.damping < 0.0f || spring.response < 0.0f) {
            throw std::invalid_argument("A wind spring needs a frequency above 0 and no negative damping or response");
        }
    }

    if (!(settings.swayFadeStart >= 0.0f) || !(settings.swayFadeEnd > settings.swayFadeStart) || settings.swayShadowCascades < 0) {
        throw std::invalid_argument("The sway's fade must end after it starts, from 0 m on, and its shadow cascades not be negative");
    }

    for (const WindFlutter& flutter : settings.flutters) {
        if (!(flutter.hertz > 0.0f) || !(flutter.metres > 0.0f)) {
            throw std::invalid_argument("A wind flutter needs hertz and metres above 0");
        }
    }

    const Vec3 bands = windHangingBands(settings);

    if (!(bands.x > bands.y) || !(bands.y > bands.z)) {
        throw std::invalid_argument("The hanging springs' frequencies must fall from the short band to the long");
    }
}

Vec2 windFlutterRate(const WindSettings& settings, int category) {
    const WindFlutter& flutter = settings.flutters[static_cast<std::size_t>(category)];
    return Vec2{flutter.hertz, flutter.metres};
}

float windNoise(const Vec3& place) {
    const float floorX = std::floor(place.x);
    const float floorY = std::floor(place.y);
    const float floorZ = std::floor(place.z);
    const int x = static_cast<int>(floorX);
    const int y = static_cast<int>(floorY);
    const int z = static_cast<int>(floorZ);
    const float u = smoothFraction(place.x - floorX);
    const float v = smoothFraction(place.y - floorY);
    const float w = smoothFraction(place.z - floorZ);
    const float near = mix(mix(latticeValue(x, y, z), latticeValue(x + 1, y, z), u), mix(latticeValue(x, y + 1, z), latticeValue(x + 1, y + 1, z), u), v);
    const float far = mix(mix(latticeValue(x, y, z + 1), latticeValue(x + 1, y, z + 1), u),
                          mix(latticeValue(x, y + 1, z + 1), latticeValue(x + 1, y + 1, z + 1), u), v);
    return mix(near, far, w);
}

Vec2 windVelocity(const Wind& wind, const Vec2& place, float seconds) {
    const float angle = radiansFromDegrees(wind.towardsDegrees);
    const Vec2 along{std::cos(angle), std::sin(angle)};
    const Vec2 side{-along.y, along.x};
    const float gustSize = std::max(wind.gustSize, 1e-3f);
    const float turbulenceSize = std::max(wind.turbulenceSize, 1e-3f);
    const Vec2 upwind = (place - along * (wind.speed * seconds)) * (1.0f / gustSize);
    const float gust = windNoise(Vec3{upwind.x, upwind.y, static_cast<float>(WIND_GUST_EVOLUTION) * wind.speed * seconds / gustSize});
    const float speed = wind.speed * std::max(0.0f, 1.0f + wind.gustStrength * (2.0f * gust - 1.0f));
    const Vec2 eddy = place * (1.0f / turbulenceSize);
    const float swirl = windNoise(Vec3{eddy.x + static_cast<float>(WIND_TURBULENCE_OFFSET), eddy.y, wind.speed * seconds / turbulenceSize});
    return along * speed + side * (wind.turbulence * wind.speed * (2.0f * swirl - 1.0f));
}

Vec2 windSpringTarget(const Vec2& velocity, float response) {
    return velocity * (velocity.length() * response);
}

Vec4 windSpringStep(const Vec4& state, const Vec2& target, float frequency, float damping, float seconds) {
    const float step = std::clamp(seconds, 0.0f, static_cast<float>(WIND_LONGEST_STEP));
    const float omega = fullTurnRadians * frequency;
    const int substeps = std::clamp(static_cast<int>(std::ceil(omega * step / static_cast<float>(WIND_SUBSTEP_TURN))), 1, WIND_MOST_SUBSTEPS);
    const float h = step / static_cast<float>(substeps);
    Vec2 displacement{state.x, state.y};
    Vec2 velocity{state.z, state.w};

    for (int substep = 0; substep < substeps; ++substep) {
        velocity = velocity + ((target - displacement) * (omega * omega) - velocity * (2.0f * damping * omega)) * h;
        displacement = displacement + velocity * h;
    }

    return Vec4{displacement.x, displacement.y, velocity.x, velocity.y};
}

WindWindow windWindowAround(const Vec2& place, float cellMetres) {
    return WindWindow{static_cast<int>(std::floor(place.x / cellMetres)) - WIND_CELLS / 2, static_cast<int>(std::floor(place.y / cellMetres)) - WIND_CELLS / 2};
}

WindCell windCellOfTexel(int texelX, int texelZ, const WindWindow& window) {
    const int offsetX = texelX - window.originX;
    const int offsetZ = texelZ - window.originZ;
    return WindCell{window.originX + offsetX - WIND_CELLS * floorDivide(offsetX, WIND_CELLS),
                    window.originZ + offsetZ - WIND_CELLS * floorDivide(offsetZ, WIND_CELLS)};
}

bool windCellInWindow(const WindCell& cell, const WindWindow& window) {
    return cell.x >= window.originX && cell.x < window.originX + WIND_CELLS && cell.z >= window.originZ && cell.z < window.originZ + WIND_CELLS;
}

GpuWindField packWindField(const Wind* wind, const WindSettings& settings, const std::array<WindWindow, WIND_CASCADES>& windows,
                           const std::array<WindWindow, WIND_CASCADES>& previous, bool previousKnown, float seconds, float stepSeconds) {
    GpuWindField field{};
    const bool on = wind != nullptr && settings.enabled;

    if (wind != nullptr) {
        const float angle = radiansFromDegrees(wind->towardsDegrees);
        field.wind[0] = std::cos(angle);
        field.wind[1] = std::sin(angle);
        field.wind[2] = wind->speed;
        field.gusts[0] = wind->gustStrength;
        field.gusts[1] = wind->gustSize;
        field.gusts[2] = wind->turbulence;
        field.gusts[3] = wind->turbulenceSize;
    }

    field.wind[3] = seconds;
    field.step[0] = stepSeconds;
    field.step[1] = on ? 1.0f : 0.0f;

    for (std::size_t spring = 0; spring < static_cast<std::size_t>(WIND_SPRINGS); ++spring) {
        field.springs[spring][0] = settings.springs[spring].frequency;
        field.springs[spring][1] = settings.springs[spring].damping;
        field.springs[spring][2] = settings.springs[spring].response;
    }

    for (std::size_t category = 0; category < static_cast<std::size_t>(WIND_CATEGORIES); ++category) {
        field.flutters[category][0] = settings.flutters[category].hertz;
        field.flutters[category][1] = settings.flutters[category].metres;
    }

    const float cellMetres[WIND_CASCADES] = {settings.nearCellMetres, settings.farCellMetres};

    for (std::size_t cascade = 0; cascade < static_cast<std::size_t>(WIND_CASCADES); ++cascade) {
        field.windows[cascade][0] = windows[cascade].originX;
        field.windows[cascade][1] = windows[cascade].originZ;
        field.windows[cascade][2] = previous[cascade].originX;
        field.windows[cascade][3] = previous[cascade].originZ;
        field.cells[cascade][0] = cellMetres[cascade];
        field.cells[cascade][1] = 1.0f / (cellMetres[cascade] * static_cast<float>(WIND_CELLS));
    }

    field.flags[0] = previousKnown ? 1 : 0;
    field.sway[0] = on && settings.sway ? 1.0f : 0.0f;
    field.sway[1] = settings.swayFadeStart;
    field.sway[2] = settings.swayFadeEnd;
    field.sway[3] = seconds - stepSeconds;
    return field;
}

static_assert(windAnchorFixed == WIND_ANCHOR_FIXED && windAnchorOwn == WIND_ANCHOR_OWN, "scene/wind.h's anchor codes must be the shaders'");

static_assert(static_cast<int>(SwayKind::Tree) - 1 == WIND_CATEGORY_TREES && static_cast<int>(SwayKind::Plant) - 1 == WIND_CATEGORY_PLANTS &&
                  static_cast<int>(SwayKind::Grass) - 1 == WIND_CATEGORY_GRASS && static_cast<int>(SwayKind::Hanging) - 1 == WIND_CATEGORY_HANGING,
              "SwayKind must follow the field's categories after Still");

Vec4 packSway(const WindSway& sway) {
    const float category = sway.kind == SwayKind::Still ? static_cast<float>(WIND_CATEGORY_STILL) : static_cast<float>(static_cast<int>(sway.kind) - 1);
    return Vec4{category, sway.height, sway.bend, sway.flutter};
}

float windSwayFade(float distance, float start, float end) {
    return 1.0f - smoothStep(start, end, distance);
}

Vec2 windSoftLean(const Vec2& lean) {
    return lean * (1.0f / (1.0f + lean.length() / static_cast<float>(WIND_MOST_LEAN)));
}

float windWindowShare(const Vec2& place, const WindWindow& window, float cellMetres) {
    const float x = place.x / cellMetres;
    const float z = place.y / cellMetres;
    const float fromEdgeX = std::min(x - static_cast<float>(window.originX), static_cast<float>(window.originX + WIND_CELLS) - x);
    const float fromEdgeZ = std::min(z - static_cast<float>(window.originZ), static_cast<float>(window.originZ + WIND_CELLS) - z);
    return std::clamp((std::min(fromEdgeX, fromEdgeZ) - 1.0f) / static_cast<float>(WIND_BLEND_CELLS), 0.0f, 1.0f);
}

float windPhase(const Vec3& place, float metres) {
    return (place.x * 0.71f + place.y * 0.37f + place.z * 0.59f) * (fullTurnRadians / metres);
}

Vec3 windFlutterOffset(const Vec3& place, float amount, const Vec2& rate, float seconds) {
    const float turn = fullTurnRadians * rate.x * seconds;
    const float phase = windPhase(place, rate.y);
    return Vec3{std::sin(turn + phase), std::sin(1.3f * turn + 0.7f * phase + 2.0f), std::sin(0.9f * turn + 1.3f * phase + 4.0f)} * amount;
}

/*
* The main bend first: the lean's offset at the vertex's height share u, h^2 / height (the cantilever's u^2 times the
* height in metres), then the vertex pulled back to its distance from the foot, so a tree bends rather than stretches.
* The branches' bob and the leaves' flutter after, both growing with u up to the top.
*/
Vec3 windSwayOffset(const Vec3& fromFoot, float modelHeight, const Vec3& place, const Vec4& sway, float scale, const Vec2& spring, float fade,
                    const Vec2& flutterRate, float seconds) {
    if (sway.x < 0.0f) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }

    const float share = std::max(modelHeight, 0.0f) / std::max(sway.y, 1e-4f);
    const float heightMetres = sway.y * scale;
    const Vec2 lean = windSoftLean(spring * sway.z) * fade;
    const Vec2 sideways = lean * (heightMetres * share * share);
    const Vec3 bent{fromFoot.x + sideways.x, fromFoot.y, fromFoot.z + sideways.y};
    const float bentLength = bent.length();
    const Vec3 kept = bentLength > 0.0f ? bent * (fromFoot.length() / bentLength) : bent;
    Vec3 offset = kept - fromFoot;

    const float amount = lean.length();
    const float grow = std::min(share, 1.0f);
    const float trunkDistance = std::sqrt(fromFoot.x * fromFoot.x + fromFoot.z * fromFoot.z);
    const float branchTurn = fullTurnRadians * static_cast<float>(WIND_BRANCH_HERTZ) * seconds + windPhase(place, static_cast<float>(WIND_BRANCH_METRES));
    offset.y += static_cast<float>(WIND_BRANCH_SHARE) * amount * trunkDistance * grow * std::sin(branchTurn);

    return offset + windFlutterOffset(place, sway.w * amount * grow, flutterRate, seconds);
}

bool drawSways(const WindSway& sway, const Vec3& boundsCentre, float boundsRadius, const Vec3& camera, const WindSettings& settings) {
    return sway.kind != SwayKind::Still && settings.enabled && settings.sway && (boundsCentre - camera).length() - std::max(boundsRadius, 0.0f) < settings.swayFadeEnd;
}

bool tileSways(int sunCascade, const Vec3& reachCentre, float reachRadius, const Vec3& camera, const WindSettings& settings) {
    if (!settings.enabled || !settings.sway) {
        return false;
    }

    if (sunCascade >= 0) {
        return sunCascade < settings.swayShadowCascades;
    }

    return settings.swayLocalShadows && (reachRadius < 0.0f || (reachCentre - camera).length() - reachRadius < settings.swayFadeEnd);
}

std::uint64_t swayingShadowSignature(std::uint64_t signature, bool somethingSways, std::uint64_t frame) {
    return somethingSways ? mixBits(signature ^ mixBits(frame + goldenGamma)) : signature;
}

std::array<std::uint32_t, 3> packHangingVertex(const HangingVertex& vertex) {
    return {static_cast<std::uint32_t>(vertex.first) | (static_cast<std::uint32_t>(vertex.second) << 16u),
            static_cast<std::uint32_t>(floatToHalf(vertex.along)) | (static_cast<std::uint32_t>(floatToHalf(vertex.arm)) << 16u),
            static_cast<std::uint32_t>(floatToHalf(vertex.swingLength))};
}

float windChainHertz(float swingLength) {
    return std::sqrt(static_cast<float>(WIND_GRAVITY) / std::max(swingLength, static_cast<float>(WIND_SHORTEST_SWING))) / static_cast<float>(WIND_CHAIN_PERIOD);
}

Vec3 windHangingBands(const WindSettings& settings) {
    return Vec3{settings.springs[WIND_SPRING_HANGING_SHORT].frequency, settings.springs[WIND_SPRING_HANGING_SHORT + 1].frequency,
                settings.springs[WIND_SPRING_HANGING_SHORT + 2].frequency};
}

Vec3 windChainShares(float swingLength, const Vec3& bands) {
    const float hertz = windChainHertz(swingLength);

    if (hertz >= bands.x) {
        return Vec3{1.0f, 0.0f, 0.0f};
    }

    if (hertz <= bands.z) {
        return Vec3{0.0f, 0.0f, 1.0f};
    }

    if (hertz >= bands.y) {
        const float share = std::log(bands.x / hertz) / std::log(bands.x / bands.y);
        return Vec3{1.0f - share, share, 0.0f};
    }

    const float share = std::log(bands.y / hertz) / std::log(bands.y / bands.z);
    return Vec3{0.0f, 1.0f - share, share};
}

GpuWindAnchor packWindAnchor(const WindAnchor& anchor) {
    return GpuWindAnchor{{anchor.foot.x, anchor.foot.y, anchor.foot.z, anchor.height},
                         {anchor.point.x, anchor.point.y, anchor.point.z, anchor.scale},
                         {anchor.bend, 0.0f, 0.0f, 0.0f}};
}

Vec3 windAnchorOffset(const GpuWindAnchor& anchor, const Vec2& spring, float fade, float seconds) {
    const Vec3 foot{anchor.foot[0], anchor.foot[1], anchor.foot[2]};
    const Vec3 point{anchor.point[0], anchor.point[1], anchor.point[2]};
    const float scale = anchor.point[3];
    const Vec4 sway{static_cast<float>(WIND_CATEGORY_TREES), anchor.foot[3], anchor.bend[0], 0.0f};
    // No flutter (the sway's w is 0), so any rate does: one of a hertz and a metre.
    return windSwayOffset(point - foot, (point.y - foot.y) / std::max(scale, 1e-4f), point, sway, scale, spring, fade, Vec2{1.0f, 1.0f}, seconds);
}

Vec3 windSwingOffset(float arm, const Vec3& place, float swing, float flutter, const Vec2& spring, float fade, const Vec2& flutterRate, float seconds) {
    const Vec2 lean = windSoftLean(spring * swing) * fade;
    const float reach = std::max(arm, 0.0f);
    const Vec2 out = lean * reach;
    const float rise = reach - std::sqrt(std::max(reach * reach - out.lengthSquared(), 0.0f));
    return Vec3{out.x, rise, out.y} + windFlutterOffset(place, flutter * lean.length(), flutterRate, seconds);
}

Vec3 windHangingOffset(const Vec3& first, const Vec3& second, float along, const Vec3& swing) {
    return first + (second - first) * along + swing;
}
