#pragma once

/*
* Dithering for the 8-bit screen. These mirror the end of triangle.frag, so tests can check the maths without a GPU.
*/

// Noise from a pixel's position, 0..1: cheap, needs no texture, and spreads values evenly over small patches.
float interleavedGradientNoise(float pixelX, float pixelY);

// A linear colour channel nudged by noise (0..1) by up to half an 8-bit sRGB step either way, so that after the screen
// rounds it, nearby pixels land on the two levels around the true value in the right proportion.
float ditherForEightBits(float linear, float noise);