#include "rasterizer.h"
#include <algorithm>
#include <cmath>

static void plot(PixelBuffer& buffer, double x, double y, Pixel colour) {
    const double px = std::round(x);
    const double py = std::round(y);

    if (px >= 0 && py >= 0 && px < static_cast < double>(buffer.getWidth()) && py < static_cast<double>(buffer.getHeight())) {
        buffer.setPixel(static_cast<std::size_t > (px), static_cast<std::size_t>(py), colour);
    }
}

void drawLine(PixelBuffer& buffer, int x0, int y0, int x1, int y1, Pixel colour){
    const double dx = static_cast<double>(x1) - x0;
    const double dy = static_cast<double>(y1) - y0;
    const double steps = std::max(std::abs(dx), std::abs(dy));

    if (steps == 0) {
        plot(buffer, x0, y0, colour);
        return;
    }

    // By using steps and the individual stepping distance along each axis we avoid using a slope and dont run into singularities for vertical or horizontal lines.
    for (double i = 0; i <= steps; ++i) {
        const double t = i / steps;

        plot(buffer, x0 + t * dx, y0 + t * dy, colour);
    }
}