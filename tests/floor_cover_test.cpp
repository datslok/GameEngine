#include "games/forest/floor_cover.h"
#include "games/forest/forest_game.h"
#include "games/forest/forest_terrain.h"
#include "scene/model_renderer.h"

#include "forest_helpers.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
    bool inBand(float u, int band) {
        const float from = static_cast<float>(band) / static_cast<float>(floorCoverBands);
        return u >= from - 1e-5f && u <= from + 1.0f / static_cast<float>(floorCoverBands) + 1e-5f;
    }

    // A fallen leaf lies on the ground, its edges curled up a little, facing up, within its length of its middle, its u
    // within its tone's band.
    void testAFallenLeaf() {
        const FallenLeaf leaf{.centre = Vec3{1.0f, 0.0f, 2.0f}, .length = 0.15f, .turnRadians = 0.7f, .curl = 0.01f, .band = 1};
        IndexedMesh mesh;
        appendFallenLeaf(mesh, leaf);
        assert(mesh.vertices.size() == 7 && mesh.indices.size() == 6 * 3);
        assert(trianglesFaceTheirNormals(mesh));

        for (const MeshVertex& vertex : mesh.vertices) {
            assert(vertex.position.y >= 0.0f && vertex.position.y <= leaf.curl + 0.01f);
            assert(vertex.normal.y > 0.9f && inBand(vertex.uv.x, leaf.band));
            assert((vertex.position - leaf.centre).length() <= leaf.length);
        }
    }

    // A seedling: a stem from just under the ground to its height, its leaves near its top, all in the green band.
    void testASeedling() {
        const Seedling seedling{.root = Vec3{0.0f, 0.0f, 0.0f}, .height = 0.3f, .leaves = 4, .turnRadians = 0.2f, .seed = 3};
        IndexedMesh mesh;
        appendSeedling(mesh, seedling);
        float highest = -1.0f;
        float lowest = 1.0f;

        for (const MeshVertex& vertex : mesh.vertices) {
            highest = std::max(highest, vertex.position.y);
            lowest = std::min(lowest, vertex.position.y);
            assert(inBand(vertex.uv.x, floorGreenBand));
        }

        assert(lowest < 0.0f && highest > 0.27f && highest < 0.45f);
    }

    // A small fern: a rosette of feather-shaped fronds rising from its root, in the green band.
    void testAFern() {
        const FloorFern fern{.root = Vec3{0.0f, 0.0f, 0.0f}, .length = 0.4f, .fronds = 5, .seed = 9};
        IndexedMesh mesh;
        appendFloorFern(mesh, fern);
        assert(!mesh.vertices.empty());
        float highest = 0.0f;

        for (const MeshVertex& vertex : mesh.vertices) {
            highest = std::max(highest, vertex.position.y);
            assert(inBand(vertex.uv.x, floorGreenBand) && (vertex.position - fern.root).length() <= fern.length * 1.2f);
        }

        assert(highest > 0.2f * fern.length);
    }

    /*
    * A patch: as many leaves, seedlings and ferns as its density asks, all within its square; its levels fewer vertices
    * each; the same every run.
    */
    void testAPatch() {
        ForestFloorSettings settings;
        settings.leavesPerSquareMetre = 10.0f;
        const FloorPatch patch = growFloorPatch(settings, 0.5f, 4);
        const float area = settings.cellMetres * settings.cellMetres;
        assert(patch.leaves.size() == static_cast<std::size_t>(std::lround(0.5f * settings.leavesPerSquareMetre * area)));
        assert(patch.seedlings.size() == static_cast<std::size_t>(std::lround(0.5f * settings.seedlingsPerSquareMetre * area)));
        assert(patch.ferns.size() == static_cast<std::size_t>(std::lround(0.5f * settings.fernsPerSquareMetre * area)));
        const float half = 0.5f * settings.cellMetres;

        for (const FallenLeaf& leaf : patch.leaves) {
            assert(std::abs(leaf.centre.x) <= half && std::abs(leaf.centre.z) <= half);
        }

        assert(patch.level.vertices.size() < patch.mesh.vertices.size() && patch.farLevel.vertices.size() < patch.level.vertices.size());
        const FloorPatch again = growFloorPatch(settings, 0.5f, 4);
        assert(again.mesh.vertices.size() == patch.mesh.vertices.size() && again.mesh.vertices.back().position.x == patch.mesh.vertices.back().position.x);
    }

    // The cover's picture: four bands as wide as it is tall, opaque, the last one green, the litter's browner.
    void testThePicture() {
        constexpr int texels = 32;
        const ImageData image = floorCoverImage(texels, Pixel{110, 85, 60}, Pixel{60, 100, 45});
        assert(image.width == static_cast<std::uint32_t>(floorCoverBands * texels) && image.height == static_cast<std::uint32_t>(texels));
        const auto texel = [&](int band, int channel) {
            const std::size_t x = static_cast<std::size_t>(band * texels + texels / 4);
            return static_cast<int>(image.pixels[(static_cast<std::size_t>(texels / 2) * image.width + x) * 4 + static_cast<std::size_t>(channel)]);
        };

        for (std::size_t alpha = 3; alpha < image.pixels.size(); alpha += 4) {
            assert(image.pixels[alpha] == 255);
        }

        assert(texel(floorGreenBand, 1) - texel(floorGreenBand, 0) > texel(0, 1) - texel(0, 0));
        assert(floorCoverImage(texels, Pixel{110, 85, 60}, Pixel{60, 100, 45}).pixels == image.pixels);
    }

    /*
    * The forest grows its floor's cover under the trees: cells between the Ring and the clearing, drawn by the GPU's
    * cells from the floor layer's map (ROADMAP d3), as many as the cells; each kind a look following the ground, casting
    * no shadow, drawn only; the layer drawn past the patch. Its settings start-up only.
    */
    void testTheForestGrowsIt() {
        TestForest forest;
        const std::vector<GroundCell>& cells = forest.game.getFloorCells();
        assert(!cells.empty());
        bool underTrees = false;

        for (const GroundCell& cell : cells) {
            underTrees = underTrees || (cell.centre.y > treeNorthmost && cell.centre.y < treeSouthmost);
        }

        assert(underTrees);
        const PlantingField& field = forest.game.getPlantingField(forest.world);
        assert(field.layers.at(static_cast<std::size_t>(forestFloorPlanting)).coverMap == 1);
        const std::vector<std::uint8_t>& values = field.maps.coverMaps.at(1).values;
        assert(static_cast<std::size_t>(std::count_if(values.begin(), values.end(), [](std::uint8_t value) { return value != 0; })) == cells.size());
        std::size_t looks = 0;

        for (const PlantedKindLook& look : forest.game.getCoverLooks()) {
            if (look.layer == forestFloorPlanting) {
                ++looks;
                const ModelRenderer& renderer = look.models.front();
                assert(renderer.followsGround && !renderer.castsShadows && forest.assets.isDrawnOnly(renderer.parts.front().mesh));
            }
        }

        assert(looks > 0 && forest.world.get<PlantingSource>(forest.game.getAir()).field->layers.at(static_cast<std::size_t>(forestFloorPlanting)).cellSet ==
                                plantingFineCells);

        FieldCounter counter;
        visit(forest.game.getFloorSettings(), counter);
        assert(counter.fields == 15 && counter.live == 0);
    }
}

void testFloorCover() {
    testAFallenLeaf();
    testASeedling();
    testAFern();
    testAPatch();
    testThePicture();
    testTheForestGrowsIt();
}
