# The reflection probe's capture cost

Why the camera's reflection probe costs so much in the forest, and how shipped engines keep a probe cheap. Plan for phase 22; how the probe works today: `docs/architecture/reflection-probe.md`.

## Measured (home PC, release, hidden copy, three samples, GPU ms)

| Spot | Frame | CUBE at 256 |
|---|---|---|
| rim | 18.9 | 3.8 |
| ring | 39.4 | 17.7 |
| wall | 33.3 | 21.5 |
| trees (`forest-climber`) | 37.2 | 16.1 |

The ring by face size: 64 1.9 ms, 128 5.9 ms, 256 17.5 ms (256 again 17.4); the faces' triangles 25-158K at every size.

- **Pixels, not triangles:** each halving cuts the time about three times while triangles stay the same; under 1 ms is fixed (finish passes, draws).
- **About 270 ns a probe texel against about 7 ns a screen pixel** (the main pass, 1.6M pixels in 11 ms).
- **Why:** captures have no depth prepass, the cut-out leaf cards stack many layers looking into a canopy, and every layer runs the full surface shader over every light (`probe_capture.frag`). The rim looks out over the valley, where fewer cards fill the faces.
- The default went to 128 until this phase (`probe-size-128`).

## How others do it

| Engine / game | What a per-frame cube draws | Numbers |
|---|---|---|
| Unreal, real-time sky light | Only sky, atmosphere, clouds, fog and "is sky" meshes; no scene geometry | 128Â² example: 1.47 ms whole (sky and clouds 0.60, mips 0.05, convolution 0.80), sliced over frames by default |
| Unreal with Lumen | No cube: screen traces, then the Surface Cache (a simplified lit scene) | captures only a fallback with Lumen reflections off |
| Unreal without Lumen (from memory, unchecked) | Hand-placed Reflection Captures, captured at build or load; SSR on top; Scene Capture Cube exists as a gameplay tool, a full render | â€“ |
| GTA V | A low-poly world every frame: terrain, sky, some buildings; no characters or cars | 128Â² a face, about 30 draws a face, then a dual paraboloid |
| Far Cry 4 (McAuley 2015), Witcher 3 | Albedo, normal, depth captured once into a small G-buffer cube; relit at runtime for sun, sky, time of day; mips remade | relighting is a flat pass over texels, no geometry |
| Final Fantasy XV | Calls the relit G-buffer expensive; blends probes baked under different conditions | â€“ |
| Unity real-time probes | The full scene, a face a frame and then mips (14 frames), or all faces at once (9) | â€“ |
| Forza | Dynamic cubes for car reflections, the sky's light every frame | pipeline unpublished; players see a frame of lag |

## Options for phase 22

1. **A cut-down capture world** (GTA V): faces draw terrain, rocks, structures, bark and the crowns' impostor cards; no leaf cards, understorey, grass or floor cover. Cost grows with face texels at about one shaded layer each. A per-view flag in the capture culling.
2. **Capture rarely, relight every frame** (Far Cry 4): faces store albedo, normal and depth, redrawn only after the camera moves a set distance; a compute pass a frame lights the cube's texels (sun, sky, shadows, near lights), then mips and prefilter. Cost grows with how far the camera moves. New images, a relight shader and its mirror.
3. Smaller levers: a depth prepass for faces; the camera's light lists or the sun alone in captures.

Not for us: sky only (loses the forest in water and glass reflections); baked hand-placed captures (lighting must be automatic); Lumen's Surface Cache (a whole GI system).

## Sources

- Unreal, Sky Lights (real-time capture, cost): https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-lights-in-unreal-engine
- Unreal, Reflections Environment (captures a fallback with Lumen): https://dev.epicgames.com/documentation/unreal-engine/reflections-environment-in-unreal-engine
- Unreal, Lumen technical details: https://dev.epicgames.com/documentation/unreal-engine/lumen-technical-details-in-unreal-engine
- r.SkyLight.RealTimeReflectionCapture.TimeSlice (third-party CVar wiki): https://indxzero.github.io/ue544cvarwiki/articles/r.skylight.realtimereflectioncapture.timeslice
- CourrÃ¨ges, GTA V graphics study: https://www.adriancourreges.com/blog/2015/11/02/gta-v-graphics-study/
- McAuley, Rendering the World of Far Cry 4 (GDC 2015): https://www.gdcvault.com/play/1022235/Rendering-the-World-of-Far
- Rendering Techniques of Final Fantasy XV (SIGGRAPH 2016): https://www.jp.square-enix.com/tech/library/pdf/s16_final.pdf
- GameDev.net, relit G-buffer probes, Witcher 3: https://www.gamedev.net/forums/topic/684163-ibl-cubemap-blending-and-dynamic-daytime/5322058/
- Unity, reflection probe performance: https://docs.unity3d.com/Manual/RefProbePerformance.html
- Forza Motorsport 7 dynamic cube mapping: https://gamingbolt.com/?p=303985
