#include "render/gpu/debug_line_vertices.h"

std::vector<DebugLineVertex> buildDebugLineVertices(const std::vector<DebugLine>& lines) {
    std::vector<DebugLineVertex> vertices;
    vertices.reserve(lines.size() * 2);

    for (const DebugLine& line : lines) {
        for (const Vec3& end : {line.from, line.to}) {
            vertices.push_back(DebugLineVertex{{end.x, end.y, end.z}, {line.colour.x, line.colour.y, line.colour.z}});
        }
    }

    return vertices;
}