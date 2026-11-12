#ifndef RENDER_GPU_CONTACT_SHADOWS_SHARED_H
#define RENDER_GPU_CONTACT_SHADOWS_SHARED_H

// Contact shadows' numbers, shared by render/gpu/contact_shadows.h and the shaders (contact_shadows.glsl,
// contact_shadows.comp). Only #defines of plain numbers, since GLSL reads them too.

// The pass's workgroups: 8 x 8 pixels.
#define CONTACT_GROUP_SIDE 8

// The most steps a march may take (the settings' limit).
#define CONTACT_MAX_STEPS 64

// The shadow fades to nothing over this many pixels at the screen's edge, past which the depth image knows nothing.
#define CONTACT_EDGE_PIXELS 16

// A point counts as behind the surface only past this many pixels' footprint at its distance, so a surface never
// shadows itself through the depth image's rounding or its own slope within a pixel.
#define CONTACT_BIAS_PIXELS 2.0

// Each frame turns the march's start by this share of a step (the golden ratio's), so the frames' starts spread evenly
// and the anti-aliasing averages them.
#define CONTACT_NOISE_FRAME_STEP 0.61803398875

// A march stops where its point comes closer to the camera than this (metres along the view), behind it or nearly.
#define CONTACT_NEAREST_W 0.05

// The most point and spot lights that get contact shadows at once (the local mask's four channels).
#define CONTACT_MAX_LIGHTS 4

// A march towards a point or spot light stops this far (metres) short of its glowing part, so the light's own bulb or
// fixture, which the depth image shows, does not shadow it: a street bulb's glow is a 24 cm box round the light.
#define CONTACT_LIGHT_CLEARANCE 0.25

// The frame's data names a point light with a contact shadow channel by its slot, a spotlight by this plus its slot.
#define CONTACT_SPOT_SLOT_BASE 65536

// A cone cosine below any real one: a point light, lighting every way.
#define CONTACT_NO_CONE -2.0

#endif
