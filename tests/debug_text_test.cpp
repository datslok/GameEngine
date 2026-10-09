#include "engine/debug_views.h"
#include "render/render_stats.h"
#include "scene/debug_draw.h"
#include "scene/debug_text.h"

#include <cassert>
#include <string>

namespace {
    const Vec3 white{1.0f, 1.0f, 1.0f};

    // Every line end of every screen line lies inside the rectangle.
    bool allInside(const DebugDraw& debug, float left, float top, float right, float bottom) {
        for (const DebugLine& line : debug.getScreenLines()) {
            for (const Vec3& end : {line.from, line.to}) {
                if (end.x < left - 0.001f || end.x > right + 0.001f || end.y < top - 0.001f || end.y > bottom + 0.001f) {
                    return false;
                }
            }
        }
        return true;
    }

    // Screen lines are kept apart from world lines: they are in window pixels, from the top-left corner.
    void testScreenLinesAreSeparate() {
        DebugDraw debug;
        debug.screenLine(Vec2{1.0f, 2.0f}, Vec2{3.0f, 4.0f}, white);

        assert(debug.getLines().empty());
        assert(debug.getScreenLines().size() == 1);
        assert(debug.getScreenLines()[0].to.x == 3.0f && debug.getScreenLines()[0].to.y == 4.0f);

        debug.clear();
        assert(debug.getScreenLines().empty());
    }

    // Every letter, digit and the punctuation the overlays use has strokes; a space has none.
    void testEveryCharacterHasStrokes() {
        const std::string characters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:-/.";

        for (char character : characters) {
            DebugDraw debug;
            debugText(debug, Vec2{0.0f, 0.0f}, 12.0f, std::string(1, character), white);
            assert(!debug.getScreenLines().empty());
        }

        DebugDraw space;
        debugText(space, Vec2{0.0f, 0.0f}, 12.0f, " ", white);
        assert(space.getScreenLines().empty());
    }

    // Lower case is drawn as upper case, and characters without a glyph show as a question mark rather than nothing.
    void testFallbacks() {
        DebugDraw lower;
        DebugDraw upper;
        debugText(lower, Vec2{0.0f, 0.0f}, 12.0f, "abc", white);
        debugText(upper, Vec2{0.0f, 0.0f}, 12.0f, "ABC", white);
        assert(lower.getScreenLines().size() == upper.getScreenLines().size());

        DebugDraw unknown;
        DebugDraw question;
        debugText(unknown, Vec2{0.0f, 0.0f}, 12.0f, "#", white);
        debugText(question, Vec2{0.0f, 0.0f}, 12.0f, "?", white);
        assert(!unknown.getScreenLines().empty());
        assert(unknown.getScreenLines().size() == question.getScreenLines().size());
    }

    // Text fills its height and runs right by a fixed advance per character, so its size is known before drawing it.
    void testTextStaysInItsBox() {
        DebugDraw debug;
        const std::string text = "DRAWN 6";
        debugText(debug, Vec2{10.0f, 20.0f}, 12.0f, text, white);

        const float width = debugTextWidth(12.0f, text.size());
        assert(width > 0.0f);
        assert(allInside(debug, 10.0f, 20.0f, 10.0f + width, 32.0f));
    }

    // The engine's culling readout sits in the top-left corner and changes with the numbers.
    void testRenderStatsReadout() {
        RenderStats stats;
        stats.drawn = 6;
        stats.culled = 1;
        stats.shadowDrawn = 12;
        stats.shadowCulled = 2;

        DebugDraw debug;
        drawRenderStats(stats, debug);
        assert(debug.getLines().empty());
        assert(!debug.getScreenLines().empty());
        assert(allInside(debug, 0.0f, 0.0f, 600.0f, 100.0f));

        RenderStats moreCulled = stats;
        moreCulled.culled = 7;
        DebugDraw other;
        drawRenderStats(moreCulled, other);
        assert(other.getScreenLines().size() != debug.getScreenLines().size());
    }
}

void testDebugText() {
    testScreenLinesAreSeparate();
    testEveryCharacterHasStrokes();
    testFallbacks();
    testTextStaysInItsBox();
    testRenderStatsReadout();
}