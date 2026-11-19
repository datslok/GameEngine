# The structures, half reclaimed

Research for ROADMAP phase 18's "The structures, half reclaimed": the Monolith (600 m), the Ring (400 m, on edge, sunk to its waist), the far towers, searchlights. Today they are generated boxes and a torus (`valley_shapes`). Concept sheets (code-painted mood sketches): https://claude.ai/artifact/3EUTrgyrfZWKjyGX7F9fAS (private).

**Direction (the user's choices, Nov 19 2026, replacing the stealth-alien look):** cyberpunk and brutalist, very imposing (references: tall slabs stepping in, vertical fins and deep slots, a heavy head or crown, legs and buttresses at the foot; concrete and dark metal, formwork panels, rust and stains; thousands of small windows, light strips, neon, warning lights; a small city or camp at the foot, fog, flying craft, dusk and night). The megatower rises out of the jungle, which stays; running and lit at night; the Monolith and the far spires redone in the new style, the Ring kept as the alien relic the city was built around. Still kept from before: a research camp in warm light for scale, the gas giant. The stealth-slab notes below stay as research for the Ring; the new pieces (brutalist massing, windows at scale, cyberpunk light) need their own research.

"src" = from the linked source; "practice" = common practice, no primary source found: numbers to tune; "recalled" = from memory, not re-checked.

## What makes 600 m read as 600 m

- **Human-sized parts next to huge ones** (src): catwalks, rails 1.1 m, a walkway on the truss bridge (members 1-3 m). The canopy (30-60 m) is the free ruler at the base. Scanlon, Radsby, Kinsey on 80.lv: [hydro plant](https://80.lv/articles/off-world-hydro-plant-building-a-unique-world-in-ue4), [scale and mood](https://80.lv/articles/simple-stylized-environment-scale-and-mood/), [epic scale](https://80.lv/articles/achieving-epic-scale-in-environment-design) (a 1.386 km ring city against a 1.8 m figure).
- **Repeated units of known size** (practice): windows 1.5-3 m, doors 2-2.5 m, floor bands 3-4 m; grouped into modules (setbacks every 50-150 m) so they do not read as a fine tile. The eye counts them.
- **Atmospheric perspective** is the strongest cue at km range: contrast falls, colour goes to the sky's. Uncharted 4's "mip fog" took fog colour from a blurred sky mip ([SIGGRAPH 2016](https://advances.realtimerendering.com/other/2016/naughty_dog/index.html)). We have the fog; the work is tuning.
- **Detail where the eye lands** (src, Kinsey): highest texel density on the most-seen edges; 4 atlased 1024 px materials for a whole city.

## The detail hierarchy

| Layer | On a 600 m slab | Made as |
|---|---|---|
| Large | setbacks, buttresses, slots, the truss | silhouette geometry |
| Medium | plates 4-20 m, bevels 0.05-0.3 m (practice) | real geometry from a generator |
| Small | split lines, vents, hatches, bolts | tiling materials, trims, decals |

- **Star Citizen** (src, [80.lv](https://80.lv/articles/secrets-of-industrial-design-in-games)): one master shader with ~25 tiling sub-materials, ~512 px/m exterior, split lines and panels as POM decals, vertex colour revealing damage. The Idris moved from floating panel-line cards to baked panel lines + a detail map ([McKnight](https://neilmcknight.artstation.com/projects/3oRLNg)).
- **City Sample** (src): Houdini, 24 modular kits of ~2,000 meshes, buildings as instanced components ([CG Channel](https://www.cgchannel.com/2022/04/download-epic-games-free-city-sample-assets-for-ue5), [80.lv](https://80.lv/articles/breakdown-creating-the-matrix-awakens-in-houdini-unreal-engine-5)).
- **For us:** a C++ generator splitting each face into plates (recursive split, depth jitter 0.1-1 m) and placing instanced greebles by rule (pipes along setback edges, lights at plate corners). Greebles leave by LOD past ~300-500 m, where a macro normal map carries them. Cost grows with instances drawn (GPU-culled already).
- **Two-scale texturing** (practice): a macro layer over the whole face (dirt gradients, bleaching, per-plate tone) over a tiling detail layer. One or two extra samples a pixel.
- **Mapping:** generated slabs are axis-aligned, so one world-space planar projection a face (1 sample) instead of triplanar (3); triplanar stays for rock.

## Weathering and moss

- **Up-facing blend** (biggest gain per line): mask = saturate(dot(N, up) - threshold), sharpened by the height map so moss fills cracks first, times AO and a large noise. Uncharted 4 shades moss as its own material (sheen, AO fresnel, wrap; SIGGRAPH 2016 above). TLOU2 vehicles blend moss "procedurally based on angle", every shader follows one wetness value ([Trevelyan](https://trevelyan.artstation.com/projects/NxaD0z)). Naughty Dog also blends by world height ([80.lv](https://80.lv/articles/building-environments-for-uncharted-the-lost-legacy/)). For us: moss also thickest near the jungle floor, fading with height. Constant cost a pixel; fits "emergent lighting": no hand painting.
- **Edge wear** (practice): the generator knows every edge, so convexity is exact; chip the clear coat where convexity x noise > threshold (bare metal loses the coat lobe). Destiny: primary, secondary and worn dye layers with wear remaps ([GDC 2017](https://advances.realtimerendering.com/destiny/gdc_2017/index.html)).
- **Streaks under edges** (practice): distance below the nearest ledge (analytic on generated plates) x a vertically stretched noise (1:20), fading over 5-30 m; rust under bolts, dark water under ledges and slots.
- **Decals:** DOOM 2016 clustered decals from one 8k atlas, up to 4,000 in view ([Courrèges](https://adriancourreges.com/blog/2016/09/09/doom-2016-graphics-study)). Mesh decals (strips on the hull) fit still structures better: no projection. Cost grows with decal pixels.

## Plants on structures

- **Placement:** Horizon places 500+ asset types on the GPU from density maps and rule graphs ([GDC 2017](https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf); slides not read, >10 MB). Unreal PCG: Poisson samples on the mesh, filtered by normal, slope and tags ([80.lv](https://80.lv/articles/leveraging-unreal-engine-5-s-pcg-to-scatter-meshes-on-meshes)). For us: the moss shader's mask is the scatter density, so ferns sit where the moss is (the main fix for "pasted on"). Cost grows with area near the camera.
- **Vines:** Luft's Ivy Generator: grow by summed forces (last direction, random, adhesion to the nearest surface, up), gravity when nothing is in reach ([Konstanz](https://graphics.uni-konstanz.de/~luft/ivy_generator/); Unity port [Hedera](https://github.com/radiatoryang/hedera)). Hanging hundreds of metres: few thick (0.1-0.5 m) catenaries or chains from ledge anchors, bundled, leaf fringe only near; our lianas and `wind-hanging` already do the hanging and swing. Vines must wrap edges and darken the surface under them (a contact mask).
- **Ground contact:** rubble, soil and ferns where the Ring meets the ground plus a height-band moss blend; a hard intersection line is the commonest giveaway.

## Lights at scale

- **Count sells scale, not brightness:** hundreds of small lights in a rhythm (slots, beacons) are a ruler; beacon blinking is motion at distance.
- **Strips that cast light** (practice): real clustered lights near the camera only; further, emissive + bloom + screen-space GI.
- **Far lights as sprites** (practice): camera-facing quads at least 1-2 px, dimming gently with distance, fogged; GTA V's coronas ([Courrèges](https://www.adriancourreges.com/blog/2015/11/02/gta-v-graphics-study-part-3/)). Cost grows with lights visible; thousands is trivial.
- **Lit windows without rooms:** interior mapping (a ray against virtual room planes, lit flag by hash; [van Dongen](https://joostdevblog.blogspot.com/2018/09/interior-mapping-real-rooms-without.html)). Few ALU a window pixel.

## Distance

- **No impostors for the giants:** their silhouette changes with parallax over km. Keep geometry, fold plates and greebles into the normal map by level.
- **Far towers:** low-poly with flattened shading in the haze; octahedral impostors (Fortnite 12x12 frames, [Epic](https://dev.epicgames.com/documentation/unreal-engine/impostor-baker-plugin-in-unreal-engine)) only if needed.

## Searchlights: beams kilometres long

**The integral.** Point light, no extinction: ray `o + t d`, light `s`, `b = d·(o-s)`, `h² = |o-s|² - b²`: `∫ I/(h²+(t+b)²) dt = (I/h)[atan((T+b)/h) - atan(b/h)]` (exact; but km beams have optical depth ≥ 1, so extinction matters).

- **Sun et al. 2005** ([PDF](https://cave.cs.columbia.edu/old/publications/pdfs/Sun_TOG05.pdf)), homogeneous medium with extinction: `T_sv = β D_sv`, `T_vp = β D_vp`, `γ` the angle between ray and light; `A0 = β I0 e^(-T_sv cos γ) / (2π T_sv sin γ)`, `A1 = T_sv sin γ`; `L = A0 [F(A1, π/4 + ½ atan((T_vp - T_sv cos γ)/(T_sv sin γ))) - F(A1, γ/2)]`, `F(u,v) = ∫₀^v exp(-u tan ξ) dξ` in a 64x64 table (< 2 % error; single scattering within 4 % of Monte Carlo for `T_sv < 2`). Cone sources: Geiger et al. 2023 ([PDF](https://www.cs.princeton.edu/~fheide/papers/SourcesInFog.pdf), not read).
- **Exponential height fog has no closed form** for in-scattering (exp of exp times 1/r²). Closed: optical depth along any segment, `τ = (σ0/k) e^(-k(o_y-y0)) (1 - e^(-k d_y t)) / d_y` ([Quilez](https://iquilezles.org/articles/fog/)); our fog already uses it.
- **Equiangular sampling** (Kulla and Fajardo 2012, recalled): `t = h tan θ - b`, θ uniform between the clip angles, removes the 1/r² peak exactly; what is left (density, both transmittances in closed form, phase, spot falloff, shadow) is smooth, so 8-16 nodes converge. The practical near-analytic answer in height fog.
- **Bounds:** exact ray-cone intersection (apex `s`, axis `a`, `c = cos half-angle`, `CO = o - s`: `A = (a·d)² - c²`, `B = 2[(a·d)(a·CO) - c²(d·CO)]`, `C = (a·CO)² - c²|CO|²`; keep roots with `a·(x-s) > 0`; camera inside the cone and `A ≈ 0` are special cases; [Quilez intersectors](https://iquilezles.org/articles/intersectors/)), clipped to scene depth and the far cap.

**Shadows in beams.**

- Epipolar sampling (Engelhardt and Dachsbacher 2010) and 1D min-max mipmaps (Chen et al. 2011, 55 fps; [MIT](https://groups.csail.mit.edu/graphics/mmvs/); Intel's version: Yusov, GDC 2013): lit segments in O(log n), analytic integral each.
- Polygonal light volumes (Billeter et al. 2010, [PDF](https://www.cse.chalmers.se/~uffe/volumetricshadows.pdf)): a mesh displaced by the shadow map, exact to its resolution; ~90 fps for two lights on a GTX 480. NVIDIA's volumetric lighting (Fallout 4, Arkham Knight PC) follows it (recalled).
- Killzone Shadow Fall ([GDC 2014](https://www.guerrilla-games.com/media/News/Files/GDC2014_Valient_Killzone_Graphics.pdf)): per-light march at half resolution, jittered start, bilateral upsample, last frame reused.
- Froxels (AC IV ~1.1 ms total, [Wronski](https://www.advances.realtimerendering.com/s2014/wronski/bwronski_volumetric_fog_siggraph2014.pdf); [Frostbite](https://www.ea.com/frostbite/news/physically-based-unified-volumetric-rendering-in-frostbite)): 64 slices over km are tens to hundreds of metres deep: too coarse for these beams. Screen-space radial blur needs the source on screen: wrong for searchlights seen from the side.
- Mesh cone beams (UDK, CryEngine, Unity's Volumetric Light Beam): additive cone with edge, depth and near fades; fine from the side at km range, fail inside or along the beam, ignore occlusion and fog density, alias when thin.

**Phase and density.**

- Henyey-Greenstein `p = (1-g²) / (4π (1+g²-2g cos θ)^1.5)`; Schlick `k ≈ 1.55g - 0.55g³`, `p = (1-k²) / (4π (1-k cos θ)²)`. At g = 0.8, along the beam is ~260x brighter than side-on, plus path length grows as 1/sin. Mist droplets g ≈ 0.7-0.9; games lower it or blend two lobes so a beam at the camera does not blind.
- Koschmieder `σ = 3.912 / V`: haze (V 2-10 km) 4e-4 to 2e-3 /m, mist (1-2 km) 2e-3 to 4e-3, dense valley fog (100-200 m) 0.02-0.04. A beam seen along 5 km: σ ≈ 2e-4 to 1e-3 (optical depth 1-5), height falloff 1/100-1/300 m.

**What the cost grows with.**

| Approach | Grows with |
|---|---|
| Analytic / equiangular over the clipped segment | pixels in the cone hull x lights x nodes (8-16); not beam length |
| View-space ray march | pixels x steps; steps grow with length |
| Epipolar + 1D min-max | lines x samples + shadow map x log n |
| Polygonal light volume | shadow map resolution + pixels x transitions |
| Froxels | grid size x lights; fixed, coarse at range |
| Mesh cone | covered pixels only |

**Recommendation (1-8 searchlights, beams to ~5 km):** a low-poly cone hull per light rasterised so only covered pixels work; per pixel the exact cone clip and 8-16 equiangular nodes with closed-form height-fog transmittance, HG phase, spot falloff and a shadow lookup; half resolution, blue-noise jitter, depth-aware upsample, TAA. Shadows: the apex is fixed and only the direction sweeps, so one depth cube (or dual paraboloid) a light for still casters, redrawn only when they change (destruction), plus a small per-frame map for movers in the cone (1024² over 90° ≈ 7.7 m a texel at 5 km: pillars and structures, not leaves). Searchlights stay out of the froxels so light is not counted twice. Min-max traversal only if shafts step behind thin pillars; mesh cones only as a far fallback.

## The stealth slabs

[V] read in a primary source, [S] secondary or search snippets, [K] background knowledge, not checked.

- **Three scales of detail** [K]: facets 80-110 m, chamfers, ridge and shoulder 5-20 m, panels and seams 2-3 m. A lone 100 m facet with nothing smaller reads as a 1 m box.
- **Precedent** [S]: Destiny 2's Vex palette layered a Vex alpha over solid metal for depth ([80.lv](https://80.lv/articles/environment-design-of-destiny-2/)); Control's brutalism with many concrete variants ([Game Developer](https://www.gamedeveloper.com/design/exploring-the-world-driven-game-design-of-i-control-i-)); Halo 4's Forerunner metal criticised as too clean, so macro wear matters.
- **Edges:** mid-poly single-segment chamfers with face-weighted normals keep faces flat and catch a highlight ([polycount](https://polycount.com/discussion/comment/2500571)); Sunset Overdrive bakes 45 degree bevels into a trim normal map ([GDC 2015](https://gdcvault.com/play/1022323/The-Ultimate-Trim-Texturing-Techniques)). At 1 km a 0.1-0.3 m chamfer is sub-pixel: far facets read by their tone and sky reflection, not bevels [K].
- **Per-facet variation** [K]: albedo ±10-15 % and roughness ±0.05-0.1 by facet, or coplanar facets merge; macro streaks down each facet, darker at the jungle line.
- **Meshes** [K]: vertices split per face (about 3x a smooth mesh, a few thousand triangles a slab: negligible). The engine's `buildIndexedMesh` already keeps hard edges; `Mesh` triangles without corner normals are flat.
- **Seams** [K]: geometry only for silhouettes and sawtooth breaks of 5 m or more; panels and seams as a tiling detail normal and roughness map, through Toksvig; parallax only within ~30 m. Decals as DOOM 2016 (clustered, 8k atlas, up to 4,000 in view [S]) only for unique markings.

## The matte coating

- Unreal's table: charcoal and fresh asphalt 0.02 linear, specular 0.5 = F0 0.04, never zero the specular, raise roughness instead [V: [UE docs](https://dev.epicgames.com/documentation/unreal-engine/physically-based-materials-in-unreal-engine)]. Matte black paints reflect ~2-6 % [S: [SPIE 2022](https://instrumentation.tamu.edu/media/files/SPIE2022/2022%20SPIE%2012188-177%20-%20Reflectivity%20charcacterization%20of%20various%20black%20and%20white%20materials.pdf)]; radar-absorbent paint is often grey (the B-2) [S].
- **Choice** [K]: base 0.03-0.05 linear (sRGB ~50-60), F0 0.04, roughness 0.55-0.75 varied by facet; no sheen.
- **Readability** [K]: at albedo 0.04 the specular is as strong as the diffuse, so the sky's reflection, changing with facet angle, shows the form; specular occlusion and sky light matter most. Risk: in shadow, facets merge into black. Help: per-facet roughness, the lines lighting nearby, aerial perspective, exposure set by the jungle.

## Straight light lines

- **Pixel size** [K]: at 1080p and 60 degree field of view a pixel is ~1 m at 1 km; a 0.3 m line is 0.1-0.3 px.
- **Phone-wire AA** [V: [Humus](http://www.humus.name/index.php?page=3D&ID=89)]: never narrower than 1 px; scale intensity by true width / drawn width, so light per unit length is conserved and a far line neither flickers nor vanishes. Under TAA/DLSS ~1-1.5 px of the internal resolution, or neighbourhood clamping drops it [K; Salvi GDC 2016 [S]].
- **Options** [K]: (a) camera-facing emissive strips widened in the vertex shader (cost grows with segments; must write motion vectors); (b) analytic distance-to-line in the facet shader with `fwidth` coverage (cost grows with shaded pixels; barycentric edges via `VK_KHR_fragment_shader_barycentric`); (c) texture masks (blur up close).
- **Bloom** [V: [CoD AW](https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/)]: mip pyramid, 1/(1+luma) weighted first downsample against fireflies, 3x3 tent upsample, no threshold, blend ~0.04; it spreads a conserved sub-pixel line over pixels and hides residual shimmer.
- **Lines as lights:** Karis tube lights (closest point on the segment to the reflection ray, sphere-light normalisation; the course slides' closestPoint has a sign error [S: [UIUC](https://cs418.cs.illinois.edu/website/text/ue4-pbr.html)], ~2x a point light [K]); Heitz-Hill LTC line lights (SIGGRAPH 2017, 2 fetches a pixel a light; slides not read). Only lines within ~50-150 m as lights, chosen by projected size; far ones emissive only [K].

## The research camp

- **CC0 props, Poly Haven** [V via its API]: `flood_lights`, `portable_searchlight`, `security_light`, `portable_generator`, `exterior_aircon_unit`, `power_box_01`, `utility_box_01`, `security_camera_01`, `modular_electricity_poles`, `modular_electric_cables`, `plastic_crate_01`-`03`, `wooden_military_crate`, `Barrel_01`, `metal_jerrycan`, `propane_tank`, `concrete_road_barrier`. No containers, tents, domes or masts.
- **Kenney** ([Space Kit](https://kenney.nl/assets/space-kit), [Survival Kit](https://kenney.nl/assets/survival-kit)) and **Quaternius** ([Sci-Fi Essentials](https://quaternius.com/packs/scifiessentialskit.html)): CC0 [V] but stylised, likely to clash with the scanned look.
- **Generated** [K]: modules and containers as boxes with corrugation (20 ft = 6.06 x 2.44 x 2.59 m), a geodesic dome (frequency 2-3), a 15-30 m lattice mast, floodlight towers (a pole and 4-6 panels, each a spot light), a pad. A few hundred triangles each; cost is the 4-8 shadowed spot lights (lights x pixels covered).

## The gas giant

[K] = background knowledge, not checked.

- **Ways to draw it:** a skybox (no phase, memory over the whole sky); real geometry (orbit-to-ground, needs camera-relative LOD: phase 13); an **analytic body in the sky shader** (ray-sphere planet, ray-plane ring, exact silhouette and phase; cost grows with covered pixels). Unreal: two atmosphere lights, sun and moon ([docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere-component-in-unreal-engine)); per-pixel transmittance is opt-in ([Hillaire 2020](https://blog.selfshadow.com/publications/s2020-shading-course/hillaire/s2020_pbs_hillaire_slides.pdf)) but needed across a 25 degree disc.
- **Shading:** Minnaert `L = L0 μ0^k μ^(k-1)`, k 0.9-1 [K]; full-phase `L0 = p E_sun / π`, geometric albedo 0.538 (Jupiter), 0.499 (Saturn); a rim haze term. Night side black except ringshine (~1-5 % [K]).
- **Shadows, analytic:** ring on planet `T = exp(-τ(r) / |μ_sun|)` at the ray-ring-plane hit; planet on ring by ray-sphere toward the sun.
- **Rings:** slab single scattering (lit and unlit sides [K]); main rings backscatter (g -0.66 to -0.77, [EPSC-DPS 2011](https://meetingorganizer.copernicus.org/EPSC-DPS2011/EPSC-DPS2011-549-1.pdf)), dusty rings forward-scatter (two-term HG 0.7/-0.5, [Hedman and Stark](https://arxiv.org/pdf/1508.00261)), so backlit rings glow. Profile ([Cuzzi et al. 2024](https://link.springer.com/article/10.1007/s11214-024-01104-y)): C 1.24-1.53 Rs (τ 0.05-0.1), B 1.53-1.95 (1 to >5), Cassini Division 1.95-2.03 (0.1-0.2), A 2.03-2.27 (~0.5); a 1D RGBA texture of colour and τ, 2-8K texels.
- **Through our sky:** `L = L_planet · T_view + S_inscatter`, T from the transmittance table per pixel; drawn at far depth so terrain, fog and clouds cover it for free. Daylight washes it out by itself: blue sky 3,000-8,000 cd/m2 [K] against ~20,000 at 1 AU or ~750 at Jupiter's distance.
- **Bands:** [Solar System Scope](https://www.solarsystemscope.com/textures/) `8k_jupiter`, `8k_saturn`, `8k_saturn_ring_alpha` (CC BY 4.0, credit needed); NASA [PIA07782](https://photojournal.jpl.nasa.gov/catalog/PIA07782) (Cassini Jupiter map). A 25 degree disc at 4K is ~830 px, so 4096x2048 BC7 (10.7 MiB with mips) is enough, 8192 for zoom; `textureGrad` across the longitude seam. Flow-noise detail later.
- **Real sizes** (2 asin(R/a)): Jupiter from Io 19.5, Europa 12.2 degrees; Saturn from Dione 18.4, Tethys 23.6, Enceladus 29.3 degrees. Regular moons orbit in the ring plane, so from them rings are edge-on; an open ring needs an inclined orbit (a deliberate fiction). Tidally locked: the planet stays put, its phase cycles once an orbit.
- **Planetshine:** `E = p E_sun (R/d)² Φ(α)`, `Φ = (sin α + (π-α) cos α)/π`: a 20 degree full disc gives ~70 lux at 5.2 AU, ~1,900 lux at 1 AU (full moon 0.25 lux): a real second directional light with a wide source. Eclipses: circle-circle overlap of the discs on the CPU, multiplying the sun.

## Planned: the Bastion

The user's choices (Nov 19 2026): concept A2, the terraced Bastion (sheets above); stages in the order mass, windows, lights; the strut rebuilt as an armoured bridge.

- **Shape kit (`bastion-mass`):** every part a convex prism or a loft of two convex sections (box, chamfered box, battered glacis, wedge corbel); a quad's two triangles and a cap's fan share one pair of facet numbers, so a flat face keeps one tone. Fins on each face of a tier: the face's flat span W at pitch p gets an odd n = 2 round((W / p - 1) / 2) + 1 fins at W / n, so they never end in a half fin and stand symmetric; every 4th from the middle a major fin.
- **Numbers (the approved sheet, the research where it had none):** 610 m; glacis 0-80 m, 1.45 x the shaft's width at the ground; tiers to 0.49, 0.77, 0.92 of the height at widths 1, 0.75, 0.55 of 200 m, depth 0.75 of the width; head 0.92-1.0, overhanging 30 m a side on 45 degree corbels under the major fins; fins every 7 m, 1.5 m wide, 2.5 m deep, major 6 m wide, 8 m deep. Far towers: the spires' places and heights, width height / 4-8, fins only, every 24 m.
- **Concrete:** albedo 0.25, roughness 0.9, the seam map in formwork: a 16.5 m tile of 2.75 m pours, panels a seventh of the tile staggered by half a panel row on row, tie holes on a grid of about 0.6 m. The Ring keeps the stealth coating and its sawtooth seams.
- **Bridge:** a chamfered box 14 x 8 m with parapets from inside the pillar into the second tier's side, 331 m up, walked on.
- **Cost grows with:** nothing per frame beyond drawing a few tens of thousands of triangles, still and GPU-culled.

## Planned: obstruction lights

The user's choices (Nov 19 2026): sprites plus small real red lights on the concrete, far towers blinking too, an LED flash.

- **The rules** [V]: medium-intensity flashing red (FAA L-864, ICAO type B), 2000 cd, 20-40 flashes a minute (the FAA now asks 30 ([L-864](https://flightlight.com/what-is-an-faa-l-864-obstruction-light/))); LED units about 0.5 s on (one: 0.5 s on, 2.5 s off ([AVlite](https://manualedistruzioni.it/avlite/lighting-equipment/av-ol-imb/supplement/1cryt))), older up to 1.5 s; every light on a structure in unison ([Clampco](https://clampco.it/obstruction-light-medium-intensity-type-b-miolb-m1_xxs/)); levels at most about 105 m apart, seen from every side.
- **Drawn as the stars are** (`sky.md`, Stars): illuminance at the eye E = I T / d^2 (T the fog's transmittance) shared by a tent over a pixel either side, so it keeps its light at any distance and never flickers; past a pixel or two the lens's disc at its own luminance (2000 cd over a 15 cm lens about 28,000 cd/m2), bloom giving the glow. Fixed-size sprites (Flight Simulator's) are the failure ([forum](https://forums.flightsimulator.com/t/airport-and-general-light-sprites-are-not-fixed-yet-general-night-lighting-thread-as-well/752834)); X-Plane shrinks the bulb; the old OpenGL point-size attenuation the same idea ([SIGGRAPH 1998 notes](https://web.tecgraf.puc-rio.br/~ismael/Cursos/Cidade_CG/labs/OpenGL/OpenGL_siggraph1998/node157.html)).
- **Plan:** a `Beacon` component (colour, candela, lens radius, flash period, on-time, phase); the flash from the simulation clock in a mirrored library; drawn after the fog is applied, fogged at its own point, in the reactive mask; the Bastion's levels at its tiers' corners, its own phase, the far towers each their own rate and phase; a small unshadowed red point light at each Bastion beacon, pulsing with it.
- **Cost grows with:** beacons on screen (six vertices each); the real lights with the pixels they reach. Later: a glow scattered in the far haze, with the searchlights (stage 14).

## The military megatower (direction v5)

Concept sheets (four concepts: Bastion, Spear, Bulwark, Twin Pylons): https://claude.ai/artifact/KfyixvEX1qjCe2cwvLAW2z (private). [V] read in a primary source, [S] secondary or snippet, [K] background knowledge, not checked.

**Precedent.**

- Blade Runner 2049 (DNEG): "heavy top" buildings, a slab on a column; scales leap-frogging (base, a much larger middle, larger still); signage kept sparse; thick even fog, areas pulled into it on purpose ([vfxblog](https://vfxblog.com/2017/10/26/lets-move-to-la-in-2049)) [V]. Deakins: London brutalism "the key" [S].
- Control's Oldest House: 33 Thomas St, windowless, "20th century fortress", long vertical concrete indents ([Game Developer](https://www.gamedeveloper.com/art/the-real-buildings-that-inspired-i-control-i-s-oldest-house)) [V]; 33 Thomas St is 170 m, 29 floors of 5.5 m, six protrusions for ducts and stairs, vent openings at floors 10 and 29 ([Wikipedia](https://en.wikipedia.org/wiki/33_Thomas_Street)) [V].
- Cyberpunk 2077: big shapes first, detail grouped, negative space for the eye to rest; "corporate neomilitarism" one of its four eras ([80.lv](https://80.lv/articles/interview-how-cyberpunk-2077-s-night-city-was-built-almost-entirely-by-hand/)) [V].
- Pattern [K]: one dominant silhouette, a heavy top, a stepped or battered base, detail clustered at base and crown, plain shafts between, fog for depth.

**Military, not residential.**

- Flak towers: 39-54 m tall, 43-71 m square, walls to 3.5 m, gun platforms at the corners ([Wikipedia](https://en.wikipedia.org/wiki/Flak_tower)) [V]. Cheyenne Mountain tunnels 14 x 18.4 m, 25 t blast door [V]. PAVE PAWS array faces 22.1 m, tilted 20 degrees [V]; Fylingdales radomes 40 m [V].
- Rules [K]: openings under 5-10 % of the wall (25-40 % residential), only in bands at command and plant levels (slits, a visor band); a 5-6 m plant rhythm, not 3 m floors; battered base walls 8-15 degrees, 45 degree chamfers; bolted plates, masts and sensors at the crown, hazard stripes and stencils at doors and pads.

**Generator parameters for the 600 m tower** [K, from the above]:

| Part | Numbers |
|---|---|
| Plinth | bastion 45-60 m tall, 1.3-1.5 x the shaft's width, battered 8-15 degrees, corner platforms cantilevered 8-12 m |
| Setbacks | 3-4, near 0.35, 0.60, 0.82 of the height, each 6-12 % of the width a side, a heavy band 4-6 m |
| Fins | minor every 6-8 m, 1.5-3 m deep; every 3rd-4th a major fin 4-8 m wide, 6-10 m deep, full height; slots 2-4 m wide, 5-10 m deep |
| Rhythm | joints every 5.5 m, pours 2.75 m, formwork 1.22 x 2.44 m, ties on 0.6 m |
| Head | 0.12-0.18 of the height, overhanging 10-20 m a side on 45 degree corbels; visor band 6-10 m of slits 0.4-0.8 x 4-10 m |
| Openings | under 5 %; hangar bays 30-50 x 10-15 m at 0.6-0.75 of the height; pads 25-35 m, 3-5 m thick |
| Crown | radomes 12-25 m, phased arrays 22 m tilted 20 degrees, lattice masts 40-100 m |
| Materials | concrete albedo 0.22-0.30 linear, roughness 0.8-0.95 (wet streaks 0.4-0.6); painted steel 0.06, 0.55; rust (0.20, 0.09, 0.04), 0.9; stains x 0.55-0.75, 3-10 x a ledge's depth long, rust 0.5-3 m under steel |
| Far towers | height 4-8 x width, fins only 8-15 m wide on a 20-30 m pitch, head 0.1-0.15 of the height, a crown motif each by seed |

- Shape grammars (Müller et al. 2006 CGA, after Wonka 2003; [SIGGRAPH](https://history.siggraph.org/?p=113547)) [S]: scope boxes split by `Subdiv` and `Repeat` (whole tiles fitted), `Comp` into faces; order for us: mass, faces, bays (fins and slots), level bands, openings in tagged bands, clutter. Cost grows with instances near the camera; far towers need mass and fins only.

## Lights at night (direction v5)

- **Windows as a procedural mask** in the facet's plane (the same coordinates as the seam map): cell = floor(place / pitch), the opening a box step, a hash per cell for lit or dark, colour, luma; about 20-40 ALU a pixel, no texture, cost grows with the tower's pixels [K]. City Sample's window material: one threshold for the share lit, luma and colour variation, a seed per instance ([forum](https://forums.unrealengine.com/t/city-sample-buildings-light-of-window/662133/2)) [S]. Interior mapping (van Dongen 2008) is pointless for slits.
- **Far and small:** at 0.67 of 1080p a render pixel is about 0.9 mrad (0.9 m at 1 km); a slit is sub-pixel past about 300 m. Box-filter the mask by the pixel footprint and fade to its expected value (share lit x luminance x opening area / cell area) past a cell, or TAA's clamp erases or flickers it [K].
- **Point lights as sprites:** GTA V draws every small light as an instanced 32 x 32 quad ([Courrèges](https://www.adriancourreges.com/blog/2015/11/02/gta-v-graphics-study-part-2/)) [V]; keep energy, L = I / (d^2 Omega) with Omega at least 2 x 2 render pixels; MSFS's sprites that do not dim with distance are the failure to avoid [S]. A 2000 cd beacon at 5 km over one pixel is about 100 cd/m2: visible without faking.
- **Obstruction lights** (FAA AC 70/7460-1M, [poster](https://www.flashtechnology.com/wp-content/uploads/2021/01/Flash-FAA-AC-70_7460-1M-Tower-Guidelines-Poster.pdf)) [V]: over 1750 ft, 11 L-864 red (2000 cd) flashing together at 30 a minute; levels at most about 105 m apart [K]; ICAO medium-intensity red 2000 cd, 20-60 flashes a minute, low-intensity steady 32 cd ([Annex 14](https://www.flashtechnology.com/wp-content/uploads/2017/09/ICAO-Annex-14-Chapter-6-2013.pdf)) [S]. Pad edges aviation green [S].
- **Luminance** [K]: lit window from outside 20-150 cd/m2, LED billboards capped 300-600 cd/m2 [S], neon tube 10^3-10^4, concrete under sky glow 10^-3-10^-2; sodium 2000 K, LED 3000-5000 K. Night exposure: clamp eye adaptation's range and meter so a few bright sprites do not pull it down.
- **Markings** (stencils, hull numbers, chevrons) are painted albedo lit by floods, not emissive [K].
- **Bloom:** CoD AW's 13-tap downsample with a Karis average on the first step only, tent upsample, against pulsing ([Jimenez](https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/)) [S].
- **Cost:** the mask grows with tower pixels (well under 0.1 ms estimated); sprites with visible emitters (ten thousand is trivial); real clustered lights only for a few dozen emitters that light geometry (hangar mouths, fin feet, pads); shadowed spots for searchlights only.

## The engine today (survey)

- **Have:** flat-shaded `Mesh` by default, `buildIndexedMesh` keeps hard edges, `addDrawnMeshWithLevels`, still objects (`addShape`, `markRenderChanged`); up-facing moss blend (`GroundPaint::Strata` top layer, as the rock) with `groundProjected`, `groundStreaks`, `groundVariation`; spot lights with atlas shadows (8 shadowed, 256 total), animated per tick; the height fog's closed form (`fogOpticalDepth`, `henyeyGreensteinPhase`, `spotCone` mirrored in C++); drifting rocks posed in closed form from the simulation clock; `hangingStrand`/`hangFrom`/`catenaryStrand` from any world point; `sky_object` meshes placed as sky bodies.
- **Gaps:** no prism/chamfer/profile helper; no per-object tint or animated emission (`GpuInstance` has none, nothing time-based in shaders); ground layers carry no clear coat or emissive; no line/tube lights; spot lights do not light fog past the 100 m froxel grid, no beam pass; the sky shader draws only the sun and one moon (`GpuAtmosphere`); nothing finds ledge anchors on a structure.

## Open

- No primary source found for overgrowth shaders in HZD/HFW/TLOU2, or Halo, Destiny, NieR or Death Stranding megastructure talks; panel and bevel sizes are practice.
- Worth reading in full before building: HZD GDC 2017 placement slides, the Uncharted 4 SIGGRAPH 2016 PDF, Geiger 2023.
