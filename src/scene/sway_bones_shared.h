#ifndef SCENE_SWAY_BONES_SHARED_H
#define SCENE_SWAY_BONES_SHARED_H

// Bindings to bones (scene/sway_bones.h) as the GPU reads them, numbers shared by C++ and GLSL.

// A vertex bound to no bone (it sways by its own vertices, or stands still): every pool vertex not bound has it.
#define SWAY_UNBOUND 0xFFFFFFFFu

// The most bones a skeleton holds: a binding keeps its bone in 16 bits, the all-ones bone left for SWAY_UNBOUND.
#define SWAY_MOST_BONES 65535

#endif
