#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

#include "test_progress.h"

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
void testCpuTiming();
void testBrdf();
void testParallax();
void testHeightMap();
void testTypicalGpu();
void testGroundLayers();
void testForest();
void testForestStructures();
void testFacetVariation();
void testWindowMask();
void testIslandHangings();
void testHangingMoss();
void testDriftingRocks();
void testForestTrees();
void testUnderstorey();
void testPalms();
void testValleyMaps();
void testPlanting();
void testLianas();
void testClimbers();
void testEpiphytes();
void testGrass();
void testFloorCover();
void testWind();
void testWindSway();
void testWindHanging();
void testSimplifyMesh();
void testLevelOfDetail();
void testLevelFade();
void testTestProgress();
void testCardThinning();
void testTerrain();
void testGroundFollow();
void testImpostors();
void testLeafClusters();
void testDepthPrepass();
void testCullRecords();
void testCameraCulling();
void testShadowCulling();
void testMeshPool();
void testTextureEncoding();
void testCoverageMips();
void testBlockCompression();
void testTextureCooking();
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
void testGoldenViewScenes();
void testSun();
void testCelestial();
void testStars();
void testBeacon();
void testBeaconSplat();
void testEyeAdaptation();
void testGameRegistry();
void testCommandRegistry();
void testDocs();
void printDocsReport();
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
void testStillDraws();
void testWorldCells();
void testStillInstances();
void testLastingBuffers();
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
void testBackgroundWork();
void testStartupClock();
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
* The slow tests, run only with --slow (make test-slow):
* - vertex block descent and its coarse levels, opt-in solvers parked until phase 10 needs them, whose tests settle
*   sheets with every level on one thread (about 31 s in release). Run them when the cloth solver changes.
* - the golden views' scenes, built and checked to hold still and repeat (about 70 s). The golden skill runs them before
*   a golden run.
*/
void runSlowTests() {
    testClothHierarchy();
    testClothBlockDescent();
    testGoldenViewScenes();
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

    struct RoutineTest {
        const char* name;
        void (*run)();
    };

    // Last run's times, beside tests.exe (program its path), so the progress bar knows how long each test takes.
    std::string timesPath(const char* program) {
        const std::string path{program};
        const std::size_t slash = path.find_last_of("/\\");
        return (slash == std::string::npos ? std::string{} : path.substr(0, slash + 1)) + "test_times.txt";
    }

    std::string readFile(const std::string& path) {
        std::ifstream file{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
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

// Every routine test, run in this order (make test).
const RoutineTest routineTests[] = {
    {"testPixelBuffer", testPixelBuffer},
    {"testRasterizer", testRasterizer},
    {"testFilledTriangle", testFilledTriangle},
    {"testVec2", testVec2},
    {"testVec3", testVec3},
    {"testVec4", testVec4},
    {"testMat4", testMat4},
    {"testQuat", testQuat},
    {"testClipper", testClipper},
    {"testDepthBuffer", testDepthBuffer},
    {"testTriangleDepth", testTriangleDepth},
    {"testShading", testShading},
    {"testTransform", testTransform},
    {"testObjLoader", testObjLoader},
    {"testObjUvs", testObjUvs},
    {"testGltfLoader", testGltfLoader},
    {"testGltfAttributes", testGltfAttributes},
    {"testGltfTextures", testGltfTextures},
    {"testGlbTextures", testGlbTextures},
    {"testModelBounds", testModelBounds},
    {"testRay", testRay},
    {"testMoveToController", testMoveToController},
    {"testCharacter", testCharacter},
    {"testInput", testInput},
    {"testFixedTimestep", testFixedTimestep},
    {"testSdlInput", testSdlInput},
    {"testWorld", testWorld},
    {"testModelRenderer", testModelRenderer},
    {"testEdgePan", testEdgePan},
    {"testDemoGame", testDemoGame},
    {"testCameraControllers", testCameraControllers},
    {"testAssetManager", testAssetManager},
    {"testIndexedMesh", testIndexedMesh},
    {"testLighting", testLighting},
    {"testLightUniforms", testLightUniforms},
    {"testMaterialUniforms", testMaterialUniforms},
    {"testGpuScene", testGpuScene},
    {"testDeferredChanges", testDeferredChanges},
    {"testDeterminism", testDeterminism},
    {"testEngineSettings", testEngineSettings},
    {"testCpuTiming", testCpuTiming},
    {"testBrdf", testBrdf},
    {"testParallax", testParallax},
    {"testHeightMap", testHeightMap},
    {"testTypicalGpu", testTypicalGpu},
    {"testGroundLayers", testGroundLayers},
    {"testDepthPrepass", testDepthPrepass},
    {"testCullRecords", testCullRecords},
    {"testCameraCulling", testCameraCulling},
    {"testShadowCulling", testShadowCulling},
    {"testMeshPool", testMeshPool},
    {"testTextureEncoding", testTextureEncoding},
    {"testCoverageMips", testCoverageMips},
    {"testBlockCompression", testBlockCompression},
    {"testTextureCooking", testTextureCooking},
    {"testAlphaMask", testAlphaMask},
    {"testTransparency", testTransparency},
    {"testGltfMaterials", testGltfMaterials},
    {"testGltfNormalMaps", testGltfNormalMaps},
    {"testSmoothNormals", testSmoothNormals},
    {"testNormalMatrix", testNormalMatrix},
    {"testNormalMapping", testNormalMapping},
    {"testHalfFloat", testHalfFloat},
    {"testImageCompare", testImageCompare},
    {"testGoldenViews", testGoldenViews},
    {"testSun", testSun},
    {"testCelestial", testCelestial},
    {"testStars", testStars},
    {"testBeacon", testBeacon},
    {"testBeaconSplat", testBeaconSplat},
    {"testEyeAdaptation", testEyeAdaptation},
    {"testGameRegistry", testGameRegistry},
    {"testCommandRegistry", testCommandRegistry},
    {"testEngineCommands", testEngineCommands},
    {"testCommandSocket", testCommandSocket},
    {"testPlayerControls", testPlayerControls},
    {"testLightLab", testLightLab},
    {"testForest", testForest},
    {"testForestStructures", testForestStructures},
    {"testFacetVariation", testFacetVariation},
    {"testWindowMask", testWindowMask},
    {"testIslandHangings", testIslandHangings},
    {"testHangingMoss", testHangingMoss},
    {"testDriftingRocks", testDriftingRocks},
    {"testForestTrees", testForestTrees},
    {"testUnderstorey", testUnderstorey},
    {"testPalms", testPalms},
    {"testValleyMaps", testValleyMaps},
    {"testPlanting", testPlanting},
    {"testLianas", testLianas},
    {"testClimbers", testClimbers},
    {"testEpiphytes", testEpiphytes},
    {"testGrass", testGrass},
    {"testFloorCover", testFloorCover},
    {"testWind", testWind},
    {"testWindSway", testWindSway},
    {"testWindHanging", testWindHanging},
    {"testSimplifyMesh", testSimplifyMesh},
    {"testLevelOfDetail", testLevelOfDetail},
    {"testLevelFade", testLevelFade},
    {"testCardThinning", testCardThinning},
    {"testTerrain", testTerrain},
    {"testGroundFollow", testGroundFollow},
    {"testImpostors", testImpostors},
    {"testLeafClusters", testLeafClusters},
    {"testFlashlight", testFlashlight},
    {"testRawImagePng", testRawImagePng},
    {"testTangents", testTangents},
    {"testGpuDepthRange", testGpuDepthRange},
    {"testCameraRelative", testCameraRelative},
    {"testMipLevels", testMipLevels},
    {"testSrgb", testSrgb},
    {"testFrameDescription", testFrameDescription},
    {"testStillDraws", testStillDraws},
    {"testWorldCells", testWorldCells},
    {"testStillInstances", testStillInstances},
    {"testLastingBuffers", testLastingBuffers},
    {"testShadowMap", testShadowMap},
    {"testShadowAtlas", testShadowAtlas},
    {"testSoftShadow", testSoftShadow},
    {"testCascades", testCascades},
    {"testDepthRange", testDepthRange},
    {"testAtmosphere", testAtmosphere},
    {"testRayTracing", testRayTracing},
    {"testVideoMemory", testVideoMemory},
    {"testSkyObject", testSkyObject},
    {"testLightPriority", testLightPriority},
    {"testDebugDraw", testDebugDraw},
    {"testCulling", testCulling},
    {"testDebugText", testDebugText},
    {"testDither", testDither},
    {"testSphereMesh", testSphereMesh},
    {"testLatheMesh", testLatheMesh},
    {"testFirefly", testFirefly},
    {"testSpinner", testSpinner},
    {"testClosestPoints", testClosestPoints},
    {"testDelaunay", testDelaunay},
    {"testOverlap", testOverlap},
    {"testRaycast", testRaycast},
    {"testCollisionMesh", testCollisionMesh},
    {"testAffineInverse", testAffineInverse},
    {"testCollider", testCollider},
    {"testColliderRaycast", testColliderRaycast},
    {"testShapeBounds", testShapeBounds},
    {"testUniformGrid", testUniformGrid},
    {"testCollisionScene", testCollisionScene},
    {"testTriangleContact", testTriangleContact},
    {"testStaticLevel", testStaticLevel},
    {"testCharacterController", testCharacterController},
    {"testAngles", testAngles},
    {"testFirstPersonBody", testFirstPersonBody},
    {"testSphereCast", testSphereCast},
    {"testParticle", testParticle},
    {"testGrenade", testGrenade},
    {"testProfiler", testProfiler},
    {"testJobSystem", testJobSystem},
    {"testDeformingMesh", testDeformingMesh},
    {"testDeformingSlots", testDeformingSlots},
    {"testGpuTiming", testGpuTiming},
    {"testToneMapping", testToneMapping},
    {"testToneMapUniforms", testToneMapUniforms},
    {"testCameraFinish", testCameraFinish},
    {"testMotionBlur", testMotionBlur},
    {"testDepthOfField", testDepthOfField},
    {"testFrostedGlass", testFrostedGlass},
    {"testImageLights", testImageLights},
    {"testBloom", testBloom},
    {"testBlueNoise", testBlueNoise},
    {"testTaa", testTaa},
    {"testScreenReflections", testScreenReflections},
    {"testReflectionProbe", testReflectionProbe},
    {"testPlanarReflection", testPlanarReflection},
    {"testMirroredLight", testMirroredLight},
    {"testAmbientOcclusion", testAmbientOcclusion},
    {"testContactShadows", testContactShadows},
    {"testVolumetricFog", testVolumetricFog},
    {"testSkyVisibility", testSkyVisibility},
    {"testPipelineCompiler", testPipelineCompiler},
    {"testBackgroundWork", testBackgroundWork},
    {"testStartupClock", testStartupClock},
    {"testLoadingScreen", testLoadingScreen},
    {"testStaticGeometry", testStaticGeometry},
    {"testUpscaling", testUpscaling},
    {"testDisplayOutput", testDisplayOutput},
    {"testSettingsFile", testSettingsFile},
    {"testQualityPreset", testQualityPreset},
    {"testOverlayComposite", testOverlayComposite},
    {"testSwapchainChoice", testSwapchainChoice},
    {"testImageState", testImageState},
    {"testUniformRing", testUniformRing},
    {"testFloatTolerance", testFloatTolerance},
    {"testLightFalloff", testLightFalloff},
    {"testComputeDispatch", testComputeDispatch},
    {"testClusters", testClusters},
    {"testTestProgress", testTestProgress},
    {"testDocs", testDocs},
};

namespace {
    // Every routine test in order, with the progress bar (test_progress.h), its time kept for --times and for the next run's bar.
    void runRoutineTests(const char* program) {
        std::vector<std::string> names;

        for (const RoutineTest& test : routineTests) {
            names.emplace_back(test.name);
        }

        const std::string path = timesPath(program);
        const std::vector<double> expected = expectedTestSeconds(names, parseTestTimes(readFile(path)));
        ProgressState state;
        state.total = names.size();

        for (const double seconds : expected) {
            state.expectedTotal += seconds;
        }

        ConsoleProgress progress;
        const auto runStart = std::chrono::steady_clock::now();

        for (std::size_t index = 0; index < names.size(); ++index) {
            state.current = names[index];
            state.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart).count();
            progress.show(state);

            const auto start = std::chrono::steady_clock::now();
            routineTests[index].run();
            testTimes.emplace_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), routineTests[index].name);
            ++state.done;
            state.expectedDone += expected[index];
        }

        state.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart).count();
        progress.finish(state);
        std::ofstream{path, std::ios::binary} << formatTestTimes(testTimes);
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

    if (argumentCount > 1 && std::string_view{arguments[1]} == "--docs-report") {
        printDocsReport();
        return 0;
    }

    runRoutineTests(arguments[0]);

    if (argumentCount > 1 && std::string_view{arguments[1]} == "--times") {
        printTestTimes(30);
    }

    std::cout << "All tests passed!\n";
    return 0;
}
