#ifndef SCENE_IMPOSTOR_SHARED_H
#define SCENE_IMPOSTOR_SHARED_H

/*
* Impostor numbers shared by scene/impostor.cpp (the tested mirror) and impostor.glsl, so the two cannot drift apart. GLSL
* reads this file too, so it may only hold #defines of plain numbers.
*/

// How many points a card's scene sun shadow is read at (impostorPlantShadowSample): the plant's middle and one each
// way along its frame's three axes.
#define IMPOSTOR_PLANT_SHADOW_SAMPLES 7

#endif
