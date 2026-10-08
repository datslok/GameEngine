#include "gameplay/edge_pan.h"

/*
* Each axis is checked on its own, so a corner pans diagonally.
*/
Vec2 getEdgePanDirection(const Input& input, float margin) {
    const Vec2 size = input.getWindowSize();

    if (!input.isCursorInWindow() || size.x <= 0.0f || size.y <= 0.0f) {
        return Vec2{};
    }

    const Vec2 cursor = input.getCursorPosition();
    Vec2 direction{};

    if (cursor.x < margin) {
        direction.x = -1.0f;
    } else if (cursor.x >= size.x - margin) {
        direction.x = 1.0f;
    }

    if (cursor.y < margin) {
        direction.y = -1.0f;
    } else if (cursor.y >= size.y - margin) {
        direction.y = 1.0f;
    }

    return direction;
}
