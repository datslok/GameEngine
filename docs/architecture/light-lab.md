# Light lab

Covers: `src/games/lightlab/`

`LightLabGame` (`--game lightlab`, the default game, title "Light lab"): a sunlit field for judging shadows, lights, glass, reflections, fog and global illumination. Settings under F5 Light lab (`lightLab.<group>.<field>`). Tests: `light_lab_test`.

## Game

- **Controls:** `PlayerControls` (`labViews`: free camera from (0, 2, 6) towards (0, 2.5, -30), MOBA look point (0, 0, -8), body at the origin, 70 degrees, far plane 1000 m); F4 the engine's debug view; F the flashlight; L the back room's lamp; K the street lights.
- **Air entity:** an `Atmosphere` while `LightLabSunSettings::atmosphere` is on (the default; its ground the grass's albedo), and the mist's `Fog` while that is on. F5 Light lab, Atmosphere, for the air's numbers.
- **Probes:** a `ProbeCascades` entity, so light probes follow the camera when ray tracing is on; the level is `isStatic`.
- **Scene events** (`signalProbeEvent`): every light switch signals a `LightChange` over its reach; the street one over `labStreetBounds` grown by the lamps' range; a clock moved by hand past `labClockJumpHours` (0.1 h) one everywhere (`signalClockJumps`, measured the short way round the day against `clockHoursSeen`, which follows the ticks).

## Lights

| Light | Switch | What |
|---|---|---|
| Flashlight | F, or `lightLab.flashlight.on` | the shared `Flashlight` (`gameplay/flashlight`) in the right hand in every mode, placed after the camera each frame; off at the start so golden views keep the sunlit picture |
| Fence torch | `lightLab.fenceTorch.on`, `bearing`, `dip` | a second `Flashlight` just above the first fence post (`labFenceTorchPosition`), east and 10 degrees down by default (`fenceTorchPose`: bearing from north towards east); placed and switched every frame, so its beam stands still in the mist and can be seen from the side, where forward scattering shows it best |
| Desk lamp | `lightLab.deskLamp.on` | `labDeskLampBeam`: a warm shadowed spotlight (intensity 3, range 6 m, a 1 cm emitter for sharp shadows, still) at `labDeskLampPosition`, west of and 35 cm over the pen table, aimed at its middle so the pens' shadows fall east into the start's view |
| Ceiling lamp | L (`toggleLamp`) | `labLampLight` (warm, intensity 2, range 7 m, not fast) at `labLampPosition` under the back room's ceiling; its plaster panel (`labLampFixtureHalfSize`, no shadow, so it blocks neither its light nor probe rays) glows `labLampGlow` (6) times its colour while on |
| Street lamps | automatic, K flips | below |

All but the flashlights are still lights: their bounce goes into the probes' long memory, which their switch restarts.

## Sun and sky

`LightLabSunSettings` (group Sun): latitude 45, year 2026 (dates the moon and stars), day 80 (March equinox), solar hours 15.5, `hoursPerSecond` 0 (still; 0.025 runs a day in 16 minutes), sun intensity 4 above the air, sky brightness 1 (the hand-made day's), `sunDiscScale` 5 (looks only: shadows keep the real size).

- Ticks advance the clock (`advanceLabClock`: past midnight into the next day, past the year's end into the next year, so the moon moves on).
- `updateDaylight`, each frame, from `daylightFor(towardsSun, settings)` → `Daylight {sun, sky, background}`:
  - With the atmosphere: the sun white at its intensity above the air; the renderer dims, reddens and draws the sky.
  - Without (the hand-made day): sunlight fading in between -1 and 4 degrees of elevation and warming below 30 (from (1, 0.96, 0.9) to (1, 0.55, 0.3)); the sky (tint (0.6, 0.75, 1)) as background and `AmbientLight`, full above 20 degrees, dimming to a 3% night share at -6; its own sun disc (`getSunDisc`: black, emissive, no shadow, no `PreviousTransform`), posed by `skyObjectPose` at 0.9 of the far plane and glowing `discRadiance`, hidden once the sunlight is gone.
- Bodies' positions: `sun-moon-stars.md`.

**Night** (`LightLabNightSettings`, group Night; dials by eye, since the real moon is far darker than eye adaptation makes up):

| Field | Default | Meaning |
|---|---|---|
| `moonIntensity` | 0.05 | full moon above the air |
| `moonPhaseContrast` | 0.25 | phase curve to this power: 1 real, 0 every phase full; squeezes a quarter moon's 3.5 stops as the eye's sense of brightness does |
| `moonDiscScale` | 5 | looks only |
| `starBrightness` | 1 | magnitude 0's light in millionths (`labStarBrightnessUnit`) |
| `starContrast` | 0.7 | |

- The moon (`getMoon`): a `DirectionalLight` from `moonlightFor(towardsMoon, towardsSun, settings, atmosphere)`: `labMoonColour` (1, 0.94, 0.86), the phase-dimmed intensity, `reflectsSun`, shadows, the moon's angular size; without the atmosphere it fades below the horizon here.
- The `StarrySky` (catalogue loaded once in `onInit`, shared), its brightness times `starVisibility(towardsSun)`: none above -4 degrees of sun, all below -14 (`labStarsAppearBelowDegrees`, `labStarsFullBelowDegrees`), since the dial makes stars far brighter than real.

**Mist** (`LightLabMistSettings`, group Mist): on at the start; a `Fog` of 0.01 per metre at the ground (about 300 m of sight), thinning by e every 10 m up, so sunbeams through the window and the lamps' halos show. Fog itself: `fog.md`.

## Level (`light_lab_level`)

Plain boxes, testable without loading: `LabBlock {centre, halfSize, LabSurface}`; `LabSurface` names each material (stone, wood, bark, leaves, plaster, floorboards, paint, street, metals, mirrors and glasses). `buildLightLabLayout()` → `LightLabLayout`, groups `towers`, `fence`, `trees`, `building`, `street`, `windows`, `streetLamps`, `reflectors`, `table`. Each group is one entity of cube parts and, with the ground (`Mesh::plane(labGroundHalfSize)`, 400 m), goes into the `StaticLevel`: the triangles drawn are the triangles walked on.

| Piece | Where and what | For |
|---|---|---|
| Towers | `labTowerDistances` 5 to 320 m, doubling; each 0.1 of its distance wide and 0.4 tall, fanned 9 degrees apart from -27 | cascades: all look the same size, each cascade doubling |
| Fence | posts 10 cm wide, 2 m tall every 4 m along x = -6 from z = -4 to -300, one 6 cm rail at 1 m; a chain-link panel between (`chainLinkPanel`, double-sided cut-out, its wire made by `chainLinkImage`/`chainLinkAlpha`: 256 texels a 0.5 m tile, 8 diamonds, 3 mm wire, mips keeping coverage) | thin casters, cut-outs to the horizon |
| Trees | 400 box trees, 0.8-1.3 size, 40 to 380 m out; placed by hashing each attempt's number (`Fnv1aHash`, `mixBits`); clear of the view wedge north (34 degrees either side plus 15 m), the building (15 m) and the street (`labStreetKeepClear`, 10 m) | far shadows |
| Building | south-west: `labSunlitRoom` (window `labWindow` in its west wall, a red panel lining its east wall), `labBackRoom` (front door `labFrontDoor` in its east wall), `labInnerDoorway` between; 0.3 m walls, a 2 cm floorboard slab (`labFloorTop`), a roof | bounce light, sunbeams |
| Window pane | `labWindowPane`, 1 cm, mid-wall, `WindowGlass` (transmission 1, roughness 0.02, faint green); out of collision so the sun comes through | glass |
| Mirror and steel | `labMirror` and `labSteelPanel` on the sunlit room's south wall, a sharp and a rough mirror side by side | mirrored lights, planar mirrors |
| Reflection corner | `labPolishedSlab` north-east of the start, the chrome ball (`labMirrorBallCentre`), a solid glass ball (`labGlassBallCentre`) and a frosted panel (`labFrostedPanel`) | reflections, refraction |
| Painted balls | `labPaintedBallCentres`: bare and clear-coated red paint (`getPaintedBalls`) | clear coat |
| Brick wall | `labBrickWall` with `labCobbleStrip` in front: textures, height and normal maps made by `brickImages`/`cobbleImages`, laid as `tiledPanel`s lifted `labPanelLift` | parallax occlusion mapping |
| Pen table | (`labTableX`, `labTableZ`) = (-2.5, 2), 80 x 50 cm, 75 cm high; 1 cm pens and a 7 mm pencil, one standing, each 14 cm; static (`buildPenTable`) | contact shadows |
| Bottle | `labBottleBase` on the table: 25 cm, 6.4 cm across, `Mesh::lathe` of `labBottleProfile`, thin green `BottleGlass`, double-sided so both walls tint (`getBottle`) | coloured shadows |

**Stained glass** (`lightLab.stainedGlass.on`, off at the start; each switch a scene event round the room): the pane's material swaps to stained glass, two textures from `stainedGlassImages` (256 texels, 4 x 4 cells of six strong colours, no neighbours alike, 4-texel lead passing nothing: colour in sRGB, transmission linear). The sun's half-degree disc blurs the lead lines on the floor, as in real sunlight.

## The street

East to west along `labStreetZ` (45 m south) from x = -150 to 150.

- A 10 m asphalt road (2 cm slab) between 3 m pavements raised `labKerbHeight` (15 cm); a 10 m cross street round x = 0 so the start can walk in.
- Houses with fronts at `labHouseFrontOffset` (8 m): 10 m deep, 6 to 12 m wide, 3 or 4 storeys of 3 m, brick or render, solid blocks. Widths, storeys and surfaces hashed per house number (`StreetHashUse`, apart from the trees'); a house that would leave less than the narrowest takes the rest of its stretch.
- A window per 3 m bay and storey (`LabWindow {pane, lit, tint}`: 1.2 x 1.5 m, standing 3 cm out; lit and tint hashed per window, `labLitWindowShare` 0.4).
- 30 lamps per kerb 10 m apart, the south side 5 m further east: bulb 6 m up and 0.5 m in from the kerb, a post 0.6 m back towards the houses, an arm and a 0.8 m shade over the bulb, all Metal and static.
- In the game: blocks and windows static (`getStreetWindows`, one part per window in layout order); the bulbs' glow one static renderer casting no shadow; each lamp an entity at its bulb holding, while on, `labStreetLampLight` (warm, intensity 12, range 15 m, source radius 0.5 m, shadowed, not fast).
- **Switching** (`updateStreetLights`, each frame after the daylight and in `prepareGoldenView`): on while the sun is below `labStreetLightsBelowDegrees` (-2, `streetLightsWantedFor`), flipped while `streetOverridden` (K; cleared when the sun's answer changes at dusk or dawn). A change calls `setStreetLights`: every lamp's light added or removed, the bulbs' glow (`labStreetBulbGlow`, 8 times the light's colour) and the lit windows' (`labWarmWindowGlow` to `labCoolWindowGlow` by tint) set, one scene event over the street.

## Golden views

`towers`, `overview`, `sunlit-room`, `sunset-hdr` (HDR output), `sun-disc`, `flashlight`, `back-room-lamp`, `moonlit` and `street-night` (from (-40, 1.7, 43) down the road at 22:00); each sets its hour and its lights in `prepareGoldenView`.
