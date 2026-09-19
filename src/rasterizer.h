#pragma once
#include "pixelbuffer.h"
#include "depthbuffer.h"

/*
* Draws a line between two points by interpolating between their coordinates.
*/
void drawLine(PixelBuffer& buffer, int x0, int y0, int x1, int y1, Pixel colour);

/*
* Draws the edges of a triangle to create its outline.
*/
void drawTriangleOutline(PixelBuffer& buffer, int x0, int y0, int x1, int y1, int x2, int y2, Pixel colour);

void fillTriangle(PixelBuffer& buffer, int x0, int y0, int x1, int y1, int x2, int y2, Pixel colour);
#include "depthbuffer.h"

void fillTriangleDepth(
    PixelBuffer& buffer,
    DepthBuffer& depthBuffer,

    int x0, int y0, float depth0,
    int x1, int y1, float depth1,
    int x2, int y2, float depth2,

    Pixel colour
);
