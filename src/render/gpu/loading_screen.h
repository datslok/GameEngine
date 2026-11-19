#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

/*
* The screen shown while the game builds its world and the pipelines are built (render/vulkan/pipeline_compiler.h): a
* title, how many of how many when it can count, and a bar. Drawn with nothing but cleared rectangles
* (vkCmdClearAttachments), since no pipeline is ready to draw it, so its text is a block font: each glyph 5 by 7 pixels,
* each pixel a square rectangle. What goes where is worked out here, without the graphics API.
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

// The screen's titles: while the game builds its world, while the meshes go up and the first frame is prepared, and
// while the pipelines are built.
inline constexpr std::string_view loadingWorldTitle = "LOADING THE WORLD";
inline constexpr std::string_view loadingSceneTitle = "PREPARING THE SCENE";
inline constexpr std::string_view loadingShadersTitle = "BUILDING SHADERS";

// Frames the sliding block takes from one end of the bar to the other: about a second and a half at the screen's 30 a second.
inline constexpr std::uint64_t loadingSlideFrames = 45;

/*
* What the screen shows: its title, and done of total (counted), or for work that cannot count (a game building its
* world) no count and a block sliding along the bar, placed by the screen's frame.
*/
struct LoadingProgress {
    std::string_view title = loadingShadersTitle;
    std::size_t done = 0;
    std::size_t total = 0;
    bool counted = true;
    std::uint64_t frame = 0;
};

/*
* For a window this size: the title over "done / total", centred across, a font pixel a 180th of the height (at least
* 2), and the bar below, half the width, filled by the share done (none when nothing was asked, at most full). Not
* counted: the count's line left empty (so the screen does not jump when the count begins) and the bar's fill a block a
* quarter of its width, going from its left end to its right in loadingSlideFrames frames and back.
*/
LoadingScreenLayout layoutLoadingScreen(int width, int height, const LoadingProgress& progress);
