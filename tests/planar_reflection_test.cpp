#include "render/gpu/camera_relative.h"
#include "render/gpu/planar_reflection.h"
#include "scene/camera.h"
#include "scene/flat_faces.h"
#include "scene/mesh.h"

#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <optional>
#include <vector>

namespace {
    bool nearVec(const Vec3& a, const Vec3& b, float tolerance) {
        return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
    }

    Vec4 clipOf(const Mat4& matrix, const Vec3& point) {
        return matrix * Vec4{point.x, point.y, point.z, 1.0f};
    }

    Camera makeCamera() {
        return Camera{Vec3{0.0f, 1.6f, 0.0f}, Vec3{0.0f, 1.6f, -5.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 16.0f / 9.0f, 0.1f, 500.0f};
    }

    // A box is six flat faces, a sphere none, a sheet one, and a face's normal points out of the mesh.
    void testFlatFaces() {
        const std::vector<FlatFace> box = findFlatFaces(Mesh::cube());
        assert(box.size() == 6);

        for (const FlatFace& face : box) {
            const Vec3 centre = face.bounds.centre();
            assert(centre.dot(face.normal) > 0.0f);
            const Vec3 size = face.bounds.size();
            assert(near(size.x * face.normal.x, 0.0f, 1e-5f) && near(size.y * face.normal.y, 0.0f, 1e-5f) && near(size.z * face.normal.z, 0.0f, 1e-5f));
        }

        assert(findFlatFaces(Mesh::sphere()).empty());
        const std::vector<FlatFace> sheet = findFlatFaces(Mesh::grid(4, 4, 1.0f, 1.0f));
        assert(sheet.size() == 1 && nearVec(sheet[0].normal, Vec3{0.0f, 0.0f, 1.0f}, 1e-5f));
    }

    // A point goes to the far side of the plane, as far from it as it was; twice comes back; the matrix agrees.
    void testReflection() {
        const MirrorPlane plane{Vec3{0.0f, 0.0f, 1.0f}, -2.0f};
        assert(nearVec(reflectAcrossPlane(Vec3{1.0f, 2.0f, 0.0f}, plane), Vec3{1.0f, 2.0f, -4.0f}, 1e-6f));
        const Vec3 tilted = Vec3{1.0f, 2.0f, 2.0f}.normalized();
        const MirrorPlane slanted{tilted, 0.7f};
        const Vec3 point{0.3f, -1.0f, 4.0f};
        const Vec3 mirrored = reflectAcrossPlane(point, slanted);
        assert(nearVec(reflectAcrossPlane(mirrored, slanted), point, 1e-5f));
        assert(nearVec(transformPoint(planeReflection(slanted), point), mirrored, 1e-5f));
        const MirrorPlane fromCamera = planeRelativeTo(plane, Vec3{0.0f, 0.0f, 3.0f});
        assert(near(fromCamera.offset, -5.0f, 1e-6f));
    }

    /*
    * The camera 5 m from a wall mirror, measured from the camera as the capture is. What stands in front of the mirror
    * lands on the pixel where the eye sees it in the mirror, so the reflection pass reads the image at the mirror's own
    * pixel. What is behind the mirror is cut away, nearer means nearer, and everything kept is inside the depth range.
    */
    void testPlanarView() {
        const Camera camera = makeCamera();
        const Mat4 viewProjection = cameraRelativeViewProjection(camera);
        const MirrorPlane plane{Vec3{0.0f, 0.0f, 1.0f}, -5.0f};
        const Mat4 mirrored = planarViewProjection(viewProjection, plane);

        const Vec3 inRoom{0.5f, -0.3f, -2.0f};
        const Vec4 seen = clipOf(viewProjection, reflectAcrossPlane(inRoom, plane));
        const Vec4 drawn = clipOf(mirrored, inRoom);
        assert(drawn.w > 0.0f);
        assert(near(drawn.x / drawn.w, seen.x / seen.w, 1e-4f) && near(drawn.y / drawn.w, seen.y / seen.w, 1e-4f));

        // Depth: 0 <= z <= w for what is kept; past the mirror z > w, which the GPU clips.
        assert(drawn.z >= 0.0f && drawn.z <= drawn.w);
        const Vec4 behind = clipOf(mirrored, Vec3{0.5f, -0.3f, -6.0f});
        assert(behind.z > behind.w);

        // Reversed-Z, as everywhere: further along the mirrored ray (nearer the camera's side) has the smaller depth.
        const Vec4 nearMirror = clipOf(mirrored, Vec3{0.2f, 0.0f, -4.5f});
        const Vec4 farFromMirror = clipOf(mirrored, Vec3{0.2f, 0.0f, 20.0f});
        assert(nearMirror.z / nearMirror.w > farFromMirror.z / farFromMirror.w);
        assert(farFromMirror.z >= 0.0f);

        // On the mirror itself (just off it, past the lift) it is the nearest thing there is.
        const Vec4 onMirror = clipOf(mirrored, Vec3{0.2f, 0.0f, -5.0f + 2.0f * static_cast<float>(PLANAR_CLIP_LIFT)});
        assert(onMirror.z / onMirror.w > 0.95f && onMirror.z <= onMirror.w);
    }

    DrawItem glossyPanel(const Mat4& model, const std::vector<FlatFace>& faces, float roughness = 0.0f, float metallic = 1.0f) {
        DrawItem draw;
        draw.model = model;
        draw.material.metallic = metallic;
        draw.material.roughness = roughness;
        draw.flatFaces = faces;
        return draw;
    }

    // A thin box's big face towards the camera wins; matte, dull and glass surfaces, and faces turned away, never do.
    void testChoosingAMirror() {
        const std::vector<FlatFace> box = findFlatFaces(Mesh::cube());
        const Vec3 camera{0.0f, 0.0f, 0.0f};

        // A panel 4 m wide, 2 high and 2 cm thick at z = -5.
        const Mat4 panel = Mat4::translation(0.0f, 0.0f, -5.0f) * Mat4::scaling(2.0f, 1.0f, 0.01f);
        std::vector<DrawItem> draws{glossyPanel(panel, box)};
        std::optional<MirrorPlane> chosen = choosePlanarMirror(draws, {0}, camera, 0.4f, std::nullopt);
        assert(chosen.has_value());
        assert(nearVec(chosen->normal, Vec3{0.0f, 0.0f, 1.0f}, 1e-5f));
        assert(near(chosen->offset, -5.0f + 0.01f * 1.0f, 0.02f));

        assert(!choosePlanarMirror(draws, {}, camera, 0.4f, std::nullopt));
        assert(!choosePlanarMirror({glossyPanel(panel, box, 0.5f)}, {0}, camera, 0.4f, std::nullopt));
        assert(!choosePlanarMirror({glossyPanel(panel, box, 0.0f, 0.0f)}, {0}, camera, 0.4f, std::nullopt));
        assert(!choosePlanarMirror({glossyPanel(panel, {})}, {0}, camera, 0.4f, std::nullopt));

        DrawItem glass = glossyPanel(panel, box);
        glass.material.transmission = 1.0f;
        assert(!choosePlanarMirror({glass}, {0}, camera, 0.4f, std::nullopt));

        // The nearer of two wins; once chosen, one a little better does not steal it, or mirrors would pop as you walk.
        const Mat4 nearer = Mat4::translation(1.0f, 0.0f, -3.0f) * Mat4::scaling(2.0f, 1.0f, 0.01f);
        std::vector<DrawItem> two{glossyPanel(panel, box), glossyPanel(nearer, box)};
        const std::optional<MirrorPlane> best = choosePlanarMirror(two, {0, 1}, camera, 0.4f, std::nullopt);
        assert(best && near(best->offset, -3.0f + 0.01f * 1.0f, 0.02f));

        const Mat4 slightlyNearer = Mat4::translation(3.0f, 0.0f, -4.9f) * Mat4::scaling(2.0f, 1.0f, 0.01f);
        std::vector<DrawItem> close{glossyPanel(panel, box), glossyPanel(slightlyNearer, box)};
        const std::optional<MirrorPlane> kept = choosePlanarMirror(close, {0, 1}, camera, 0.4f, chosen);
        assert(kept && near(kept->offset, chosen->offset, 1e-4f));
    }

    // On the plane and facing along it; off it, or tilted away, is not the mirror.
    void testOnTheMirror() {
        const Vec3 normal{0.0f, 0.0f, 1.0f};
        assert(isOnPlanarMirror(Vec3{1.0f, 0.0f, -5.0f}, normal, normal, -5.0f) == 1.0f);
        assert(isOnPlanarMirror(Vec3{1.0f, 0.0f, -4.5f}, normal, normal, -5.0f) == 0.0f);
        assert(isOnPlanarMirror(Vec3{1.0f, 0.0f, -5.0f}, Vec3{1.0f, 0.0f, 0.0f}, normal, -5.0f) == 0.0f);

        // The depth's rounding grows with distance, and so does the tolerance.
        assert(isOnPlanarMirror(Vec3{0.0f, 0.0f, -100.3f}, normal, normal, -100.0f) == 1.0f);
    }

    // The sky's rays in the mirror: each mirrored as a direction, the origin as a point, the rest kept.
    void testMirroredRays() {
        GpuRayDebugView view{};
        view.forward[2] = -1.0f;
        view.right[0] = 1.5f;
        view.up[1] = 0.8f;
        view.originAndView[2] = 3.0f;
        view.screen[0] = 960.0f;
        const GpuRayDebugView mirrored = mirroredRays(view, MirrorPlane{Vec3{0.0f, 0.0f, 1.0f}, -2.0f});
        assert(mirrored.forward[2] == 1.0f && mirrored.right[0] == 1.5f && mirrored.up[1] == 0.8f);
        assert(near(mirrored.originAndView[2], -7.0f, 1e-5f) && mirrored.screen[0] == 960.0f);
    }

    void testRenderSize() {
        assert((planarRenderSize(RenderSize{1920, 1080}, 0.5f) == RenderSize{960, 540}));
        assert((planarRenderSize(RenderSize{3, 1}, 0.25f) == RenderSize{1, 1}));
    }
}

void testPlanarReflection() {
    testFlatFaces();
    testReflection();
    testPlanarView();
    testChoosingAMirror();
    testOnTheMirror();
    testMirroredRays();
    testRenderSize();
}
