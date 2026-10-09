#version 450

// Resets one shadow tile to "nothing in the way": a triangle big enough to cover the whole viewport (the tile), at the
// far depth, 1. It is drawn with the depth test set to always pass, so it overwrites whatever was there. The atlas is
// kept between frames, so only the tiles being redrawn are reset, and SDL cannot clear part of a texture by itself.
void main() {
    const vec2 corners[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corners[gl_VertexIndex], 1.0, 1.0);
}
