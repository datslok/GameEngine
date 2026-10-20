#include "assets/star_catalogue.h"
#include "math/angles.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {
    // The comma-separated fields of a line, empty ones kept.
    std::vector<std::string> fields(const std::string& line) {
        std::vector<std::string> parts;
        std::stringstream stream{line};
        std::string part;

        while (std::getline(stream, part, ',')) {
            parts.push_back(part);
        }

        // A trailing comma leaves an empty last field that getline does not return.
        if (!line.empty() && line.back() == ',') {
            parts.emplace_back();
        }

        return parts;
    }
}

std::vector<CatalogueStar> parseStarCatalogue(std::istream& input) {
    std::vector<CatalogueStar> stars;
    std::string line;
    int lineNumber = 0;

    while (std::getline(input, line)) {
        ++lineNumber;

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty() || line.front() == '#') {
            continue;
        }

        const std::vector<std::string> parts = fields(line);

        try {
            if (parts.size() != 5) {
                throw std::invalid_argument("expected 5 fields");
            }

            CatalogueStar star;
            star.rightAscension = radiansFromDegrees(std::stof(parts[1]));
            star.declination = radiansFromDegrees(std::stof(parts[2]));
            star.magnitude = std::stof(parts[3]);
            star.colourIndex = parts[4].empty() ? unknownColourIndex : std::stof(parts[4]);
            stars.push_back(star);
        } catch (const std::exception& error) {
            throw std::runtime_error("Star catalogue line " + std::to_string(lineNumber) + " unreadable (" + error.what() + "): " + line);
        }
    }

    return stars;
}

std::vector<CatalogueStar> loadStarCatalogue(const std::string& path) {
    std::ifstream file{path};

    if (!file) {
        throw std::runtime_error("Cannot open star catalogue: " + path);
    }

    return parseStarCatalogue(file);
}
