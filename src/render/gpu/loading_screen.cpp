#include "render/gpu/loading_screen.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace {
    // One glyph: its seven rows of five, 'X' lit.
    using Glyph = std::array<const char*, blockGlyphHeight>;

    struct GlyphEntry {
        char character;
        Glyph rows;
    };

    // A classic 5 x 7 face, capitals, digits and a little punctuation.
    constexpr std::array<GlyphEntry, 39> glyphs{{
        {'A', {".XXX.", "X...X", "X...X", "XXXXX", "X...X", "X...X", "X...X"}},
        {'B', {"XXXX.", "X...X", "X...X", "XXXX.", "X...X", "X...X", "XXXX."}},
        {'C', {".XXX.", "X...X", "X....", "X....", "X....", "X...X", ".XXX."}},
        {'D', {"XXXX.", "X...X", "X...X", "X...X", "X...X", "X...X", "XXXX."}},
        {'E', {"XXXXX", "X....", "X....", "XXXX.", "X....", "X....", "XXXXX"}},
        {'F', {"XXXXX", "X....", "X....", "XXXX.", "X....", "X....", "X...."}},
        {'G', {".XXX.", "X...X", "X....", "X.XXX", "X...X", "X...X", ".XXXX"}},
        {'H', {"X...X", "X...X", "X...X", "XXXXX", "X...X", "X...X", "X...X"}},
        {'I', {".XXX.", "..X..", "..X..", "..X..", "..X..", "..X..", ".XXX."}},
        {'J', {"..XXX", "...X.", "...X.", "...X.", "...X.", "X..X.", ".XX.."}},
        {'K', {"X...X", "X..X.", "X.X..", "XX...", "X.X..", "X..X.", "X...X"}},
        {'L', {"X....", "X....", "X....", "X....", "X....", "X....", "XXXXX"}},
        {'M', {"X...X", "XX.XX", "X.X.X", "X.X.X", "X...X", "X...X", "X...X"}},
        {'N', {"X...X", "X...X", "XX..X", "X.X.X", "X..XX", "X...X", "X...X"}},
        {'O', {".XXX.", "X...X", "X...X", "X...X", "X...X", "X...X", ".XXX."}},
        {'P', {"XXXX.", "X...X", "X...X", "XXXX.", "X....", "X....", "X...."}},
        {'Q', {".XXX.", "X...X", "X...X", "X...X", "X.X.X", "X..X.", ".XX.X"}},
        {'R', {"XXXX.", "X...X", "X...X", "XXXX.", "X.X..", "X..X.", "X...X"}},
        {'S', {".XXXX", "X....", "X....", ".XXX.", "....X", "....X", "XXXX."}},
        {'T', {"XXXXX", "..X..", "..X..", "..X..", "..X..", "..X..", "..X.."}},
        {'U', {"X...X", "X...X", "X...X", "X...X", "X...X", "X...X", ".XXX."}},
        {'V', {"X...X", "X...X", "X...X", "X...X", "X...X", ".X.X.", "..X.."}},
        {'W', {"X...X", "X...X", "X...X", "X.X.X", "X.X.X", "X.X.X", ".X.X."}},
        {'X', {"X...X", "X...X", ".X.X.", "..X..", ".X.X.", "X...X", "X...X"}},
        {'Y', {"X...X", "X...X", ".X.X.", "..X..", "..X..", "..X..", "..X.."}},
        {'Z', {"XXXXX", "....X", "...X.", "..X..", ".X...", "X....", "XXXXX"}},
        {'0', {".XXX.", "X...X", "X..XX", "X.X.X", "XX..X", "X...X", ".XXX."}},
        {'1', {"..X..", ".XX..", "..X..", "..X..", "..X..", "..X..", ".XXX."}},
        {'2', {".XXX.", "X...X", "....X", "...X.", "..X..", ".X...", "XXXXX"}},
        {'3', {"XXXXX", "...X.", "..X..", "...X.", "....X", "X...X", ".XXX."}},
        {'4', {"...X.", "..XX.", ".X.X.", "X..X.", "XXXXX", "...X.", "...X."}},
        {'5', {"XXXXX", "X....", "XXXX.", "....X", "....X", "X...X", ".XXX."}},
        {'6', {"..XX.", ".X...", "X....", "XXXX.", "X...X", "X...X", ".XXX."}},
        {'7', {"XXXXX", "....X", "...X.", "..X..", ".X...", ".X...", ".X..."}},
        {'8', {".XXX.", "X...X", "X...X", ".XXX.", "X...X", "X...X", ".XXX."}},
        {'9', {".XXX.", "X...X", "X...X", ".XXXX", "....X", "...X.", ".XX.."}},
        {'/', {"....X", "....X", "...X.", "..X..", ".X...", "X....", "X...."}},
        {'.', {".....", ".....", ".....", ".....", ".....", ".XX..", ".XX.."}},
        {':', {".....", ".XX..", ".XX..", ".....", ".XX..", ".XX..", "....."}},
    }};

    const Glyph* glyphOf(char character) {
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));

        for (const GlyphEntry& entry : glyphs) {
            if (entry.character == upper) {
                return &entry.rows;
            }
        }

        return nullptr;
    }

    // The screen's title, the font pixel as a share of its height (and the least), the gaps and the bar, in font pixels.
    constexpr std::string_view loadingTitle = "BUILDING SHADERS";
    constexpr int heightPerPixel = 180;
    constexpr int smallestPixel = 2;
    constexpr int lineGap = 4;
    constexpr int barGap = 8;
    constexpr int barThickness = 3;
}

std::vector<ScreenRect> blockText(std::string_view text, int x, int y, int pixel) {
    std::vector<ScreenRect> lit;
    const int advance = (blockGlyphWidth + 1) * pixel;

    for (std::size_t index = 0; index < text.size(); ++index) {
        const Glyph* glyph = glyphOf(text[index]);

        if (glyph == nullptr) {
            continue;
        }

        const int left = x + static_cast<int>(index) * advance;

        for (int row = 0; row < blockGlyphHeight; ++row) {
            for (int column = 0; column < blockGlyphWidth; ++column) {
                if ((*glyph)[static_cast<std::size_t>(row)][column] == 'X') {
                    lit.push_back(ScreenRect{left + column * pixel, y + row * pixel, pixel, pixel});
                }
            }
        }
    }

    return lit;
}

int blockTextWidth(std::string_view text, int pixel) {
    if (text.empty()) {
        return 0;
    }

    const int letters = static_cast<int>(text.size());
    return (letters * blockGlyphWidth + (letters - 1)) * pixel;
}

// The title, the count and the bar stacked and centred as one block.
LoadingScreenLayout layoutLoadingScreen(int width, int height, std::size_t built, std::size_t total) {
    const int pixel = std::max(smallestPixel, height / heightPerPixel);
    const int lineHeight = blockGlyphHeight * pixel;
    const int blockHeight = 2 * lineHeight + (lineGap + barGap + barThickness) * pixel;
    const int top = std::max(0, (height - blockHeight) / 2);
    const std::string count = std::to_string(built) + " / " + std::to_string(total);

    LoadingScreenLayout screen;
    screen.text = blockText(loadingTitle, (width - blockTextWidth(loadingTitle, pixel)) / 2, top, pixel);
    const int countTop = top + lineHeight + lineGap * pixel;
    const std::vector<ScreenRect> counted = blockText(count, (width - blockTextWidth(count, pixel)) / 2, countTop, pixel);
    screen.text.insert(screen.text.end(), counted.begin(), counted.end());

    const int barWidth = width / 2;
    screen.barBack = ScreenRect{(width - barWidth) / 2, countTop + lineHeight + barGap * pixel, barWidth, barThickness * pixel};
    screen.barFill = screen.barBack;
    screen.barFill.width = total == 0 ? 0 : static_cast<int>(static_cast<long long>(barWidth) * static_cast<long long>(std::min(built, total)) / static_cast<long long>(total));
    return screen;
}
