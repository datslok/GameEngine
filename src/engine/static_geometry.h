#pragma once

#include "assets/asset_manager.h"
#include "physics/collision_settings.h"
#include "physics/static_level.h"
#include "scene/frame_description.h"

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

/*
* The static level as the sky visibility grid's rays see it (render/gpu/sky_visibility_bake.h), built by the engine from
* the frame's own draws, so no game has to hand it over: whatever a game marks static (ModelRenderer::isStatic, as the
* light probes already ask) is in it. The game's own StaticLevel, for walking on, may differ (a game may walk on less
* than it draws), and the renderer never sees games.
*/

// Whether a draw stands in the sky's way: static, solid (it casts shadows; the glowing stand-ins for lights do not, as
// ray tracing's RAY_MASK_SOLID leaves them out too) and not seen through (glass and blended surfaces let light past).
bool blocksSky(const DrawItem& draw);

// A number for the frame's sky-blocking draws, their meshes, matrices and what colours their bounce light (colour,
// texture, metallic share) in order: equal while they are.
std::uint64_t skyBlockersFingerprint(std::span<const DrawItem> draws);

/*
* The level's collision grid for these rays: 1 m cells, and only a floor-sized triangle on the list every ray tests
* (measured on the light lab: 1.35 microseconds a ray against its 29,632 triangles, against 3.1 with the default 2 m
* cells, whose large-item list every ray tests).
*/
CollisionSettings skyRayCollision();

class StaticGeometry {
public:
    // Rebuilds the level from the blocking draws when they differ from last time (their meshes from assets).
    void update(std::span<const DrawItem> draws, const AssetManager& assets);

    // The level, shared with the frames that carry it; none when nothing blocks the sky.
    std::shared_ptr<const StaticLevel> getLevel() const;

    /*
    * Each of the level's triangles' diffuse albedo, in linear light and in the level's triangle order, for the bounce
    * light the sky visibility grid gathers: the material's colour decoded from sRGB, times its texture's mean (each texel
    * decoded first), times one less the metallic share. None when there is no level.
    */
    std::shared_ptr<const std::vector<Vec3>> getAlbedo() const;

    // Changes whenever the level does (0 before the first update).
    std::uint64_t getVersion() const;

private:
    // A texture's mean colour, in linear light, worked out once.
    Vec3 textureMean(TextureHandle texture, const AssetManager& assets);

    std::shared_ptr<const StaticLevel> level;
    std::shared_ptr<const std::vector<Vec3>> albedo;
    std::map<std::uint32_t, Vec3> textureMeans;
    std::uint64_t fingerprint = 0;
    std::uint64_t version = 0;
};
