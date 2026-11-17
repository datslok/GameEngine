# Cloth lab

Covers: `src/games/clothlab/`

`ClothLabGame` (`--game clothlab`, title "Cloth lab"): a station per cloth milestone on a plain floor. The solver it drives: `cloth.md`. Tests: `cloth_lab_test`, `cloth_lab_dress_test` (parked with the cloth: `runParkedTests`).

## Layout (`cloth_lab_layout`)

`buildClothLabLayout()` → `ClothLabLayout`, metres, stations along x in front of the player's start (`clothLabFirstPersonFeet`, (0, 0, 4)):

| Station | x | What |
|---|---|---|
| Mannequin | -7.5 | `mannequinFeet`: `clothLabMannequinModel` (`assets/models/human.obj`) wearing the dress |
| Draping | -4 | `ball` (radius 0.4) on `pedestal` (0.6 m), its centre 0.2 m in front of `drapeFrame`'s bar 2 m up |
| Piling | 0 | `mat`, 3 x 3 m, 1 cm thick |
| Fabrics | 4.5 | `fabricFrame`, a 3.6 m bar 2 m up for two sheets side by side |

- `ClothLabFrame`: a bar along x (`barCentre`, `barHalfLength`) on two posts (`clothLabFrameBlocks`: posts, then bar; `clothLabBarUnderside`, where pinned cloth hangs). Thin square sections: `clothLabPostHalfWidth` 0.04, `clothLabBarHalfThickness` 0.025.
- `blocks()`: every box; `stations()`: each station's box, which tests keep apart and clear of the start.
- The floor: `clothLabFloorHalfSize` (30 m) each way.
- Start places for the sheets: `sheetCentre`, `drapeSheetCentre`, `bigSheetCentre`, `pileSheetBottom`.

## Scene

- The floor and one renderer of every station's part (pale concrete, a dark blue mat, grey stone, brushed steel, a 64 x 32 ball), all static level (walked on, probes moved off them).
- A fixed sun: `clothLabLatitudeDegrees` 45, `clothLabDayOfYear` 172, `clothLabSolarHours` 15, `clothLabSunIntensity` 4 above the air; an `Atmosphere` (its ground the floor's colour) and `ProbeCascades`.
- **Bodies** (`addBody`): a `Collider` each for the ball (sphere), every layout block (box) and the floor (a box `clothLabFloorDepth`, 1 m, deep under the ground, so nothing passes through in a substep).
- **Controls:** `PlayerControls` (free camera from (0, 2.4, 6.5) towards (0, 1, -1)), F4's debug view. Golden view `stations` from the free camera's start.
- **Each tick:** the lab's `ClothSettings` (`getClothSettings`, F5 Cloth) given to every cloth, the hooks moved, the dress readied, then `stepCloths(world, seconds, getJobs(), getClothSolver())`; the solver (CPU or GPU) follows `EngineSettings::clothOnGpu`.

## Sheets

All made by `addSheet` (a `Cloth` driving a deforming mesh of its own, double-sided cotton), sharing the lab's `ClothSettings`.

| Sheet | Accessor | Mesh | Start and pins |
|---|---|---|---|
| Hanging | `getSheet` | `Mesh::grid` 28 x 30 squares, 1.4 x 1.5 m, deep red | flat, level with the fabric bar's underside under its left half, pinned at its back corners (the hooks) |
| Drape | `getDrapeSheet` | 24 squares a side, 1.2 m, cream | flat, level with the drape bar, pinned at its back corners; falls onto the ball |
| Big | `getBigSheet` | 100 squares a side, 2 m (10,201 points) | `ClothLabStationSettings::bigSheet`, off: flat 1.2 m over the mat, its corners moving pins drawn in to 1.6 m during the first tick, a hammock |
| Pile | `getPileSheet` | 40 squares a side, 0.8 m (2 cm squares, a triangle's height well over the layers' 5 mm) | `pileSheet`, on: standing on its bottom edge 2 cm over the mat, leaning back 0.15 rad, free, so it folds onto itself; self-collision's station (CPU only) |

- Separate cloths do not collide with each other, so the big and pile sheets are best not on together.
- **Hooks:** the hanging sheet's two pins are moving pins (`addClothPin`). `ClothLabHookSettings::hookGap` (F5 Hooks; the sheet's width at the start, at least `clothLabSmallestHookGap` 0.05 m) is where they should be; each tick moves the gap towards it at `clothLabHookSpeed` (0.5 m/s) about `hookMiddle`, so bunching shows folds.
- **Dragging** (`updateGrab`, in `onInput`, from last frame's camera): a left press casts `PlayerControls::pointerRay` at the sheets within `clothLabGrabReach` (10 m) and grabs the nearest corner of the nearest hit (`getGrabbedSheet`); while held the hand (`getSheetGrab`) stays the grab's distance along the pointer; release or the pointer leaving lets go; the click that recaptures the mouse grabs nothing. Ticks hand it to the cloth (`Cloth::grab`); `onDebugDraw` draws a yellow line from point to hand.

`ClothLabStationSettings` (F5 Stations; all start-up): `bigSheet` off, `pileSheet` on, `mannequin` on, `pinnedDress` on.

## Mannequin

- `human.obj`: watertight, one piece, arms out in an A-pose, facing +z towards the start. `clothLabMannequinPlacement` puts its box's bottom middle on `mannequinFeet`, scaled to `clothLabMannequinHeight` (1.75 m).
- Pale, matte, static level (drawn, walked against, static for the probes).
- `mannequinField` builds its `DistanceFieldCollider`'s field at start-up on the game's jobs: `clothLabMannequinFieldSpacing` 5 mm, `clothLabMannequinFieldBand` 3 cm (wider than any cloth's thickness).
- Station box: `clothLabMannequinStationHalfWidth` (0.6 m) round its feet, `clothLabMannequinStationHeight` (1.85 m) high.

## Dress (`cloth_lab_dress`)

A sleeveless A-line dress to the knee in dusty rose cotton, sewn from two panels (`getDress`).

- **Sized from the mannequin:** chest about 94 cm round, waist 79, hips 94, armpits at 1.20-1.25 m, shoulder tops at 1.44 m (`dressShoulderHeight`), their ends 21 cm from the middle.
- **Pattern** (`twoPanelDress`): `DressPiece` front and back, each eight `DressEdge`s counterclockwise from the hem (`dressEdgeCount`); x across from the middle line, y up from the hem.
  - 84 cm across at the hem, 54 at the armpit (a quarter of the chest plus 3.5 cm: 14 cm of ease round), the armpit 61 cm up and cut deep (the arms stand 5-8 cm off the body there), quadratic Bezier armholes to 7 cm straps at 92-95 cm, the neckline dipping to 80 cm at the front and 90 at the back.
  - The left half is the right mirrored, bit for bit. Both panels are drawn seen from outside, so the front's right side seam is sewn to the back's left. Four seams: the sides, then the shoulders (`dressShoulderSeams`).
  - Cut by `meshPattern` at `dressSpacing` (1 cm): 12,747 points, 146 stitches.
- **Laid round the body** (`dressAroundMannequin`): each panel wrapped on an upright cylinder of `dressWrapRadius` (0.29 m), front facing +z, back -z, the axis at `dressAxisOffset` (0.66 m up, the hem; 2 cm back, as the chest and hips reach further back). The straps start about 15 cm over the shoulders, so their seams close in the air above them; starting lower or wider lost a strap off the shoulder.
- **Made in one place** for the lab and the dress-sized yardstick: `dressOnMannequin(body, feet, spacing, settings, pinned, jobs)` → `DressOnMannequin` (mesh, `Cloth`, strap pins' paths, empty when held by contact, and `dressHemPoints`); `prepareDressTick(dress, strapPaths, fabric, simulationSeconds, tickSeconds)` readies each tick.
- **Sewn, then hung:** weightless for `dressSewingSeconds` (1 s) while the seams close, then it settles onto the shoulders.
- **Pinned at the shoulders** (`pinnedDress`, "Dress pinned at the shoulders"), as on a dress form:
  - Both points of every shoulder stitch are pins (`dressStrapPins`, added together by `addClothPins`), from the start, since GPU cloth takes its pins at its first upload.
  - Each pin's `StrapPinPath` (`strapPinPosition`): across to where it meets its partner (their starts' middle) by `dressStrapsMeetSeconds` (0.5 s), then down onto the shoulder by the end of the sewing, each leg eased by `smoothStep`, since a pin goes through the body if its way does. The landing is straight below the meeting point where the body's field equals the cloth's thickness (1 mm steps, at most 30 cm).
  - Every other point is tethered to its nearest strap pins on its own panel, measured along the panel round its armhole (`ClothRestPaths`), so the dress hangs as a pinned sheet does. The back neckline's middle rides up the neck, past its tethers' length: bodies win over tethers.
  - Off: held by contact with the shoulders alone, as the dress yardstick also measures it.
