#include "assets/asset_manager.h"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
    template <typename Function>
    bool throws(Function function) {
        try {
            function();
        } catch (const std::exception&) {
            return true;
        }
        return false;
    }

    std::shared_ptr<const std::vector<std::uint8_t>> readFileBytes(const char* path) {
        std::ifstream file{path, std::ios::binary};
        assert(file);
        return std::make_shared<const std::vector<std::uint8_t>>(
            std::istreambuf_iterator<char>{file},
            std::istreambuf_iterator<char>{}
        );
    }
}

void testAssetManager() {
    // A default handle means "none" and is never valid.
    assert(!MeshHandle{}.isValid());
    assert(!TextureHandle{}.isValid());

    AssetManager assets;

    // Handles are dense indices, handed out in order.
    const MeshHandle cube = assets.addMesh(Mesh::cube());
    const MeshHandle plane = assets.addMesh(Mesh::plane(1.0f));
    assert(cube.index == 0);
    assert(plane.index == 1);
    assert(assets.getMeshCount() == 2);
    assert(assets.getMesh(cube).vertices.size() == Mesh::cube().vertices.size());

    // The same shared mesh always gives the same handle, so two ducks share one upload.
    const auto shared = std::make_shared<const Mesh>(Mesh::cube());
    const MeshHandle first = assets.addMesh(shared);
    assert(assets.addMesh(shared) == first);
    assert(assets.getMeshCount() == 3);

    // Files are loaded once: the same path returns the same handle without loading again.
    const MeshHandle pyramid = assets.loadMesh("assets/models/pyramid.obj");
    assert(assets.loadMesh("assets/models/pyramid.obj") == pyramid);
    assert(assets.getMeshCount() == 4);
    assert(!assets.getMesh(pyramid).triangles.empty());

    // Smoothing is opt-in and part of the cache key: the same file loaded smooth is a different mesh.
    const MeshHandle smoothPyramid = assets.loadMesh("assets/models/pyramid.obj", MeshLoadOptions{.smoothNormals = true});
    assert(smoothPyramid != pyramid);
    assert(assets.loadMesh("assets/models/pyramid.obj", MeshLoadOptions{.smoothNormals = true}) == smoothPyramid);
    assert(assets.getMeshCount() == 5);

    // The pyramid file has no normals: the plain load leaves them to the face, the smooth load fills them in.
    assert(!assets.getMesh(pyramid).triangles[0].normals[0].has_value());
    assert(assets.getMesh(smoothPyramid).triangles[0].normals[0].has_value());

    // A failed load is not remembered, so it fails again instead of returning a bad handle.
    assert(throws([&] { assets.loadMesh("assets/models/missing.obj"); }));
    assert(throws([&] { assets.loadMesh("assets/models/missing.obj"); }));
    assert(assets.getMeshCount() == 5);

    // Textures are cached by path and flip setting, because a flipped image has different pixels.
    const TextureHandle demo = assets.loadTexture("assets/textures/demo.png");
    assert(assets.loadTexture("assets/textures/demo.png") == demo);
    const TextureHandle unflipped = assets.loadTexture("assets/textures/demo.png", false);
    assert(!(unflipped == demo));
    assert(assets.getTextureCount() == 2);
    assert(assets.getTexture(demo).width > 0);

    // Embedded images are cached by their byte buffer.
    const auto bytes = readFileBytes("assets/textures/demo.png");
    const TextureHandle embedded = assets.loadTexture(bytes, false);
    assert(assets.loadTexture(bytes, false) == embedded);
    assert(assets.getTextureCount() == 3);
    assert(assets.getTexture(embedded).pixels == assets.getTexture(unflipped).pixels);

    // Images made in code must have four bytes per pixel.
    const TextureHandle white = assets.addTexture(ImageData{1, 1, {255, 255, 255, 255}});
    assert(white.index == 3);
    assert(throws([&] { assets.addTexture(ImageData{2, 2, {255, 255, 255, 255}}); }));

    // Handles this manager did not create are rejected.
    assert(throws([&] { assets.getMesh(MeshHandle{}); }));
    assert(throws([&] { assets.getMesh(MeshHandle{99}); }));
    assert(throws([&] { assets.getTexture(TextureHandle{}); }));
    assert(throws([&] { assets.addMesh(std::shared_ptr<const Mesh>{}); }));
}
