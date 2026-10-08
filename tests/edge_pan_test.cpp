#include "gameplay/edge_pan.h"

#include <cassert>

namespace {
    Vec2 panAt(float x, float y, bool insideWindow = true) {
        Input input;
        input.setWindowSize(Vec2{800.0f, 600.0f});
        input.setCursor(Vec2{x, y}, insideWindow);
        return getEdgePanDirection(input, 5.0f);
    }
}

void testEdgePan() {
    // Away from the edges: no panning.
    assert(panAt(400.0f, 300.0f).x == 0.0f);
    assert(panAt(400.0f, 300.0f).y == 0.0f);

    // Each edge, just inside the margin.
    assert(panAt(4.0f, 300.0f).x == -1.0f);
    assert(panAt(796.0f, 300.0f).x == 1.0f);
    assert(panAt(400.0f, 4.0f).y == -1.0f);
    assert(panAt(400.0f, 596.0f).y == 1.0f);

    // Just outside the margin does nothing.
    assert(panAt(5.0f, 300.0f).x == 0.0f);
    assert(panAt(794.0f, 300.0f).x == 0.0f);

    // A corner pans on both axes.
    const Vec2 corner = panAt(0.0f, 599.0f);
    assert(corner.x == -1.0f);
    assert(corner.y == 1.0f);

    // A cursor outside the window, or an unknown window size, never pans.
    assert(panAt(0.0f, 0.0f, false).x == 0.0f);

    Input noSize;
    noSize.setCursor(Vec2{0.0f, 0.0f}, true);
    assert(getEdgePanDirection(noSize).x == 0.0f);
}
