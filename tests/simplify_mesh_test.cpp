#include "mesh_helpers.h"
#include "scene/simplify_mesh.h"
#include "scene/indexed_mesh.h"
#include "scene/mesh.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {
    bool samePoint(const Vec3& a, const Vec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    // Every index names a vertex, and every vertex is named: the unused ones are dropped.
    void assertCompact(const IndexedMesh& mesh) {
        assert(!mesh.indices.empty() && mesh.indices.size() % 3 == 0);
        std::vector<bool> used(mesh.vertices.size(), false);

        for (std::uint32_t index : mesh.indices) {
            assert(index < mesh.vertices.size());
            used[index] = true;
        }

        for (bool named : used) {
            assert(named);
        }
    }

    /*
    * Simplifying keeps a share of the triangles (no more than asked, within the error allowed), uses only vertices the
    * mesh had (meshoptimizer collapses edges onto existing vertices), drops the unused ones, and with the border locked
    * keeps every border vertex, so a cut-out leaf's outline is not cut into: the bounds stay the same.
    */
    void testSimplifyingKeepsAShareAndTheBorder() {
        const IndexedMesh sheet = rippledSheet();
        const IndexedMesh simpler = simplifyMesh(sheet, 0.25f, 0.02f, true);
        assertCompact(simpler);
        assert(simpler.indices.size() <= sheet.indices.size() / 2);

        for (const MeshVertex& vertex : simpler.vertices) {
            bool found = false;

            for (const MeshVertex& original : sheet.vertices) {
                found = found || (samePoint(vertex.position, original.position) && vertex.uv.x == original.uv.x && vertex.uv.y == original.uv.y);
            }

            assert(found);
        }

        const Aabb before = indexedMeshBounds(sheet);
        const Aabb after = indexedMeshBounds(simpler);
        assert(samePoint(before.minimum, after.minimum) && samePoint(before.maximum, after.maximum));
    }

    // Unlocked, the border may go too, so a shadow's stand-in can drop much further than the seen mesh.
    void testAnUnlockedBorderSimplifiesFurther() {
        const IndexedMesh sheet = rippledSheet();
        const IndexedMesh locked = simplifyMesh(sheet, 0.02f, 0.05f, true);
        const IndexedMesh unlocked = simplifyMesh(sheet, 0.02f, 0.05f, false);
        assertCompact(unlocked);
        assert(unlocked.indices.size() < locked.indices.size());
    }

    // Keeping everything changes nothing.
    void testKeepingAllIsTheSameMesh() {
        const IndexedMesh sheet = rippledSheet();
        const IndexedMesh same = simplifyMesh(sheet, 1.0f, 0.0f, true);
        assert(same.indices.size() == sheet.indices.size() && same.vertices.size() == sheet.vertices.size());
    }
}

void testSimplifyMesh() {
    testSimplifyingKeepsAShareAndTheBorder();
    testAnUnlockedBorderSimplifiesFurther();
    testKeepingAllIsTheSameMesh();
}
