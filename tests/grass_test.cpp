#include "games/forest/forest_game.h"
#include "games/forest/forest_terrain.h"
#include "games/forest/grass.h"
#include "render/gpu/material_shared.h"
#include "render/gpu/material_uniforms.h"
#include "scene/model_renderer.h"

#include "forest_helpers.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <set>
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
    * The forest grows grass in the open: on the meadow at the start, none on the path's mud; each cell following the
    * ground, casting no shadow, still, drawn only; its settings start-up only.
    */
    void testTheForestGrowsIt() {
        TestForest forest;
        const std::vector<Entity>& grass = forest.game.getGrass();
        assert(!grass.empty());
        bool onMeadow = false;

        for (const Entity entity : grass) {
            const ModelRenderer& renderer = forest.world.get<ModelRenderer>(entity);
            assert(renderer.followsGround && !renderer.castsShadows && renderer.isStill());
            assert(forest.assets.isDrawnOnly(renderer.parts.front().mesh));
            const Vec3& place = forest.world.get<Transform>(entity).position;
            assert(place.y == 0.0f);
            onMeadow = onMeadow || (Vec2{place.x, place.z} - forestStartPlace).length() < 20.0f;
            assert(forestPathDistance(place.x, place.z) > 0.0f);
        }

        assert(onMeadow);

        FieldCounter counter;
        visit(forest.game.getGrassSettings(), counter);
        assert(counter.fields == 14 && counter.live == 0);
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
}
