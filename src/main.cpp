#include <iostream>
#include "pixel.h"
#include "pixelbuffer.h"
#include <cassert>

int main() {
    PixelBuffer buffer{1920, 1080};
    buffer.print();
}