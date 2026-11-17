#ifndef SCENE_CARD_THINNING_SHARED_H
#define SCENE_CARD_THINNING_SHARED_H

// Cards thinned one by one (scene/card_thinning.h and its GPU twin, card_thinning.glsl). Only #defines of plain
// numbers, since GLSL includes this file too.

// How far below its rank the kept share falls while a card shrinks from whole to nothing: a share of the cards, so about
// this many of them are shrinking at any moment and none pops.
#define CARD_THINNING_BAND 0.05

#endif
