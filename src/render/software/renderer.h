#pragma once

#include "scene/camera.h"
#include "render/software/depthbuffer.h"
#include "math/mat4.h"
#include "scene/mesh.h"
#include "render/software/pixelbuffer.h"

/*
* Handles the rendering pipeline by transforming and drawing mesh geometry
* into the pixel buffer while using depth testing to determine visible surfaces.
*/
class Renderer {
public:
    explicit Renderer(PixelBuffer& buffer);

    void clear(Pixel colour);

    void drawMesh(
        const Mesh& mesh,
        const Mat4& model,
        const Camera& camera,
        Pixel colour,
        bool wireframe = true
    );

private:
    PixelBuffer& buffer;
    DepthBuffer depthBuffer;
};