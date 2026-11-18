#ifndef INSTANCE_SHARED_H
#define INSTANCE_SHARED_H

// What kind of object an instance is, beyond its matrix (GpuInstance::material[3], InstanceData.material.w in the
// shaders): a plain mesh; an impostor's card (scene/impostor.h), which the vertex shader turns to face the viewer; or a
// mesh following the ground (scene/ground_follow.h), each vertex lifted by the land's height at its place.
#define INSTANCE_KIND_PLAIN 0
#define INSTANCE_KIND_IMPOSTOR 1
#define INSTANCE_KIND_FOLLOWS_GROUND 2

// A draw list entry's top bit: the view draws the instance with its thinned mesh (scene/card_thinning.h), so the vertex
// shaders thin its cards.
#define DRAW_LIST_THINNED_BIT 0x80000000u

// Its next seven bits: the instance's level fade (level_fade.glsl, render/gpu/level_fade.h), 0 for none; the low 24
// bits are the instance's number.
#define DRAW_LIST_FADE_SHIFT 24u
#define DRAW_LIST_FADE_MASK 0x7f000000u
#define DRAW_LIST_INSTANCE_MASK 0x00ffffffu

// A fade field: this bit for the coarser of the two levels drawn, then the step plus 1 (1 to DRAW_LIST_FADE_STEPS).
#define DRAW_LIST_FADE_COARSE 64u
#define DRAW_LIST_FADE_STEPS 63u

#endif
