#include "core/srgb.h"
#include "render/gpu/brdf.h"
#include "render/gpu/light_falloff.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/mirrored_light.h"
#include "render/gpu/planar_reflection.h"
#include "render/gpu/shadow_cache.h"
#include "render/gpu/shadow_map.h"
#include "render/gpu/gpu_scene.h"
#include "scene/camera.h"
#include "scene/flat_faces.h"

#include <cassert>
#include <cmath>
#include <optional>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance = 1e-4f) {
        return std::abs(a - b) <= tolerance;
    }

    bool near(const Vec3& a, const Vec3& b, float tolerance = 1e-4f) {
        return (a - b).length() <= tolerance;
    }

    // The unit cube's faces, as the engine finds them at load.
    const std::vector<FlatFace>& cubeFaces() {
        static const std::vector<FlatFace> faces = findFlatFaces(Mesh::cube());
        return faces;
    }

    // The cube's face looking along +z.
    const FlatFace& frontFace() {
        for (const FlatFace& face : cubeFaces()) {
            if (face.normal.z > 0.99f) {
                return face;
            }
        }
        assert(false);
        return cubeFaces().front();
    }

    // A mirror 2 m wide, 1.5 m high and 2 cm thick, its front face at z = -5, centred 1.5 m up: a thin cube scaled so
    // its +z face lands there.
    Mat4 mirrorModel() {
        const FlatFace& face = frontFace();
        const Vec3 size = face.bounds.size();
        const float sx = 2.0f / size.x;
        const float sy = 1.5f / size.y;
        const float sz = 0.02f / (2.0f * face.bounds.centre().z); // the cube is centred on its origin
        const Mat4 scale = Mat4::scaling(sx, sy, sz);
        const Vec3 faceCentre = transformPoint(scale, face.bounds.centre());
        return Mat4::translation(-faceCentre.x, 1.5f - faceCentre.y, -5.0f - faceCentre.z) * scale;
    }

    DrawItem panel(const Mat4& model, float metallic, float roughness) {
        DrawItem draw;
        draw.model = model;
        draw.material.metallic = metallic;
        draw.material.roughness = roughness;
        draw.material.colour = Pixel{230, 230, 230};
        draw.flatFaces = cubeFaces();
        draw.bounds = BoundingSphere{transformPoint(model, Vec3{}), 1.3f};
        return draw;
    }

    DrawItem mirrorDraw() {
        return panel(mirrorModel(), 1.0f, 0.0f);
    }

    // The window of the mirror's front face.
    LightWindow frontWindow() {
        const std::optional<LightWindow> window = mirrorFaceWindow(frontFace(), mirrorModel());
        assert(window.has_value());
        return *window;
    }

    /*
    * A flat face of a box becomes a window: its plane (normal out of the surface, through the face), its middle and its
    * two edges' directions and half lengths, all in the world. A face not square to its mesh's axes is left out.
    */
    void testFaceWindow() {
        const LightWindow window = frontWindow();
        assert(near(window.normal, Vec3{0.0f, 0.0f, 1.0f}, 1e-5f));
        assert(near(window.offset, -5.0f, 1e-4f));
        assert(near(window.centre, Vec3{0.0f, 1.5f, -5.0f}, 1e-4f));
        assert(near(window.axisU.length(), 1.0f, 1e-5f) && near(window.axisV.length(), 1.0f, 1e-5f));
        assert(std::abs(window.axisU.dot(window.axisV)) < 1e-5f && std::abs(window.axisU.dot(window.normal)) < 1e-5f);
        const float halfX = std::abs(window.axisU.x) > 0.5f ? window.halfU : window.halfV;
        const float halfY = std::abs(window.axisU.x) > 0.5f ? window.halfV : window.halfU;
        assert(near(halfX, 1.0f) && near(halfY, 0.75f));

        FlatFace slanted = frontFace();
        slanted.normal = Vec3{0.6f, 0.0f, 0.8f};
        assert(!mirrorFaceWindow(slanted, mirrorModel()).has_value());

        // Turned with its model, the window turns too.
        const Mat4 turned = Mat4::rotationY(0.5f) * mirrorModel();
        const std::optional<LightWindow> turnedWindow = mirrorFaceWindow(frontFace(), turned);
        assert(turnedWindow.has_value() && near(turnedWindow->normal, transformDirection(Mat4::rotationY(0.5f), Vec3{0.0f, 0.0f, 1.0f}), 1e-5f));
    }

    /*
    * The image light shines through the window only: the straight line from the image to the point crosses the mirror's
    * plane inside its outline, with the point in front of the mirror; where it crosses is the point of reflection.
    */
    void testThroughWindow() {
        const LightWindow window = frontWindow();
        const Vec3 image{0.0f, 1.5f, -9.0f};
        Vec3 crossing;
        assert(throughMirrorWindow(window, image, Vec3{0.0f, 1.5f, 0.0f}, crossing));
        assert(near(crossing, Vec3{0.0f, 1.5f, -5.0f}, 1e-4f));
        assert(throughMirrorWindow(window, image, Vec3{1.0f, 1.0f, -1.0f}, crossing));
        assert(near(crossing.z, -5.0f, 1e-4f) && std::abs(crossing.x) < 1.0f);
        // Past the mirror's side edge, behind it, on it, and from an image in front of the glass.
        assert(!throughMirrorWindow(window, image, Vec3{6.0f, 1.5f, 0.0f}, crossing));
        assert(!throughMirrorWindow(window, image, Vec3{0.0f, 1.5f, -7.0f}, crossing));
        assert(!throughMirrorWindow(window, image, Vec3{0.3f, 1.2f, -5.0f}, crossing));
        assert(!throughMirrorWindow(window, Vec3{0.0f, 1.5f, -2.0f}, Vec3{0.0f, 1.5f, 0.0f}, crossing));
    }

    PlacedSpotLight flashlightAt(const Vec3& position, const Vec3& direction) {
        PlacedSpotLight placed;
        placed.position = position;
        placed.light.colour = Vec3{1.0f, 0.9f, 0.8f};
        placed.light.intensity = 40.0f;
        placed.light.range = 30.0f;
        placed.light.direction = direction;
        placed.light.innerAngle = 0.2f;
        placed.light.outerAngle = 0.35f;
        placed.light.sourceRadius = 0.05f;
        placed.light.emitterRadius = 0.02f;
        placed.light.fast = true;
        return placed;
    }

    /*
    * A spotlight's image: as far behind the glass as the light is in front, its beam reflected, its cone kept, its light
    * times the mirror's reflectance, no longer fast, and through the face's window with its source's slot. Its light at
    * a receiver is exactly the ray-traced search's flat mirror answer (mirroredIrradiance with the spread area the path's
    * length squared): the image is the exact answer for a flat mirror.
    */
    void testSpotImage() {
        const PlacedSpotLight real = flashlightAt(Vec3{0.3f, 1.6f, -1.0f}, Vec3{-0.05f, -0.1f, -1.0f});
        const DrawItem mirror = mirrorDraw();
        const LightWindow window = frontWindow();
        const PlacedSpotLight image = imageOfSpotLight(real, 7, window, mirror.material, 0.25f);
        assert(near(image.position, Vec3{0.3f, 1.6f, -9.0f}, 1e-4f));
        const Vec3 beam = real.light.direction.normalized();
        assert(near(image.light.direction.normalized(), Vec3{beam.x, beam.y, -beam.z}, 1e-5f));
        assert(image.light.innerAngle == real.light.innerAngle && image.light.outerAngle == real.light.outerAngle);
        assert(image.light.range == real.light.range && image.light.sourceRadius == real.light.sourceRadius);
        assert(!image.light.fast && image.window.has_value());
        assert(image.window->sourceIsSpot && image.window->sourceSlot == 7);

        const Vec3 reflectance = mirrorReflectance(mirror.material);
        const float expected = srgbToLinear(230.0f / 255.0f);
        assert(near(reflectance.x, expected, 1e-5f) && near(reflectance.z, expected, 1e-5f));
        assert(near(image.light.colour, Vec3{real.light.colour.x * reflectance.x, real.light.colour.y * reflectance.y, real.light.colour.z * reflectance.z}, 1e-6f));

        // A receiver on the reflected beam's axis: the image's falloff over the unfolded path is the search's answer.
        const Vec3 receiver = image.position + image.light.direction.normalized() * 6.0f;
        Vec3 crossing;
        assert(throughMirrorWindow(*image.window, image.position, receiver, crossing));
        const float toMirror = (crossing - real.position).length();
        const float fromMirror = (receiver - crossing).length();
        assert(near(toMirror + fromMirror, (receiver - image.position).length(), 1e-4f));
        const Vec3 viaImage = image.light.colour * (image.light.intensity * distanceFalloff((receiver - image.position).length(), image.light.range, image.light.sourceRadius));
        const float path = toMirror + fromMirror;
        const Vec3 viaSearch = mirroredIrradiance(real.light.colour * real.light.intensity, reflectance, path * path, path, real.light.range, real.light.sourceRadius);
        assert(near(viaImage, viaSearch, 1e-4f * viaSearch.length()));

        // A rough mirror widens the image's glowing size, so its shadows soften; a sharp one keeps the light's own.
        const PlacedSpotLight sharp = imageOfSpotLight(real, 7, window, mirror.material, 0.25f);
        DrawItem brushed = mirror;
        brushed.material.roughness = 0.2f;
        const PlacedSpotLight rough = imageOfSpotLight(real, 7, window, brushed.material, 0.25f);
        assert(near(sharp.light.emitterRadius, real.light.emitterRadius, 1e-6f));
        assert(rough.light.emitterRadius > sharp.light.emitterRadius);
        // Fading out as the roughness nears the limit, as the ray-traced search does.
        assert(rough.light.intensity <= real.light.intensity);
    }

    /*
    * A point light's image is a spotlight aimed at the window's middle whose inner cone just takes in the window's four
    * corners, so the cone never dims what passes the window (the window cuts the edge exactly).
    */
    void testPointImage() {
        PlacedPointLight real;
        real.position = Vec3{-0.4f, 2.0f, -2.0f};
        real.light.colour = Vec3{1.0f, 1.0f, 1.0f};
        real.light.intensity = 10.0f;
        real.light.range = 12.0f;
        const LightWindow window = frontWindow();
        const PlacedSpotLight image = imageOfPointLight(real, 3, window, mirrorDraw().material, 0.25f);
        assert(near(image.position, Vec3{-0.4f, 2.0f, -8.0f}, 1e-4f));
        assert(!image.window->sourceIsSpot && image.window->sourceSlot == 3);
        assert(image.light.outerAngle > image.light.innerAngle);
        const float cosInner = std::cos(image.light.innerAngle);
        const float cosOuter = std::cos(image.light.outerAngle);
        const Vec3 axis = image.light.direction.normalized();

        for (const float u : {-1.0f, 1.0f}) {
            for (const float v : {-1.0f, 1.0f}) {
                const Vec3 corner = window.centre + window.axisU * (window.halfU * u) + window.axisV * (window.halfV * v);
                const float along = (corner - image.position).normalized().dot(axis);
                assert(along >= cosInner - 1e-6f);
                assert(spotCone(along, cosOuter, cosInner) == 1.0f);
            }
        }
    }

    /*
    * Choosing: each light in front of a smooth metal face, within reach, becomes an image, brightest at the mirror first,
    * at most MIRROR_IMAGE_LIGHTS; matte, rough and glass surfaces and lights behind the mirror give none; the images
    * come after the frame's own spotlights.
    */
    void testChoosing() {
        FrameLighting lighting;
        lighting.spotLights.push_back(flashlightAt(Vec3{0.0f, 1.6f, 0.0f}, Vec3{0.0f, -0.05f, -1.0f}));
        PlacedPointLight behind;
        behind.position = Vec3{0.0f, 1.5f, -8.0f};
        behind.light.range = 20.0f;
        lighting.pointLights.push_back(behind);
        PlacedPointLight farAway;
        farAway.position = Vec3{0.0f, 1.5f, 40.0f};
        farAway.light.range = 10.0f;
        lighting.pointLights.push_back(farAway);

        MirroredLightSettings settings;
        const std::vector<DrawItem> draws{mirrorDraw(), panel(Mat4::translation(6.0f, 0.0f, 0.0f) * mirrorModel(), 0.0f, 0.0f),
                                          panel(Mat4::translation(-6.0f, 0.0f, 0.0f) * mirrorModel(), 1.0f, 0.9f)};
        const std::vector<PlacedSpotLight> images = chooseImageLights(lighting, draws, settings);
        // The flashlight through the front face; the lamp behind through the back face (a thin metal box is a mirror on
        // both sides); the far lamp, out of reach, through none.
        assert(images.size() == 2);
        int fromFlashlight = 0;
        for (const PlacedSpotLight& image : images) {
            const Vec3 source = image.window->sourceIsSpot ? lighting.spotLights[0].position : behind.position;
            assert(image.window->normal.dot(source) - image.window->offset > 0.0f);
            assert(image.window->sourceSlot == 0);
            if (image.window->sourceIsSpot) {
                ++fromFlashlight;
                assert(near(image.window->normal, Vec3{0.0f, 0.0f, 1.0f}, 1e-5f));
            } else {
                assert(near(image.window->normal, Vec3{0.0f, 0.0f, -1.0f}, 1e-5f));
            }
        }
        assert(fromFlashlight == 1);

        settings.imageLights = false;
        assert(chooseImageLights(lighting, draws, settings).empty());
        settings.imageLights = true;

        // Many lamps before the mirror: only the brightest few, the brightest first.
        FrameLighting many;
        for (int i = 0; i < MIRROR_IMAGE_LIGHTS + 3; ++i) {
            PlacedPointLight lamp;
            lamp.position = Vec3{0.0f, 1.5f, -4.0f + static_cast<float>(i)};
            lamp.light.range = 30.0f;
            lamp.light.intensity = 5.0f;
            many.pointLights.push_back(lamp);
        }
        const std::vector<PlacedSpotLight> kept = chooseImageLights(many, std::vector<DrawItem>{mirrorDraw()}, settings);
        assert(static_cast<int>(kept.size()) == MIRROR_IMAGE_LIGHTS);
        for (std::size_t i = 0; i < kept.size(); ++i) {
            assert(kept[i].window->sourceSlot == static_cast<int>(i));
        }
    }

    /*
    * The image's shadow tile is drawn clipped at the mirror (what stands behind the glass, the mirror itself included,
    * would shadow everything), lifted off it as the planar mirror's picture is; other tiles clip nothing; the clip plane
    * reaches the shadow pass and the cache tells tiles apart by it.
    */
    void testShadowTiles() {
        FrameLighting lighting;
        lighting.spotLights.push_back(flashlightAt(Vec3{0.0f, 1.6f, 0.0f}, Vec3{0.0f, -0.05f, -1.0f}));
        const LightWindow window = frontWindow();
        lighting.spotLights.push_back(imageOfSpotLight(lighting.spotLights[0], 0, window, mirrorDraw().material, 0.25f));
        const Camera camera{Vec3{0.0f, 1.6f, 0.0f}, Vec3{0.0f, 1.6f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.2f, 16.0f / 9.0f, 0.1f, 200.0f};
        const ShadowPlan plan = planShadows(lighting, camera);
        const ShadowTile& own = plan.tiles[static_cast<std::size_t>(plan.spots[0].firstTile)];
        const ShadowTile& mirrored = plan.tiles[static_cast<std::size_t>(plan.spots[1].firstTile)];
        const Vec4 none = noClipPlane();
        assert(own.clipPlane.x == none.x && own.clipPlane.y == none.y && own.clipPlane.z == none.z && own.clipPlane.w == none.w);
        const Vec4 expected = planarClipPlane(MirrorPlane{window.normal, window.offset});
        assert(near(mirrored.clipPlane.x, expected.x, 1e-6f) && near(mirrored.clipPlane.z, expected.z, 1e-6f) && near(mirrored.clipPlane.w, expected.w, 1e-6f));
        assert(planarClipDistance(mirrored.clipPlane, Vec3{0.0f, 1.5f, -5.0f}) < 0.0f);
        assert(planarClipDistance(mirrored.clipPlane, Vec3{0.0f, 1.5f, -4.9f}) > 0.0f);

        const GpuShadowView view = packShadowView(mirrored.matrix, mirrored.clipPlane);
        assert(view.clipPlane[2] == mirrored.clipPlane.z && view.clipPlane[3] == mirrored.clipPlane.w);

        ShadowTileMemory memory;
        memory.remember(mirrored, 42);
        assert(memory.isCurrent(mirrored, 42));
        ShadowTile moved = mirrored;
        moved.clipPlane.w += 0.5f;
        assert(!memory.isCurrent(moved, 42));
    }

    // The window reaches the shader measured from the camera, as the light's position is; a plain spotlight has none.
    void testPacking() {
        FrameLighting lighting;
        lighting.spotLights.push_back(flashlightAt(Vec3{0.0f, 1.6f, 0.0f}, Vec3{0.0f, -0.05f, -1.0f}));
        const LightWindow window = frontWindow();
        lighting.spotLights.push_back(imageOfSpotLight(lighting.spotLights[0], 0, window, mirrorDraw().material, 0.25f));
        const Vec3 camera{1.0f, 2.0f, 3.0f};
        const GpuLights lights = packLighting(lighting, camera);
        assert(lights.spots[0].windowPlane[0] == 0.0f && lights.spots[0].windowPlane[1] == 0.0f && lights.spots[0].windowPlane[2] == 0.0f);
        const GpuSpotLight& image = lights.spots[1];
        assert(near(image.windowPlane[2], 1.0f, 1e-6f));
        assert(near(image.windowPlane[3], window.offset - window.normal.dot(camera), 1e-5f));
        assert(near(Vec3{image.windowCentre[0], image.windowCentre[1], image.windowCentre[2]}, window.centre - camera, 1e-5f));
        assert(image.windowSource == 0 + 1 && near(image.windowAxisU[3], window.halfU, 1e-6f));
        assert(!lights.fastSpots[1]);
    }
}

void testImageLights() {
    testFaceWindow();
    testThroughWindow();
    testSpotImage();
    testPointImage();
    testChoosing();
    testShadowTiles();
    testPacking();
}
