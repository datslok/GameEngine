#include "games/forest/hanging_moss.h"
#include "games/forest/forest_game.h"
#include "games/forest/island_hangings.h"
#include "games/forest/valley_shapes.h"
#include "games/forest/valley_strata.h"
#include "assets/asset_manager.h"
#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "scene/model_renderer.h"

#include "forest_helpers.h"
#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <functional>
#include <vector>

namespace {
    // Every triangle of a mesh faces the way its corners' normals say.
    bool trianglesFaceTheirNormals(const IndexedMesh& mesh) {
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const MeshVertex& a = mesh.vertices[mesh.indices[i]];
            const MeshVertex& b = mesh.vertices[mesh.indices[i + 1]];
            const MeshVertex& c = mesh.vertices[mesh.indices[i + 2]];
            const Vec3 facing = (b.position - a.position).cross(c.position - a.position);

            if (facing.lengthSquared() > 1e-12f && facing.dot(a.normal + b.normal + c.normal) <= 0.0f) {
                return false;
            }
        }

        return true;
    }

    // The share of a row of the image (row 0 at the bottom, v 0) that is moss (alpha at least half).
    float rowCoverage(const ImageData& image, std::uint32_t row) {
        int covered = 0;

        for (std::uint32_t column = 0; column < image.width; ++column) {
            covered += image.pixels[(static_cast<std::size_t>(row) * image.width + column) * 4 + 3] >= 128 ? 1 : 0;
        }

        return static_cast<float>(covered) / static_cast<float>(image.width);
    }

    /*
    * The strand texture: moss hangs from its top edge (the last rows, v 1) in thin strands, so the top is mostly
    * covered and the bottom mostly holes, with clear gaps between strands; the strands are the colour given, lighter
    * and darker; the same seed draws the same image.
    */
    void testTheStrandTextureHangsFromItsTop() {
        const Pixel colour{90, 110, 60};
        const ImageData image = mossStrandImage(128, 256, colour, 5u);
        assert(image.width == 128 && image.height == 256 && image.pixels.size() == 128u * 256u * 4u);
        assert(image.encoding == TextureEncoding::Srgb);

        const float top = rowCoverage(image, 250);
        const float middle = rowCoverage(image, 128);
        const float bottom = rowCoverage(image, 5);
        assert(top > 0.5f && top > middle && middle > bottom && bottom < 0.15f);
        assert(top < 0.98f);

        int opaque = 0;

        for (std::size_t texel = 0; texel < image.pixels.size() / 4; ++texel) {
            if (image.pixels[texel * 4 + 3] == 255) {
                ++opaque;
                const int green = image.pixels[texel * 4 + 1];
                assert(green >= colour.g / 3 && green <= 255);
            }
        }

        assert(opaque > 1000);
        const ImageData again = mossStrandImage(128, 256, colour, 5u);
        assert(again.pixels == image.pixels);
        assert(mossStrandImage(128, 256, colour, 6u).pixels != image.pixels);
    }

    /*
    * A clump: two crossed vertical cards hanging from its top point, each length long and widthShare of it wide, the
    * texture's top (v 1) at the top; four vertices and two triangles each, facing their normals; the tangent across
    * (+u), the bitangent up (+v).
    */
    void testAClumpIsTwoCrossedCards() {
        IndexedMesh moss;
        const Vec3 top{3.0f, 40.0f, -2.0f};
        appendMossClump(moss, top, 5.0f, 0.7f);

        assert(moss.vertices.size() == 8 && moss.indices.size() == 12);
        assert(trianglesFaceTheirNormals(moss));
        float highest = -1e9f;
        float lowest = 1e9f;

        for (const MeshVertex& vertex : moss.vertices) {
            highest = std::max(highest, vertex.position.y);
            lowest = std::min(lowest, vertex.position.y);
            assert(std::abs(vertex.normal.y) < 1e-5f && near(vertex.normal.length(), 1.0f, 1e-5f));
            assert(near((vertex.uv.y == 1.0f ? top.y : top.y - 5.0f), vertex.position.y, 1e-4f));
            const Vec3 tangent{vertex.tangent.x, vertex.tangent.y, vertex.tangent.z};
            assert(vertex.tangent.w == 1.0f && vertex.normal.cross(tangent).y > 0.99f);
            assert(near(Vec2{vertex.position.x - top.x, vertex.position.z - top.z}.length(), 0.5f * mossClumpWidthShare * 5.0f, 1e-4f));
        }

        assert(near(highest, top.y, 1e-5f) && near(lowest, top.y - 5.0f, 1e-5f));

        // The two cards are square to each other.
        assert(std::abs(moss.vertices[0].normal.dot(moss.vertices[4].normal)) < 1e-4f);
    }

    /*
    * Lips: where a column's point stands out past the one below it by at least least metres (out from its row's
    * middle), with that way out. A plain cylinder has none; one with a row sticking out 3 m has a lip at every column of
    * that row and none elsewhere.
    */
    void testLipsAreFoundWhereRowsStandOut() {
        const auto cylinder = [](float ledgeRadius) {
            return surfaceGrid(12, 16, false, [ledgeRadius](int row, int column) {
                const float angle = 6.2831853f * static_cast<float>(column) / 16.0f;
                const float radius = row == 6 ? ledgeRadius : 10.0f;
                return Vec3{radius * std::sin(angle), 2.5f * static_cast<float>(row), radius * std::cos(angle)};
            });
        };

        assert(overhangLips(cylinder(10.0f), 1.0f).empty());
        const std::vector<MossPlace> lips = overhangLips(cylinder(13.0f), 1.0f);
        assert(lips.size() == 16);

        for (const MossPlace& lip : lips) {
            assert(near(lip.point.y, 15.0f, 1e-4f) && near(Vec2{lip.point.x, lip.point.z}.length(), 13.0f, 1e-3f));
            assert(std::abs(lip.outward.y) < 1e-6f && near(lip.outward.length(), 1.0f, 1e-5f));
            assert(lip.outward.dot(Vec3{lip.point.x, 0.0f, lip.point.z}) > 0.0f);
        }
    }

    RockCarving testCarving() {
        return RockCarving{.bands = valleyStrata(), .warp = strataWarpImage(64), .warpTileMetres = 1500.0f, .warpMetres = 6.0f};
    }

    /*
    * A pillar's moss hangs from its ledges: a pillar of one hard bed has lips all round it at that bed, about ledgeShare
    * of them mossed. Soft rock alone has only a few, where a vertical joint (a groove) ends and the rock above it stands
    * out past the groove below, and near the foot; moss may hang there too.
    */
    void testPillarsGrowMossUnderTheirLedges() {
        RockCarving soft{.bands = {{.bottom = -1000.0f, .top = 2000.0f}}, .warp = {}, .warpTileMetres = 1500.0f, .warpMetres = 0.0f};
        RockCarving ledged{.bands = {{.bottom = -1000.0f, .top = 100.0f}, {.bottom = 100.0f, .top = 110.0f, .hard = true}, {.bottom = 110.0f, .top = 2000.0f}},
                           .warp = {}, .warpTileMetres = 1500.0f, .warpMetres = 0.0f};
        const PillarPlacement placement{.foot = Vec3{0.0f, 0.0f, 0.0f}, .radius = 20.0f, .height = 220.0f, .turnRadians = 0.0f, .seed = 9u};
        const ForestMossSettings settings;

        const std::size_t softLips = overhangLips(pillarSurface(placement, soft), mossLipMetres).size();
        const SurfaceGrid surface = pillarSurface(placement, ledged);
        const std::vector<MossPlace> lips = overhangLips(surface, mossLipMetres);
        assert(lips.size() >= 30 && softLips * 4 < lips.size());
        std::size_t atTheBed = 0;

        for (const MossPlace& lip : lips) {
            atTheBed += lip.point.y > 85.0f && lip.point.y < 115.0f ? 1 : 0;
        }

        assert(atTheBed * 5 >= lips.size() * 4);

        const IndexedMesh moss = pillarMoss(placement, surface, settings);
        const std::size_t clumps = moss.vertices.size() / 8;
        assert(moss.vertices.size() == clumps * 8);
        assert(clumps >= static_cast<std::size_t>(0.5f * settings.ledgeShare * static_cast<float>(lips.size())));
        assert(clumps <= static_cast<std::size_t>(1.5f * settings.ledgeShare * static_cast<float>(lips.size())) + 1);

        const IndexedMesh again = pillarMoss(placement, surface, settings);
        assert(again.vertices.size() == moss.vertices.size() && again.vertices.back().position.x == moss.vertices.back().position.x);
    }

    /*
    * An island's moss: on about tipShare of its roots' and vines' tips, and undersidePerTenMetres clumps for each 10 m of
    * radius scattered over its underside, all below its rim.
    */
    void testIslandsGrowMossOnTipsAndUndersides() {
        const IslandPlacement island{.rim = Vec3{100.0f, 500.0f, -200.0f}, .radius = 60.0f, .turnRadians = 0.4f, .seed = 21u};
        const SurfaceGrid surface = islandSurface(island, testCarving());
        const IslandHangings hangings = islandHangings(island, surface, ForestHangingSettings{});
        assert(hangings.tips.size() == hangings.anchors.size());

        for (std::size_t strand = 0; strand < hangings.tips.size(); ++strand) {
            assert(hangings.tips[strand].y < hangings.anchors[strand].y);
        }

        const ForestMossSettings settings;
        const IndexedMesh moss = islandMoss(island, surface, hangings.tips, settings);
        const std::size_t clumps = moss.vertices.size() / 8;
        const std::size_t underside = static_cast<std::size_t>(std::lround(settings.undersidePerTenMetres * 6.0f));
        const float tips = settings.tipShare * static_cast<float>(hangings.tips.size());
        assert(clumps >= underside + static_cast<std::size_t>(0.5f * tips) && clumps <= underside + static_cast<std::size_t>(1.5f * tips) + 1);

        for (const MeshVertex& vertex : moss.vertices) {
            assert(vertex.position.y < 0.0f);
        }

        ForestMossSettings none = settings;
        none.tipShare = 0.0f;
        none.undersidePerTenMetres = 0.0f;
        assert(islandMoss(island, surface, hangings.tips, none).vertices.empty());
    }

    // Counts the fields a visit names, and whether all of them apply at start-up.
    class FieldCounter : public SettingsVisitor {
    public:
        int fields = 0;
        bool allAtStartup = true;

    protected:
        void visitFloat(const char*, float&, const FloatLimits& limits) override {
            ++fields;
            allAtStartup = allAtStartup && limits.applies == FieldApplies::AtStartup;
        }

        void visitInt(const char*, int&, const IntLimits& limits) override {
            ++fields;
            allAtStartup = allAtStartup && limits.applies == FieldApplies::AtStartup;
        }

        void visitBool(const char*, bool&, FieldApplies applies) override {
            ++fields;
            allAtStartup = allAtStartup && applies == FieldApplies::AtStartup;
        }

        void visitChoice(const char*, int&, const char* const*, int, FieldApplies applies) override {
            ++fields;
            allAtStartup = allAtStartup && applies == FieldApplies::AtStartup;
        }

        void visitGroup(const char*, const std::function<void()>& contents) override {
            contents();
        }
    };

    void testTheSettings() {
        ForestMossSettings settings;
        FieldCounter counter;
        visit(settings, counter);
        assert(counter.fields == 4 && counter.allAtStartup);
    }

    /*
    * The game hangs it: moss meshes, drawn only, a double-sided cut-out whose texture the game made (with the stand-in
    * colour when the scan is missing), placed as the pillar or island it hangs from.
    */
    void testTheGameHangsMoss() {
        TestForest forest{ForestValleySettings{.groundCoarseness = 2, .pillarCount = 10, .islandCount = 4}};
        World& world = forest.world;
        AssetManager& assets = forest.assets;
        ForestGame& game = forest.game;

        const std::vector<Entity>& moss = game.getMoss();
        assert(!moss.empty());

        for (const Entity clumps : moss) {
            const ModelRenderer& renderer = world.get<ModelRenderer>(clumps);
            const Material& material = renderer.parts.front().material;
            assert(isCutOut(material) && material.doubleSided && material.texture.isValid());
            assert(assets.isDrawnOnly(renderer.parts.front().mesh));
            const Vec3 place = world.get<Transform>(clumps).position;
            bool onAShape = false;

            for (const Entity shape : game.getRockShapes()) {
                onAShape = onAShape || (world.get<Transform>(shape).position - place).lengthSquared() == 0.0f;
            }

            assert(onAShape);
        }
    }
}

void testHangingMoss() {
    testTheStrandTextureHangsFromItsTop();
    testAClumpIsTwoCrossedCards();
    testLipsAreFoundWhereRowsStandOut();
    testPillarsGrowMossUnderTheirLedges();
    testIslandsGrowMossOnTipsAndUndersides();
    testTheSettings();
    testTheGameHangsMoss();
}
