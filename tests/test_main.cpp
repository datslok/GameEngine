#include <iostream>

void testPixelBuffer();
void testRasterizer();
void testFilledTriangle();
void testVec2();
void testVec3();
void testVec4();
void testMat4();
void testClipper();
void testDepthBuffer();
void testTriangleDepth();
void testShading();
void testTransform();
void testScene();
void testObjLoader();
void testObjUvs();
void testGltfLoader();
void testGltfAttributes();
void testGltfTextures();
void testGlbTextures();
void testModelBounds();
void testSceneModel();

int main(){
    testPixelBuffer();
    testRasterizer();
    testFilledTriangle();
    testVec2();
    testVec3();
    testVec4();
    testMat4();
    testClipper();
    testDepthBuffer();
    testTriangleDepth();
    testShading();
    testTransform();
    testScene();
    testObjLoader();
    testObjUvs();
    testGltfLoader();
    testGltfAttributes();
    testGltfTextures();
    testGlbTextures();
    testModelBounds();
    testSceneModel();

    std::cout << "All tests passed!\n";
    return 0;
}