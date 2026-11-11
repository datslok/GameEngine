#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

/*
* The screen shown while the pipelines are built (render/vulkan/pipeline_compiler.h): "BUILDING SHADERS", how many of
* how many, and a bar. Drawn with nothing but cleared rectangles (vkCmdClearAttachments), since no pipeline is ready to
* draw it, so its text is a block font: each glyph 5 by 7 pixels, each pixel a square rectangle. What goes where is
* worked out here, without the graphics API.
*/

// A rectangle in pixels, measured from the window's top-left.
struct ScreenRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

inline constexpr int blockGlyphWidth = 5;
inline constexpr int blockGlyphHeight = 7;

/*
* The lit pixels of text in the block font, each pixel size pixels square, its top-left at (x, y), one font pixel
* between letters. Capitals, digits, '/', '.' and ':'; small letters are drawn as capitals, anything else is blank.
*/
std::vector<ScreenRect> blockText(std::string_view text, int x, int y, int pixel);

// How wide that text is, in screen pixels (no gap after the last letter).
int blockTextWidth(std::string_view text, int pixel);

// The title and count's pixels (the title's first), the bar's track and its filled part (from its left).
struct LoadingScreenLayout {
    std::vector<ScreenRect> text;
    ScreenRect barBack;
    ScreenRect barFill;
};

/*
* For a window this size with built of total pipelines done: the title over "built / total", centred across, a font
* pixel a 180th of the height (at least 2), and the bar below, half the width, filled by the share built (none when
* nothing was asked, at most full).
*/
LoadingScreenLayout layoutLoadingScreen(int width, int height, std::size_t built, std::size_t total);
