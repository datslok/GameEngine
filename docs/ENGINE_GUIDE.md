# Engine guide

How the engine works and why it is built this way, for people learning it: concepts and reasons, with physics analogies, jargon defined the first time. For the exact names and numbers, each chapter links the matching file in `docs/architecture/`; for what comes next, see [ROADMAP.md](../ROADMAP.md).

**Covers:** the engine core, rendering, lighting and global illumination (phases 0 to 6 and 16). Not yet: cloth (phase 7, parked) and the forest's systems (phase 18: level of detail, terrain, wind); until their chapters exist, read their architecture files.

## Chapters

Read in order the first time; each stands alone after that.

| Chapter | What it explains |
|---|---|
| `docs/guide/foundations.md` | The layers, the frame loop and time, input, entities and components, assets, meshes and normals, cameras and quaternions |
| `docs/guide/gpu.md` | How a frame reaches the GPU: buffers, Vulkan, barriers, timing, compute, ray queries, textures, gamma, depth precision |
| `docs/guide/gpu-scene.md` | Drawing thousands of things cheaply: the GPU scene, culling, the depth prepass, clustered shading |
| `docs/guide/shading.md` | Lights and how surfaces answer them: physically based materials, clear coat, parallax occlusion mapping |
| `docs/guide/shadows.md` | Shadow maps, soft edges, the sun's cascades, contact shadows, coloured shadows, light priority |
| `docs/guide/sky-and-image.md` | The sun, moon, stars and real sky; eye adaptation, anti-aliasing, upscaling, the camera's finish |
| `docs/guide/bounce-light.md` | Global illumination: ambient occlusion, screen bounce light, sky visibility, the light probes |
| `docs/guide/reflections-and-fog.md` | Reflections from the screen, a probe and mirrors; mirrored lights; volumetric fog |
| `docs/guide/physics.md` | Shapes, rays, the broad phase, the static level, the character controller, particles |
| `docs/guide/tools.md` | Debug drawing, the F4 readout, the F5 settings window, commands |
| `docs/guide/testing.md` | Unit tests, determinism, golden images, the GPU tier, the docs test |

## Keeping it current

At the end of each phase (the `docs-review` skill): update the chapters the phase touched, add one for a new system, keep each under 30 KB (split past it), and update the Covers line above.
