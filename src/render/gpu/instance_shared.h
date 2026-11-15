#ifndef INSTANCE_SHARED_H
#define INSTANCE_SHARED_H

// What kind of object an instance is, beyond its matrix (GpuInstance::material[3], InstanceData.material.w in the
// shaders): a plain mesh; an impostor's card (scene/impostor.h), which the vertex shader turns to face the viewer; or a
// mesh following the ground (scene/ground_follow.h), each vertex lifted by the land's height at its place.
#define INSTANCE_KIND_PLAIN 0
#define INSTANCE_KIND_IMPOSTOR 1
#define INSTANCE_KIND_FOLLOWS_GROUND 2

#endif
