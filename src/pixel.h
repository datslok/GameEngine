#include <cstdint>
#pragma once

/*
* Represents a pixel using 8-bit RGB colour channels.
* Each channel stores a value from 0 to 255.
*/
struct Pixel{
    uint8_t r;
    uint8_t g;
    uint8_t b;
};