#pragma once

#include "camera.h"
#include "depthbuffer.h"
#include "mat4.h"
#include "mesh.h"
#include "pixelbuffer.h"

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