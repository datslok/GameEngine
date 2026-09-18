#include <iostream>

void testPixelBuffer();
void testRasterizer();
void testFilledTriangle();
void testVec2();
void testVec3();
void testVec4();

int main() {
    testPixelBuffer();
    testRasterizer();
    testFilledTriangle();
    testVec2();
    testVec3();
    testVec4();

    std::cout << "All tests passed!\n";
    return 0;
}