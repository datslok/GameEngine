#pragma once

#include "mesh.h"

#include <istream>
#include <string>

// Read OBJ data from a stream, such as a file or test string.
Mesh parseObj(std::istream& input);

// Open an OBJ file and return its mesh.
Mesh loadObj(const std::string& filename);