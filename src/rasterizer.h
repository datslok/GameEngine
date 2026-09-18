#pragma once
#include "pixelbuffer.h"

void drawLine(PixelBuffer& buffer, int x0, int y0, int x1, int y1, Pixel colour);

void drawTriangleOutline(PixelBuffer& buffer, int x0, int y0, int x1, int y1, int x2, int y2, Pixel colour);

void fillTriangle(PixelBuffer& buffer, int x0, int y0, int x1, int y1, int x2, int y2, Pixel colour);