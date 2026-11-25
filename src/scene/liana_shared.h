#ifndef SCENE_LIANA_SHARED_H
#define SCENE_LIANA_SHARED_H

// The lianas' numbers, shared by scene/liana_shape.h and the shaders (liana.glsl, liana_data.glsl). Only #defines of
// plain numbers, since GLSL reads them too.

// Set in a draw's liana name (ModelRenderer::lianaShape, the instance's hanging.w) when it draws the liana's ivy, not
// its tube; the rest is one past its record.
#define LIANA_IVY_BIT 0x80000000u

// A liana's anchor on no planted tree (LianaShape::anchors' layers): a rope's foot in the ground, fixed.
#define LIANA_NO_TREE 0xFFFFFFFFu

// The ivy atlas (LeafSet017): two leaves across, three up, each cell 3:2, so a card is this many times its length wide.
#define LIANA_LEAF_COLUMNS 2
#define LIANA_LEAF_ROWS 3
#define LIANA_LEAF_WIDTH_SHARE 1.5

// How much a leaf droops from pointing straight out, and how far a card strays from its even place (a share of its
// spacing, either way half of it).
#define LIANA_LEAF_DROOP 0.6
#define LIANA_LEAF_JITTER 0.6

// Thinning by distance: over this share of the thinning distance past it, every other card shrinks away while the rest
// grow by the square root of 2 (the same leaf area); over this share of the ivy's last distance before it, all shrink
// away.
#define LIANA_LEAF_FADE_BAND 0.25

// The third word of each card's hash (pcg3d of the liana's seed, the card and this).
#define LIANA_LEAF_HASH_USE 0x1eafu

// The ivy templates' fewest and most cards (lianaLeafTemplateCards: each twice the one before).
#define LIANA_LEAF_FEWEST_CARDS 16
#define LIANA_LEAF_MOST_CARDS 256

// How far from its x axis every template vertex lies (lianaClusterSpan): the tube's on its unit circle (cos, sin), the
// ivy's corners at (+-1, +-1).
#define LIANA_TUBE_TEMPLATE_RADIUS 1.0
#define LIANA_IVY_TEMPLATE_RADIUS 1.41421356

// The tube template's length: its x is its share along times this, so its rings lie as far apart as about its width
// round (a unit a ring at the forest's finest level) and the clusters cut from it are short runs of whole rings, not
// strips its whole length (lianaTemplateMesh).
#define LIANA_TUBE_TEMPLATE_LENGTH 64.0

// A span's bound grown by this share of the liana's length (lianaSpanBound): the float curve strays from the exact one
// by millimetres (tests/liana_shape_test.cpp).
#define LIANA_SPAN_SLACK 0.001

#endif
