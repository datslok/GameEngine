# Skies that look great: a physical base with art controls

Gathered Nov 18 2026 for phase 20 (the user: Red Dead Redemption 2's and The Witcher 3's skies "clearly aren't realistic" but look great, and looking great wins). Claims marked **[inference]** are ours; **[unverified]** rests on a summary, not the deck.

## What the engine has

Physical sky (Hillaire 2020's tables, `sky.md`), sun, moon with phases and catalogue stars (`sun-moon-stars.md`), froxel fog with sun shadows (`fog.md`), eye adaptation, AgX, white balance and grade (`camera-effects.md`). No art layer over the sky, no clouds, no grading by time of day or weather.

## Red Dead Redemption 2 (Bauer, SIGGRAPH 2019)

- Techniques "designed to both obey the physical properties and laws of light transport but, importantly, also provide powerful means for directability" ([Advances 2019](https://advances.realtimerendering.com/s2019/index.htm)).
- One scattering solution, shared by the view, reflection maps and the sky's irradiance probe grid: fog, clouds and ground lit by the same model, so they never disagree **[inference]**.
- Froxel volumetrics near, ray-marched far, blended; two cloud layers by height profiles under a 2D mask **[unverified]**.
- Light shafts from the march itself (shadowed in the volume), not a screen-space blur.

## The Witcher 3 (REDengine 3): hand-tuned

- Environment definitions: every parameter a curve over the hour (a value and a colour a key), layered (region, then quest or interior); weather presets (clear, fog, storm) blended ([REDkit](https://cdprojektred.atlassian.net/wiki/spaces/W3REDkit/pages/6325476)).
- Sky colours by hand: zenith, horizon, horizon falloff, sun glow size and colour, brightness ([Witcher 2 RedKit](https://redkitwiki.cdprojektred.com/environment+lighting.htm)).
- Sun: `pow(dot(view, sun), k)` blending the sky toward the sun's colour, k ≈ 56 a wide glow, ≈ 2400 the disc; the moon's corona alike ([reverse-engineered](https://astralcode.blogspot.com/2019/03/reverse-engineering-rendering-of_17.html), [series index](https://astralcode.blogspot.com/2018/11/reverse-engineering-rendering-of.html)).
- Fog with separate sun-side and away-side colours (near, middle, far), chosen by the angle to the sun: most of the dusk warmth.
- Up to three grading tables blended by weight (one frame: 20% table, 80% original); god rays as a half-resolution radial blur of the sky's bright pixels.

## Others

- **Ghost of Tsushima** (Patry 2021, the best "physical + art" reference, [Advances](https://advances.realtimerendering.com/s2021/jpatry_advances2021/index.html)): physical sky in their own colour space, artists adjust it globally; clouds a 768² map refreshed over 60 frames, the silver lining's backscatter x2.16; froxel fog 10 cm to 100 km in ~0.5 ms (PS4); sky ambient directionality boosted 25%; exposure from illuminance; white balance folded into the grading matrix for free; a Purkinje shift at night.
- **Horizon (Nubis, Schneider)**: a weather map (cover, type, rain) and height profiles per cloud type; Beer's law x a "powder" term, a phase lobe for the silver lining; ~2 ms, a 16th of the pixels a frame ([2015](https://www.advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf), [2022](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-NubisEvolved-NoVideos.pdf), [2023 fly-through](https://advances.realtimerendering.com/s2023/Nubis%20Cubed%20(Advances%202023).pdf)).
- **Unreal**: the physical model with named art multipliers: sky luminance, aerial perspective distance scale (thicker haze), light kept after sunset (minimum elevation), sun disc colour, the sun's size (0.5357° by default, raised by artists), cloud phase, multiple scattering octaves, ground bounce ([SkyAtmosphere](https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere-component-properties-in-unreal-engine), [clouds](https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-cloud-component-properties-in-unreal-engine)).
- **Frostbite** (Hillaire 2016): physical sky and clouds, multiple scattering by octaves, sun disc with limb darkening ([deck](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf)).
- **Forza Horizon**: real skies filmed as 24-hour HDR time-lapses (12K, ~30 days): photographic, no free time of day.
- **GTA V**: ~14 weather "timecycles", each keyed by the hour (sky, haze, sun, tint), region modifiers on top. GTA 6: nothing public.

## The pattern

- The physical model is kept; artists scale and tint it, never replace it.
- The sun enlarged (2-4x **[inference]**), limb darkened, a separate wide glow, bloom tuned so it haloes; the air's tint kept so sunsets stay right.
- Haze tinted toward and away from the sun; grading tables blended by time and weather.
- Authoring: keys an hour, one set a weather; current and target weather blended over a transition, region overrides on top.

## What this means for the engine **[inference]**

| # | Piece | Looks | Effort | Cost grows with |
|---|---|---|---|---|
| 1 | Environment presets: curves over the hour of art multipliers (sky and haze brightness, haze distance, sun-side and away tints, sun size, glow, exposure bias, grade), one set a weather, blended by weather and region | highest | medium | nothing (a frame's constants) |
| 2 | Volumetric clouds (Nubis): weather map, cloud types, silver lining, powder, shadows on ground and fog | very high | high | pixels |
| 3 | Art-directed sun and moon: size, limb darkening, a halo lobe, light kept after sunset | high | low | pixels of sky |
| 4 | Aerial perspective from the sky's tables with a distance scale and sun-side tint (already on phase 18's list) | high | low-medium | pixels |
| 5 | Grading by environment (blended grades or tables) | medium-high | low-medium | nothing |
| 6 | Light shafts: check the froxel fog's shadowed shafts read well first; a radial blur only if not | medium | low | pixels |
| 7 | Night: moon corona, Milky Way, Purkinje shift | medium | low-medium | pixels |

Presets (1) feed the regional weather map of phase 20, so they come first; settings live in their structs' `visit()` (F5, `settings.cfg`).
