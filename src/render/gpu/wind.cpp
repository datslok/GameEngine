#include "render/gpu/wind.h"
#include "core/hash.h"
#include "math/angles.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
    constexpr const char* categoryNames[WIND_CATEGORIES] = {"Trees", "Plants", "Grass", "Hanging"};

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
        visitor.group(categoryNames[category], [&] {
            WindSpring& spring = settings.springs[static_cast<std::size_t>(category)];
            visitor.field("Frequency", spring.frequency, {.min = 0.01f, .max = 20.0f, .sliderMax = 5.0f, .unit = "Hz"});
            visitor.field("Damping", spring.damping, {.min = 0.0f, .max = 4.0f, .sliderMax = 1.5f});
            visitor.field("Response", spring.response, {.min = 0.0f, .max = 1.0f, .sliderMax = 0.1f});
        });
    }

    visitor.field("Show field", settings.showField);
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

    for (std::size_t category = 0; category < static_cast<std::size_t>(WIND_CATEGORIES); ++category) {
        field.springs[category][0] = settings.springs[category].frequency;
        field.springs[category][1] = settings.springs[category].damping;
        field.springs[category][2] = settings.springs[category].response;
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
    return field;
}
