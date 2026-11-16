#include "games/forest/climbers.h"
#include "games/forest/forest_terrain.h"
#include "math/angles.h"

#include <algorithm>
#include <cmath>

namespace {
    const Vec3 up{0.0f, 1.0f, 0.0f};

    // Where a climber starts, and how far its stem stands out from the surface beyond its own radius.
    constexpr float climberStartHeight = 0.1f;
    constexpr float stemClearance = 0.005f;

    // The metres between a stem's points.
    constexpr float stemStep = 0.25f;

    // Where the buttresses have faded enough for a climber to start winding (metres up).
    constexpr float windingStart = 2.5f;

    // Feeder roots: from this high up, their thickness, their bend's drop, the room kept from the trunk, and the
    // steps they are pushed out by until they hang clear.
    constexpr float lowestRoot = 2.5f;
    constexpr float rootRadius = 0.008f;
    constexpr float rootBend = 0.3f;
    constexpr float rootRoom = 0.03f;
    constexpr float rootPushStep = 0.05f;
    constexpr float mostRootPush = 4.0f;
    constexpr float rootGroundSink = 0.1f;

    // A big leaf's stalk: its rise above level, its length (a share of the leaf's), its radius.
    constexpr float stalkRise = 0.6f;
    constexpr float stalkShare = 0.6f;
    constexpr float stalkRadius = 0.006f;

    // The leaf's outline in its card's units (x across from its left edge, y from its stem to its tip),
    // counterclockwise: an ovate leaf, widest a third of the way up, convex for the card's fan of triangles.
    const std::vector<Vec2>& aroidOutline() {
        static const std::vector<Vec2> outline{
            Vec2{0.5f, 0.0f},  Vec2{0.82f, 0.06f}, Vec2{0.98f, 0.28f}, Vec2{0.92f, 0.55f}, Vec2{0.7f, 0.82f},
            Vec2{0.5f, 1.0f},  Vec2{0.3f, 0.82f},  Vec2{0.08f, 0.55f}, Vec2{0.02f, 0.28f}, Vec2{0.18f, 0.06f},
        };
        return outline;
    }

    // A leaf's width, a share of its length.
    constexpr float leafWidthShare = 0.75f;

    // The host's centre line, radius and frame at a height, between its points.
    struct HostRing {
        Vec3 centre;
        float radius = 0.0f;
        TubeFrame frame;
    };

    HostRing hostRing(const ClimbHost& host, const std::vector<TubeFrame>& frames, float height) {
        const std::vector<Vec3>& points = host.trunk.points;
        std::size_t upper = 1;

        while (upper + 1 < points.size() && points[upper].y < height) {
            ++upper;
        }

        const std::size_t lower = upper - 1;
        const float span = points[upper].y - points[lower].y;
        const float t = span > 1e-6f ? std::clamp((height - points[lower].y) / span, 0.0f, 1.0f) : 0.0f;
        const Vec3 across = (frames[lower].across * (1.0f - t) + frames[upper].across * t).normalized();
        const Vec3 second = (frames[lower].second * (1.0f - t) + frames[upper].second * t).normalized();
        return HostRing{points[lower] * (1.0f - t) + points[upper] * t, host.trunk.radii[lower] * (1.0f - t) + host.trunk.radii[upper] * t,
                        TubeFrame{across, second}};
    }

    float surfaceRadius(const ClimbHost& host, float radius, float angle, float height) {
        return host.lobes.count > 0 ? lobedRadius(radius, angle, height, host.lobes) : radius;
    }

    // Random numbers drawn in a fixed order from a seed.
    struct Draws {
        std::uint32_t seed;
        std::uint64_t next = 0;

        float operator()() {
            return forestUnitRandom(seed, next++);
        }
    };

    // The angle round the host near the ground where its lobes stand out least: between the buttresses, one of the
    // narrowest at random.
    float startAngle(const ClimbHost& host, const std::vector<TubeFrame>& frames, Draws& draw) {
        constexpr int tries = 180;
        const HostRing ring = hostRing(host, frames, climberStartHeight);
        std::vector<float> narrow;

        for (int step = 0; step < tries; ++step) {
            const float angle = fullTurnRadians * static_cast<float>(step) / static_cast<float>(tries);

            if (surfaceRadius(host, ring.radius, angle, climberStartHeight) < ring.radius * 1.02f) {
                narrow.push_back(angle);
            }
        }

        if (narrow.empty()) {
            return draw() * fullTurnRadians;
        }

        return narrow[std::min(narrow.size() - 1, static_cast<std::size_t>(draw() * static_cast<float>(narrow.size())))];
    }

    // A unit direction turned by angle about axis (Rodrigues).
    Vec3 turned(const Vec3& direction, const Vec3& axis, float angle) {
        return direction * std::cos(angle) + axis.cross(direction) * std::sin(angle) + axis * (axis.dot(direction) * (1.0f - std::cos(angle)));
    }

    std::uint8_t channelByte(float value) {
        return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 255.0f));
    }
}

void visit(ForestClimberSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Tree share", settings.treeShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Most per tree", settings.mostPerTree, {.min = 1, .max = 8, .sliderMax = 4, .applies = FieldApplies::AtStartup});
    visitor.field("Least height share", settings.leastHeightShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Most height share", settings.mostHeightShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Twist", settings.twist, {.min = 0.0f, .max = 3.0f, .sliderMax = 1.0f, .unit = "turns", .applies = FieldApplies::AtStartup});
    visitor.field("Stem diameter", settings.stemDiameter, {.min = 0.005f, .max = 0.2f, .sliderMax = 0.1f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Shingle height", settings.shingleHeight, {.min = 0.0f, .max = 50.0f, .sliderMax = 15.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Shingle leaf length", settings.shingleLeafLength, {.min = 0.02f, .max = 1.0f, .sliderMax = 0.4f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Shingle spacing", settings.shingleSpacing, {.min = 0.02f, .max = 2.0f, .sliderMax = 0.5f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Big leaf length", settings.bigLeafLength, {.min = 0.05f, .max = 3.0f, .sliderMax = 1.5f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Big leaf spacing", settings.bigLeafSpacing, {.min = 0.05f, .max = 5.0f, .sliderMax = 2.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Root spacing", settings.rootSpacing, {.min = 0.2f, .max = 20.0f, .sliderMax = 5.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Mid-air share", settings.midAirShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
}

HostPoint hostSurface(const ClimbHost& host, const std::vector<TubeFrame>& frames, float height, float angle) {
    const HostRing ring = hostRing(host, frames, height);
    const Vec3 outward = ring.frame.direction(angle);
    return HostPoint{ring.centre + outward * surfaceRadius(host, ring.radius, angle, height), outward};
}

// The point's offset from the centre line at its height, square to the trunk there (along = across x second).
float hostClearance(const ClimbHost& host, const std::vector<TubeFrame>& frames, const Vec3& point) {
    const HostRing ring = hostRing(host, frames, point.y);
    const Vec3 along = ring.frame.across.cross(ring.frame.second).normalized();
    const Vec3 offset = point - ring.centre;
    const Vec3 square = offset - along * offset.dot(along);
    float angle = std::atan2(square.dot(ring.frame.second), square.dot(ring.frame.across));

    if (angle < 0.0f) {
        angle += fullTurnRadians;
    }

    return square.length() - surfaceRadius(host, ring.radius, angle, point.y);
}

namespace {
    // One climber grown into meshes (growClimber's work, shared by growClimbers).
    void appendClimber(ClimberMeshes& meshes, const ClimbHost& host, const std::vector<TubeFrame>& frames, const ForestClimberSettings& settings,
                       std::uint32_t seed) {
        Draws draw{seed};
        const float leastShare = std::min(settings.leastHeightShare, settings.mostHeightShare);
        const float mostShare = std::max(settings.leastHeightShare, settings.mostHeightShare);
        const float top = host.topHeight * (leastShare + (mostShare - leastShare) * draw());
        const float firstAngle = startAngle(host, frames, draw);
        const float turns = settings.twist * (2.0f * draw() - 1.0f);
        const float wanderPhase = draw() * fullTurnRadians;
        const float footRadius = 0.5f * settings.stemDiameter;

        if (top <= climberStartHeight + stemStep) {
            return;
        }

        // The stem: its angle fixed between the buttresses, then winding, with a little wander.
        HangingStrand stem;
        std::vector<Vec3> outwards;
        const int steps = static_cast<int>(std::ceil((top - climberStartHeight) / stemStep));

        for (int step = 0; step <= steps; ++step) {
            const float height = climberStartHeight + (top - climberStartHeight) * static_cast<float>(step) / static_cast<float>(steps);
            const float winding = std::max(height - windingStart, 0.0f) / std::max(top - windingStart, 1.0f);
            const float angle = firstAngle + turns * fullTurnRadians * winding + 0.08f * std::sin(1.3f * height + wanderPhase) * winding;
            const float radius = footRadius * (1.0f - 0.4f * height / top);
            const HostPoint surface = hostSurface(host, frames, height, angle);
            stem.points.push_back(surface.position + surface.outward * (radius + stemClearance));
            stem.radii.push_back(radius);
            outwards.push_back(surface.outward);
        }

        appendTube(meshes.stems, stem, 5, 0.5f);
        meshes.top = std::max(meshes.top, top);

        if (meshes.facing.lengthSquared() == 0.0f) {
            const Vec3& third = outwards[outwards.size() / 3];
            meshes.facing = Vec3{third.x, 0.0f, third.z}.normalized();
        }

        // The leaves along it, alternately to either side.
        float travelled = 0.0f;
        float nextLeaf = 0.0f;
        int side = 1;

        for (std::size_t point = 1; point < stem.points.size(); ++point) {
            travelled += (stem.points[point] - stem.points[point - 1]).length();

            if (travelled < nextLeaf) {
                continue;
            }

            const Vec3& place = stem.points[point];
            const Vec3& outward = outwards[point];
            const Vec3 upAlong = (up - outward * up.dot(outward)).normalized();
            const Vec3 aside = outward.cross(upAlong).normalized();
            side = -side;

            if (place.y < settings.shingleHeight) {
                // A shingle: flat on the bark over the stem, pointing up and a little aside.
                const float length = settings.shingleLeafLength * (0.8f + 0.4f * draw()) * (0.7f + 0.6f * place.y / std::max(settings.shingleHeight, 0.1f));
                const Vec3 tip = (upAlong * std::cos(0.4f) + aside * (std::sin(0.4f) * static_cast<float>(side))).normalized();
                appendOutlinedLeafCard(meshes.leaves, place + outward * (stem.radii[point] + 0.003f), tip, outward, outward, length,
                                       length * leafWidthShare, Vec2{0.0f, 0.0f}, Vec2{1.0f, 1.0f}, aroidOutline());
                ++meshes.shingleLeaves;
                nextLeaf = travelled + settings.shingleSpacing * (0.8f + 0.4f * draw());
                continue;
            }

            // A big leaf on a stalk rising out and aside, the blade hanging from its end.
            const float share = std::clamp((place.y - settings.shingleHeight) / std::max(top - settings.shingleHeight, 1.0f), 0.0f, 1.0f);
            const float length = settings.bigLeafLength * (0.7f + 0.6f * draw()) * (0.7f + 0.3f * share);
            const Vec3 level = turned(outward, up, (0.5f + 0.4f * draw()) * static_cast<float>(side));
            const Vec3 stalkDirection = (level * std::cos(stalkRise) + up * std::sin(stalkRise)).normalized();
            const Vec3 stalkStart = place + outward * stem.radii[point];
            const Vec3 stalkEnd = stalkStart + stalkDirection * (length * stalkShare);
            appendTube(meshes.stems, HangingStrand{{stalkStart, stalkEnd}, {stalkRadius, stalkRadius}}, 3, 0.5f);

            const Vec3 hanging = (level * 0.55f + up * -0.83f).normalized();
            const Vec3 across = up.cross(level).normalized();
            Vec3 facing = across.cross(hanging).normalized();

            if (facing.dot(level) < 0.0f) {
                facing = facing * -1.0f;
            }

            appendOutlinedLeafCard(meshes.leaves, stalkEnd, hanging, facing, facing, length, length * leafWidthShare, Vec2{0.0f, 0.0f}, Vec2{1.0f, 1.0f},
                                   aroidOutline());
            ++meshes.bigLeaves;
            nextLeaf = travelled + settings.bigLeafSpacing * (0.8f + 0.4f * draw());
        }

        // The feeder roots, from lowestRoot up: each pushed out until it hangs clear of the trunk all the way down.
        float rootHeight = lowestRoot;

        while (rootHeight < top) {
            const std::size_t point = std::min(stem.points.size() - 1, static_cast<std::size_t>((rootHeight - climberStartHeight) / (top - climberStartHeight) *
                                                                                                static_cast<float>(steps)));
            const Vec3 start = stem.points[point];
            const Vec3 outward = Vec3{outwards[point].x, 0.0f, outwards[point].z}.normalized();
            const bool midAir = draw() < settings.midAirShare;
            const float end = midAir ? start.y * (0.3f + 0.5f * draw()) : -rootGroundSink;
            HangingStrand root;

            for (float push = rootPushStep; push <= mostRootPush; push += rootPushStep) {
                root = HangingStrand{};
                root.points.push_back(start);
                const Vec3 column = start + outward * push;

                for (float height = start.y - rootBend; height > end; height -= 0.5f) {
                    root.points.push_back(Vec3{column.x, height, column.z});
                }

                root.points.push_back(Vec3{column.x, end, column.z});
                const bool clear = std::all_of(root.points.begin() + 1, root.points.end(), [&](const Vec3& place) {
                    return hostClearance(host, frames, place) > rootRadius + rootRoom;
                });

                if (clear) {
                    break;
                }
            }

            root.radii.assign(root.points.size(), rootRadius);

            // A root ending in mid air swings below where it leaves the stem (on top of the tree's own sway there); one
            // rooted in the ground does not.
            if (midAir) {
                hangFrom(root, windAnchorOwn);
            }

            appendTube(meshes.stems, root, 3, 0.5f);
            meshes.rootStrands.push_back(root);
            ++meshes.roots;
            rootHeight += settings.rootSpacing * (0.7f + 0.6f * draw());
        }
    }
}

ClimberMeshes growClimber(const ClimbHost& host, const ForestClimberSettings& settings, std::uint32_t seed) {
    ClimberMeshes meshes;

    if (host.trunk.points.size() >= 2) {
        appendClimber(meshes, host, tubeFrames(host.trunk), settings, seed);
    }

    return meshes;
}

int climbersOnTree(const ForestClimberSettings& settings, std::uint32_t seed) {
    if (forestUnitRandom(seed, 0) >= settings.treeShare) {
        return 0;
    }

    const int most = std::max(settings.mostPerTree, 1);
    return 1 + std::min(most - 1, static_cast<int>(forestUnitRandom(seed, 1) * static_cast<float>(most)));
}

ClimberMeshes growClimbers(const ClimbHost& host, const ForestClimberSettings& settings, std::uint32_t seed) {
    ClimberMeshes meshes;

    if (host.trunk.points.size() < 2) {
        return meshes;
    }

    const std::vector<TubeFrame> frames = tubeFrames(host.trunk);
    const int count = climbersOnTree(settings, seed);

    for (int climber = 0; climber < count; ++climber) {
        appendClimber(meshes, host, frames, settings, forestShapeSeed(seed, static_cast<std::uint32_t>(climber) + 1));
    }

    return meshes;
}

/*
* Lateral veins leave the midrib towards the tip and curve out: a vein's place goes with v less a share of how far out
* the texel is, so the lines slant up from the midrib; pale and sunk, the blade darker towards its edges.
*/
AroidLeafImages aroidLeafImages(int texels, Pixel colour, std::uint32_t seed) {
    constexpr float veinsAlong = 9.0f;
    constexpr float veinSlant = 2.5f;
    constexpr float veinHalfWidth = 0.07f;
    constexpr float midribHalfWidth = 0.04f;
    constexpr float edgeDarkening = 0.35f;
    constexpr float mottle = 0.06f;
    const std::uint32_t size = static_cast<std::uint32_t>(std::max(texels, 1));
    AroidLeafImages images;
    images.colour.width = size;
    images.colour.height = size;
    images.colour.encoding = TextureEncoding::Srgb;
    images.colour.pixels.reserve(static_cast<std::size_t>(size) * size * 4);
    images.height.width = size;
    images.height.height = size;
    images.height.pixels.reserve(static_cast<std::size_t>(size) * size * 4);

    for (std::uint32_t y = 0; y < size; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);

        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const float out = std::fabs(u - 0.5f) * 2.0f;
            const float midrib = std::max(0.0f, 1.0f - out / midribHalfWidth);
            const float veinPlace = v * veinsAlong - out * veinSlant;
            const float fromVein = std::fabs(veinPlace - std::round(veinPlace));
            const float vein = out > midribHalfWidth ? std::max(0.0f, 1.0f - fromVein / veinHalfWidth) * (1.0f - out) : 0.0f;
            const std::uint64_t cell = static_cast<std::uint64_t>(u * 16.0f) + 16 * static_cast<std::uint64_t>(v * 16.0f);
            const float shade = (1.0f + mottle * (2.0f * forestUnitRandom(seed, cell) - 1.0f)) * (1.0f - edgeDarkening * out * out) * (1.0f + 0.12f * vein);

            const float red = static_cast<float>(colour.r) * shade * (1.0f + 0.7f * midrib);
            const float green = static_cast<float>(colour.g) * shade * (1.0f + 0.5f * midrib);
            const float blue = static_cast<float>(colour.b) * shade * (1.0f + 0.2f * midrib);
            images.colour.pixels.insert(images.colour.pixels.end(), {channelByte(red), channelByte(green), channelByte(blue), 255});

            const std::uint8_t height = channelByte(255.0f * (0.5f + 0.3f * midrib - 0.12f * vein));
            images.height.pixels.insert(images.height.pixels.end(), {height, height, height, 255});
        }
    }

    return images;
}
