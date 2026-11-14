#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

void testPixelBuffer();
void testRasterizer();
void testFilledTriangle();
void testVec2();
void testVec3();
void testVec4();
void testMat4();
void testQuat();
void testClipper();
void testDepthBuffer();
void testTriangleDepth();
void testShading();
void testTransform();
void testObjLoader();
void testObjUvs();
void testGltfLoader();
void testGltfAttributes();
void testGltfTextures();
void testGlbTextures();
void testModelBounds();
void testRay();
void testMoveToController();
void testCharacter();
void testInput();
void testFixedTimestep();
void testSdlInput();
void testWorld();
void testModelRenderer();
void testEdgePan();
void testDemoGame();
void testCameraControllers();
void testAssetManager();
void testIndexedMesh();
void testLighting();
void testLightUniforms();
void testMaterialUniforms();
void testGpuScene();
void testDeferredChanges();
void testDeterminism();
void testEngineSettings();
void testBrdf();
void testParallax();
void testHeightMap();
void testTypicalGpu();
void testGroundLayers();
void testForest();
void testIslandHangings();
void testHangingMoss();
void testDriftingRocks();
void testForestTrees();
void testDepthPrepass();
void testTextureEncoding();
void testCoverageMips();
void testAlphaMask();
void testTransparency();
void testGltfMaterials();
void testGltfNormalMaps();
void testSmoothNormals();
void testNormalMatrix();
void testNormalMapping();
void testHalfFloat();
void testImageCompare();
void testGoldenViews();
void testSun();
void testCelestial();
void testStars();
void testEyeAdaptation();
void testGameRegistry();
void testCommandRegistry();
void testEngineCommands();
void testCommandSocket();
void testPlayerControls();
void testLightLab();
void testFlashlight();
void testRawImagePng();
void testTangents();
void testGpuDepthRange();
void testCameraRelative();
void testMipLevels();
void testSrgb();
void testFrameDescription();
void testShadowMap();
void testShadowAtlas();
void testSoftShadow();
void testCascades();
void testDepthRange();
void testDdgi();
void testDdgiSampling();
void testProbeRelocation();
void testProbeCascades();
void testAtmosphere();
void testRayTracing();
void testVideoMemory();
void testSkyObject();
void testLightPriority();
void testDebugDraw();
void testCulling();
void testDebugText();
void testDither();
void testSphereMesh();
void testLatheMesh();
void testFirefly();
void testSpinner();
void testClosestPoints();
void testDelaunay();
void testOverlap();
void testRaycast();
void testCollisionMesh();
void testAffineInverse();
void testCollider();
void testColliderRaycast();
void testShapeBounds();
void testUniformGrid();
void testCollisionScene();
void testTriangleContact();
void testStaticLevel();
void testCharacterController();
void testAngles();
void testFirstPersonBody();
void testSphereCast();
void testParticle();
void testGrenade();
void testProfiler();
void testJobSystem();
void testClothLab();
void testClothLabDress();
void testDeformingMesh();
void testDeformingSlots();
void testCloth();
void testClothSolver();
void testClothGrab();
void testClothBending();
void testClothCollision();
void testClothParallel();
void testClothYardstick();
void testClothSelfCollision();
void testClothHierarchy();
void testClothBlockDescent();
void testClothGpu();
void testClothTiming();
void testClothTethers();
void testClothSeams();
void testClothPattern();
void testSignedDistanceField();
void testClothFieldCollision();
void testGpuClothEngine();
void testGpuTiming();
void testToneMapping();
void testToneMapUniforms();
void testCameraFinish();
void testMotionBlur();
void testDepthOfField();
void testFrostedGlass();
void testImageLights();
void testBloom();
void testBlueNoise();
void testTaa();
void testScreenReflections();
void testReflectionProbe();
void testPlanarReflection();
void testMirroredLight();
void testAmbientOcclusion();
void testContactShadows();
void testVolumetricFog();
void testSkyVisibility();
void testPipelineCompiler();
void testLoadingScreen();
void testStaticGeometry();
void testUpscaling();
void testDisplayOutput();
void testSettingsFile();
void testQualityPreset();
void testOverlayComposite();
void testSwapchainChoice();
void testImageState();
void testUniformRing();
void testFloatTolerance();
void testLightFalloff();
void testClusters();
void testComputeDispatch();

/*
* The slow tests, run only with --slow (make test-slow): vertex block descent and its coarse levels, opt-in solvers
* parked until phase 10 needs them, whose tests settle sheets with every level on one thread (about 31 s in release, as
* much as all the routine tests together). Run them when the cloth solver changes.
*/
void runSlowTests() {
    testClothHierarchy();
    testClothBlockDescent();
}

/*
* Tests of work on hold (ROADMAP phases 7 and 16): the light probes and the cloth, left out of make test until that work
* resumes, for a quicker routine run. make test-parked runs them; they may break meanwhile, which is fine until then.
*/
void runParkedTests() {
    testDdgi();
    testDdgiSampling();
    testProbeRelocation();
    testProbeCascades();
    testClothLab();
    testClothLabDress();
    testCloth();
    testClothSolver();
    testClothGrab();
    testClothBending();
    testClothCollision();
    testClothParallel();
    testClothYardstick();
    testClothSelfCollision();
    testClothGpu();
    testClothTiming();
    testClothTethers();
    testClothSeams();
    testClothPattern();
    testSignedDistanceField();
    testClothFieldCollision();
    testGpuClothEngine();
}

namespace {
    // Each routine test's wall time, for --times: which tests make the run slow.
    std::vector<std::pair<double, const char*>> testTimes;

    void runTest(const char* name, void (*test)()) {
        const auto start = std::chrono::steady_clock::now();
        test();
        testTimes.emplace_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), name);
    }

    // The slowest tests first, with their share of the whole run.
    void printTestTimes(std::size_t shown) {
        std::sort(testTimes.begin(), testTimes.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        double total = 0.0;

        for (const auto& [seconds, name] : testTimes) {
            total += seconds;
        }

        std::cout << std::fixed << std::setprecision(2) << "Routine tests: " << total << " s in all; the slowest:\n";

        for (std::size_t i = 0; i < std::min(shown, testTimes.size()); ++i) {
            std::cout << std::setw(8) << testTimes[i].first << " s  " << std::setw(5) << std::setprecision(1)
                      << 100.0 * testTimes[i].first / total << "%  " << testTimes[i].second << std::setprecision(2) << "\n";
        }
    }
}

int main(int argumentCount, char** arguments) {
    if (argumentCount > 1 && std::string_view{arguments[1]} == "--slow") {
        runSlowTests();
        std::cout << "All slow tests passed!\n";
        return 0;
    }

    if (argumentCount > 1 && std::string_view{arguments[1]} == "--parked") {
        runParkedTests();
        std::cout << "All parked tests passed!\n";
        return 0;
    }

    runTest("testPixelBuffer", testPixelBuffer);
    runTest("testRasterizer", testRasterizer);
    runTest("testFilledTriangle", testFilledTriangle);
    runTest("testVec2", testVec2);
    runTest("testVec3", testVec3);
    runTest("testVec4", testVec4);
    runTest("testMat4", testMat4);
    runTest("testQuat", testQuat);
    runTest("testClipper", testClipper);
    runTest("testDepthBuffer", testDepthBuffer);
    runTest("testTriangleDepth", testTriangleDepth);
    runTest("testShading", testShading);
    runTest("testTransform", testTransform);
    runTest("testObjLoader", testObjLoader);
    runTest("testObjUvs", testObjUvs);
    runTest("testGltfLoader", testGltfLoader);
    runTest("testGltfAttributes", testGltfAttributes);
    runTest("testGltfTextures", testGltfTextures);
    runTest("testGlbTextures", testGlbTextures);
    runTest("testModelBounds", testModelBounds);
    runTest("testRay", testRay);
    runTest("testMoveToController", testMoveToController);
    runTest("testCharacter", testCharacter);
    runTest("testInput", testInput);
    runTest("testFixedTimestep", testFixedTimestep);
    runTest("testSdlInput", testSdlInput);
    runTest("testWorld", testWorld);
    runTest("testModelRenderer", testModelRenderer);
    runTest("testEdgePan", testEdgePan);
    runTest("testDemoGame", testDemoGame);
    runTest("testCameraControllers", testCameraControllers);
    runTest("testAssetManager", testAssetManager);
    runTest("testIndexedMesh", testIndexedMesh);
    runTest("testLighting", testLighting);
    runTest("testLightUniforms", testLightUniforms);
    runTest("testMaterialUniforms", testMaterialUniforms);
    runTest("testGpuScene", testGpuScene);
    runTest("testDeferredChanges", testDeferredChanges);
    runTest("testDeterminism", testDeterminism);
    runTest("testEngineSettings", testEngineSettings);
    runTest("testBrdf", testBrdf);
    runTest("testParallax", testParallax);
    runTest("testHeightMap", testHeightMap);
    runTest("testTypicalGpu", testTypicalGpu);
    runTest("testGroundLayers", testGroundLayers);
    runTest("testDepthPrepass", testDepthPrepass);
    runTest("testTextureEncoding", testTextureEncoding);
    runTest("testCoverageMips", testCoverageMips);
    runTest("testAlphaMask", testAlphaMask);
    runTest("testTransparency", testTransparency);
    runTest("testGltfMaterials", testGltfMaterials);
    runTest("testGltfNormalMaps", testGltfNormalMaps);
    runTest("testSmoothNormals", testSmoothNormals);
    runTest("testNormalMatrix", testNormalMatrix);
    runTest("testNormalMapping", testNormalMapping);
    runTest("testHalfFloat", testHalfFloat);
    runTest("testImageCompare", testImageCompare);
    runTest("testGoldenViews", testGoldenViews);
    runTest("testSun", testSun);
    runTest("testCelestial", testCelestial);
    runTest("testStars", testStars);
    runTest("testEyeAdaptation", testEyeAdaptation);
    runTest("testGameRegistry", testGameRegistry);
    runTest("testCommandRegistry", testCommandRegistry);
    runTest("testEngineCommands", testEngineCommands);
    runTest("testCommandSocket", testCommandSocket);
    runTest("testPlayerControls", testPlayerControls);
    runTest("testLightLab", testLightLab);
    runTest("testForest", testForest);
    runTest("testIslandHangings", testIslandHangings);
    runTest("testHangingMoss", testHangingMoss);
    runTest("testDriftingRocks", testDriftingRocks);
    runTest("testForestTrees", testForestTrees);
    runTest("testFlashlight", testFlashlight);
    runTest("testRawImagePng", testRawImagePng);
    runTest("testTangents", testTangents);
    runTest("testGpuDepthRange", testGpuDepthRange);
    runTest("testCameraRelative", testCameraRelative);
    runTest("testMipLevels", testMipLevels);
    runTest("testSrgb", testSrgb);
    runTest("testFrameDescription", testFrameDescription);
    runTest("testShadowMap", testShadowMap);
    runTest("testShadowAtlas", testShadowAtlas);
    runTest("testSoftShadow", testSoftShadow);
    runTest("testCascades", testCascades);
    runTest("testDepthRange", testDepthRange);
    runTest("testAtmosphere", testAtmosphere);
    runTest("testRayTracing", testRayTracing);
    runTest("testVideoMemory", testVideoMemory);
    runTest("testSkyObject", testSkyObject);
    runTest("testLightPriority", testLightPriority);
    runTest("testDebugDraw", testDebugDraw);
    runTest("testCulling", testCulling);
    runTest("testDebugText", testDebugText);
    runTest("testDither", testDither);
    runTest("testSphereMesh", testSphereMesh);
    runTest("testLatheMesh", testLatheMesh);
    runTest("testFirefly", testFirefly);
    runTest("testSpinner", testSpinner);
    runTest("testClosestPoints", testClosestPoints);
    runTest("testDelaunay", testDelaunay);
    runTest("testOverlap", testOverlap);
    runTest("testRaycast", testRaycast);
    runTest("testCollisionMesh", testCollisionMesh);
    runTest("testAffineInverse", testAffineInverse);
    runTest("testCollider", testCollider);
    runTest("testColliderRaycast", testColliderRaycast);
    runTest("testShapeBounds", testShapeBounds);
    runTest("testUniformGrid", testUniformGrid);
    runTest("testCollisionScene", testCollisionScene);
    runTest("testTriangleContact", testTriangleContact);
    runTest("testStaticLevel", testStaticLevel);
    runTest("testCharacterController", testCharacterController);
    runTest("testAngles", testAngles);
    runTest("testFirstPersonBody", testFirstPersonBody);
    runTest("testSphereCast", testSphereCast);
    runTest("testParticle", testParticle);
    runTest("testGrenade", testGrenade);
    runTest("testProfiler", testProfiler);
    runTest("testJobSystem", testJobSystem);
    runTest("testDeformingMesh", testDeformingMesh);
    runTest("testDeformingSlots", testDeformingSlots);
    runTest("testGpuTiming", testGpuTiming);
    runTest("testToneMapping", testToneMapping);
    runTest("testToneMapUniforms", testToneMapUniforms);
    runTest("testCameraFinish", testCameraFinish);
    runTest("testMotionBlur", testMotionBlur);
    runTest("testDepthOfField", testDepthOfField);
    runTest("testFrostedGlass", testFrostedGlass);
    runTest("testImageLights", testImageLights);
    runTest("testBloom", testBloom);
    runTest("testBlueNoise", testBlueNoise);
    runTest("testTaa", testTaa);
    runTest("testScreenReflections", testScreenReflections);
    runTest("testReflectionProbe", testReflectionProbe);
    runTest("testPlanarReflection", testPlanarReflection);
    runTest("testMirroredLight", testMirroredLight);
    runTest("testAmbientOcclusion", testAmbientOcclusion);
    runTest("testContactShadows", testContactShadows);
    runTest("testVolumetricFog", testVolumetricFog);
    runTest("testSkyVisibility", testSkyVisibility);
    runTest("testPipelineCompiler", testPipelineCompiler);
    runTest("testLoadingScreen", testLoadingScreen);
    runTest("testStaticGeometry", testStaticGeometry);
    runTest("testUpscaling", testUpscaling);
    runTest("testDisplayOutput", testDisplayOutput);
    runTest("testSettingsFile", testSettingsFile);
    runTest("testQualityPreset", testQualityPreset);
    runTest("testOverlayComposite", testOverlayComposite);
    runTest("testSwapchainChoice", testSwapchainChoice);
    runTest("testImageState", testImageState);
    runTest("testUniformRing", testUniformRing);
    runTest("testFloatTolerance", testFloatTolerance);
    runTest("testLightFalloff", testLightFalloff);
    runTest("testComputeDispatch", testComputeDispatch);
    runTest("testClusters", testClusters);

    if (argumentCount > 1 && std::string_view{arguments[1]} == "--times") {
        printTestTimes(30);
    }

    std::cout << "All tests passed!\n";
    return 0;
}
