#include "obj_loader.h"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>
#include <optional>

namespace {
    struct FaceCorner {
        std::size_t vertex;
        std::optional<Vec3> normal;
    };

    // Resolve an OBJ index against either the vertex or normal list.
    std::size_t parseIndex(
        const std::string& text,
        std::size_t count,
        const std::string& kind
    ) {
        if (text.empty()) {
            throw std::runtime_error(
                "OBJ face is missing a " + kind + " index"
            );
        }

        std::size_t charactersRead = 0;
        long long objIndex;

        try {
            objIndex = std::stoll(text, &charactersRead);
        }
        catch (const std::invalid_argument&) {
            throw std::runtime_error(
                "OBJ " + kind + " index is not an integer"
            );
        }
        catch (const std::out_of_range&) {
            throw std::runtime_error(
                "OBJ " + kind + " index is too large"
            );
        }

        if (charactersRead != text.size()) {
            throw std::runtime_error(
                "OBJ " + kind + " index contains invalid characters"
            );
        }

        if (objIndex == 0) {
            throw std::runtime_error(
                "OBJ " + kind + " indices cannot be zero"
            );
        }

        if (objIndex > 0) {
            const unsigned long long index =
                static_cast<unsigned long long>(objIndex);

            if (index > count) {
                throw std::runtime_error(
                    "OBJ index is outside the " + kind + " list"
                );
            }

            return static_cast<std::size_t>(index - 1);
        }

        // Avoid overflow even for the smallest possible long long.
        const unsigned long long distance =
            static_cast<unsigned long long>(-(objIndex + 1)) + 1ULL;

        if (distance > count) {
            throw std::runtime_error(
                "OBJ negative index is outside the " + kind + " list"
            );
        }

        return count - static_cast<std::size_t>(distance);
    }

    FaceCorner parseCorner(
        const std::string& entry,
        std::size_t vertexCount,
        const std::vector<Vec3>& normals
    ) {
        const std::size_t firstSlash = entry.find('/');

        FaceCorner corner{
            parseIndex(entry.substr(0, firstSlash), vertexCount, "vertex"),
            std::nullopt
        };

        // Position only: "3".
        if (firstSlash == std::string::npos) {
            return corner;
        }

        const std::size_t secondSlash = entry.find('/', firstSlash + 1);

        // Position and texture coordinate: "3/2".
        // Texture coordinates are still ignored.
        if (secondSlash == std::string::npos) {
            return corner;
        }

        if (entry.find('/', secondSlash + 1) != std::string::npos) {
            throw std::runtime_error("OBJ face entry has too many slashes");
        }

        // Position and normal: "3//1" or "3/2/1".
        const std::size_t normalIndex = parseIndex(
            entry.substr(secondSlash + 1),
            normals.size(),
            "normal"
        );

        corner.normal = normals[normalIndex];
        return corner;
    }
}

Mesh parseObj(std::istream& input) {
    Mesh mesh;
    std::vector<Vec3> normals;
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
            } else if (type == "vn") {
                float x;
                float y;
                float z;

                if (!(lineStream >> x >> y >> z)) {
                    throw std::runtime_error(
                        "Normal requires three coordinates"
                    );
                }

                if (!std::isfinite(x) ||
                    !std::isfinite(y) ||
                    !std::isfinite(z)) {
                    throw std::runtime_error(
                        "Normal coordinates must be finite"
                    );
                }

                std::string extra;

                if (lineStream >> extra) {
                    throw std::runtime_error(
                        "Extra normal values are not supported"
                    );
                }

                // Normalize using double intermediates to avoid
                // overflow when the input components are large.
                const double length = std::hypot(
                    static_cast<double>(x),
                    static_cast<double>(y),
                    static_cast<double>(z)
                );

                if (length == 0.0) {
                    throw std::runtime_error("Normal cannot be zero");
                }

                normals.push_back(Vec3{
                    static_cast<float>(x / length),
                    static_cast<float>(y / length),
                    static_cast<float>(z / length)
                });
            } else if (type == "f") {
                std::vector<FaceCorner> corners;
                std::string entry;

                while (lineStream >> entry) {
                    corners.push_back(
                        parseCorner(entry, mesh.vertices.size(), normals)
                    );
                }

                if (corners.size() < 3) {
                    throw std::runtime_error(
                        "Face requires at least three vertex entries"
                    );
                }

                // Triangulate a convex polygon while preserving
                // the normal belonging to each original corner.
                for (std::size_t index = 1;
                     index + 1 < corners.size();
                     ++index) {

                    Triangle triangle{
                        corners[0].vertex,
                        corners[index].vertex,
                        corners[index + 1].vertex
                    };

                    triangle.normals[0] = corners[0].normal;
                    triangle.normals[1] = corners[index].normal;
                    triangle.normals[2] = corners[index + 1].normal;

                    mesh.triangles.push_back(triangle);
                }

                // Keep only the original polygon's boundary edges.
                for (std::size_t index = 0; index < corners.size(); ++index) {
                    const std::size_t next = (index + 1) % corners.size();

                    mesh.edges.push_back(Edge{
                        corners[index].vertex,
                        corners[next].vertex
                    });
                }
            }

            // Other records, such as texture coordinates, smoothing
            // groups and materials, are currently ignored.
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