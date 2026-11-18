#include "render/gpu/mesh_pool.h"

#include <cassert>
#include <stdexcept>
#include <vector>

namespace {
    // Kept while it fits; else doubled from at least the smallest until it does; refused past 4 GB, clamped below it.
    void testCapacity() {
        assert(poolCapacity(1000, 800, 64, 48) == 1000);
        assert(poolCapacity(0, 10, 64, 48) == 64);
        assert(poolCapacity(64, 65, 64, 48) == 128);
        assert(poolCapacity(64, 1000, 64, 48) == 1024);

        // Doubling would pass 4 GB of 48-byte vertices; what is needed still fits, so as much as fits.
        const std::uint64_t most = largestPoolBytes / 48;
        assert(poolCapacity(most / 2 + 10, most / 2 + 20, 64, 48) == most);

        bool refused = false;

        try {
            poolCapacity(64, most + 1, 64, 48);
        } catch (const std::length_error&) {
            refused = true;
        }

        assert(refused);
    }

    // Each mesh goes after the last, vertices and indices apart; its indices stay its own (the draw adds its offset).
    void testPlacement() {
        MeshPoolLayout layout;
        const MeshPlacement first = layout.place(100, 300);
        const MeshPlacement second = layout.place(40, 60);
        assert(first.firstVertex == 0 && first.firstIndex == 0 && first.vertexCount == 100 && first.indexCount == 300 && first.pooled);
        assert(second.firstVertex == 100 && second.firstIndex == 300 && second.vertexCount == 40 && second.indexCount == 60);
        assert(layout.getVertices() == 140 && layout.getIndices() == 360);
    }

    // A batch's indirect draw: its mesh's indices where they are, each instance numbered from its first draw list entry.
    void testCommands() {
        const DrawBatch batch{MeshHandle{3}, 120, 7, true, false};
        const GpuDrawCommand command = packDrawCommand(batch, MeshPlacement{2000, 50, 9000, 4500, true});
        assert(command.indexCount == 4500 && command.instanceCount == 7 && command.firstInstance == 120);
        assert(command.firstIndex == 9000 && command.vertexOffset == 2000);

        // A mesh with its own buffers: from their start.
        const GpuDrawCommand own = packDrawCommand(batch, MeshPlacement{0, 50, 0, 4500, false});
        assert(own.firstIndex == 0 && own.vertexOffset == 0);
    }

    // Consecutive pooled batches drawn the same way share a run; a change of style or a mesh with its own buffers splits.
    void testRuns() {
        const std::vector<DrawBatch> batches{
            DrawBatch{MeshHandle{0}, 0, 1, false, false}, DrawBatch{MeshHandle{1}, 1, 1, false, false}, DrawBatch{MeshHandle{9}, 2, 1, false, false},
            DrawBatch{MeshHandle{2}, 3, 1, false, false}, DrawBatch{MeshHandle{3}, 4, 1, false, true},  DrawBatch{MeshHandle{4}, 5, 1, false, true},
            DrawBatch{MeshHandle{5}, 6, 1, true, true},
        };
        const std::vector<DrawRun> runs = drawRuns(batches, [](MeshHandle mesh) { return mesh.index != 9; });
        assert(runs.size() == 5);
        assert(runs[0].first == 0 && runs[0].count == 2 && runs[0].pooled);
        assert(runs[1].first == 2 && runs[1].count == 1 && !runs[1].pooled);
        assert(runs[2].first == 3 && runs[2].count == 1 && runs[2].pooled);
        assert(runs[3].first == 4 && runs[3].count == 2);
        assert(runs[4].first == 6 && runs[4].count == 1);
        assert(drawRuns({}, [](MeshHandle) { return true; }).empty());
    }
}

/*
* The shared mesh buffer (render/gpu/mesh_pool.h): growth, placement, draw commands and runs drawn by one call.
*/
void testMeshPool() {
    testCapacity();
    testPlacement();
    testCommands();
    testRuns();
}
