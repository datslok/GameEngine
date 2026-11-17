// Card thinning's records as the scene's vertex shaders read them (render/gpu/gpu_scene.h, packCardThinning; scene/
// card_thinning.h), and what they do to a vertex. Included by the passes that thin cards: the camera's, the velocity
// pass and the captures (never the shadow tiles, whose draw list entries are never marked).
#ifndef CARD_THINNING_DATA_GLSL
#define CARD_THINNING_DATA_GLSL

#include "card_thinning.glsl"
#include "instances.glsl"

// A thinned mesh's header (card size, growth, least share) and then one record a vertex (its card's anchor and rank);
// a mesh's entry is one past its header.
layout(std430, set = 1, binding = 42) readonly buffer ThinningCards {
    vec4 thinningRecords[];
};

/*
* A vertex of an instance as the view thins it (entry its draw list entry, marked where the view draws its thinned mesh;
* vertex its number in the mesh; eye the viewer measured from the camera; errorPerMetre the view's error allowed a
* metre away): the share kept at the distance its level is chosen at (to the instance's level sphere's surface, as
* levelDistance), its card shrunk or grown about its anchor. Unmarked entries, and meshes without records, unchanged.
*/
vec3 thinnedVertex(InstanceData instance, uint entry, int vertex, vec3 position, vec3 eye, vec3 cameraOrigin, float errorPerMetre) {
    uint records = instance.hanging.z;

    if ((entry & DRAW_LIST_THINNED_BIT) == 0u || records == 0u || errorPerMetre <= 0.0) {
        return position;
    }

    vec4 header = thinningRecords[records - 1u];
    vec4 card = thinningRecords[records + uint(vertex)];
    vec4 sphere = instance.levelSphere;
    float distance = sphere.w < 0.0 ? 0.0 : max(length(sphere.xyz - cameraOrigin - eye) - sphere.w, 0.0);
    float share = thinnedShare(header.xyz, distance * errorPerMetre);
    return thinnedPosition(position, card.xyz, thinnedCardScale(header.xyz, card.w, share));
}

#endif
