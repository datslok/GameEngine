# Parallax and blended grounds

Covers: `src/render/gpu/parallax`, `src/render/gpu/ground_layers`, `src/render/gpu/window_mask`, `src/core/height_map`, `assets/shaders/parallax.glsl`, `assets/shaders/ground_layers.glsl`, `assets/shaders/window_mask.glsl`

Surface detail beyond a normal map: height maps the view ray searches (parallax occlusion mapping), and grounds of up to four blended materials. Both run in the main pass's `scene_surface.glsl`; the lighting after them is `shading.md`.

## Parallax occlusion mapping (`render/gpu/parallax`)

Tatarchuk 2006. A normal map only tilts the light; a height map (`Material::heightTexture`, linear grey, 1 at the polygon, 0 deepest) lets each pixel follow its view ray into the surface to where it first drops below the ground, and read every texture there, so a brick's near side hides the mortar behind it. Numbers: `render/gpu/parallax_shared.h`.

**The search** (`parallaxSearch` mirrors the main pass's loop):

- In the surface's own frame (`toTangentFrame`: the normal map's tangent, bitangent, normal, made orthonormal) and texture units: `Material::heightScale` is the deepest point's depth as a share of the texture's width, so one number fits any texture size.
- A fraction t down the ray sits at height `1 - t`, shifted by `parallaxDelta` × t (the view's sideways part over its normal part × depth). The cosine is held at least `PARALLAX_SMALLEST_VIEW_COSINE` (0.1), so a grazing view shifts at most ten depths.
- Steps (`parallaxSteps`): `PARALLAX_FEWEST_STEPS` (8) head-on, more towards grazing, up to `MaterialSettings::parallaxSteps` (32; at most `PARALLAX_MOST_STEPS` 64). The last step is exactly the bottom, which every ray reaches.
- The first step whose gap (`parallaxGap`) is ≤ 0 ends it; `parallaxCrossing` intersects the straight line between that step and the one before, removing stair steps.

**Mips:** every read (search and surface textures at the hit) uses `textureGrad` with the unshifted coordinates' pixel steps × the mip bias (`surfaceTexture`): the shift jumps at a brick's edge, and its own steps would pick wild mips there. Materials without a height map keep plain reads; the choice is per material, so whole 2 × 2 quads agree and derivatives stay valid.

**Distance fade:** `parallaxDepthPixels` (the depth seen head-on in pixels) scales the depth by `parallaxFade`: all of it from `PARALLAX_FADE_FULL_PIXELS` (2), none under `PARALLAX_FADE_NONE_PIXELS` (0.5), where the mips no longer hold the bumps and the shift would only shimmer.

**Self-shadowing** (`MaterialSettings::parallaxSelfShadows`, on; `parallaxShadow`): for the brightest directional light, the ray from the hit up towards it to the top in `PARALLAX_SHADOW_STEPS` (16) steps. Each sample's share falls as the ground pokes above the ray, by `PARALLAX_SHADOW_SOFTNESS` (4) × how far it pokes over how far the ray has climbed (`parallaxShadowSample`); the light gets the least share.

**Limits** (by design):

- Silhouettes stay flat.
- The depth image keeps the flat polygon (writing the shifted depth would break the prepass's `EQUAL` test and early depth rejection), so ambient occlusion, contact shadows, screen reflections and other objects' shadows see it flat.
- Cut-outs and transparent materials take no height map (`packMaterial` packs depth 0): a cut-out's holes come from the prepass at the unshifted place.

**Where:** right after the material is read, before any other texture, in every main-pass pipeline including the reflection probe's capture. `GpuFrameData::materialOptions`: x on, y self-shadows, z the most steps. F5 Materials (`engine.render.materials.parallax`, `parallaxSelfShadows`, `parallaxSteps`).

**Normal maps from heights:** `normalMapFromHeight` (`core/height_map`) makes a normal map from a height map at its depth (central differences, wrapping), so generated surfaces light as their parallax shows them.

Test scene: the light lab's low brick wall and cobbled strip (`labBrickWall`, `brickImages`, `cobbleImages`, `tiledPanel`).

## Blended grounds (`render/gpu/ground_layers`)

One mesh, several surfaces: `Material::groundLayers` holds up to four ordinary materials, `groundWeights` a linear RGBA texture (a channel a layer) saying how much of each covers each place, `groundRepeats` how often each layer's textures repeat. Every field: `scene/material.h`.

### Height blending (`groundBlendWeights`, after Mishkinis 2013)

Cross-fading by the weights alone smears borders; instead the texel standing higher shows, so borders follow the textures' shapes (moss in cracks, grass through litter).

- A painted layer's share = its weight × how far its height comes within `groundBlendDepth` (0.2) of the highest painted layer's, normalised. Shallow = crisp borders, deep = plain cross-fade.
- An unpainted layer never shows, whatever its height, so its reads are skipped (most pixels read one or two layers). Nothing painted → the first layer; all shares zero → the painted weights, normalised (8-bit weights need not sum to 1).
- A layer without a height map (or `heightScale` 0) stands halfway; one without a normal map lies flat (otherwise the slot's white stand-in would always win and tilt normals).
- **Weights read as a cubic B-spline** (`groundPaintTaps`, Sigg and Hadwiger 2005): sixteen texels in four bilinear reads, each with the surface's own footprint. Plain bilinear's slope jumps at every texel, and the height blend sharpens those kinks into staircases along the texture grid.

### Projected layers (`Material::groundProjection = Axes`, triplanar)

For generated shapes stretched to size (the forest's pillars and islands), whose UVs stretch with them.

- Each layer read along the object's own three axes, in metres (`groundRepeats` then count repeats a metre): the texture keeps its size at any scale and stays stuck to a moving object (a world projection would slide).
- **Frame** (`projectionFrame`): model = R × S, so the 3 × 3 part's column lengths are S. `triangle.vert` passes S·p (`framePosition`) and R (`frameAxes`, flat); the fragment turns the normal into the frame by Rᵀ.
- **Shares** (`projectionWeights`): each axis's |n|⁴, normalised, less `GROUND_PROJECTION_SMALLEST` (0.1), rescaled to sum 1, so a share fades out smoothly and most pixels read one or two projections. A zero share is not read; mips come from each place's own steps taken before any branch.
- **Places** (`projectedPlace`): along x (z, y), along y (x, z), along z (x, y), so up the picture is up the object on every side.
- **Normals** (`projectedNormal`): each projection's texel in its own plane, combined with the surface normal by the whiteout blend (Barré-Brisebois and Hill 2012, as Golus 2017 applies it), blended by shares, turned back by R. A flat map gives the surface normal back; on an axis-aligned face it equals `perturbNormal`. Toksvig reads the blended texels.

### Facet projection (`groundProjection = Facets`)

For flat facets turned every which way (the forest's structures), where triplanar would cross-fade a straight seam on a facet turned 30 degrees and draw it twice. Same frame and metres as triplanar; one read a layer.

- **Axes** (`facetAxes`): across `A = normalize(up x n)` (level), up `B = n x A` (the steepest way up); `A x B = n`. Facing straight up or down, A is the object's x.
- **Place** (`facetPlace`): `(p.A, p.y)` on steep facets (`|n.y|` under `GROUND_FACET_STEEP`, 0.5), so a seam at one height meets itself round every edge (stretch at most 15 %); `(p.A, p.B)` on flatter ones. Linear in p, so the pixel steps map through it for mips.
- **Normal** (`facetNormal`): the decoded tilt turned by (A, B, n), normalised.
- **Streaks and tone:** one read each, streaks across `p.A` and down the height (`facetStreakPlace`), side share the normal's level part (`facetSideShare`).
- The facets' own numbers (`facetTone`, `facetRoughness`) keep the mesh's texture coordinates, so a faceted ground paints by strata, not by a weights texture over them.

### Hex tiling (`Material::groundHexTiling`, `hexTiling`)

Heitz and Neyret 2018, in Mikkelsen 2022's form, against visible repeats over big objects.

- Coordinates × `GROUND_HEX_SCALE` (2√3: a cell a little under a third of a tile) fall in a triangle grid whose corners are hex cell centres; each corner reads at its own random whole-tile offset (`pcg3d` of its cell).
- Weights: barycentric to the sixth power, normalised, those under `GROUND_HEX_SMALLEST` (0.02) dropped, so most places read one or two taps.
- Offsets only, no rotation: layered rock stays level and normal maps need no turning. Every tap uses the unmoved coordinates' mip.

### Strata (`groundPaint = GroundPaint::Strata`)

Layers painted by world height, so every object with the material shows the same beds at the same heights (a valley's rock).

- `groundWeights` is a strip from `strataBottom` to `strataTop` (`strataPlace`), read at the fragment's world height, its mip from the height's change across the pixel.
- `groundTopLayer` takes over where the normal faces up (`strataPaint`): none below `groundTopStart` of its upward part, all from `groundTopFull`, smoothstep between.
- **Wavy beds** (`strataWarp`, `warpedStrataHeight`): a noise texture's red (0.5 none) over world x, z moves the read height by up to `strataWarpMetres`, repeating every `strataWarpTile` m.

### Streaks and tone (projected layers and facets only)

- **Water streaks** (`groundStreaks`): red = clean rock (1) to darkest stain (0), read on the two side projections stretched down the height (`streakPlace`: `streakWidth` across, `streakLength` down), darkening the albedo by up to `streakStrength`, on sides only (`streakShade`).
- **Tone variation** (`groundVariation`, `variationShade`): slow noise (0.5 none) along each projection every `variationTile` m, lightening or darkening by up to `variationStrength`.

### Slit windows (`render/gpu/window_mask`, faceted strata grounds)

Rows of slit windows on walls without geometry, lit by the main pass's emission (research: `docs/research/STRUCTURES.md`, "Lights at night"). Numbers: `window_mask_shared.h`.

- **Bands** (`Material::windowBands`): a linear strip over the strata's heights: red how much of the height rows cover, green the share lit, blue the share in the cool colour, alpha the slit's share of its cell across. Read like the strata (its mip from the height's change), so far away the rows' cover averages.
- **Cells:** `windowPitch` metres across each face (`facetPlace`'s first coordinate), rows `WINDOW_ROW_HEIGHT` (1 m) for the hash; each window's lit, colour and brightness numbers are `pcg3d` of its column, row and the face's seed (`windowFaceSeed`: the facet numbers rounded to 1024 steps, so interpolating three equal corners cannot change it).
- **Box-filtered across** (`pulseIntegral`, `pulseCoverage`): the slits' exact share of the pixel's footprint, from the running cover of periodic pulses, so a far slit is its true coverage, not a flickering hard edge.
- **Far fade** (`windowFarShare`): from 0.5 to 1.5 cells a pixel, each window's states blend to their expected values (share lit, the colours' mix, brightness 1; lit windows vary 0.6-1.4), so a band's average light is the same near and far.
- **Emission** (`windowEmission`): cover x across x lit x brightness x colour (`windowWarm`, `windowCool`, linear, brightness included), added to the surface's emission.

### Packing and the main pass

- `MaterialTable::add` adds each layer first as a material of its own (shared with identical ones); the ground's `GpuMaterial` names them by index + 1 in `groundLayers`, with repeats, weights slot, switches (`GROUND_PROJECTED`, `GROUND_FACETS` with it, `GROUND_STRATA`, `GROUND_HEX`), strata, warp, streak and variation numbers in `groundBlend`, `groundFacing`, `groundSlots`, `groundStreaks`; the window bands' slot in `groundSlots.w` (faceted strata grounds only) and their colours and pitch in `windowWarm`, `windowCool`; which lane holds what: `render/gpu/material_uniforms.h`.
- Throws `std::invalid_argument` for more than four layers or none, no weights texture, a layered layer, or a top layer not among them.
- In `scene_surface.glsl`: the weights (B-spline at the mesh's UVs, or the strip with the top layer), then each needed layer's colour, metallic-roughness, occlusion, height and normal (at the ground's `normalScale`) at its repeated UVs, per projection or in the facet's plane, blended into the surface the rest of the shader lights. Parallax does not run on the layers.

## Tests

- `parallax_test`: frame, steps, shifts, the crossing, flat maps, a tilted plane's exact crossing, a brick hiding mortar one way only, self-shadowing, the fade, the settings. `height_map_test`.
- `ground_layers_test`: the blend, uneven and empty weights, refused materials, the frame, shares, places, normals, facet axes, places, normals and streaks, the windows' packing, strata and top layer, warp, streaks, hex tiling, tone variation. `window_mask_test`: the running cover, box filtering against brute force, seeds and hashes, the far fade, emission near and far, and the near windows averaging to the far value.
- GPU: `parallax_gpu_test`, `ground_layers_gpu_test`, `window_mask_gpu_test`. Also `material_uniforms_test`, `engine_commands_test`, `light_lab_test`.
