#include "core/command_registry.h"
#include "games/forest/climbers.h"
#include "games/forest/forest_game.h"
#include "games/forest/forest_trees.h"
#include "games/forest/island_hangings.h"
#include "math/angles.h"
#include "scene/model_renderer.h"

#include "forest_helpers.h"
#include "test_jobs.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <string>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    // A trunk as the trees grow theirs: from under the ground up past its fork, leaning a little, thicker at the foot,
    // its buttresses' lobes near the ground.
    ClimbHost testTrunk() {
        ClimbHost host;

        for (int point = 0; point <= 30; ++point) {
            const float y = -1.0f + static_cast<float>(point) * 0.5f;
            const float share = std::clamp((y + 1.0f) / 13.0f, 0.0f, 1.0f);
            host.trunk.points.push_back(Vec3{0.02f * y, y, -0.01f * y});
            host.trunk.radii.push_back(0.6f * (1.0f + 0.5f * (1.0f - share)));
        }

        host.lobes = TubeLobes{.count = 5, .amplitude = 1.5f, .heightMetres = 1.5f, .sharpness = 24.0f, .groundHeight = 0.0f, .seed = 9};
        host.topHeight = 12.0f;
        return host;
    }

    // The tube's frames are the ones appendTube builds its rings with: each ring's vertex at an angle is its point plus
    // that frame's direction times the radius.
    void testTubeFramesMatchTheTube() {
        const ClimbHost host = testTrunk();
        IndexedMesh tube;
        constexpr int sides = 12;
        appendTube(tube, host.trunk, sides, 1.5f);
        const std::vector<TubeFrame> frames = tubeFrames(host.trunk);
        assert(frames.size() == host.trunk.points.size());

        for (std::size_t ring = 0; ring < frames.size(); ++ring) {
            for (int side = 0; side < sides; ++side) {
                const float angle = fullTurnRadians * static_cast<float>(side) / static_cast<float>(sides);
                const Vec3 expected = host.trunk.points[ring] + frames[ring].direction(angle) * host.trunk.radii[ring];
                const Vec3& vertex = tube.vertices[ring * (sides + 1) + static_cast<std::size_t>(side)].position;
                assert((vertex - expected).length() < 1e-4f);
            }
        }
    }

    /*
    * The host's surface: at a height and an angle round it, a point its lobed radius from its centre line there, facing
    * out; a point's clearance is how far outside the surface it is across (negative inside).
    */
    void testTheHostsSurface() {
        ClimbHost plain = testTrunk();
        plain.lobes = TubeLobes{};
        const std::vector<TubeFrame> frames = tubeFrames(plain.trunk);
        const HostPoint surface = hostSurface(plain, frames, 6.0f, 1.0f);
        // The trunk leans, so its frames tilt a little: the surface point is within a lean's share of a radius of the height.
        assert(near(surface.position.y, 6.0f, 0.02f) && near(hostClearance(plain, frames, surface.position), 0.0f, 0.01f));
        assert(near(hostClearance(plain, frames, surface.position + surface.outward * 0.2f), 0.2f, 0.01f));
        assert(hostClearance(plain, frames, Vec3{0.12f, 6.0f, -0.06f}) < -0.3f);

        // On the buttresses the surface stands farther out, near the ground.
        const ClimbHost lobed = testTrunk();
        const std::vector<TubeFrame> lobedFrames = tubeFrames(lobed.trunk);
        float widest = 0.0f;

        for (int step = 0; step < 64; ++step) {
            const HostPoint point = hostSurface(lobed, lobedFrames, 0.3f, fullTurnRadians * static_cast<float>(step) / 64.0f);
            const Vec3 centre = Vec3{0.02f * 0.3f, 0.3f, -0.01f * 0.3f};
            widest = std::max(widest, Vec3{point.position.x - centre.x, 0.0f, point.position.z - centre.z}.length());
        }

        assert(widest > 1.5f);
    }

    /*
    * One climber up the trunk: never inside it (stems, roots and leaves all outside its surface, the buttresses too);
    * small leaves pressed to the bark below the shingle height and big ones standing off above it; feeder roots
    * hanging straight down outside the buttresses, some ending in mid air; up to between its least and most share of
    * the trunk; the same every run.
    */
    void testAClimberHugsTheTrunk() {
        const ClimbHost host = testTrunk();
        const std::vector<TubeFrame> frames = tubeFrames(host.trunk);
        const ForestClimberSettings settings;
        const ClimberMeshes climber = growClimber(host, settings, 3);
        assert(!climber.stems.vertices.empty() && !climber.leaves.vertices.empty());
        assert(climber.shingleLeaves > 0 && climber.bigLeaves > 0 && climber.roots > 0);
        assert(climber.top >= settings.leastHeightShare * host.topHeight - 0.5f && climber.top <= settings.mostHeightShare * host.topHeight + 0.5f);
        assert(near(climber.facing.length(), 1.0f, 1e-4f) && climber.facing.y == 0.0f);

        for (const MeshVertex& vertex : climber.stems.vertices) {
            assert(hostClearance(host, frames, vertex.position) > -0.02f);
        }

        float farthestBig = 0.0f;

        for (const MeshVertex& vertex : climber.leaves.vertices) {
            const float clearance = hostClearance(host, frames, vertex.position);
            assert(clearance > -0.02f);

            if (vertex.position.y < settings.shingleHeight - settings.shingleLeafLength * 2.0f && vertex.position.y > 1.0f) {
                assert(clearance < 0.1f);
            }

            if (vertex.position.y > settings.shingleHeight + 1.0f) {
                farthestBig = std::max(farthestBig, clearance);
            }
        }

        assert(farthestBig > 0.2f);

        // The roots: straight down past their bend, outside the trunk all the way, some to the ground, some not.
        assert(climber.rootStrands.size() == static_cast<std::size_t>(climber.roots));
        int midAir = 0;

        for (const HangingStrand& root : climber.rootStrands) {
            for (std::size_t point = 2; point < root.points.size(); ++point) {
                assert(near(root.points[point].x, root.points[1].x, 1e-4f) && near(root.points[point].z, root.points[1].z, 1e-4f));
                assert(root.points[point].y < root.points[point - 1].y);
            }

            for (const Vec3& point : root.points) {
                assert(hostClearance(host, frames, point) > 0.0f);
            }

            midAir += root.points.back().y > 0.2f ? 1 : 0;
        }

        assert(midAir < climber.roots);

        const ClimberMeshes again = growClimber(host, settings, 3);
        assert(again.stems.vertices.size() == climber.stems.vertices.size() && again.leaves.vertices.size() == climber.leaves.vertices.size());
    }

    // How many climbers a tree carries: none on treeShare's other share, else 1 to mostPerTree, by its seed.
    void testHowManyClimbers() {
        const ForestClimberSettings settings;
        int carrying = 0;
        constexpr int trees = 400;

        for (int tree = 0; tree < trees; ++tree) {
            const int count = climbersOnTree(settings, static_cast<std::uint32_t>(tree) + 1);
            assert(count >= 0 && count <= settings.mostPerTree);
            carrying += count > 0 ? 1 : 0;
        }

        assert(near(static_cast<float>(carrying) / trees, settings.treeShare, 0.08f));
    }

    // The leaf's picture: opaque (its outline is its geometry), a paler midrib down the middle, the same every run.
    void testTheLeafsPicture() {
        const AroidLeafImages images = aroidLeafImages(64, Pixel{60, 100, 45}, 5);
        assert(images.colour.width == 64 && images.colour.height == 64 && images.height.width == 64);
        const auto green = [&](std::uint32_t x, std::uint32_t y) {
            return images.colour.pixels[(static_cast<std::size_t>(y) * 64 + x) * 4 + 1];
        };

        for (std::size_t texel = 3; texel < images.colour.pixels.size(); texel += 4) {
            assert(images.colour.pixels[texel] == 255);
        }

        assert(green(32, 30) > green(8, 30));
        const AroidLeafImages again = aroidLeafImages(64, Pixel{60, 100, 45}, 5);
        assert(again.colour.pixels == images.colour.pixels);
    }

    // A grown tree keeps its trunk for climbers: from its foot under the ground to the fork, thicker below, its lobes.
    void testTreesKeepTheirTrunk() {
        ForestTreeSettings settings;
        settings.attractors = 200;
        const TreeMeshes tree = growTreeMeshes(settings, 4, 1.5f);
        const ClimbHost& host = tree.trunk;
        assert(host.trunk.points.size() >= 2 && host.trunk.points.size() == host.trunk.radii.size());
        assert(host.trunk.points.front().y < 0.0f && host.topHeight > 1.0f);
        assert(host.trunk.radii.front() > host.trunk.radii.back());
        assert(host.lobes.count == (settings.buttress > 0.0f ? buttressCount : 0));
    }

    // The forest's big trees carry them: still, drawn only, into the nearest cascades; their settings start-up only.
    void testTheForestGrowsThem() {
        TestForest forest;
        const std::vector<Entity>& climbers = forest.game.getClimbers();
        assert(!climbers.empty());

        for (const Entity entity : climbers) {
            const ModelRenderer& renderer = forest.world.get<ModelRenderer>(entity);
            assert(renderer.still && renderer.shadowCascades == climberShadowCascades);
            assert(forest.assets.isDrawnOnly(renderer.parts.front().mesh));
        }

        FieldCounter counter;
        visit(forest.game.getClimberSettings(), counter);
        assert(counter.fields == 13 && counter.live == 0);

        // forest-climber places the camera beside a tree carrying climbers, looking at its trunk; none past the last.
        CommandRegistry registry;
        forest.game.addCommands(registry, forest.world);
        const std::vector<TreePlacement>& climbed = forest.game.getClimbedTrees();
        assert(!climbed.empty());
        assert(registry.execute("forest-climber").ok && registry.execute("forest-climber 0").ok);
        const Vec3 eye = forest.game.getCamera().getPosition();
        const Vec3 foot = climbed.front().foot;
        const float across = Vec3{eye.x - foot.x, 0.0f, eye.z - foot.z}.length();
        assert(across > 3.0f && across < 12.0f);
        assert(forest.game.getClimbedFacing().size() == climbed.size());
        const Vec3 fromFoot = Vec3{eye.x - foot.x, 0.0f, eye.z - foot.z}.normalized();
        assert(fromFoot.dot(forest.game.getClimbedFacing().front()) > 0.99f);
        assert(!registry.execute("forest-climber " + std::to_string(climbed.size())).ok);
        assert(!registry.execute("forest-climber many").ok);
    }
}

void testClimbers() {
    testTubeFramesMatchTheTube();
    testTheHostsSurface();
    testAClimberHugsTheTrunk();
    testHowManyClimbers();
    testTheLeafsPicture();
    testTreesKeepTheirTrunk();
    testTheForestGrowsThem();
}
