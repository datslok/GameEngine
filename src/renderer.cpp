#include "renderer.h"

#include "clipper.h"
#include "rasterizer.h"
#include "vec3.h"
#include "vec4.h"

#include <cmath>
#include <cstddef>
#include <vector>

namespace{
    struct ScreenPoint{
        int x;
        int y;
        float depth;
    };

    ScreenPoint projectToScreen(
        const Vec4& point,
        std::size_t width,
        std::size_t height
    ){
        const float ndcX = point.x / point.w;
        const float ndcY = point.y / point.w;
        const float ndcZ = point.z / point.w;

        return ScreenPoint{
            static_cast<int>(std::lround(
                (ndcX + 1.0f) * 0.5f *
                static_cast<float>(width - 1)
            )),
            static_cast<int>(std::lround(
                (1.0f - ndcY) * 0.5f *
                static_cast<float>(height - 1)
            )),
            (ndcZ + 1.0f) * 0.5f
        };
    }
}

Renderer::Renderer(PixelBuffer& buffer)
    : buffer(buffer),
      depthBuffer(buffer.getWidth(), buffer.getHeight()){
}

void Renderer::clear(Pixel colour){
    buffer.clear(colour);
    depthBuffer.clear();
}

void Renderer::drawMesh(
    const Mesh& mesh,
    const Mat4& model,
    const Camera& camera,
    Pixel colour,
    bool wireframe
){
    if (buffer.getWidth() == 0 || buffer.getHeight() == 0){
        return;
    }

    const Mat4 modelView = camera.getViewMatrix() * model;
    const Mat4 projection = camera.getProjectionMatrix();

    std::vector<Vec4> viewVertices(mesh.vertices.size());
    std::vector<Vec4> clipVertices(mesh.vertices.size());

    for (std::size_t index = 0; index < mesh.vertices.size(); ++index){
        viewVertices[index] = modelView * mesh.vertices[index];
        clipVertices[index] = projection * viewVertices[index];
    }

    for (const Triangle& triangle : mesh.triangles){
        const Vec4& viewFirst = viewVertices[triangle.first];
        const Vec4& viewSecond = viewVertices[triangle.second];
        const Vec4& viewThird = viewVertices[triangle.third];

        const Vec3 edgeFirst{
            viewSecond.x - viewFirst.x,
            viewSecond.y - viewFirst.y,
            viewSecond.z - viewFirst.z
        };

        const Vec3 edgeSecond{
            viewThird.x - viewFirst.x,
            viewThird.y - viewFirst.y,
            viewThird.z - viewFirst.z
        };

        const Vec3 normal = edgeFirst.cross(edgeSecond);

        const Vec3 toCamera{
            -viewFirst.x,
            -viewFirst.y,
            -viewFirst.z
        };

        if (normal.dot(toCamera) <= 0.0f){
            continue;
        }

        const std::vector<Vec4> polygon = clipTriangle(
            clipVertices[triangle.first],
            clipVertices[triangle.second],
            clipVertices[triangle.third]
        );

        for (std::size_t index = 1; index + 1 < polygon.size(); ++index){
            const Vec4& first = polygon[0];
            const Vec4& second = polygon[index];
            const Vec4& third = polygon[index + 1];

            if (first.w <= 0.0f || second.w <= 0.0f || third.w <= 0.0f){
                continue;
            }

            const ScreenPoint screenFirst = projectToScreen(
                first, buffer.getWidth(), buffer.getHeight()
            );

            const ScreenPoint screenSecond = projectToScreen(
                second, buffer.getWidth(), buffer.getHeight()
            );

            const ScreenPoint screenThird = projectToScreen(
                third, buffer.getWidth(), buffer.getHeight()
            );

            fillTriangleDepth(
                buffer,
                depthBuffer,
                screenFirst.x, screenFirst.y, screenFirst.depth,
                screenSecond.x, screenSecond.y, screenSecond.depth,
                screenThird.x, screenThird.y, screenThird.depth,
                colour
            );
        }
    }

    // Optional debug overlay: edges are not depth-tested.
    if (wireframe){
        for (const Edge& edge : mesh.edges){
            Vec4 start = clipVertices[edge.start];
            Vec4 end = clipVertices[edge.end];

            if (!clipLine(start, end)){
                continue;
            }

            if (start.w <= 0.0f || end.w <= 0.0f){
                continue;
            }

            const ScreenPoint screenStart = projectToScreen(
                start, buffer.getWidth(), buffer.getHeight()
            );

            const ScreenPoint screenEnd = projectToScreen(
                end, buffer.getWidth(), buffer.getHeight()
            );

            drawLine(
                buffer,
                screenStart.x, screenStart.y,
                screenEnd.x, screenEnd.y,
                Pixel{255, 255, 255}
            );
        }
    }
}