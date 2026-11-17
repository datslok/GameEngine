#pragma once

#include "math/vec3.h"
#include "scene/card_thinning_shared.h"

/*
* Cards thinned one by one (docs/LOD.md, Hiding the switches): instead of swapping a crown of leaf cards for a coarser
* mesh of fewer, bigger cards in one frame, the vertex shaders shrink the cards that mesh leaves out away one by one
* as the view allows more error, and grow the rest towards their size there, so the crown never jumps (SpeedTree's
* runtime; Cook et al. 2007's stochastic simplification). The coarser mesh is the kept cards alone, first in the mesh,
* so where the fade ends the switch changes nothing seen.
*
* The share kept is spent exactly as the levels spend their error: the kept cards' sides grow by share^-growth, so the
* cloud strays by cardSize (share^-growth - 1), and the share drawn is the one whose error the view allows. Growth 0.5
* keeps the cards' area (Cook); less keeps them smaller, which people preferred in Neubert et al. 2011's study.
*/
struct CardThinning {
    float cardSize = 0.0f;  // the cards' side, in metres, which the error grows from; 0 for a mesh not thinned
    float growth = 0.5f;    // the kept cards' sides grow by share^-growth
    float leastShare = 1.0f; // the share the coarsest thinning keeps (the coarser mesh's)
};

// A card's record, one a vertex of a thinned mesh: the point its card grows and shrinks about (its stem) and its rank,
// the share below which it shrinks away (0: a card the least share keeps, always whole).
struct ThinningCard {
    Vec3 anchor;
    float rank = 0.0f;
};

inline constexpr float cardThinningBand = static_cast<float>(CARD_THINNING_BAND);

// How far the cloud strays at a kept share: cardSize (share^-growth - 1); 0 at a full share.
float thinningError(const CardThinning& thinning, float share);

// The share whose error is errorMetres (thinningError's inverse), held between the least share and 1; 1 for a mesh not
// thinned.
float thinnedShare(const CardThinning& thinning, float errorMetres);

// How whole a card of this rank is at a share: 1 at or above its rank, 0 a band below, smooth between; rank 0 always 1.
float cardPresence(float rank, float share);

// A card's size at a share: grown by share^-growth, times its presence.
float thinnedCardScale(const CardThinning& thinning, float rank, float share);

// A corner of a card scaled about its anchor.
Vec3 thinnedPosition(const Vec3& position, const ThinningCard& card, float scale);

// A rank for a card the least share leaves out, from a random number in 0..1: spread over [leastShare + band, 1], so
// every such card is gone at the least share.
float removedCardRank(float unitRandom, float leastShare);
