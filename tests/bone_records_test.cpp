#include "render/gpu/bone_records.h"
#include "render/gpu/twig_records.h"

#include "test_levels.h"

#include <cassert>
#include <stdexcept>
#include <vector>

namespace {
    // A binding is one word: its bone low, its weight in 16 bits above; back again within 1 / 65535. Bones past the
    // word's room, or below 0, are refused; no bound vertex can read as unbound.
    void testABindingInAWord() {
        assert(packSwayBinding(SwayBinding{.bone = 7, .weight = 1.0f}) == (7u | 0xFFFF0000u));
        assert(packSwayBinding(SwayBinding{.bone = 0, .weight = 0.0f}) == 0u);
        const SwayBinding back = unpackSwayBinding(packSwayBinding(SwayBinding{.bone = 1234, .weight = 0.3f}));
        assert(back.bone == 1234 && near(back.weight, 0.3f, 1.0f / 65535.0f));
        assert(packSwayBinding(SwayBinding{.bone = 65533, .weight = 1.0f}) != swayUnbound);
        assert(throwsInvalid([] { packSwayBinding(SwayBinding{.bone = 65534, .weight = 1.0f}); }));
        assert(throwsInvalid([] { packSwayBinding(SwayBinding{.bone = -1, .weight = 1.0f}); }));
    }

    // Two skeletons in one table: the second's bones after the first's, parents and chains still counted within each.
    void testSkeletonsInOneTable() {
        SwaySkeleton first;
        appendSwayChain(first.bones, {Vec3{}, Vec3{0.0f, 4.0f, 0.0f}}, {0.3f, 0.2f}, -1, 0, 2.0f);
        first.height = 6.0f;
        SwaySkeleton second;
        appendSwayChain(second.bones, {Vec3{}, Vec3{0.0f, 6.0f, 0.0f}}, {0.5f, 0.1f}, -1, 0, 2.0f);
        appendSwayChain(second.bones, {Vec3{0.0f, 2.0f, 0.0f}, Vec3{2.0f, 3.0f, 0.0f}}, {0.1f, 0.05f}, 0, 1, 2.0f);
        second.height = 8.0f;

        const SwaySkeletonTables tables = packSwaySkeletons({&first, &second});
        assert(tables.skeletons.size() == 2 && tables.bones.size() == first.bones.size() + second.bones.size());
        assert(tables.skeletons[0].firstBone == 0 && tables.skeletons[0].boneCount == 2 && tables.skeletons[0].height == 6.0f);
        assert(tables.skeletons[1].firstBone == 2 && tables.skeletons[1].boneCount == 4 && near(tables.skeletons[1].footRadius, 0.5f, 1e-6f));

        const GpuSwayBone& branch = tables.bones[2 + 3];
        const SwayBone& source = second.bones[3];
        assert(branch.parent == 0 && branch.chainStart == 3 && branch.level == 1 && near(branch.chainLength, source.chainLength, 1e-6f));
        assert(branch.pivot[0] == source.pivot.x && branch.tip[1] == source.tip.y && branch.radius == source.radius && branch.chainRadius == 0.1f);
        assert(tables.bones[1].parent == 0 && tables.bones[0].parent == -1);

        // Each skeleton's rest sphere as swaySkeletonSphere makes it, grown by its reach.
        second.reach = 0.5f;
        const SwaySphere sphere = swaySkeletonSphere(second);
        const float* packed = packSwaySkeletons({&first, &second}).skeletons[1].sphere;
        assert(packed[0] == sphere.centre.x && packed[1] == sphere.centre.y && packed[2] == sphere.centre.z && packed[3] == sphere.radius);
        assert(sphere.radius > 0.5f);

        // A skeleton with no bones, or a child before its parent, is refused.
        SwaySkeleton empty;
        assert(throwsInvalid([&] { packSwaySkeletons({&empty}); }));
        SwaySkeleton disordered = first;
        disordered.bones[0].parent = 1;
        assert(throwsInvalid([&] { packSwaySkeletons({&disordered}); }));
    }

    /*
    * The pool's bindings: a word a pool vertex up to the last bound mesh's end, a bound mesh's at its place, everything
    * else unbound; a mesh without bindings changes nothing. A bound mesh outside the pools, or with the wrong count, is
    * refused.
    */
    void testThePoolsBindings() {
        IndexedMesh bound;
        bound.vertices.resize(3);
        bound.swayBindings = {SwayBinding{.bone = 1, .weight = 1.0f}, SwayBinding{.bone = 2, .weight = 0.0f}, SwayBinding{.bone = 3, .weight = 0.5f}};
        IndexedMesh plain;
        plain.vertices.resize(5);

        std::vector<std::uint32_t> bindings;
        placeSwayBindings(bindings, plain, MeshPlacement{.firstVertex = 0, .vertexCount = 5, .pooled = true});
        placeSwayBindings(bindings, bound, MeshPlacement{.firstVertex = 10, .vertexCount = 3, .pooled = true});
        assert(bindings.size() == 13);

        for (std::size_t vertex = 0; vertex < 10; ++vertex) {
            assert(bindings[vertex] == swayUnbound);
        }

        assert(bindings[10] == packSwayBinding(bound.swayBindings[0]) && bindings[12] == packSwayBinding(bound.swayBindings[2]));

        // A later bound mesh earlier in the pool leaves the rest as it was.
        placeSwayBindings(bindings, bound, MeshPlacement{.firstVertex = 2, .vertexCount = 3, .pooled = true});
        assert(bindings.size() == 13 && bindings[3] == packSwayBinding(bound.swayBindings[1]) && bindings[10] == packSwayBinding(bound.swayBindings[0]));

        assert(throwsInvalid([&] { placeSwayBindings(bindings, bound, MeshPlacement{.firstVertex = 0, .vertexCount = 3, .pooled = false}); }));
        IndexedMesh miscounted = bound;
        miscounted.swayBindings.pop_back();
        assert(throwsInvalid([&] { placeSwayBindings(bindings, miscounted, MeshPlacement{.firstVertex = 0, .vertexCount = 3, .pooled = true}); }));
    }

    // A twig's binding beside its record: its bone's, or unbound for a twig on none.
    void testATwigsBinding() {
        TwigInstance twig;
        assert(packTwigBinding(twig) == swayUnbound);
        twig.binding = SwayBinding{.bone = 9, .weight = 0.25f};
        assert(packTwigBinding(twig) == packSwayBinding(twig.binding));
    }
}

void testBoneRecords() {
    testABindingInAWord();
    testSkeletonsInOneTable();
    testThePoolsBindings();
    testATwigsBinding();
}
