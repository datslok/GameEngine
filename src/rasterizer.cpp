#include "rasterizer.h"
#include <algorithm>
#include <cmath>

static long long edgeFunction(
    int ax, int ay,
    int bx, int by,
    int px, int py
) {
    return
        (static_cast<long long>(bx) - ax) *
        (static_cast<long long>(py) - ay)
        -
        (static_cast<long long>(by) - ay) *
        (static_cast<long long>(px) - ax);
}

void fillTriangleDepth(
    PixelBuffer& buffer,
    DepthBuffer& depthBuffer,

    int x0, int y0, float depth0,
    int x1, int y1, float depth1,
    int x2, int y2, float depth2,

    Pixel colour
){
    const int bufferWidth =
        static_cast<int>(buffer.getWidth());

    const int bufferHeight =
        static_cast<int>(buffer.getHeight());

    if (bufferWidth == 0 || bufferHeight == 0){
        return;
    }

    const int minX =
        std::max(0, std::min({x0, x1, x2}));

    const int maxX =
        std::min(bufferWidth - 1, std::max({x0, x1, x2}));

    const int minY =
        std::max(0, std::min({y0, y1, y2}));

    const int maxY =
        std::min(bufferHeight - 1, std::max({y0, y1, y2}));

    if (minX > maxX || minY > maxY) {
        return;
    }

    const long long area = edgeFunction(x0, y0, x1, y1, x2, y2);

    if (area == 0){
        return;
    }

    for (int y = minY; y <= maxY; ++y){
        for (int x = minX; x <= maxX; ++x){
            const long long edge0 =
                edgeFunction(x1, y1, x2, y2, x, y);

            const long long edge1 =
                edgeFunction(x2, y2, x0, y0, x, y);

            const long long edge2 =
                edgeFunction(x0, y0, x1, y1, x, y);

            const bool allPositive =
                edge0 >= 0 &&
                edge1 >= 0 &&
                edge2 >= 0;

            const bool allNegative =
                edge0 <= 0 &&
                edge1 <= 0 &&
                edge2 <= 0;

            if (!allPositive && !allNegative){
                continue;
            }

            const float weight0 =
                static_cast<float>(edge0) /
                static_cast<float>(area);

            const float weight1 =
                static_cast<float>(edge1) /
                static_cast<float>(area);

            const float weight2 =
                static_cast<float>(edge2) /
                static_cast<float>(area);

            const float depth =
                weight0 * depth0 +
                weight1 * depth1 +
                weight2 * depth2;

            if (depthBuffer.testAndSet(
                    static_cast<std::size_t>(x),
                    static_cast<std::size_t>(y),
                    depth)) {
                buffer.setPixel(
                    static_cast<std::size_t>(x),
                    static_cast<std::size_t>(y),
                    colour
                );
            }
        }
    }
}

/*
* Round coordinates to pixel position and set the pixel when it lies within the buffer.
*/
static void plot(PixelBuffer& buffer, double x, double y, Pixel colour){
    const double px = std::round(x);
    const double py = std::round(y);

    if (px >= 0 && py >= 0 && px < static_cast < double>(buffer.getWidth()) && py < static_cast<double>(buffer.getHeight())) {
        buffer.setPixel(static_cast<std::size_t > (px), static_cast<std::size_t>(py), colour);
    }
}

/*
* Draw a line by interpolating between endpoints, using enough steps to ensure a continous line across the pixel buffer.
*/
void drawLine(PixelBuffer& buffer, int x0, int y0, int x1, int y1, Pixel colour){
    const double dx = static_cast<double>(x1) - x0;
    const double dy = static_cast<double>(y1) - y0;
    const double steps = std::max(std::abs(dx), std::abs(dy));

    if (steps == 0) {
        plot(buffer, x0, y0, colour);
        return;
    }

    // By using steps and the individual stepping distance along each axis, we avoid using a slope and don't run into singularities for vertical or horizontal lines.
    for (double i = 0; i <= steps; ++i) {
        const double t = i / steps;

        plot(buffer, x0 + t * dx, y0 + t * dy, colour);
    }
}

/*
* Draw each pair of triangle vertices to create a complete outline of the triangle.
*/
void drawTriangleOutline(PixelBuffer& buffer, int x0, int y0, int x1, int y1, int x2, int y2, Pixel colour){
    drawLine(buffer, x0, y0, x1, y1, colour);
    drawLine(buffer, x1, y1, x2, y2, colour);
    drawLine(buffer, x2, y2, x0, y0, colour);
}

/*
* Fill the area within the triangle by checking pixels within the bounding box, reducing unnecessary checks outside the area where the triangle can exist.
* The edge function is used to determine whether a pixel lies within the triangle.
*/
void fillTriangle(PixelBuffer& buffer, int x0, int y0, int x1, int y1, int x2, int y2, Pixel colour){
    const int bufferWidth = static_cast<int>(buffer.getWidth());
    const int bufferHeight = static_cast<int>(buffer.getHeight());

    if (bufferWidth == 0 || bufferHeight == 0){
        return;
    }

    const int minX = std::max(0, std::min({x0, x1, x2}));
    const int maxX = std::min(bufferWidth - 1, std::max({x0, x1, x2}));
    const int minY = std::max(0, std::min({y0, y1, y2}));
    const int maxY = std::min(bufferHeight - 1, std::max({y0, y1, y2}));

    if (minX > maxX || minY > maxY){
        return;
    }

    const long long area = edgeFunction(x0, y0, x1, y1, x2, y2);

    // The three vertices are collinear.
    if (area == 0){
        return;
    }

    for (int y = minY; y <= maxY; ++y){
        for (int x = minX; x <= maxX; ++x){
            const long long edge0 =
                edgeFunction(x1, y1, x2, y2, x, y);

            const long long edge1 =
                edgeFunction(x2, y2, x0, y0, x, y);

            const long long edge2 =
                edgeFunction(x0, y0, x1, y1, x, y);

            const bool allPositive =
                edge0 >= 0 && edge1 >= 0 && edge2 >= 0;

            const bool allNegative =
                edge0 <= 0 && edge1 <= 0 && edge2 <= 0;

            if (allPositive || allNegative){
                buffer.setPixel(
                    static_cast<std::size_t>(x),
                    static_cast<std::size_t>(y),
                    colour
                );
            }
        }
    }
}