#include "scene/debug_text.h"

#include <cctype>
#include <initializer_list>
#include <vector>

namespace {
    struct GridPoint {
        int x;
        int y; // down from the top
    };

    // A glyph is a few polylines on a grid 4 wide and 6 tall; consecutive points of a stroke are joined.
    using Stroke = std::vector<GridPoint>;
    using Glyph = std::vector<Stroke>;

    constexpr float gridHeight = 6.0f;
    constexpr float advanceInGrid = 6.0f; // 4 for the glyph, 2 of space

    const Glyph& glyphFor(char character) {
        static const Glyph question = {{{0, 1}, {0, 0}, {4, 0}, {4, 3}, {2, 3}, {2, 4}}, {{2, 5}, {2, 6}}};
        static const Glyph space = {};
        static const Glyph letters[26] = {
            {{{0, 6}, {0, 2}, {2, 0}, {4, 2}, {4, 6}}, {{0, 3}, {4, 3}}},                               // A
            {{{0, 0}, {0, 6}, {3, 6}, {4, 5}, {4, 4}, {3, 3}, {0, 3}}, {{0, 0}, {3, 0}, {4, 1}, {4, 2}, {3, 3}}}, // B
            {{{4, 0}, {0, 0}, {0, 6}, {4, 6}}},                                                          // C
            {{{0, 0}, {0, 6}, {2, 6}, {4, 4}, {4, 2}, {2, 0}, {0, 0}}},                                  // D
            {{{4, 0}, {0, 0}, {0, 6}, {4, 6}}, {{0, 3}, {3, 3}}},                                        // E
            {{{4, 0}, {0, 0}, {0, 6}}, {{0, 3}, {3, 3}}},                                                // F
            {{{4, 0}, {0, 0}, {0, 6}, {4, 6}, {4, 3}, {2, 3}}},                                          // G
            {{{0, 0}, {0, 6}}, {{4, 0}, {4, 6}}, {{0, 3}, {4, 3}}},                                      // H
            {{{1, 0}, {3, 0}}, {{2, 0}, {2, 6}}, {{1, 6}, {3, 6}}},                                      // I
            {{{4, 0}, {4, 6}, {0, 6}, {0, 4}}},                                                          // J
            {{{0, 0}, {0, 6}}, {{4, 0}, {0, 3}, {4, 6}}},                                                // K
            {{{0, 0}, {0, 6}, {4, 6}}},                                                                  // L
            {{{0, 6}, {0, 0}, {2, 3}, {4, 0}, {4, 6}}},                                                  // M
            {{{0, 6}, {0, 0}, {4, 6}, {4, 0}}},                                                          // N
            {{{0, 0}, {4, 0}, {4, 6}, {0, 6}, {0, 0}}},                                                  // O
            {{{0, 6}, {0, 0}, {4, 0}, {4, 3}, {0, 3}}},                                                  // P
            {{{0, 0}, {4, 0}, {4, 6}, {0, 6}, {0, 0}}, {{2, 4}, {4, 6}}},                                // Q
            {{{0, 6}, {0, 0}, {4, 0}, {4, 3}, {0, 3}}, {{1, 3}, {4, 6}}},                                // R
            {{{4, 0}, {0, 0}, {0, 3}, {4, 3}, {4, 6}, {0, 6}}},                                          // S
            {{{0, 0}, {4, 0}}, {{2, 0}, {2, 6}}},                                                        // T
            {{{0, 0}, {0, 6}, {4, 6}, {4, 0}}},                                                          // U
            {{{0, 0}, {2, 6}, {4, 0}}},                                                                  // V
            {{{0, 0}, {1, 6}, {2, 3}, {3, 6}, {4, 0}}},                                                  // W
            {{{0, 0}, {4, 6}}, {{4, 0}, {0, 6}}},                                                        // X
            {{{0, 0}, {2, 3}, {4, 0}}, {{2, 3}, {2, 6}}},                                                // Y
            {{{0, 0}, {4, 0}, {0, 6}, {4, 6}}}                                                           // Z
        };
        static const Glyph digits[10] = {
            {{{0, 0}, {4, 0}, {4, 6}, {0, 6}, {0, 0}}, {{0, 6}, {4, 0}}},                                // 0 (slashed, so it is not O)
            {{{1, 1}, {2, 0}, {2, 6}}, {{1, 6}, {3, 6}}},                                                // 1
            {{{0, 0}, {4, 0}, {4, 3}, {0, 3}, {0, 6}, {4, 6}}},                                          // 2
            {{{0, 0}, {4, 0}, {4, 6}, {0, 6}}, {{1, 3}, {4, 3}}},                                        // 3
            {{{0, 0}, {0, 3}, {4, 3}}, {{4, 0}, {4, 6}}},                                                // 4
            {{{4, 0}, {0, 0}, {0, 3}, {4, 3}, {4, 6}, {0, 6}}},                                          // 5
            {{{4, 0}, {0, 0}, {0, 6}, {4, 6}, {4, 3}, {0, 3}}},                                          // 6
            {{{0, 0}, {4, 0}, {1, 6}}},                                                                  // 7
            {{{0, 0}, {4, 0}, {4, 6}, {0, 6}, {0, 0}}, {{0, 3}, {4, 3}}},                                // 8
            {{{4, 3}, {0, 3}, {0, 0}, {4, 0}, {4, 6}, {0, 6}}}                                           // 9
        };
        static const Glyph colon = {{{2, 1}, {2, 2}}, {{2, 4}, {2, 5}}};
        static const Glyph dash = {{{1, 3}, {3, 3}}};
        static const Glyph slash = {{{0, 6}, {4, 0}}};
        static const Glyph dot = {{{2, 5}, {2, 6}}};

        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));

        if (upper >= 'A' && upper <= 'Z') {
            return letters[upper - 'A'];
        }

        if (upper >= '0' && upper <= '9') {
            return digits[upper - '0'];
        }

        switch (upper) {
        case ' ': return space;
        case ':': return colon;
        case '-': return dash;
        case '/': return slash;
        case '.': return dot;
        default:  return question;
        }
    }
}

void debugText(DebugDraw& debug, const Vec2& topLeft, float height, std::string_view text, const Vec3& colour) {
    const float scale = height / gridHeight;

    for (std::size_t index = 0; index < text.size(); ++index) {
        const float left = topLeft.x + static_cast<float>(index) * advanceInGrid * scale;

        for (const Stroke& stroke : glyphFor(text[index])) {
            for (std::size_t point = 1; point < stroke.size(); ++point) {
                const GridPoint& a = stroke[point - 1];
                const GridPoint& b = stroke[point];
                debug.screenLine(
                    Vec2{left + static_cast<float>(a.x) * scale, topLeft.y + static_cast<float>(a.y) * scale},
                    Vec2{left + static_cast<float>(b.x) * scale, topLeft.y + static_cast<float>(b.y) * scale},
                    colour
                );
            }
        }
    }
}

// The last character has no trailing gap.
float debugTextWidth(float height, std::size_t characters) {
    if (characters == 0) {
        return 0.0f;
    }

    const float scale = height / gridHeight;
    return (static_cast<float>(characters - 1) * advanceInGrid + 4.0f) * scale;
}