#pragma once

#include "scene/mesh.h"

/*
* Give every corner that has no normal a smooth one: the average of the faces around its position whose normals are within
* creaseAngle (radians) of the corner's own face. Faces past the crease angle stay sharp, so hard edges keep their shape.
* Normals already in the mesh are kept, and triangles with no area are left without normals.
*/
void generateSmoothNormals(Mesh& mesh, float creaseAngle);
