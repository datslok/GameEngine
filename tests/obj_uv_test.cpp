#include "obj_loader.h"

#include <cassert>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
    void expectUvError(const std::string& source) {
        std::istringstream input{source};
        bool threw = false;

        try {
            parseObj(input);
        }
        catch (const std::runtime_error&) {
            threw = true;
        }

        assert(threw);
    }
}

void testObjUvs() {
    const std::string positions =
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 1 1 0\n"
        "v 0 1 0\n";

    const std::string uvs =
        "vt 0 0\n"
        "vt 1 0\n"
        "vt 1 1\n"
        "vt 0 1\n";

    // Negative UV indices and quad triangulation.
    {
        std::istringstream input{
            positions + uvs +
            "f 1/-4 2/-3 3/-2 4/-1\n"
        };

        const Mesh mesh = parseObj(input);
        assert(mesh.triangles.size() == 2);

        const Triangle& first = mesh.triangles[0];
        const Triangle& second = mesh.triangles[1];

        assert(first.uvs[0].value().x == 0.0f);
        assert(first.uvs[0].value().y == 0.0f);
        assert(first.uvs[1].value().x == 1.0f);
        assert(first.uvs[1].value().y == 0.0f);
        assert(first.uvs[2].value().x == 1.0f);
        assert(first.uvs[2].value().y == 1.0f);

        assert(second.uvs[0].value().x == 0.0f);
        assert(second.uvs[0].value().y == 0.0f);
        assert(second.uvs[1].value().x == 1.0f);
        assert(second.uvs[1].value().y == 1.0f);
        assert(second.uvs[2].value().x == 0.0f);
        assert(second.uvs[2].value().y == 1.0f);
    }

    // UVs and normals have independent indices.
    // A shared position can have different UVs on different faces.
    {
        std::istringstream input{
            positions + uvs +
            "vn 0 0 1\n"
            "f 1/2/1 2/3/1 3/4/1\n"
            "f 1/1/1 3/3/1 4/4/1\n"
        };

        const Mesh mesh = parseObj(input);

        assert(mesh.triangles[0].first == mesh.triangles[1].first);
        assert(mesh.triangles[0].uvs[0].value().x == 1.0f);
        assert(mesh.triangles[1].uvs[0].value().x == 0.0f);
        assert(mesh.triangles[0].normals[0].value().z == 1.0f);
    }

    // Faces without UVs remain valid, with or without normals.
    {
        std::istringstream input{
            positions +
            "vn 0 0 1\n"
            "f 1 2 3\n"
            "f 1//1 3//1 4//1\n"
        };

        const Mesh mesh = parseObj(input);

        for (const Triangle& triangle : mesh.triangles) {
            for (std::size_t corner = 0; corner < 3; ++corner) {
                assert(!triangle.uvs[corner].has_value());
            }
        }
    }

    // Preserve UVs outside 0–1 for repeating textures.
    {
        std::istringstream input{
            positions +
            "vt -2 3\n"
            "f 1/1 2/1 3/1\n"
        };

        const Mesh mesh = parseObj(input);
        assert(mesh.triangles[0].uvs[0].value().x == -2.0f);
        assert(mesh.triangles[0].uvs[0].value().y == 3.0f);
    }

    expectUvError(positions + uvs + "f 1/0 2/1 3/1\n");
    expectUvError(positions + uvs + "f 1/5 2/1 3/1\n");
    expectUvError(positions + uvs + "f 1/-5 2/1 3/1\n");
    expectUvError(positions + uvs + "f 1/abc 2/1 3/1\n");
    expectUvError(positions + "f 1/1 2/1 3/1\n");
    expectUvError("vt 0\n");
    expectUvError("vt 0 1 extra\n");

    // Check continuity along each cube face's shared diagonal.
    {
        const Mesh cube = Mesh::cube();

        for (std::size_t index = 0;
             index < cube.triangles.size();
             index += 2) {
            const Triangle& first = cube.triangles[index];
            const Triangle& second = cube.triangles[index + 1];

            assert(first.first == second.first);
            assert(first.third == second.second);

            assert(first.uvs[0].value().x == second.uvs[0].value().x);
            assert(first.uvs[0].value().y == second.uvs[0].value().y);
            assert(first.uvs[2].value().x == second.uvs[1].value().x);
            assert(first.uvs[2].value().y == second.uvs[1].value().y);
        }
    }
}