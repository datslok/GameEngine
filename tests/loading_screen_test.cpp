#include "render/gpu/loading_screen.h"

#include <algorithm>
#include <cassert>
#include <string>
#include <vector>

namespace {
    bool inside(const ScreenRect& rect, int x, int y, int width, int height) {
        return rect.x >= x && rect.y >= y && rect.x + rect.width <= x + width && rect.y + rect.height <= y + height;
    }

    /*
    * The block font: each glyph 5 by 7 pixels with one pixel between letters, each pixel a square of the given size.
    * An L is its left column and its bottom row (11 pixels); a space and characters it does not know are blank; small
    * letters are drawn as capitals.
    */
    void testBlockText() {
        const std::vector<ScreenRect> l = blockText("L", 10, 20, 3);
        assert(l.size() == 11);
        for (const ScreenRect& pixel : l) {
            assert(pixel.width == 3 && pixel.height == 3);
            assert(inside(pixel, 10, 20, 15, 21));
        }

        assert(blockText(" ", 0, 0, 2).empty() && blockText("~", 0, 0, 2).empty());
        assert(blockText("l", 0, 0, 2).size() == blockText("L", 0, 0, 2).size());
        assert(blockTextWidth("AB", 2) == (5 + 1 + 5) * 2);
        assert(blockTextWidth("", 2) == 0);

        // Every character the loading screen writes has a glyph.
        for (const char character : std::string{"BUILDING SHADERS 0123456789/"}) {
            if (character != ' ') {
                assert(!blockText(std::string(1, character), 0, 0, 1).empty());
            }
        }

        // The second letter starts six pixels after the first.
        const std::vector<ScreenRect> two = blockText("LL", 0, 0, 1);
        assert(two.size() == 22);
        assert(std::any_of(two.begin(), two.end(), [](const ScreenRect& pixel) { return pixel.x == 6; }));
        assert(std::none_of(two.begin(), two.end(), [](const ScreenRect& pixel) { return pixel.x == 5; }));
    }

    /*
    * The screen: "BUILDING SHADERS" over "built / total", both centred across, then a bar whose filled part is the share
    * built; everything on screen, the pixels sized to the window's height.
    */
    void testLayout() {
        const LoadingScreenLayout screen = layoutLoadingScreen(1920, 1080, 37, 212);
        const int pixel = std::max(2, 1080 / 180);
        const std::size_t titlePixels = blockText("BUILDING SHADERS", 0, 0, pixel).size();
        const std::size_t countPixels = blockText("37 / 212", 0, 0, pixel).size();
        assert(screen.text.size() == titlePixels + countPixels);

        for (const ScreenRect& rect : screen.text) {
            assert(inside(rect, 0, 0, 1920, 1080));
        }

        int left = 1920;
        int right = 0;
        for (std::size_t index = 0; index < titlePixels; ++index) {
            left = std::min(left, screen.text[index].x);
            right = std::max(right, screen.text[index].x + screen.text[index].width);
        }
        assert(std::abs((left + right) / 2 - 960) <= pixel * 3);

        assert(inside(screen.barBack, 0, 0, 1920, 1080));
        assert(screen.barFill.x == screen.barBack.x && screen.barFill.y == screen.barBack.y && screen.barFill.height == screen.barBack.height);
        assert(screen.barFill.width == screen.barBack.width * 37 / 212);
        assert(screen.barBack.y > screen.text.back().y);

        // Nothing yet, all done, more done than asked (kept to the bar), and nothing asked.
        assert(layoutLoadingScreen(1920, 1080, 0, 212).barFill.width == 0);
        const LoadingScreenLayout full = layoutLoadingScreen(1920, 1080, 212, 212);
        assert(full.barFill.width == full.barBack.width);
        assert(layoutLoadingScreen(1920, 1080, 300, 212).barFill.width == full.barBack.width);
        assert(layoutLoadingScreen(1920, 1080, 0, 0).barFill.width == 0);

        // A small window still fits.
        const LoadingScreenLayout small = layoutLoadingScreen(320, 200, 5, 9);
        for (const ScreenRect& rect : small.text) {
            assert(inside(rect, 0, 0, 320, 200));
        }
        assert(inside(small.barBack, 0, 0, 320, 200));
    }
}

void testLoadingScreen() {
    testBlockText();
    testLayout();
}
