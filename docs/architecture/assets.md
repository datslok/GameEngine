# Assets

Covers: `src/assets/asset_manager`, `src/assets/obj_loader`, `src/assets/gltf_loader`, `src/assets/image_loader`, `src/assets/cgltf.cpp`, `src/scene/asset_handles.h`, `src/scene/image_data.h`, `src/scene/model`, `src/assets/texture_cooking`, `src/core/block_compression`

What the game loads or makes on the CPU, and how it reaches the GPU. Folder rules (UV origin, flips, caching keys, layering): `src/assets/CLAUDE.md`. Materials: [materials.md](materials.md). Meshes and their render format: [meshes.md](meshes.md).

## Handles

- `MeshHandle`, `TextureHandle` (`AssetHandle<Tag>`): an index into the manager's arrays; the renderer numbers its GPU copies the same way, so a lookup is one array access.
- The tag only keeps the two types apart. `isValid()` is false for the default (`noneIndex`): no mesh, or no texture (plain colour).
- No generation counter: assets are never unloaded, so a handle cannot go stale. Unloading would need one, as `Entity` has.
- `TextureEncoding` (`Srgb` colour, `Linear` data) lives beside the handles, so `scene/` and `render/` can name it without `assets/`.

## AssetManager

Owns every CPU-side mesh, image and imported model. Append-only, so "new since last frame" is every handle past the last count.

| Call | Gives | Cached by |
|---|---|---|
| `addMesh(Mesh)` | a mesh made in code | nothing (always new) |
| `addMesh(shared_ptr<const Mesh>)` | a shared mesh (a model part's) | the pointer |
| `addDeformingMesh(Mesh)` | a mesh whose vertices move; its `DeformingTopology` built here | nothing (never shared) |
| `addDrawnMesh(IndexedMesh)` | a mesh kept only in the render format | nothing |
| `addDrawnMeshWithLevels` | a drawn-only mesh plus its `simplifiedLevels`, named on it | nothing |
| `loadMesh(path, MeshLoadOptions)` | an OBJ file | path, `smoothNormals`, `creaseAngle` (0 when not smoothing) |
| `loadTexture(path, flip, encoding, coverageCutoff)` | an image file | path, flip, encoding, cutoff |
| `loadTexture(bytes, flip, encoding, coverageCutoff)` | an embedded image (`.glb`) | the buffer's address, encoding, cutoff; the buffer kept alive so the address cannot be reused |
| `loadTexture(TextureSource, ...)` | whichever the source names; none for an empty source | as above; a source naming both throws |
| `addTexture(ImageData, coverageCutoff)` | an image made in code (impostors, generated textures) | nothing |
| `loadModel(path)` | a glTF model, as a stable `const Model&` | path (`std::map`, so references stay valid) |

- `MeshLoadOptions`: `smoothNormals` off by default (only the game knows if a shape is meant faceted), `creaseAngle` 60°. Smoothing: [meshes.md](meshes.md).
- `coverageCutoff`: a cut-out's colour texture gets its mips built on the CPU ([materials.md](materials.md)). The same file on an opaque material is a separate texture.
- `getMesh`, `getTexture` throw `std::out_of_range` for an unknown or none handle; `getMesh` on a drawn-only mesh throws `std::invalid_argument`.
- Meshes are held by `shared_ptr`, so `getMesh` references survive later additions.

### Drawn-only meshes

`addDrawnMesh`, `isDrawnOnly`, `getDrawnMesh`: big generated scenery (the forest's rocks, island hangings) kept as `IndexedMesh` only.

- About a fifth of a `Mesh`'s memory: a `Mesh` keeps 168 bytes a triangle for the whole run; the render format 48 a vertex and 12 a triangle.
- `getMesh` refuses it, so it cannot be walked against, cast at on the CPU or marked static by mistake. An empty one is refused.
- Uploaded as it is, bounds from `indexedMeshBounds`, no flat faces (never a planar mirror).

### Levels of detail

`setMeshLevels(base, levels)`, `setMeshLevelOptions`, `getMeshLevels`, `getLevelsVersion` (bumped on every change, so kept still draws rebuild). Validation in the header; how levels are chosen: `docs/research/LOD.md`.

### Models to entities

- `makeMaterial(MaterialSource)`: loads each texture with its encoding ([materials.md](materials.md)).
- `makeModelRenderer(model, normalization)`: each part becomes a `RenderPart` (mesh handle, material, `normalization * part.transform`, folded in once). A part without a mesh throws before anything is registered.
- `makeCollisionMesh(renderer)`: one `TriangleMesh` of every part placed by its local transform, in the entity's own space, lining up with what is drawn.
- `Model::getNormalizationMatrix(targetSize)`: centres the model and scales its longest side (default 2); `meshBounds`, `Model::getBounds`.

## Upload

`Application::prepareNewResources`, before each frame, uploads every handle the renderer lacks (games may load at any time):

| Mesh kind | Uploaded | Bounds | Flat faces |
|---|---|---|---|
| ordinary | `buildIndexedMesh` once, then up in a batch (`uploadMeshes`) | `meshBounds` | `findFlatFaces` |
| deforming | its topology's `rest`, in the batch as deforming | `meshBounds` (replaced each frame) | none |
| drawn-only | as it is | `indexedMeshBounds` | none |

- **Cluster levels** only for meshes marked with `setClusterLevels` (not deforming ones), read by `hasClusterLevels` as the mesh goes up and by the draws through `getClusterLevels` (`attachMeshLevels`; `mesh-clusters.md`).

Textures: new ones are cooked together, a texture a job, then uploaded in handle order: `uploadCompressedTexture` for a compressed one, else `uploadTexture(width, height, pixels, encoding, mips)`. GPU side: [meshes.md](meshes.md) (mesh buffers), Colour and textures below.

## Colour and textures on the GPU

- `ImageData`: RGBA8 rows, `encoding`, optional CPU-built `mips` (`ImageLevel`s), `storage` (`Compressible`, or `Exact` for a game's lookup tables: the forest's ground weights, strata, warp, streaks, variation). In `scene/` so scene code can make images without `assets/`. The CPU copy stays RGBA8 (games read it: impostor bakes, leaf clusters).
- `surfaceTexture(encoding)` (`render/vulkan/vulkan_texture`): `R8G8B8A8_SRGB` for colour (the GPU decodes when sampling and when averaging mips), `R8G8B8A8_UNORM` for data.
- `VulkanTexture`: level 0 copied in, smaller levels blitted from the one above (`mipLevelCount`, `render/gpu/mip_levels`), or copied in when built on the CPU.
- Compressed (below): `compressedSurfaceFormat` gives `BC7_SRGB_BLOCK` or `BC7_UNORM_BLOCK`; every level copied in, no blits.
- The surface sampler: trilinear, repeat, anisotropy up to 16 (or the GPU's limit), `maxLod` `VK_LOD_CLAMP_NONE` (0 would lock sampling to level 0).
- A missing texture binds the white stand-in, `whiteTextureSlot` 0; a handle's bindless slot is `bindlessTextureSlot` (handle + 1). White multiplies colours and factors by 1.
- The SDR swapchain is `B8G8R8A8_SRGB`, encoding on write. Linear light, sRGB bytes and the decode: [materials.md](materials.md), Colour.

## Texture compression

Research and choices: `docs/research/TEXTURECOMPRESSION.md` (BC7 for everything compressible, normal maps included, so Toksvig keeps reading the averaged length).

| Step | Where | Does |
|---|---|---|
| Choose | `shouldCompress`, `bc7OptionsFor` | `Exact` stays RGBA8; sRGB colour perceptual weights, linear data plain |
| Mips | `compressTexture`, `averagedMips` (`core/coverage_mips`) | the image's built mips (cut-outs) or each level halved on the CPU, sRGB as light, normals not renormalised |
| Encode | `compressBc7` (`core/block_compression`, bc7enc in `external/bc7enc`) | a row of blocks a job; edge blocks repeat the last row and column; same blocks on any thread count |
| Keep | `cookTexture`, `textureCacheKey`, `readCompressedTexture`, `writeCompressedTexture` | `build/texture_cache/<16 hex>.bc7`, keyed by FNV-1a of shape, encoding, texels, built mips, options and the file version; written to a temporary file and renamed; a missing, short or inconsistent file is encoded again; an unwritable folder is skipped |
| Upload | `Application::prepareNewResources` | only when `engine.textureCompression.compress` and the GPU has `textureCompressionBC` (`VulkanDevice::supportsBlockCompression`); logs the time when over 0.5 s |

- Settings (`TextureCompressionSettings`, start-up fields): `compress` (on), `quality` 0-4 (bc7enc's uber level, part of the key).
- The file version (`fileVersion`, `texture_cooking.cpp`) goes up when the layout, the encoder or the mips change, so stale files are never found.
- `decompressBc7` decodes as the GPU does, for tests.

## Loaders

| Loader | Reads | Rejects |
|---|---|---|
| `loadObj`, `parseObj` (`assets/obj_loader`) | `v x y z`, `vn` (normalised; zero throws), `vt` 2D (outside 0..1 kept for tiling), `f` with position, position/UV, position//normal and position/UV/normal entries, negative indices too; convex polygons fanned, each corner keeping its normal | out-of-range indices; other records (materials, smoothing groups) ignored |
| `loadGltf` (`assets/gltf_loader`, cgltf) | the default scene (else the first); each node's primitives as `ModelPart`s with world transforms; per-corner normals, `TEXCOORD_0`, tangents (normalised, w must be ±1); materials ([materials.md](materials.md)); images external (relative paths, URI-decoded) or embedded (PNG or JPEG) | required extensions, non-triangle primitives, morph targets, skins, sparse accessors, UV sets past 0, texture transforms, data URIs and absolute or remote image paths, a textured primitive without `TEXCOORD_0` |
| `loadImage`, `loadImageFromMemory` (`assets/image_loader`, SDL3_image) | any format SDL3_image decodes, converted to RGBA8, rows flipped by default | |
| `saveRawImagePng`, `loadRawImagePng` | 8-bit PNG exactly as given, no flip (golden images) | |
| `loadStarCatalogue`, `parseStarCatalogue` (`assets/star_catalogue`) | CSV lines "number, right ascension, declination, magnitude, colour index" (degrees, J2000), `#` comments; blank colour index becomes `unknownColourIndex` (0.65, the sun's) | a line it cannot read, named in the error |

- glTF images: `GltfImages` copies each embedded image once per load, keyed by cgltf's image, so slots naming one image (an ORM texture) share bytes and the manager decodes and uploads it once.
- The light lab reads `brightStarCataloguePath` (`assets/stars/bright_stars.csv`: the Yale Bright Star Catalogue to magnitude 6.5).

## Tests

`block_compression_test`, `texture_cooking_test`, `asset_manager_test`, `obj_loader_test`, `obj_uv_test`, `gltf_loader_test`, `gltf_attributes_test`, `gltf_texture_test`, `glb_texture_test`, `gltf_material_test`, `gltf_normal_map_test`, `texture_encoding_test`, `raw_image_png_test`, `stars_test`.
