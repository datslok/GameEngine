// Cards thinned one by one: pure maths, mirrored by scene/card_thinning.h (each function tested against it). No buffer
// or texture reads here; card_thinning_data.glsl reads the records with these.
#ifndef CARD_THINNING_GLSL
#define CARD_THINNING_GLSL

#include "scene/card_thinning_shared.h"

// How far the cloud strays at a kept share: cardSize (share^-growth - 1). thinning: card size, growth, least share.
float thinningError(vec3 thinning, float share) {
    return thinning.x * (pow(share, -thinning.y) - 1.0);
}

// The share whose error is errorMetres, held between the least share and 1; 1 for a mesh not thinned.
float thinnedShare(vec3 thinning, float errorMetres) {
    if (thinning.x <= 0.0 || thinning.y <= 0.0 || errorMetres <= 0.0) {
        return 1.0;
    }

    float share = pow(1.0 + errorMetres / thinning.x, -1.0 / thinning.y);
    return clamp(share, min(thinning.z, 1.0), 1.0);
}

// How whole a card of this rank is at a share: 1 at or above its rank, 0 a band below; rank 0 always 1.
float cardPresence(float rank, float share) {
    return rank <= 0.0 ? 1.0 : smoothstep(rank - float(CARD_THINNING_BAND), rank, share);
}

// A card's size at a share: grown by share^-growth, times its presence.
float thinnedCardScale(vec3 thinning, float rank, float share) {
    return pow(share, -thinning.y) * cardPresence(rank, share);
}

// A corner of a card scaled about its anchor.
vec3 thinnedPosition(vec3 position, vec3 anchor, float scale) {
    return anchor + (position - anchor) * scale;
}

#endif
