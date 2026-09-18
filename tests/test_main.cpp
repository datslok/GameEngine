#include <iostream>

void testPixelBuffer();
void testRasterizer();
void testFilledTriangle();
void testVec2();

int main() {
    testPixelBuffer();
    testRasterizer();
    testFilledTriangle();
    testVec2();

    std::cout << "All tests passed!\n";
    return 0;
}