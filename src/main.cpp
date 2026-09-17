#include <iostream>
#include "pixel.h"

int main() {
    Pixel pixel{255, 0, 0};
    std::cout << "R: " << static_cast<int>(pixel.r)
          << ", G: " << static_cast<int>(pixel.g)
          << ", B: " << static_cast<int>(pixel.b)
          << "\n";
    return 0; 
}