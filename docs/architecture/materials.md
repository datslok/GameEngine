# Materials

Covers: `src/scene/material.h`, `src/core/coverage_mips`, `src/core/srgb`, `src/render/gpu/alpha_mask`, `src/render/gpu/facet_variation`, `assets/shaders/facet_variation.glsl`, `assets/shaders/alpha_mask.glsl`, `assets/shaders/cut_out_fragment.glsl`, `assets/shaders/cut_out.frag`, `assets/shaders/cut_out_prepass.frag`

What a surface is made of, from file to `Material`, and how cut-outs keep their holes. How materials are shaded: [shading.md](shading.md) (BRDF, normal maps, clear coat), [parallax-and-grounds.md](parallax-and-grounds.md) (height maps, ground layers). Glass and blended surfaces drawn: [glass.md](glass.md). Loading and texture caching: [assets.md](assets.md).

## From file to Material

`Model` part → `MaterialSource` (`scene/model.h`) → `AssetManager::makeMaterial` → `Material` (`scene/material.h`) → `packMaterial` (`render/gpu/material_uniforms`) → `GpuMaterial`.

- `MaterialSource`: the same numbers as `Material`, its defaults taken from `Material{}`, but each texture a `TextureSource` (a path or embedded bytes), all sharing one `flipTextureVertically`.
- `makeMaterial` copies the numbers and loads each texture once with its encoding (below). Field meanings: the header.
- `packMaterial` decodes the sRGB colour, clamps, zeroes what a missing texture or an excluded material cannot use, and sets the flags (`render/gpu/material_shared.h`). Its layout and the material buffer: [gpu-scene.md](gpu-scene.md).

## Material fields

| Group | Fields | Defaults that matter |
|---|---|---|
| Base | `colour` (sRGB bytes), `texture` | white |
| Metal and roughness | `metallic`, `roughness`, `metallicRoughnessTexture` (glTF packing: roughness green, metallic blue, multiplying the numbers) | 0, 0.5 |
| Normal map | `normalTexture`, `normalScale` | 1 |
| Baked occlusion | `occlusionTexture` (red: 0 buried, 1 open), `occlusionStrength` 0..1 | 1 |
| Glow | `emissive` (linear, may exceed 1), `emissiveTexture` (sRGB, multiplying it) | black |
| Alpha | `alphaMode` (`Opaque`, `Mask`, `Blend`), `alphaCutoff`, `opacity`, `doubleSided`, `backKeepsNormal` | Opaque, 0.5, 1, false |
| Leaves | `diffuseTransmission`, `diffuseTransmissionTint` | 0 |
| Faceted shapes | `facetTone` (a share), `facetRoughness`: each triangle's texture coordinate read as its facet's two random numbers (`render/gpu/facet_variation.h`, applied after any blended layers) | 0 |
| Thin glass | `transmission`, `transmissionTexture` (red) | 0 |
| Thick glass | `thickness` (model units), `thicknessTexture` (green), `attenuationColour`, `attenuationDistance`, `ior` | 0, infinite, 1.5 |
| Clear coat | `clearCoat` (red texture), `clearCoatRoughness` (green texture), `clearCoatNormalTexture`, `clearCoatNormalScale` | 0, 0, 1 |
| Parallax | `heightTexture` (red: 1 top, 0 deepest), `heightScale` (texture units) | 0.03 |
| Impostor | `impostorFrames` (atlas frames a side; `scene/impostor.h`) | 0 |
| Ground layers | `groundLayers` (up to four plain materials), `groundWeights`, `groundRepeats`, `groundBlendDepth`, `groundProjection` (`TextureCoordinates`, `Axes` or `Facets`), `groundPaint` (`TextureCoordinates` or `Strata`), the strata, top-layer, warp, streak, hex tiling and variation fields | none |

- `isCutOut(material)`: `Mask`. `isTransparent(material)`: `Blend` or `transmission` above 0, so drawn after the opaque scene.
- Clear coat is ignored on transparent materials (their glass is a varnish already); parallax on cut-outs and transparent ones.
- `backKeepsNormal`: a double-sided back keeping the front's normal (grass tilted up, lit from above whichever way a blade faces).
- Ground layers are set by games in code (the forest), never loaded; shaded as [parallax-and-grounds.md](parallax-and-grounds.md) describes.

## Texture encodings

`makeMaterial` loads:

| Encoding | Textures |
|---|---|
| `Srgb` | colour (with the cut-out's coverage cutoff), emissive |
| `Linear` | metallic-roughness, normal, occlusion, transmission, thickness, the three clear coat ones, height |

- The encoding is part of both texture caches' keys and stored in `ImageData::encoding`.
- ORM (occlusion, roughness, metallic in red, green, blue): occlusion and metallic-roughness name the same file or bytes, read the same way, so they are one texture.

## glTF materials as loaded

`readMaterial` in `assets/gltf_loader`; each texture through `readTexture`.

| glTF | Becomes | Checked |
|---|---|---|
| no material | metallic and roughness 1 (glTF's defaults), the rest `Material`'s | |
| `baseColorFactor` rgb | `colour`, encoded by `linearToSrgbByte` (factors are linear) | |
| `baseColorFactor` alpha | `opacity` | 0..1 |
| `metallicFactor`, `roughnessFactor` and their texture | `metallic`, `roughness` | 0..1 |
| `normalTexture` and `scale` | `normalTexture`, `normalScale` | finite |
| `occlusionTexture` and `strength` | `occlusionTexture`, `occlusionStrength` | 0..1 |
| `emissiveFactor` × `KHR_materials_emissive_strength`, `emissiveTexture` | `emissive`, `emissiveTexture` | factor 0..1, strength ≥ 0 |
| `alphaMode` `MASK`, `alphaCutoff` | `Mask`, `alphaCutoff` | ≥ 0 |
| `alphaMode` `BLEND` | `Blend` | |
| `doubleSided` | `doubleSided` | |
| `KHR_materials_transmission` | `transmission`, `transmissionTexture` | 0..1 |
| `KHR_materials_volume` | `thickness`, `thicknessTexture`, `attenuationColour`, `attenuationDistance` (cgltf's largest float → infinite) | ≥ 0, 0..1, > 0 |
| `KHR_materials_ior` | `ior` (1.5 without it) | ≥ 1 |
| `KHR_materials_clearcoat` | factor, roughness, their textures, normal texture and scale | 0..1, finite |

Not read from glTF: height maps, diffuse transmission, ground layers (set in code).

## Colour

- Shading is in linear light; `Material::colour` bytes are sRGB like image files, decoded by `srgbByteToLinear` (`core/srgb`) in `packMaterial`.
- Light colours, intensities and `emissive` are linear.
- A black `colour` with a glow shows only the glow, unchanged by lighting (the demo's moon and MOBA marker).

## Cut-outs and double-sided surfaces

A cut-out (`Mask`) is a sheet with holes (leaves, wire, grates): a texel is kept, fully opaque, where its alpha (colour texture's × `opacity`) reaches `alphaCutoff`, else discarded. No blending or sorting.

- `isCutAway(alpha, cutoff)` (`render/gpu/alpha_mask`, mirrored in `alpha_mask.glsl`).
- Double-sided: both sides drawn, the back shaded as the front's mirror image (`facingSign`: normal and whole tangent frame turned round, so bumps stand out of the side seen).

### Coverage-preserving mips

Plain averaging mixes a wire with the holes beside it, so a few levels down everything falls under the cutoff and fences vanish. Castaño's fix (`core/coverage_mips`), on the CPU at load:

1. `halveImage`: each level from the last as averaged, 2 × 2 blocks, an odd edge folded in, sRGB colour averaged as light, alpha as a plain share.
2. Scale that level's alpha alone so `alphaCoverage` (share of texels passing the cutoff) stays the full size's.
3. The scale by bisection on a 256-bin alpha histogram: the nearest reachable coverage, then of the scales giving it, the one nearest 1 (solid stays solid).

- The cutoff the texels meet is `alphaCutoff / opacity` (infinite for opacity 0).
- `loadTexture` and `addTexture` take it as `coverageCutoff`; the levels go into `ImageData::mips`, which `VulkanTexture` copies in instead of blitting.

### Drawing cut-outs

`batchByMesh` groups each view's draws by style (plain, double-sided, cut-out, double-sided cut-out), cut-outs last; `drawBatches` switches pipeline and cull mode per batch.

| Pass | Plain | Cut-out |
|---|---|---|
| Depth prepass | no fragment shader | `cut_out_prepass.frag` (`fragmentCutAway`, with the frame's mip bias; then a fading level's dither, level-of-detail.md) |
| Main pass after the prepass | as usual | same pipeline: the holes have no depth for its `EQUAL` test to match |
| Main pass without the prepass, probe capture | as usual | specialization constant `MATERIAL_CUT_OUT_CONSTANT` adds the discard |
| Shadow pass | `shadow.vert` alone | `shadow_cut_out.vert` with UVs, `cut_out.frag` |
| Velocity pass | `motion.frag` | its cut-out constant, so motion behind the holes survives; a fading level's dither as the prepass |

- Double-sided batches cull nothing (`vkCmdSetCullMode`); others keep the pass's culling (front faces in the mirror-image probe capture, whose shading flips `gl_FrontFacing` back).
- **Rays:** a cut-out's ray instance is not opaque (`rayInstanceNotOpaque`, `FORCE_NO_OPAQUE`). `traceRayQuery` (`ray_scene.glsl`) confirms a candidate only where its full-size texel is solid (`candidateIsSolid`; rays have no footprint). `committedHitFacesRay` counts either side of a double-sided surface as front, so no ray takes a leaf for a wall's inside. Needs sets 0 and 1 bound (`RAY_SCENE_MATERIALS`).

## Tests

`coverage_mips_test`, `alpha_mask_test`, `material_uniforms_test`, `gltf_material_test`, `texture_encoding_test`, `asset_manager_test`, `gpu_scene_test` (batching by style), `ray_tracing_test` (the opaque flag), `light_lab_test` (the chain-link fence); GPU: `alpha_mask_gpu_test`.
