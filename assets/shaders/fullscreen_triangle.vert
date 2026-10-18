#version 450

// One triangle big enough to cover the whole viewport, made in the shader (no vertex input), at the far depth, 1. Its
// corners lie at -1 and 3, so the visible square from -1 to 1 is inside it: one triangle instead of two avoids the seam
// along a quad's diagonal, where both triangles would shade the same pixels.
//
// Three kinds of pass use it. The shadow tile reset draws it with the depth test set to always pass, overwriting a tile
// with "nothing in the way" (the atlas is kept between frames, so only redrawn tiles are reset, and SDL cannot clear part
// of a texture by itself). Each bloom step draws it over one level of the chain, and the tone mapping pass draws it once
// per frame to run its shader on every pixel of the screen.
void main() {
    const vec2 corners[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corners[gl_VertexIndex], 1.0, 1.0);
}
