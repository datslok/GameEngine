// Twigs drawn as instances of prototypes, included with #include "twig_drawing.glsl": mirrors render/gpu/twig_records.h
// (twigInstance, twigLevel, twigBrickKept, twigKept) and declares its records. Pure functions, the errors' sides precise,
// so the twig pass chooses what the cluster cut would.
#ifndef TWIG_DRAWING_GLSL
#define TWIG_DRAWING_GLSL

#include "render/gpu/twig_drawing_shared.h"
#include "mesh_cluster_culling.glsl"

// A twig: its matrix's three rows (prototype frame to plant frame), its lean and prototype. Must match GpuTwig.
struct Twig {
    vec4 rows[3];
    vec3 lean;
    uint prototype;
};

// A brick: its sphere in the plant's frame and its twigs' run. Must match GpuTwigBrick.
struct TwigBrick {
    vec4 sphere;
    uint firstTwig;
    uint twigCount;
    uint unused0;
    uint unused1;
};

// A prototype's level: its group's sphere and error, its clusters' run. Must match GpuTwigLevel.
struct TwigLevel {
    vec4 sphere;
    float error;
    uint firstCluster;
    uint clusterCount;
    uint unused;
};

// A prototype's levels in the level table. Must match GpuTwigPrototype.
struct TwigPrototype {
    uint firstLevel;
    uint levelCount;
    uint unused0;
    uint unused1;
};

// The twig's matrix (its rows, then 0 0 0 1), column by column as GLSL holds it.
mat4 twigMatrix(Twig twig) {
    return transpose(mat4(twig.rows[0], twig.rows[1], twig.rows[2], vec4(0.0, 0.0, 0.0, 1.0)));
}

// A twig of a placed part (twigInstance): the twig's matrix after the part's, its turn after the part's normals, its
// scale (its across column's length) on the part's.
InstanceData twigInstance(InstanceData part, Twig twig) {
    mat4 matrix = twigMatrix(twig);
    mat4 turn = matrix;
    turn[3] = vec4(0.0, 0.0, 0.0, 1.0);
    InstanceData instance = part;
    instance.model = part.model * matrix;
    instance.normalMatrix = part.normalMatrix * turn;
    instance.handedness.y = part.handedness.y * length(vec3(twig.rows[0].x, twig.rows[1].x, twig.rows[2].x));
    return instance;
}

/*
* Whether a twig draws a level (twigLevelDrawn; the pass walks a prototype's levels with it, as twigLevel does): its own
* error (the level before's group's, none for the finest) fits the view and its group's does not.
*/
bool twigLevelDrawn(TwigLevel before, bool finest, TwigLevel level, mat4 model, LevelView view) {
    bool ownFits = finest || meshClusterErrorPixels(before.sphere, before.error, model, view) <= view.pixelError;
    bool coarserFits = level.error <= MESH_CLUSTER_NEVER_COARSER && meshClusterErrorPixels(level.sphere, level.error, model, view) <= view.pixelError;
    return ownFits && !coarserFits;
}

// Whether a view keeps a sphere placed by a model: always where the part is not rigid, else in the frustum.
bool twigSphereKept(vec4 sphere, mat4 placedBy, InstanceData part, uint entry, CullRecord record, CameraCull view) {
    if (!meshClusterRigid(part, entry, record, view)) {
        return true;
    }

    return sphereInFrustum(view.frustum, meshClusterPlacedSphere(sphere, placedBy));
}

// A brick (twigBrickKept): its sphere in the plant's frame, placed by the part.
bool twigBrickKept(TwigBrick brick, InstanceData part, uint entry, CullRecord record, CameraCull view) {
    return twigSphereKept(brick.sphere, part.model, part, entry, record, view);
}

// A twig (twigKept): its prototype's coarsest group's sphere, placed by the twig.
bool twigKept(TwigLevel coarsest, InstanceData twig, uint entry, CullRecord record, CameraCull view) {
    return twigSphereKept(coarsest.sphere, twig.model, twig, entry, record, view);
}

#endif
