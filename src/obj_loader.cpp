#include "obj_loader.h"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

namespace {
    std::size_t parseVertexIndex(const std::string& entry, std::size_t vertexCount){
        // In "3/2/1", only "3" identifies the vertex position.
        const std::size_t slashPosition = entry.find('/');
        const std::string indexText = entry.substr(0, slashPosition);

        if (indexText.empty()) {
            throw std::runtime_error("OBJ face is missing a vertex index");
        }

        std::size_t charactersRead = 0;
        long long objIndex;

        try {
            objIndex = std::stoll(indexText, &charactersRead);
        }
        catch (const std::invalid_argument&) {
            throw std::runtime_error("OBJ vertex index is not an integer");
        }
        catch (const std::out_of_range&) {
            throw std::runtime_error("OBJ vertex index is too large");
        }

        // Reject entries such as "3abc".
        if (charactersRead != indexText.size()) {
            throw std::runtime_error("OBJ vertex index contains invalid characters");
        }

        if (objIndex == 0) {
            throw std::runtime_error("OBJ vertex indices cannot be zero");
        }

        if (objIndex > 0) {
            const unsigned long long index = static_cast<unsigned long long>(objIndex);

            if (index > vertexCount) {
                throw std::runtime_error("OBJ vertex index is outside the vertex list");
            }

            return static_cast<std::size_t>(index - 1);
        }

        // Calculate the distance backward without overflowing
        // when objIndex is the smallest possible long long.
        const unsigned long long distance = static_cast<unsigned long long>(-(objIndex + 1)) + 1ULL;

        if (distance > vertexCount) {
            throw std::runtime_error("OBJ negative index is outside the vertex list");
        }

        return vertexCount - static_cast<std::size_t>(distance);
    }
}

Mesh parseObj(std::istream& input) {
    Mesh mesh;
    std::string line;
    std::size_t lineNumber = 0;

    while (std::getline(input, line)) {
        ++lineNumber;

        // Remove comments, including comments after a face or vertex.
        const std::size_t commentPosition = line.find('#');

        if (commentPosition != std::string::npos) {
            line.erase(commentPosition);
        }

        std::istringstream lineStream{line};
        std::string type;

        // Skip empty lines.
        if (!(lineStream >> type)) {
            continue;
        }

        try {
            if (type == "v") {
                float x;
                float y;
                float z;

                if (!(lineStream >> x >> y >> z)) {
                    throw std::runtime_error(
                        "Vertex requires three coordinates"
                    );
                }

                if (!std::isfinite(x) ||
                    !std::isfinite(y) ||
                    !std::isfinite(z)) {
                    throw std::runtime_error(
                        "Vertex coordinates must be finite"
                    );
                }

                // This first version supports only v x y z.
                std::string extra;

                if (lineStream >> extra) {
                    throw std::runtime_error(
                        "Extra vertex values are not supported"
                    );
                }

                mesh.vertices.push_back(Vec4{x, y, z, 1.0f});
            }
            else if (type == "f") {
                std::vector<std::size_t> indices;
                std::string entry;

                // Read all vertex entries belonging to this face.
                while (lineStream >> entry) {
                    indices.push_back(
                        parseVertexIndex(entry, mesh.vertices.size())
                    );
                }

                if (indices.size() < 3) {
                    throw std::runtime_error(
                        "Face requires at least three vertex entries"
                    );
                }

                // Create a triangle fan around the first vertex.
                for (std::size_t index = 1;
                    index + 1 < indices.size();
                    ++index) {

                    mesh.triangles.push_back(
                        Triangle{
                            indices[0],
                            indices[index],
                            indices[index + 1]
                        }
                    );
                }

                // Add the original polygon's boundary edges.
                for (std::size_t index = 0; index < indices.size(); ++index) {
                    const std::size_t next = (index + 1) % indices.size();

                    mesh.edges.push_back(
                        Edge{indices[index], indices[next]}
                    );
                }
            }

            // Other records, such as normals and materials,
            // are ignored by this first version.
        }
        catch (const std::runtime_error& error) {
            throw std::runtime_error(
                "OBJ line " + std::to_string(lineNumber) +
                ": " + error.what()
            );
        }
    }

    if (input.bad() || (input.fail() && !input.eof())) {
        throw std::runtime_error("Failed while reading OBJ data");
    }

    return mesh;
}

Mesh loadObj(const std::string& filename) {
    std::ifstream file{filename};

    if (!file.is_open()) {
        throw std::runtime_error(
            "Could not open OBJ file: " + filename
        );
    }

    return parseObj(file);
}