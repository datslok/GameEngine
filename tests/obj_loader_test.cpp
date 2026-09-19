#include "obj_loader.h"

#include <cassert>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
    void expectObjError(
        const std::string& source,
        const std::string& expectedMessage
    ) {
        std::istringstream input{source};
        bool threw = false;

        try {
            parseObj(input);
        }
        catch (const std::runtime_error& error) {
            threw = true;

            assert(
                std::string{error.what()}.find(expectedMessage) !=
                std::string::npos
            );
        }

        assert(threw);
    }
}

void testObjLoader() {
    const std::string vertices =
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n";

    // Read positions, triangle indices and wireframe edges.
    {
        std::istringstream input{vertices + "f 1 2 3\n"};
        const Mesh mesh = parseObj(input);

        assert(mesh.vertices.size() == 3);

        assert(mesh.vertices[0].x == 0.0f);
        assert(mesh.vertices[0].y == 0.0f);
        assert(mesh.vertices[0].z == 0.0f);

        assert(mesh.vertices[1].x == 1.0f);
        assert(mesh.vertices[1].y == 0.0f);
        assert(mesh.vertices[1].z == 0.0f);

        assert(mesh.vertices[2].x == 0.0f);
        assert(mesh.vertices[2].y == 1.0f);
        assert(mesh.vertices[2].z == 0.0f);

        for (const Vec4& vertex : mesh.vertices) {
            assert(vertex.w == 1.0f);
        }

        assert(mesh.triangles.size() == 1);
        assert(mesh.triangles[0].first == 0);
        assert(mesh.triangles[0].second == 1);
        assert(mesh.triangles[0].third == 2);

        assert(mesh.edges.size() == 3);
        assert(mesh.edges[0].start == 0);
        assert(mesh.edges[0].end == 1);
        assert(mesh.edges[1].start == 1);
        assert(mesh.edges[1].end == 2);
        assert(mesh.edges[2].start == 2);
        assert(mesh.edges[2].end == 0);
    }

    // Negative indices count backward from the current vertex list.
    // Preserve the face's original vertex order.
    {
        std::istringstream input{vertices + "f -1 -2 -3\n"};
        const Mesh mesh = parseObj(input);

        assert(mesh.triangles.size() == 1);
        assert(mesh.triangles[0].first == 2);
        assert(mesh.triangles[0].second == 1);
        assert(mesh.triangles[0].third == 0);
    }

    // Accept the supported face-entry formats.
    {
        const std::string source =
            vertices +
            "vt 0 0\n"
            "vn 0 0 1\n"
            "f 1/1 2/1 3/1\n"
            "f 1//1 2//1 3//1\n"
            "f 1/1/1 2/1/1 3/1/1\n";

        std::istringstream input{source};
        const Mesh mesh = parseObj(input);

        assert(mesh.triangles.size() == 3);

        for (const Triangle& triangle : mesh.triangles) {
            assert(triangle.first == 0);
            assert(triangle.second == 1);
            assert(triangle.third == 2);
        }
    }

    // Handle whitespace, comments and a final line without a newline.
    {
        std::istringstream input{
            "# Example triangle\n"
            "\n"
            "  v 0 0 0 # First vertex\n"
            "\tv 1 0 0\n"
            "v 0 1 0\n"
            "f 1 2 3 # Triangle"
        };

        const Mesh mesh = parseObj(input);

        assert(mesh.vertices.size() == 3);
        assert(mesh.triangles.size() == 1);
    }

    // Empty input produces an empty mesh.
    {
        std::istringstream input{""};
        const Mesh mesh = parseObj(input);

        assert(mesh.vertices.empty());
        assert(mesh.triangles.empty());
        assert(mesh.edges.empty());
    }

    // Invalid vertex references.
    expectObjError(
        vertices + "f 0 2 3\n",
        "OBJ line 4: OBJ vertex indices cannot be zero"
    );

    expectObjError(
        vertices + "f 1 2 4\n",
        "outside the vertex list"
    );

    expectObjError(
        vertices + "f -4 -2 -1\n",
        "outside the vertex list"
    );

    expectObjError(
        vertices + "f 1abc 2 3\n",
        "invalid characters"
    );

    expectObjError(
        vertices + "f /1/1 2 3\n",
        "missing a vertex index"
    );

    expectObjError(
        vertices + "f 999999999999999999999999 2 3\n",
        "too large"
    );

    // Missing coordinates and unsupported extra vertex values.
    expectObjError("v 1 2\n", "three coordinates");
    expectObjError("v 1 2 3 1\n", "Extra vertex values");

    // Only triangle faces are supported.
    expectObjError(vertices + "f 1 2\n", "three vertex entries");

    // A quad becomes two triangles and four boundary edges.
    {
        std::istringstream input{
            "v 0 0 0\n"
            "v 2 0 0\n"
            "v 2 2 0\n"
            "v 0 2 0\n"
            "f 1 2 3 4\n"
        };

        const Mesh mesh = parseObj(input);

        assert(mesh.triangles.size() == 2);

        assert(mesh.triangles[0].first == 0);
        assert(mesh.triangles[0].second == 1);
        assert(mesh.triangles[0].third == 2);

        assert(mesh.triangles[1].first == 0);
        assert(mesh.triangles[1].second == 2);
        assert(mesh.triangles[1].third == 3);

        assert(mesh.edges.size() == 4);

        for (std::size_t index = 0; index < 4; ++index) {
            assert(mesh.edges[index].start == index);
            assert(mesh.edges[index].end == (index + 1) % 4);
        }
    }

    // A face without normals keeps the flat-shading fallback.
    {
        std::istringstream input{vertices + "f 1 2 3\n"};
        const Mesh mesh = parseObj(input);

        assert(!mesh.triangles[0].normals[0].has_value());
        assert(!mesh.triangles[0].normals[1].has_value());
        assert(!mesh.triangles[0].normals[2].has_value());
    }

    // Normal indices are independent of position indices.
    // Negative indices work, and non-unit normals are normalized.
    {
        std::istringstream input{
            vertices +
            "vn 2 0 0\n"
            "vn 0 3 0\n"
            "vn 0 0 4\n"
            "f 1//-1 2//-3 3//-2\n"
        };

        const Mesh mesh = parseObj(input);
        const Triangle& triangle = mesh.triangles[0];

        assert(triangle.normals[0].value().z == 1.0f);
        assert(triangle.normals[1].value().x == 1.0f);
        assert(triangle.normals[2].value().y == 1.0f);
    }

    // A shared position may use different normals on different faces.
    {
        std::istringstream input{
            vertices +
            "vn 0 0 1\n"
            "vn 0 1 0\n"
            "f 1//1 2//1 3//1\n"
            "f 1//2 2//2 3//2\n"
        };

        const Mesh mesh = parseObj(input);

        assert(mesh.triangles[0].first == mesh.triangles[1].first);
        assert(mesh.triangles[0].normals[0].value().z == 1.0f);
        assert(mesh.triangles[1].normals[0].value().y == 1.0f);
    }

    // Quad triangulation preserves the original corner normals.
    // Also exercise the position/texture/normal form.
    {
        std::istringstream input{
            "v 0 0 0\n"
            "v 1 0 0\n"
            "v 1 1 0\n"
            "v 0 1 0\n"
            "vt 0 0\n"
            "vn 1 0 0\n"
            "vn 0 1 0\n"
            "vn 0 0 1\n"
            "vn -1 0 0\n"
            "f 1/1/1 2/1/2 3/1/3 4/1/4\n"
        };

        const Mesh mesh = parseObj(input);

        assert(mesh.triangles.size() == 2);

        const Triangle& firstTriangle = mesh.triangles[0];
        const Triangle& secondTriangle = mesh.triangles[1];

        assert(firstTriangle.normals[0].value().x == 1.0f);
        assert(firstTriangle.normals[1].value().y == 1.0f);
        assert(firstTriangle.normals[2].value().z == 1.0f);

        assert(secondTriangle.normals[0].value().x == 1.0f);
        assert(secondTriangle.normals[1].value().z == 1.0f);
        assert(secondTriangle.normals[2].value().x == -1.0f);
    }

    // Invalid normal records and references.
    expectObjError("vn 0 1\n", "three coordinates");
    expectObjError("vn 0 0 0\n", "Normal cannot be zero");

    expectObjError(
        vertices + "vn 0 0 1\nf 1//0 2//1 3//1\n",
        "normal indices cannot be zero"
    );

    expectObjError(
        vertices + "vn 0 0 1\nf 1//2 2//1 3//1\n",
        "outside the normal list"
    );

    expectObjError(
        vertices + "vn 0 0 1\nf 1//-2 2//1 3//1\n",
        "outside the normal list"
    );

    expectObjError(
        vertices + "vn 0 0 1\nf 1//abc 2//1 3//1\n",
        "normal index is not an integer"
    );

    expectObjError(
        vertices + "f 1// 2 3\n",
        "missing a normal index"
    );
}