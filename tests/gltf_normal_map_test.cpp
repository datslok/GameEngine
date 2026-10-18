#include "assets/gltf_loader.h"

#include <cassert>
#include <cmath>
#include <cstddef>

namespace {
    // glTF counts v down from the image's top row, which decides which way tangents' bitangents point.
    void testGltfMeshesCountUvsFromTheTop() {
        const Model model = loadGltf("assets/models/NormalTangentTest.glb");

        for (const ModelPart& part : model.parts) {
            assert(part.mesh->uvOrigin == UvOrigin::TopLeft);
        }
    }

    /*
    * The mirror test model supplies a tangent per vertex: a unit direction across the normal, and a sign of exactly +1
    * or -1 (glTF requires it), both of which occur since half the model's pictures are mirrored.
    */
    void testSuppliedTangentsAreRead() {
        const Model model = loadGltf("assets/models/NormalTangentMirrorTest.glb");
        bool positive = false;
        bool negative = false;

        for (const ModelPart& part : model.parts) {
            for (const Triangle& triangle : part.mesh->triangles) {
                for (std::size_t corner = 0; corner < 3; ++corner) {
                    assert(triangle.tangents[corner].has_value());
                    const Vec4 tangent = *triangle.tangents[corner];
                    const Vec3 direction{tangent.x, tangent.y, tangent.z};

                    assert(std::abs(direction.length() - 1.0f) < 0.001f);
                    assert(std::abs(direction.dot(*triangle.normals[corner])) < 0.01f);
                    assert(tangent.w == 1.0f || tangent.w == -1.0f);
                    positive = positive || tangent.w > 0.0f;
                    negative = negative || tangent.w < 0.0f;
                }
            }
        }

        assert(positive && negative);
    }

    // Without a TANGENT attribute the corners have none, and the renderer generates them.
    void testMissingTangentsStayEmpty() {
        const Model model = loadGltf("assets/models/NormalTangentTest.glb");

        for (const ModelPart& part : model.parts) {
            for (const Triangle& triangle : part.mesh->triangles) {
                for (const auto& tangent : triangle.tangents) {
                    assert(!tangent.has_value());
                }
            }
        }
    }

    // The comparison model: the same shape twice, only the second with a normal map (an image packed in the .glb).
    void testNormalTextureIsRead() {
        const Model model = loadGltf("assets/models/CompareNormal.glb");
        assert(model.parts.size() == 2);

        std::size_t withNormalMap = 0;

        for (const ModelPart& part : model.parts) {
            if (part.material.normalTexture.isSet()) {
                ++withNormalMap;
                assert(part.material.normalTexture.embeddedImage != nullptr);
                assert(part.material.normalScale == 1.0f);
                assert(part.material.hasTexture());
            }
        }

        assert(withNormalMap == 1);
    }
}

void testGltfNormalMaps() {
    testGltfMeshesCountUvsFromTheTop();
    testSuppliedTangentsAreRead();
    testMissingTangentsStayEmpty();
    testNormalTextureIsRead();
}
