#include <iostream>

void testPixelBuffer();
void testRasterizer();
void testFilledTriangle();

int main() {
    testPixelBuffer();
    testRasterizer();
    testFilledTriangle();

    std::cout << "All tests passed!\n";
    return 0;
}