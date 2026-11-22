#ifndef BONE_RECORDS_SHARED_H
#define BONE_RECORDS_SHARED_H

// Trees' bones as the GPU reads them (render/gpu/bone_records.h), numbers shared by C++ and GLSL.

// A vertex bound to no bone (it sways by its own vertices, or stands still): every pool vertex not bound has it.
#define SWAY_UNBOUND 0xFFFFFFFFu

// The most bones a skeleton holds: a binding keeps its bone in 16 bits, the all-ones bone left for SWAY_UNBOUND.
#define SWAY_MOST_BONES 65535

#endif
