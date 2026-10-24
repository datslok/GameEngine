// The shadow tiles and the shadow atlas in the frame's set 1 (render/gpu/shadow_map.h), and one plain comparison against
// a tile. Declarations and a pure lookup, included by the main pass (scene_surface.glsl) and the fog's grid
// (fog_light.comp), after frame_data.glsl (the atlas texel size).
#ifndef SCENE_SHADOW_TILES_GLSL
#define SCENE_SHADOW_TILES_GLSL

// Must match ShadowTileData in render/gpu/shadow_map.h.
struct ShadowTileData {
    mat4 matrix; // the shadow pass's view-projection, for points measured from the camera
    vec4 offset; // against acne: x per unit of distance from the light, y fixed (a sun cascade's: its base bias towards the sun)
    vec4 rect;   // where the tile is in the atlas, 0..1: xy corner, zw size
    // Perspective tiles: x near, y far plane, z world size of a texel per unit of distance. A sun's cascade boxes: x 0
    // (no near plane), y the box's depth, z one texel's world size, w the cascade's radius.
    vec4 lens;
};

layout(std430, set = 1, binding = 6) readonly buffer ShadowTiles {
    ShadowTileData shadowTiles[];
};

// The shadow atlas: every light's depth images in square tiles of 256 to 2048 texels, packed into one texture. A shadow sampler compares a depth we give it
// with the stored one and returns 1 (lit) or 0 (blocked), blended across neighbouring texels for slightly soft edges.
layout(set = 1, binding = 7) uniform sampler2DShadow shadowAtlas;

// The same atlas read as plain numbers, the stored depths themselves, which soft shadows need to find how far away the
// things blocking a light are (a comparison only says whether something is in the way).
layout(set = 1, binding = 8) uniform sampler2D shadowDepths;

/*
* One comparison against a tile at a point (measured from the camera), the hardware blending the four nearest texels'
* answers: 1 lit, 0 blocked, and lit outside the tile's view. For points in the air (the fog's grid), where the frames'
* moving samples and last frame's light average the edges, so the main pass's wider filters would only cost.
*/
float shadowTap(int tile, vec3 position) {
    if (shadowTiles[tile].rect.z <= 0.0) {
        return 1.0;
    }

    vec4 clip = shadowTiles[tile].matrix * vec4(position, 1.0);
    vec3 projected = clip.xyz / clip.w;

    if (clip.w <= 0.0 || any(greaterThan(abs(projected.xy), vec2(1.0))) || projected.z < 0.0 || projected.z > 1.0) {
        return 1.0;
    }

    vec2 withinTile = vec2(projected.x * 0.5 + 0.5, 0.5 - projected.y * 0.5);
    return texture(shadowAtlas, vec3(shadowTiles[tile].rect.xy + withinTile * shadowTiles[tile].rect.zw, projected.z));
}

#endif
