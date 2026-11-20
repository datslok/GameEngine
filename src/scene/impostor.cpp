#include "scene/impostor.h"

#include "core/srgb.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

/*
* Brucks's hemi-octahedral fold: the direction scaled onto the octahedron |x| + |y| + |z| = 1, its upper half turned
* 45 degrees into the square (u = x + z, v = x - z, each -1..1).
*/
Vec2 hemiOctahedralEncode(const Vec3& direction) {
    const Vec3 above{direction.x, std::max(direction.y, 0.0f), direction.z};
    const float sum = std::abs(above.x) + above.y + std::abs(above.z);
    const float x = sum > 0.0f ? above.x / sum : 0.0f;
    const float z = sum > 0.0f ? above.z / sum : 0.0f;
    return Vec2{(x + z) * 0.5f + 0.5f, (x - z) * 0.5f + 0.5f};
}

Vec3 hemiOctahedralDecode(const Vec2& square) {
    const float u = square.x * 2.0f - 1.0f;
    const float v = square.y * 2.0f - 1.0f;
    const float x = (u + v) * 0.5f;
    const float z = (u - v) * 0.5f;
    return Vec3{x, std::max(1.0f - std::abs(x) - std::abs(z), 0.0f), z}.normalized();
}

Vec3 impostorFrameDirection(const Vec2& frame, int frames) {
    return hemiOctahedralDecode(frame / static_cast<float>(frames - 1));
}

// Right is up x direction, made unit; straight up (where that vanishes) it is x.
ImpostorBasis impostorFrameBasis(const Vec3& direction) {
    const Vec3 across = Vec3{0.0f, 1.0f, 0.0f}.cross(direction);
    const Vec3 right = across.lengthSquared() > 1e-8f ? across.normalized() : Vec3{1.0f, 0.0f, 0.0f};
    return ImpostorBasis{right, direction.cross(right).normalized()};
}

ImpostorBlend impostorFrameBlend(const Vec3& viewDirection, int frames) {
    const float last = static_cast<float>(frames - 1);
    const Vec2 place = hemiOctahedralEncode(viewDirection) * last;
    const float column = std::clamp(std::floor(place.x), 0.0f, last - 1.0f);
    const float row = std::clamp(std::floor(place.y), 0.0f, last - 1.0f);
    const float across = std::clamp(place.x - column, 0.0f, 1.0f);
    const float down = std::clamp(place.y - row, 0.0f, 1.0f);
    return ImpostorBlend{{Vec2{column, row}, Vec2{column + 1.0f, row}, Vec2{column, row + 1.0f}, Vec2{column + 1.0f, row + 1.0f}},
                         {(1.0f - across) * (1.0f - down), across * (1.0f - down), (1.0f - across) * down, across * down}};
}

Vec2 impostorFrameUv(const Vec3& cardPoint, const Vec3& frameDirection) {
    const ImpostorBasis basis = impostorFrameBasis(frameDirection);
    return Vec2{cardPoint.dot(basis.right) * 0.5f + 0.5f, cardPoint.dot(basis.up) * 0.5f + 0.5f};
}

Vec2 impostorAtlasUv(const Vec2& frameUv, const Vec2& frame, int frames) {
    return (frame + frameUv) / static_cast<float>(frames);
}

std::uint8_t impostorDepthEncode(float depth) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(depth * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f));
}

float impostorDepthDecode(float stored) {
    return stored * 2.0f - 1.0f;
}

// The card point's own depth along the frame swapped for the surface's; its place on the frame's plane kept.
Vec3 impostorSurfacePoint(const Vec3& cardPoint, const Vec3& frameDirection, float depth) {
    return cardPoint + frameDirection * (depth - cardPoint.dot(frameDirection));
}

float impostorGatherLit(const std::array<float, 4>& storedDepths, const std::array<float, 4>& covered, const Vec2& fraction, float pointDepth,
                        float bias) {
    const std::array<float, 4> weights{(1.0f - fraction.x) * fraction.y, fraction.x * fraction.y, fraction.x * (1.0f - fraction.y),
                                       (1.0f - fraction.x) * (1.0f - fraction.y)};
    float lit = 0.0f;

    for (std::size_t texel = 0; texel < 4; ++texel) {
        if (covered[texel] < 0.5f || impostorDepthDecode(storedDepths[texel]) <= pointDepth + bias) {
            lit += weights[texel];
        }
    }

    return lit;
}

float impostorShadowBias(float biasTexels, float frameTexels) {
    return biasTexels * 2.0f / frameTexels;
}

Vec3 impostorShadowPoint(const Vec3& point, const Vec3& normal, const Vec3& sunDirection, float bias) {
    const float facing = normal.dot(sunDirection);
    const float sine = std::sqrt(std::max(1.0f - facing * facing, 0.0f));
    return point + normal * ((facing < 0.0f ? -bias : bias) * sine);
}

namespace {
    // Bilinear, wrapping (bark tiles), each channel 0..1 as stored.
    std::array<float, 4> sampleTexture(const ImageData& image, const Vec2& uv) {
        const float x = uv.x * static_cast<float>(image.width) - 0.5f;
        const float y = uv.y * static_cast<float>(image.height) - 0.5f;
        const float left = std::floor(x);
        const float top = std::floor(y);
        const float across = x - left;
        const float down = y - top;
        const auto wrap = [](float coordinate, std::uint32_t size) {
            const long long whole = static_cast<long long>(coordinate) % static_cast<long long>(size);
            return static_cast<std::size_t>(whole < 0 ? whole + size : whole);
        };
        std::array<float, 4> result{};

        for (int corner = 0; corner < 4; ++corner) {
            const std::size_t column = wrap(left + static_cast<float>(corner % 2), image.width);
            const std::size_t row = wrap(top + static_cast<float>(corner / 2), image.height);
            const float weight = (corner % 2 ? across : 1.0f - across) * (corner / 2 ? down : 1.0f - down);
            const std::uint8_t* texel = &image.pixels[(row * image.width + column) * 4];

            for (int channel = 0; channel < 4; ++channel) {
                result[static_cast<std::size_t>(channel)] += weight * static_cast<float>(texel[channel]) / 255.0f;
            }
        }

        return result;
    }

    std::uint8_t toByte(float value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }

    // Twice the signed area of (a, b, p): which side of the edge a-b the point lies.
    float edge(const Vec2& a, const Vec2& b, const Vec2& p) {
        return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    }

    struct FrameTarget {
        ImageData& colour;
        ImageData& normal;
        std::uint32_t left = 0;
        std::uint32_t top = 0;
        int texels = 0;
    };

    // One frame: every part's triangles, nearest kept (depth grows towards the viewer).
    void bakeFrame(const std::vector<ImpostorPart>& parts, const Vec3& centre, float radius, const Vec3& direction, const FrameTarget& target) {
        const ImpostorBasis basis = impostorFrameBasis(direction);
        const float texels = static_cast<float>(target.texels);
        std::vector<float> nearest(static_cast<std::size_t>(target.texels) * static_cast<std::size_t>(target.texels),
                                   std::numeric_limits<float>::lowest());

        const auto project = [&](const Vec3& point) {
            const Vec3 offset = (point - centre) / radius;
            return Vec2{(offset.dot(basis.right) * 0.5f + 0.5f) * texels, (offset.dot(basis.up) * 0.5f + 0.5f) * texels};
        };

        for (const ImpostorPart& part : parts) {
            const IndexedMesh& mesh = *part.mesh;
            const Vec3 tint{srgbToLinear(part.tint.r / 255.0f), srgbToLinear(part.tint.g / 255.0f), srgbToLinear(part.tint.b / 255.0f)};

            for (std::size_t first = 0; first + 2 < mesh.indices.size(); first += 3) {
                const MeshVertex* corners[3] = {&mesh.vertices[mesh.indices[first]], &mesh.vertices[mesh.indices[first + 1]],
                                                &mesh.vertices[mesh.indices[first + 2]]};
                const Vec2 a = project(corners[0]->position);
                const Vec2 b = project(corners[1]->position);
                const Vec2 c = project(corners[2]->position);
                const float area = edge(a, b, c);

                if (std::abs(area) < 1e-12f) {
                    continue;
                }

                const float depths[3] = {(corners[0]->position - centre).dot(direction), (corners[1]->position - centre).dot(direction),
                                         (corners[2]->position - centre).dot(direction)};
                const int minX = std::max(static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))), 0);
                const int maxX = std::min(static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))), target.texels - 1);
                const int minY = std::max(static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))), 0);
                const int maxY = std::min(static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))), target.texels - 1);

                for (int y = minY; y <= maxY; ++y) {
                    for (int x = minX; x <= maxX; ++x) {
                        const Vec2 p{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
                        const float wa = edge(b, c, p) / area;
                        const float wb = edge(c, a, p) / area;
                        const float wc = 1.0f - wa - wb;

                        if (wa < 0.0f || wb < 0.0f || wc < 0.0f) {
                            continue;
                        }

                        const float depth = wa * depths[0] + wb * depths[1] + wc * depths[2];
                        const std::size_t slot = static_cast<std::size_t>(y) * static_cast<std::size_t>(target.texels) + static_cast<std::size_t>(x);

                        if (depth <= nearest[slot]) {
                            continue;
                        }

                        Vec3 colour = tint;

                        if (part.colour != nullptr && part.colour->width > 0) {
                            const Vec2 uv = corners[0]->uv * wa + corners[1]->uv * wb + corners[2]->uv * wc;
                            const std::array<float, 4> texel = sampleTexture(*part.colour, uv);

                            if (part.alphaCutoff > 0.0f && texel[3] < part.alphaCutoff) {
                                continue;
                            }

                            colour = Vec3{srgbToLinear(texel[0]) * tint.x, srgbToLinear(texel[1]) * tint.y, srgbToLinear(texel[2]) * tint.z};
                        }

                        Vec3 normal = (corners[0]->normal * wa + corners[1]->normal * wb + corners[2]->normal * wc).normalized();

                        if (normal.dot(direction) < 0.0f) {
                            normal = normal * -1.0f;
                        }

                        nearest[slot] = depth;
                        const std::size_t texel = ((target.top + static_cast<std::size_t>(y)) * target.colour.width + target.left + static_cast<std::size_t>(x)) * 4;
                        target.colour.pixels[texel] = linearToSrgbByte(colour.x);
                        target.colour.pixels[texel + 1] = linearToSrgbByte(colour.y);
                        target.colour.pixels[texel + 2] = linearToSrgbByte(colour.z);
                        target.colour.pixels[texel + 3] = 255;
                        target.normal.pixels[texel] = toByte(normal.x * 0.5f + 0.5f);
                        target.normal.pixels[texel + 1] = toByte(normal.y * 0.5f + 0.5f);
                        target.normal.pixels[texel + 2] = toByte(normal.z * 0.5f + 0.5f);
                        target.normal.pixels[texel + 3] = impostorDepthEncode(depth / radius);
                    }
                }
            }
        }
    }

    /*
    * Empty texels take the mean of their filled neighbours, ring by ring, within the frame: the colour's alpha stays 0 (the
    * coverage), the normal's alpha (the depth) spreads with it, so a view's filtered depth near an edge is the edge's.
    */
    void spreadIntoEmpty(const FrameTarget& target, int rings) {
        const int size = target.texels;
        std::vector<bool> filled(static_cast<std::size_t>(size) * static_cast<std::size_t>(size));
        const auto at = [&](int x, int y) {
            return ((target.top + static_cast<std::size_t>(y)) * target.colour.width + target.left + static_cast<std::size_t>(x)) * 4;
        };

        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                filled[static_cast<std::size_t>(y * size + x)] = target.colour.pixels[at(x, y) + 3] > 0;
            }
        }

        for (int ring = 0; ring < rings; ++ring) {
            std::vector<bool> next = filled;

            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    if (filled[static_cast<std::size_t>(y * size + x)]) {
                        continue;
                    }

                    float sums[2][4] = {};
                    int count = 0;

                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = x + dx;
                            const int ny = y + dy;

                            if (nx < 0 || ny < 0 || nx >= size || ny >= size || !filled[static_cast<std::size_t>(ny * size + nx)]) {
                                continue;
                            }

                            for (int channel = 0; channel < 4; ++channel) {
                                sums[0][channel] += target.colour.pixels[at(nx, ny) + static_cast<std::size_t>(channel)];
                                sums[1][channel] += target.normal.pixels[at(nx, ny) + static_cast<std::size_t>(channel)];
                            }

                            ++count;
                        }
                    }

                    if (count > 0) {
                        for (int channel = 0; channel < 3; ++channel) {
                            target.colour.pixels[at(x, y) + static_cast<std::size_t>(channel)] = static_cast<std::uint8_t>(sums[0][channel] / static_cast<float>(count));
                            target.normal.pixels[at(x, y) + static_cast<std::size_t>(channel)] = static_cast<std::uint8_t>(sums[1][channel] / static_cast<float>(count));
                        }

                        target.normal.pixels[at(x, y) + 3] = static_cast<std::uint8_t>(sums[1][3] / static_cast<float>(count));

                        next[static_cast<std::size_t>(y * size + x)] = true;
                    }
                }
            }

            filled = std::move(next);
        }
    }

    // How many rings of empty texels take their neighbours' colour: enough for the mips a frame's size has.
    constexpr int spreadRings = 8;
}

ImpostorBake bakeImpostor(const std::vector<ImpostorPart>& parts, int frames, int frameTexels, JobSystem& jobs) {
    if (frames < 2 || frameTexels < 1) {
        throw std::invalid_argument("An impostor needs at least 2 frames a side and a texel a frame");
    }

    Vec3 lowest{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 highest = lowest * -1.0f;

    for (const ImpostorPart& part : parts) {
        for (const MeshVertex& vertex : part.mesh->vertices) {
            lowest = Vec3{std::min(lowest.x, vertex.position.x), std::min(lowest.y, vertex.position.y), std::min(lowest.z, vertex.position.z)};
            highest = Vec3{std::max(highest.x, vertex.position.x), std::max(highest.y, vertex.position.y), std::max(highest.z, vertex.position.z)};
        }
    }

    if (!(highest.x >= lowest.x)) {
        throw std::invalid_argument("An impostor needs something to bake");
    }

    ImpostorBake bake;
    bake.centre = (lowest + highest) * 0.5f;
    bake.radius = std::max(((highest - lowest) * 0.5f).length(), 1e-4f);
    const std::uint32_t side = static_cast<std::uint32_t>(frames * frameTexels);

    for (ImageData* image : {&bake.colour, &bake.normal}) {
        image->width = side;
        image->height = side;
        image->pixels.assign(static_cast<std::size_t>(side) * side * 4, 0);
    }

    bake.colour.encoding = TextureEncoding::Srgb;
    bake.normal.encoding = TextureEncoding::Linear;
    // Its alpha is the depth the sun's test compares exactly: BC7 would smear it across a block at the silhouettes.
    bake.normal.storage = TextureStorage::Exact;

    jobs.parallelFor(static_cast<std::size_t>(frames) * static_cast<std::size_t>(frames), 1, [&](std::size_t index) {
        const int column = static_cast<int>(index) % frames;
        const int row = static_cast<int>(index) / frames;
        const FrameTarget target{bake.colour, bake.normal, static_cast<std::uint32_t>(column * frameTexels), static_cast<std::uint32_t>(row * frameTexels), frameTexels};
        bakeFrame(parts, bake.centre, bake.radius, impostorFrameDirection(Vec2{static_cast<float>(column), static_cast<float>(row)}, frames), target);
        spreadIntoEmpty(target, spreadRings);
    });

    return bake;
}

/*
* As impostor_sampling.glsl's impostorSunLight: from the leaf moved off by the normal offset, per frame round the sun,
* the 2 x 2 texels round the point's place (kept
* inside the frame) gathered from the depth (the normal's alpha) and the coverage (the colour's alpha), tested, filtered.
*/
float impostorSunLight(const ImpostorBake& bake, int frames, const Vec3& leaf, const Vec3& normal, const Vec3& sunDirection, float biasTexels) {
    const ImpostorBlend blend = impostorFrameBlend(sunDirection, frames);
    const int frameTexels = static_cast<int>(bake.normal.width) / frames;
    const float bias = impostorShadowBias(biasTexels, static_cast<float>(frameTexels));
    const Vec3 point = impostorShadowPoint(leaf, normal, sunDirection, bias);
    float lit = 0.0f;

    for (std::size_t corner = 0; corner < 4; ++corner) {
        const Vec3 direction = impostorFrameDirection(blend.frames[corner], frames);
        const Vec2 frameUv = impostorFrameUv(point, direction);

        if (frameUv.x < 0.0f || frameUv.y < 0.0f || frameUv.x > 1.0f || frameUv.y > 1.0f) {
            lit += blend.weights[corner];
            continue;
        }

        const Vec2 place = impostorAtlasUv(frameUv, blend.frames[corner], frames) * static_cast<float>(bake.normal.width) - Vec2{0.5f, 0.5f};
        const int frameLeft = static_cast<int>(blend.frames[corner].x) * frameTexels;
        const int frameTop = static_cast<int>(blend.frames[corner].y) * frameTexels;
        const int left = std::clamp(static_cast<int>(std::floor(place.x)), frameLeft, frameLeft + frameTexels - 2);
        const int top = std::clamp(static_cast<int>(std::floor(place.y)), frameTop, frameTop + frameTexels - 2);
        const Vec2 fraction{std::clamp(place.x - static_cast<float>(left), 0.0f, 1.0f), std::clamp(place.y - static_cast<float>(top), 0.0f, 1.0f)};
        // textureGather's order: (0, 1), (1, 1), (1, 0), (0, 0).
        const int offsets[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        std::array<float, 4> depths{};
        std::array<float, 4> covered{};

        for (std::size_t texel = 0; texel < 4; ++texel) {
            const std::size_t at = (static_cast<std::size_t>(top + offsets[texel][1]) * bake.normal.width + static_cast<std::size_t>(left + offsets[texel][0])) * 4 + 3;
            depths[texel] = static_cast<float>(bake.normal.pixels[at]) / 255.0f;
            covered[texel] = static_cast<float>(bake.colour.pixels[at]) / 255.0f;
        }

        lit += blend.weights[corner] * impostorGatherLit(depths, covered, fraction, point.dot(direction), bias);
    }

    return lit;
}

IndexedMesh impostorCardMesh(const Vec3& centre, float radius) {
    IndexedMesh card;

    for (const Vec2& corner : {Vec2{-1.0f, -1.0f}, Vec2{1.0f, -1.0f}, Vec2{1.0f, 1.0f}, Vec2{-1.0f, 1.0f}}) {
        MeshVertex vertex{};
        vertex.position = Vec3{centre.x + radius * corner.x, centre.y + radius * corner.y, centre.z};
        vertex.normal = Vec3{radius, 0.0f, 0.0f};
        vertex.uv = corner;
        card.vertices.push_back(vertex);
    }

    card.indices = {0, 1, 2, 0, 2, 3};
    return card;
}

Material impostorMaterial(TextureHandle colour, TextureHandle normal, int frames) {
    Material material;
    material.texture = colour;
    material.normalTexture = normal;
    material.alphaMode = AlphaMode::Mask;
    material.alphaCutoff = impostorCoverageCutoff;
    material.doubleSided = true;
    material.roughness = 0.8f;
    material.impostorFrames = frames;
    return material;
}
