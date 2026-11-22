// A twig's record and matrix (render/gpu/twig_records.h), included by twig_drawing.glsl and, for the mesh shaders'
// twig instances, drawn_objects.glsl.
#ifndef TWIG_RECORD_GLSL
#define TWIG_RECORD_GLSL

#include "render/gpu/twig_drawing_shared.h"

// A twig: its matrix's three rows (prototype frame to plant frame), its lean and prototype. Must match GpuTwig.
struct Twig {
    vec4 rows[3];
    vec3 lean;
    uint prototype;
};

// The twig's matrix (its rows, then 0 0 0 1), column by column as GLSL holds it.
mat4 twigMatrix(Twig twig) {
    return transpose(mat4(twig.rows[0], twig.rows[1], twig.rows[2], vec4(0.0, 0.0, 0.0, 1.0)));
}

#endif
