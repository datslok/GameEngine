#include <iostream>

void testPixelBuffer();
void testRasterizer();
void testFilledTriangle();
void testVec2();
void testVec3();

int main() {
    testPixelBuffer();
    testRasterizer();
    testFilledTriangle();
    testVec2();
    testVec3();

    std::cout << "All tests passed!\n";
    return 0;
}