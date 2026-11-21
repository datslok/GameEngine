#include "games/forest/forest_game.h"
#include "games/forest/forest_terrain.h"
#include "games/forest/grass.h"
#include "render/gpu/material_shared.h"
#include "render/gpu/material_uniforms.h"
#include "scene/model_renderer.h"

#include "forest_helpers.h"

#include "test_jobs.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    /*
    * One blade: from its root on the ground (y 0) up to its height, leaning its way, narrowing to a point at its tip;
    * its normals tilted up (so a field is lit from above whichever way its blades face), its u within its tone's band
    * and v from 0 at the root to 1 at the tip.
    */
    void testABlade() {
        GrassBlade blade;
        blade.root = Vec3{1.0f, 0.0f, -2.0f};
        blade.height = 0.5f;
        blade.lean = Vec3{1.0f, 0.0f, 0.0f};
        blade.leanShare = 0.3f;
        blade.facingRadians = 0.4f;
        blade.width = 0.012f;
        blade.band = 2;
        IndexedMesh mesh;
        appendGrassBlade(mesh, blade, 4);
        assert(mesh.vertices.size() == 9 && mesh.indices.size() == 7 * 3);

        float highest = 0.0f;
        Vec3 tip;

        for (const MeshVertex& vertex : mesh.vertices) {
            assert(vertex.position.y >= -1e-6f && vertex.normal.y > 0.5f);
            assert(vertex.uv.x >= 0.5f - 1e-6f && vertex.uv.x <= 0.75f + 1e-6f && vertex.uv.y >= 0.0f && vertex.uv.y <= 1.0f);

            if (vertex.position.y > highest) {
                highest = vertex.position.y;
                tip = vertex.position;
            }
        }

        // The tip: below the blade's length (it leans), leaning towards +x, a single point (v 1).
        assert(highest < 0.5f && highest > 0.35f && tip.x > blade.root.x + 0.05f);
        assert(mesh.vertices.back().uv.y == 1.0f);

        // The root's two vertices the blade's width apart, on the ground.
        assert(near((mesh.vertices[0].position - mesh.vertices[1].position).length(), blade.width, 1e-4f));
        assert(mesh.vertices[0].position.y == 0.0f && mesh.vertices[1].position.y == 0.0f);
    }

    /*
    * A patch: blades all over its square (centred on its middle), as many as the density and its share ask; each
    * clump's blades share a tone band; the coarser levels keep half and a quarter of the blades; the same every run.
    */
    void testAPatch() {
        ForestGrassSettings settings;
        settings.bladesPerSquareMetre = 20.0f;
        const GrassPatch patch = growGrassPatch(settings, 1.0f, 5);
        const float half = 0.5f * settings.cellMetres;
        const int expected = static_cast<int>(std::lround(settings.bladesPerSquareMetre * settings.cellMetres * settings.cellMetres));
        assert(patch.blades.size() == static_cast<std::size_t>(expected));

        for (const GrassBlade& blade : patch.blades) {
            assert(std::abs(blade.root.x) <= half && std::abs(blade.root.z) <= half && blade.root.y == 0.0f);
            assert(blade.height >= settings.shortest * grassLeastClumpHeight - 1e-4f && blade.height <= settings.tallest * grassMostClumpHeight + 1e-4f);
            assert(blade.band >= 0 && blade.band < grassToneBands);
        }

        // Blades in one clump share its band.
        for (std::size_t a = 0; a < patch.blades.size(); ++a) {
            for (std::size_t b = a + 1; b < std::min(patch.blades.size(), a + 40); ++b) {
                if (patch.blades[a].clump == patch.blades[b].clump) {
                    assert(patch.blades[a].band == patch.blades[b].band);
                }
            }
        }

        const GrassPatch sparse = growGrassPatch(settings, 0.25f, 5);
        assert(sparse.blades.size() == static_cast<std::size_t>(std::lround(0.25f * static_cast<float>(expected))));
        // A sparser patch keeps the first of the same blades.
        assert(sparse.blades.front().root.x == patch.blades.front().root.x);

        // The levels: fewer vertices each, widened to keep the cover.
        assert(patch.level.vertices.size() < patch.mesh.vertices.size() && patch.farLevel.vertices.size() < patch.level.vertices.size());
        assert(patch.mesh.vertices.size() == patch.blades.size() * 9);

        const GrassPatch again = growGrassPatch(settings, 1.0f, 5);
        assert(again.mesh.vertices.size() == patch.mesh.vertices.size() && again.mesh.vertices[100].position.x == patch.mesh.vertices[100].position.x);
    }

    // A mesh's area: what its blades cover seen face on.
    float meshArea(const IndexedMesh& mesh) {
        float area = 0.0f;

        for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
            const Vec3& a = mesh.vertices[mesh.indices[index]].position;
            const Vec3& b = mesh.vertices[mesh.indices[index + 1]].position;
            const Vec3& c = mesh.vertices[mesh.indices[index + 2]].position;
            area += 0.5f * (b - a).cross(c - a).length();
        }

        return area;
    }

    /*
    * The coarser levels keep the cover (Ghost of Tsushima's half the blades, twice as wide): their area the full mesh's
    * times levelCover, within the few percent fewer segments trim off the curve; their blades the first of the full's.
    */
    void testTheLevelsKeepTheCover() {
        assert(grassLevelWidening(1.0f, 0.5f) == 2.0f && grassLevelWidening(1.0f, 0.25f) == 4.0f && grassLevelWidening(0.5f, 0.5f) == 1.0f);

        ForestGrassSettings settings;
        settings.bladesPerSquareMetre = 20.0f;
        const GrassPatch patch = growGrassPatch(settings, 1.0f, 7);
        const float full = meshArea(patch.mesh);
        assert(near(meshArea(patch.level) / full, 1.0f, 0.1f) && near(meshArea(patch.farLevel) / full, 1.0f, 0.1f));

        // A level's first blade is the full mesh's first: their roots' middles the same.
        const Vec3 fullRoot = (patch.mesh.vertices[0].position + patch.mesh.vertices[1].position) * 0.5f;
        const Vec3 levelRoot = (patch.level.vertices[0].position + patch.level.vertices[1].position) * 0.5f;
        assert((fullRoot - levelRoot).length() < 1e-5f);

        settings.levelCover = 0.5f;
        const GrassPatch thinner = growGrassPatch(settings, 1.0f, 7);
        assert(near(meshArea(thinner.level) / full, 0.5f, 0.05f) && near(meshArea(thinner.farLevel) / full, 0.5f, 0.05f));
    }

    // A cell's density step: none below the least paint, else the nearest of the steps, at least the first.
    void testDensitySteps() {
        ForestGrassSettings settings;
        settings.densitySteps = 4;
        assert(grassDensityStep(settings, 0.0f) == 0 && grassDensityStep(settings, settings.leastPaint * 0.5f) == 0);
        assert(grassDensityStep(settings, 0.1f) == 1 && grassDensityStep(settings, 0.5f) == 2 && grassDensityStep(settings, 1.0f) == 4);
    }

    /*
    * The blades' picture: opaque, lighter at the tip than at the root, its tone bands different, the same every run.
    */
    void testTheBladesPicture() {
        const ImageData image = grassBladeImage(64, Pixel{90, 110, 50});
        assert(image.width == 64 && image.height == 64);
        const auto green = [&](std::uint32_t x, std::uint32_t y) {
            return image.pixels[(static_cast<std::size_t>(y) * 64 + x) * 4 + 1];
        };

        for (std::size_t alpha = 3; alpha < image.pixels.size(); alpha += 4) {
            assert(image.pixels[alpha] == 255);
        }

        // Rows from the bottom (v 0, the root): the top row is lighter.
        assert(green(8, 62) > green(8, 1));
        std::set<int> tones;

        for (int band = 0; band < grassToneBands; ++band) {
            tones.insert(green(static_cast<std::uint32_t>(band * 16 + 8), 32));
        }

        assert(tones.size() == static_cast<std::size_t>(grassToneBands));
        assert(grassBladeImage(64, Pixel{90, 110, 50}).pixels == image.pixels);
    }

    // A material keeping its normal on its back says so to the shaders.
    void testTheBackKeepsItsNormal() {
        Material material;
        material.doubleSided = true;
        material.backKeepsNormal = true;
        assert((packMaterial(material).extra[1] & MATERIAL_BACK_KEEPS_NORMAL) != 0u);
        material.backKeepsNormal = false;
        assert((packMaterial(material).extra[1] & MATERIAL_BACK_KEEPS_NORMAL) == 0u);
    }

    /*
    * The forest grows grass in the open: cells on the meadow at the start, none on the path's mud. The GPU's cells draw
    * them (ROADMAP d3): the grass layer's map names each cell's patch kind and quarter turns and nothing else; each kind a
    * look following the ground, casting no shadow, drawn only; the layer drawn past the patch, its culling grown by the
    * land's relief (at least the land's rise anywhere in a cell). Its settings start-up only.
    */
    void testTheForestGrowsIt() {
        TestForest forest;
        const std::vector<GrassCell>& cells = forest.game.getGrassCells();
        const ForestGrassSettings& settings = forest.game.getGrassSettings();
        assert(!cells.empty());
        bool onMeadow = false;

        for (const GrassCell& cell : cells) {
            onMeadow = onMeadow || (cell.centre - forestStartPlace).length() < 20.0f;
            assert(forestPathDistance(cell.centre.x, cell.centre.y) > 0.0f);
        }

        assert(onMeadow);

        const PlantingField& field = forest.game.getPlantingField(forest.world);
        const PlantingLayerRecord& layer = field.layers.at(static_cast<std::size_t>(forestGrassPlanting));
        assert(layer.coverMap == 0 && layer.footprint == settings.cellMetres && layer.kinds == settings.variants * settings.densitySteps);
        const PlantingCoverMap& map = field.maps.coverMaps.at(0);
        std::size_t named = 0;

        for (const std::uint8_t value : map.values) {
            named += value != 0 ? 1 : 0;
        }

        assert(named == cells.size());

        for (const GrassCell& cell : cells) {
            const int column = static_cast<int>(std::floor(cell.centre.x / settings.cellMetres));
            const int row = static_cast<int>(std::floor(cell.centre.y / settings.cellMetres));
            assert(plantingCoverAt(map, column, row) == plantingCoverValue(forestCoverKind(cell, settings.densitySteps), cell.quarterTurns));
        }

        std::size_t looks = 0;

        for (const PlantedKindLook& look : forest.game.getCoverLooks()) {
            if (look.layer != forestGrassPlanting) {
                continue;
            }

            ++looks;
            assert(look.kind >= 0 && look.kind < layer.kinds && look.models.size() == 1);
            const ModelRenderer& renderer = look.models.front();
            assert(renderer.followsGround && !renderer.castsShadows && forest.assets.isDrawnOnly(renderer.parts.front().mesh));
        }

        assert(looks > 0);
        const PlantingLooks& drawn = *forest.world.get<PlantingSource>(forest.game.getAir()).looks;
        assert(drawn.layers.at(static_cast<std::size_t>(forestGrassPlanting)).groundRelief > 0.0f);
        assert(forest.world.get<PlantingSource>(forest.game.getAir()).field->layers.at(static_cast<std::size_t>(forestGrassPlanting)).cellSet ==
               plantingFineCells);

        // The relief is at least the land's rise anywhere in a cell (some cells, read every metre).
        const std::vector<GrassCell>& grass = forest.game.getGrassCells();
        const std::vector<GrassCell> some(grass.begin(), grass.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(grass.size(), 40)));
        const float relief = forestCoverRelief(some, 8.0f, forest.game.getGroundHeights(), testJobs());

        for (const GrassCell& cell : some) {
            const float foot = terrainSurfaceHeight(forest.game.getGroundHeights(), cell.centre.x, cell.centre.y);

            for (float x = -4.0f; x <= 4.0f; x += 1.0f) {
                for (float z = -4.0f; z <= 4.0f; z += 1.0f) {
                    assert(std::abs(terrainSurfaceHeight(forest.game.getGroundHeights(), cell.centre.x + x, cell.centre.y + z) - foot) <= relief + 1e-4f);
                }
            }
        }

        FieldCounter counter;
        visit(forest.game.getGrassSettings(), counter);
        assert(counter.fields == 14 && counter.live == 0);
    }

    /*
    * A cover map holds exactly its cells, each its kind (its variant's run of steps) and turns; a layer as many kinds as
    * variants times steps, refusing more than a layer holds.
    */
    void testTheCoverLayer() {
        const std::vector<GroundCell> cells{GroundCell{Vec2{4.0f, -12.0f}, 2, 1, 3}, GroundCell{Vec2{-20.0f, 20.0f}, 4, 3, 0}};
        const PlantingCoverMap map = forestCoverMap(cells, 8.0f, 4);
        assert(map.firstColumn == -3 && map.firstRow == -2 && map.columns == 4 && map.rows == 5);
        assert(plantingCoverAt(map, 0, -2) == plantingCoverValue(1 * 4 + 2 - 1, 3) && plantingCoverAt(map, -3, 2) == plantingCoverValue(15, 0));
        assert(std::count(map.values.begin(), map.values.end(), std::uint8_t{0}) == static_cast<std::ptrdiff_t>(map.values.size() - 2));
        assert(forestCoverMap({}, 8.0f, 4).values.empty());

        const PlantingLayerRecord layer = forestCoverLayer(8.0f, 4, 4, 1, 99u);
        assert(layer.kinds == 16 && layer.coverMap == 1 && layer.footprint == 8.0f && layer.scaleRange == 0.0f && layer.leastScale == 1.0f);
        bool refused = false;

        try {
            forestCoverLayer(8.0f, 9, 4, 0, 1u);
        } catch (const std::invalid_argument&) {
            refused = true;
        }

        assert(refused);
    }
}

void testGrass() {
    testABlade();
    testAPatch();
    testTheLevelsKeepTheCover();
    testDensitySteps();
    testTheBladesPicture();
    testTheBackKeepsItsNormal();
    testTheForestGrowsIt();
    testTheCoverLayer();
}
