#pragma once

#include "core/raw_image.h"

/*
* One frame read back from the GPU for the golden image check (engine/golden.h): the scene image as the main pass and
* bloom's input left it (half floats, linear light before tone mapping), and the final image as the screen would get it
* (8-bit sRGB for SDR, scRGB half floats for HDR).
*/
struct CapturedFrame {
    RawImage scene;
    RawImage final;
};
