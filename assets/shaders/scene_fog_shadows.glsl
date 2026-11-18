// The sun's shadows in the fog (render/gpu/volumetric_fog.h): the shadow at a point in the air, which lights the fog's
// grid (fog_light.comp), and the fog along a view with the closed form past the grid shaded by the shadows at samples
// along it (fog_apply.comp). Included after frame_data.glsl, scene_lights.glsl, cascades.glsl,
// scene_shadow_tiles.glsl and scene_fog.glsl.
#ifndef SCENE_FOG_SHADOWS_GLSL
#define SCENE_FOG_SHADOWS_GLSL

// The sun's shadow at a point in the air (measured from the camera): the finest cascade whose box holds it, the far
// shadow map last, moved towards the sun by the cascade's own bias (cascades.h), one comparison; lit past the last.
vec3 sunShadowInAir(int firstTile, int cascades, vec3 position, vec3 toLight, int farTile, float distantReach) {
    for (int cascade = 0; cascade < cascades; ++cascade) {
        int tile = firstTile + cascade;
        vec3 biased = position + toLight * shadowTiles[tile].offset.y;
        vec4 clip = shadowTiles[tile].matrix * vec4(biased, 1.0);

        if (cascadeEdgeDistance(clip.xyz / clip.w, shadowTiles[tile].lens.y / shadowTiles[tile].lens.w) > 0.0) {
            vec3 own = shadowTap(tile, biased);

            // A cascade's, with the far map's blockers further than its caster reach (distant occluders, cascades.glsl).
            if (farTile < 0 || tile == farTile || max(own.r, max(own.g, own.b)) <= 0.0) {
                return own;
            }

            return withDistantOccluders(own, shadowTap(farTile, distantOccluderPoint(position, toLight, distantReach)));
        }
    }

    return vec3(1.0);
}

/*
* The light the fog over a stretch past the grid scatters towards the eye per unit of optical depth, as the mean of its
* light at steps samples, each carrying an equal share of the light gathered (fogMediaFarSampleDistance), offset by turn
* (0..1) so each pixel and frame samples elsewhere and the anti-aliasing averages them. At each sample: the layers'
* shares there, each directional light dimmed by its shadow and by the fog above. With no steps, fogBeyondSource (the
* stretch lit throughout).
*/
vec3 fogBeyondSourceShadowed(FogFarStretch stretch, int steps, float turn) {
    if (steps <= 0) {
        return fogBeyondSource(stretch);
    }

    vec3 ambient = fogBeyondAmbient();
    vec3 sum = vec3(0.0);

    for (int k = 0; k < steps; ++k) {
        float share = (float(k) + turn) / float(steps);
        float distance = fogMediaFarSampleDistance(frameFog.media, cameraOrigin.y, stretch.direction.y, stretch.start, stretch.end, share);
        vec3 point = stretch.direction * distance;
        float height = cameraOrigin.y + point.y;
        vec4 shares = fogMediaShares(frameFog.media, height, distance);
        sum += fogMediaAmbient(frameFog.media, shares, ambient);

        for (int i = 0; i < counts.x; ++i) {
            vec3 irradiance = directional[i].radiance.rgb * fogSunThrough(height, directional[i].toLight.xyz);
            int tile = directional[i].shadowTile;

            if (tile >= 0) {
                irradiance *= sunShadowInAir(tile, directional[i].cascadeCount, point, directional[i].toLight.xyz, directional[i].farShadowTile,
                                             directional[i].distantOccluderReach);
            }

            sum += fogMediaInScattering(frameFog.media, shares, irradiance, directional[i].toLight.xyz, stretch.direction);
        }
    }

    return sum / float(steps);
}

// fogAlongView with the fog past the grid in the sun's shadows (fogBeyondSourceShadowed).
FogIntegration fogAlongViewShadowed(vec3 relativePoint, bool sky, int steps, float turn) {
    FogIntegration fog = fogNearView(relativePoint, sky);
    FogFarStretch stretch = fogFarStretch(relativePoint, sky);

    if (stretch.end > stretch.start) {
        fog = fogFollow(fog, fogOverStretch(stretch, fogBeyondSourceShadowed(stretch, steps, turn)));
    }

    return fog;
}

#endif
