#pragma once

#include "math/vec3.h"
#include "render/gpu/bloom_shared.h"
#include "render/gpu/uniform_limits.h"

#include <cstdint>
#include <vector>

/*
* Bloom: no lens or eye focuses a point of light to a perfect point. A little of all light scatters (dust and scratches
* on the glass, the fluid inside the eye), so the image is the scene blurred by a point spread function with a sharp core
* and faint, wide tails. Under ordinary light the tails drown in the surfaces' own light; around something thousands of
* times brighter than its surroundings (the moon at night, a lamp) they show as a halo.
*
* The engine models it as final = (1 - strength) * scene + strength * blur(scene), where the blur's weights sum to 1:
* light is moved around, never made or lost, and there is no brightness threshold. A wide blur straight on the screen
* would read hundreds of texels per pixel, so the scene is instead halved again and again (a chain of levels), lightly
* filtered at each halving, then grown back level by level, each level adding the one below it spread by a small tent.
* A small filter on a level 2^k times smaller covers 2^k times more of the screen, so the whole chain costs about a third
* of one full-screen pass, and the sum of blurs of doubling width has the sharp core and long tails of a real one.
*
* These functions mirror the bloom shaders (bloom_downsample.frag, bloom_upsample.frag and the mix in tone_map.frag) on
* the CPU, so tests can check the maths without a GPU.
*/

struct BloomSettings {
    bool enabled = true;

    // The share of the light that scatters, 0..1. Small: a real lens scatters a few percent.
    float strength = 0.04f;

    // How far apart the upsample's tent reads, in texels of the smaller level. 1 is the tent as designed; more spreads
    // each level wider (the halo grows), less tightens it.
    float radius = 1.0f;

    // The Karis average on the first halving (karisWeight), against fireflies: a single extremely bright pixel would
    // otherwise bloom into a blob that flickers as the pixel moves. Its price is that such pixels lose some of their glow;
    // off, every pixel's light is kept exactly.
    bool fireflyReduction = true;

    // How many levels the chain may have, 1..BLOOM_LEVEL_LIMIT; fewer on small screens (bloomLevelCount). Each level
    // doubles the halo's reach.
    std::uint32_t maxLevels = 6;
};

// Throws std::invalid_argument for a strength outside 0..1, a radius that is not positive, or a level count outside
// 1..BLOOM_LEVEL_LIMIT.
void validateBloom(const BloomSettings& settings);

/*
* How many levels the chain has for a frame this size: level 0 is half the frame, each next level half the last
* (rounded down), and a level whose shorter side would be below BLOOM_SMALLEST_LEVEL_SIDE is not made. At most
* maxLevels; 0 for a frame too small for any level, which draws no bloom.
*/
std::uint32_t bloomLevelCount(std::uint32_t frameWidth, std::uint32_t frameHeight, std::uint32_t maxLevels);

struct BloomLevelSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Level `level`'s size: the frame halved level + 1 times, rounded down like mip levels.
BloomLevelSize bloomLevelSize(std::uint32_t frameWidth, std::uint32_t frameHeight, std::uint32_t level);

// An image of linear colours, row by row from the top, as the CPU stand-in for a GPU texture.
struct BloomImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<Vec3> pixels;

    BloomImage() = default;
    BloomImage(std::uint32_t width, std::uint32_t height, const Vec3& fill = Vec3{});

    Vec3& at(std::uint32_t x, std::uint32_t y);
    const Vec3& at(std::uint32_t x, std::uint32_t y) const;
};

/*
* What the GPU's linear sampler returns at (u, v), 0..1 across the image: texel centres sit at (i + 0.5) / size, the four
* nearest are blended by distance, and reads past the edge repeat the edge texel (clamp to edge).
*/
Vec3 sampleBilinear(const BloomImage& image, float u, float v);

// The weight the Karis average gives a box of this average colour: 1 / (1 + luminance), so a lone, extremely bright
// pixel (a specular sparkle) cannot dominate a level and flicker as it moves.
float karisWeight(const Vec3& colour);

/*
* One halving: each texel of the smaller image is the 13-tap filter of the source around the same place. With karis
* (only from the full-size scene), each of the five boxes is also weighted by karisWeight and the result divided by
* the weights' sum, so a uniform area is unchanged. Throws std::invalid_argument for an empty size.
*/
BloomImage bloomDownsample(const BloomImage& source, BloomLevelSize size, bool karis);

// One growing step: adds to each texel of target the tent filter of smaller around the same place, the tent's reads
// `radius` texels of smaller apart (the GPU does this with additive blending).
void bloomUpsampleAdd(BloomImage& target, const BloomImage& smaller, float radius);

/*
* The whole chain for a scene: the result is level 0 (half the frame) after growing, already divided by the number of
* levels, so its weights sum to 1 like a true blur. Empty if the frame has no levels. Throws like validateBloom.
*/
BloomImage bloomBlur(const BloomImage& scene, const BloomSettings& settings);

// The mix: (1 - strength) * scene + strength * bloom. A strength of 0 gives back the scene exactly.
Vec3 mixBloom(const Vec3& scene, const Vec3& bloom, float strength);

/*
* What the tone mapping pass needs to mix bloom in: the strength (0 = no bloom this frame, so the bloom texture is not
* read) and what to multiply the grown level 0 by (1 / the number of levels).
*/
struct BloomComposite {
    float strength = 0.0f;
    float scale = 1.0f;
};

// The composite for a chain of this many levels: no bloom when it is switched off or there are no levels.
BloomComposite bloomComposite(const BloomSettings& settings, std::uint32_t levelCount);

/*
* Mirrors the BloomPassData block in bloom_downsample.frag and bloom_upsample.frag (std140, two vec4s): the source's and
* target's texel sizes, and the pass's option (1 for the Karis average when shrinking, the tent's radius when growing).
*/
struct BloomPassUniformData {
    float texelSizes[4]; // xy: one source texel in 0..1 units, zw: one target texel
    float options[4];    // x: the pass's option, yzw: unused
};

static_assert(sizeof(BloomPassUniformData) == 32, "BloomPassUniformData must match the shaders' BloomPassData block");
static_assert(sizeof(BloomPassUniformData) <= maxUniformBlockBytes, "the shader can only read the first 4 KB of a uniform block");

BloomPassUniformData packBloomPass(BloomLevelSize source, BloomLevelSize target, float option);
