# Texture compression

Research for phase 18's texture compression item: GPU block formats, how shipped engines choose them, encoders, and a plan for this engine. Written Nov 17 2026. Facts marked **(unverified)** come from secondary sources or forum posts.

## Why

- Every texture is `R8G8B8A8` today: 4 bytes a texel, 5.3 MB for a 1024² texture with its mips.
- The forest's files: 179 images (175 at 1024², 4 at 1024×2048), **976 MB** with mips if all are loaded. Code-made textures (impostor atlases, leaf clusters, ground, grass) come on top.
- The laptop has 8 GB of video memory, shared with the frame's targets, the scene, shadows and probes.
- Smaller textures also mean fewer bytes read per sample: faster texture-heavy passes **(gain unmeasured; to be measured, not promised)**.

## The block formats (BCn, core in desktop Vulkan behind `textureCompressionBC`)

Every format stores 4×4 texel blocks of fixed size; the GPU decodes them as it samples, so the memory saving holds on the GPU too (unlike PNG or JPEG).

| Format | Bytes / texel | Channels | Used for |
|---|---|---|---|
| BC1 | 0.5 | RGB (+1-bit alpha) | cheap colour; visible banding and block artefacts on smooth gradients |
| BC3 | 1 | RGB + separate alpha | older colour with alpha |
| BC4 | 0.5 | one | height, single masks |
| BC5 | 1 | two (each as good as a BC4) | normal maps (x, y; z rebuilt in the shader) |
| BC7 | 1 | RGB or RGBA, eight modes | high-quality colour, alpha cut-outs, packed masks |
| BC6H | 1 | RGB half float | HDR (not needed: no HDR textures here) |

- sRGB variants exist for BC1-3 and BC7 (`VK_FORMAT_BC7_SRGB_BLOCK`): the GPU decodes and filters in linear light as with `R8G8B8A8_SRGB` today.
- The GPU cannot blit into a block format, so **every mip level must be built on the CPU** before encoding (today most are blitted on the GPU).
- Levels under 4×4 still take one block.

## How shipped engines choose

- **Unreal:** normal maps BC5 by default, which drops blue and alpha ([UE 4.27 compression settings](https://docs.unrealengine.com/4.27/RenderingAndGraphics/Textures/TextureCompressionSettings)). An Epic answer: BC5 suits normals as two-channel data; BC7 is for three or four channels and packs normals worse **(forum)** ([answer](https://answers.unrealengine.com/questions/426083/view.html)). Colour, AO, roughness and metal default to BC1/BC3; BC7 is the high-quality choice ([cgbookcase guide](https://www.cgbookcase.com/learn/how-to-use-pbr-textures-in-unreal-engine)).
- **Unreal's Composite Texture:** normal-map variance (from its mips) baked into the roughness map's matching mips, so specular stays stable when a normal map's bumps average away ([docs 4.26](https://docs.unrealengine.com/4.26/en-US/RenderingAndGraphics/Textures/Composite)). Needs the same UV layout and at least as many mips. This is Toksvig moved from the shader into the texture.
- **Cooking:** engines compress at import or cook time and keep the result (Unreal's derived data cache); loading reads blocks straight into GPU memory.

## Encoders

The roadmap's principles name texture encoders as a library case (the learning is the formats, not the search for the best endpoints).

| Encoder | Licence | Notes |
|---|---|---|
| `bc7enc` / `rgbcx.h` (Rich Geldreich) | MIT or public domain | single files, plain C++; BC1-5 (`rgbcx`) and BC7; its author's 2018 tests put it at ispc_texcomp's fast speed unvectorised, better quality at slower settings ([repo](https://www.github.com/richgel999/bc7enc), [blog](https://richg42.blogspot.com/2018/04/bc7-showdown-ispctexcomp-vs-my-ispc.html)) |
| `bc7e.ispc` (in bc7enc_rdo) | Apache 2.0 | 2-3× faster than ispc_texcomp at the same quality **(author's numbers)**; needs Intel's ISPC compiler in the build ([repo](https://github.com/BinomialLLC/bc7e)) |
| ispc_texcomp (Intel) | MIT | needs ISPC; basic profile about 46.5 dB on the author's set **(author's numbers)** |
| Compressonator, NVTT, DirectXTex | various | large; DirectXTex's GPU path is Direct3D |

Encoding is a search per block: measured with bc7enc at quality 0, 0.03-0.06 s per 1024² on the laptop's 16 threads, about 180 file textures plus the made ones at every start without a cache, so the result is cached on disk.

## Normal maps and Toksvig

- The engine's Toksvig (`normal_mapping.glsl`, `toksvigRoughness`) reads how short a mip's averaged normal is.
- **BC5 loses that:** only x and y are stored, z is rebuilt so the normal is always unit length.
- Ways out: (a) BC7 for normals, x y z kept, Toksvig unchanged, x and y somewhat coarser than BC5; (b) BC5 plus the length baked elsewhere, as Unreal's Composite Texture (into a roughness texture, which not every material has, with the same UVs); (c) normal maps left uncompressed.

## Plan (for the user's go)

1. **Formats by role:** colour, emissive, cut-out colour, ORM, impostor atlases BC7 (sRGB for colour); normal maps per the choice above; single-channel height or transmission BC4. Code-made data textures read as exact numbers (ground weights, strata, streaks, variation) stay `R8G8B8A8`.
2. **Mips on the CPU** for every compressed texture: a linear-light box filter (sRGB decoded), coverage-keeping for cut-outs as today, normals averaged without renormalising (Toksvig's length kept).
3. **Encoder:** `bc7enc` + `rgbcx.h` vendored (no new compiler), run in jobs (`core/job_system`), a texture per job.
4. **Cache:** blocks of every level in a file under `build/texture_cache/` (git-ignored), named by a hash of the RGBA input, size, encoding, role, cutoff and an encoder version. Code-made textures are seeded, so they cache too. First start encodes; later starts skip PNG decode, mips and encode.
5. **Upload:** `VulkanTexture` copies the levels in (the path cut-outs already use), with the BC format; the white stand-in unchanged.
6. **Settings:** a texture compression switch in the render's texture settings (`visit()`, so F5, `settings.cfg` and `--set` reach it), read at load: off keeps today's path, for side-by-side checks. Fallback to uncompressed if the GPU lacks `textureCompressionBC`.
7. **Tests first:** encode-decode round trips (error bounds per format), CPU mips (sRGB-correct, coverage, normal length), cache keys (each input changes the name), format choice by role, the settings name.
8. **Measure:** video memory before and after (F4), GPU frame time in the jungle, start-up time cold and cached.
