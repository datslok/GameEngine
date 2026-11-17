# Shadows

How the engine decides that something blocks a light: shadow maps for lamps and the sun's cascades, soft edges from a light's size, caching, contact shadows, coloured shadows through glass, and how it chooses which lights get shadows when there are too many. Lights and shading themselves: `shading.md`.

## A depth photo from the light

A point is in shadow when something lies between it and the light. Testing that directly, every pixel against every triangle, is ray tracing. **Shadow mapping** gets the answer from one extra drawing of the scene:

1. Put a camera at the light and draw the scene keeping only depth. The result, the **shadow map**, holds for each direction how far the nearest surface is from the light: a photo taken with a rangefinder.
2. While drawing the real view, project each pixel's point into that photo. If the photo saw something nearer the light in that direction, something is in the way.

Each kind of light needs its own camera:

- A **spotlight**: one perspective camera down its beam, just wide enough for the cone.
- A **point light** shines every way, so it needs six photos, one per face of a cube round it, each a square 90° view along ±X, ±Y or ±Z. A direction belongs to the face of its largest component (`pointShadowFace`).
- A **directional light** has parallel rays, so its camera has no perspective: it is a box (an **orthographic** projection). The sun lights the whole world but a map has finite resolution, so it gets several boxes, fine near the camera and coarse far away (cascades, below). Only the brightest directional light casts shadows; others still light.

Models with `castsShadows = false` stay out of the shadow pass. A glowing sphere round its own light, for instance, would otherwise block everything.

## The atlas and its tiles

Every shadow view draws into one depth image, an **atlas** of 8192 × 4096 texels (128 MB), so one render pass fills it and shaders read a single texture. With the sun's cascades at 4096 pixels (the default) it grows to 8192 × 12288 (384 MB).

Each frame `planShadows` cuts it into square tiles: the sun's row, one per shadowed spotlight and six per shadowed point light, for up to 8 spot and 64 point lights.

**Tile sizes** are powers of two from 256 up to 1024 for cube faces and 2048 for spotlights, chosen by reach (`shadowTileSizeFor`): enough texels that one covers about 4 cm across the far end of the view (2 cm for spotlights). A lamp of range 5 has cube faces 10 m across at their far end, so 256 texels do; a 35° floodlight reaching 25 m spans 35 m there and gets 2048.

- Spotlights aim finer because their beams usually land on the ground at a slant short of their reach, which stretches each texel along the ground; and there are few of them, mostly standing still, so big tiles cost little once cached.
- A 256 tile has a sixteenth of a 1024 one's texels. Small lamps are cheap, which is how 64 shadowed point lights fit.
- If the requests still overflow, the least important tiles are halved until everything fits: the far map first, then point lights and spotlights, and only last the sun's cascades, never below 1024.

**Packing** works like a **quadtree**. The atlas starts as squares of the largest size; a tile takes the smallest free square that fits, and a square too big is split into four quarters, as often as needed. Since every size is a power of two and the largest are placed first, quarters of a split square are always used up before another is split, so no unusable gaps appear.

The shader is told each tile's rectangle. The shadow pass draws the scene once per tile, moving the viewport to it. Perspective views are made a couple of texels wider than needed, so filtering near a tile's edge never reads the neighbour.

**Skipping faces nobody sees.** A cube face can only shadow a visible pixel if its view pyramid overlaps the camera's. A lamp near you usually has a face pointing back past you at nothing on screen. `planShadows` tests each face with `Frustum::mayIntersect`:

- Both views are convex, so if all eight corners of one lie outside one side of the other, they cannot meet. It is checked both ways round, since a small view beside a big one's corner is only caught by its own sides.
- Like sphere culling, it may keep a face that just misses, but never drops one that is needed.
- A skipped face keeps its slot with an empty rectangle, so the shader still finds face `f` at the light's first tile plus `f`, and treats it as lit.

Each tile is one more drawing of every caster in it, so a shadowed point light costs up to six. That is why games shadow only the lights that matter, and why caching (next) matters.

## Shadow caching

Most lights and most of the world stand still, so most tiles would come out identical to last frame. The atlas is therefore kept between frames, and a tile is redrawn only when something in it changed.

- **Tiles keep their place.** `ShadowAtlasLayout` remembers which square each view (a light's entity and face) had and hands it back while it asks for the same size. Freed squares rejoin: four free quarters merge into their parent (a **buddy allocator**, named after the paired halves memory managers use), so the atlas does not crumble into scattered holes. If a big tile ever finds no room despite enough total area, everything is packed afresh once and redrawn.
- **The renderer remembers what it drew.** Per square, `ShadowTileMemory` keeps the view matrix and a 64-bit **hash** (a fingerprint) of the casters: their meshes and model matrices. Each frame the fingerprint is recomputed, which is cheap next to drawing; if matrix and fingerprint match, the tile is skipped. A moving light changes the matrix; a box that moves, appears or vanishes changes the fingerprint.
- **Wiping one tile.** A clear at the start of a pass would wipe the whole atlas, so each redrawn tile first gets one big triangle at the far depth, drawn with the depth test off, which overwrites exactly that tile.

When nothing needs drawing, the shadow pass is skipped. F4 counts tiles drawn, kept and skipped. Moving casters inside an otherwise still tile, the stagger and the research behind it: `docs/architecture/lights-and-shadow-atlas.md` and `docs/research/SHADOWCACHE.md`.

## Three problems and their fixes

### Shadow acne

Each shadow texel stores one depth for a whole patch of surface. Tested against itself, a sloped surface comes out half in front of and half behind its own stored depth, in stripes.

- The fix is a **bias**: each lookup moves its point off the surface along the normal by 2.5 texels of its tile (`normalOffsetTexels`), so it tests a point just above the surface. Measured in texels, the push stays in proportion on every tile size.
- GPUs can also bias the drawing itself, more on steep slopes, but that slope-scaled push has no limit: a pillar seen nearly edge-on from a lamp above got pushed behind the ground, and its shadow came loose from its base (**peter-panning**, after the boy who lost his shadow). The engine uses no hardware bias.
- The sun is the exception: its lookups move towards the sun instead (below).

### Jagged edges

A map has finite resolution, so a plain in-or-out test draws staircases. The sampler compares and blends the four nearest texels itself, and the shader averages nine such lookups in a 3 × 3 grid (**percentage-closer filtering**, PCF): at an edge, the share of samples that pass becomes a smooth ramp.

### Soft shadows that look real

A fixed blur gives every edge the same width, but real shadows are sharp where an object touches the ground and blur further away, because lights glow over an area.

Behind an edge lies a band that sees part of the glowing area but not all, the **penumbra**, like the partial eclipse ring round the moon's total shadow on Earth. By similar triangles its width is

```
penumbra = emitter radius × (receiver distance − blocker distance) / blocker distance
```

zero where blocker meets receiver, widening with the gap, wider for bigger lights. Point and spot lights get this through **percentage-closer soft shadows** (PCSS), in three steps per pixel:

1. **Search** the map round the point for things nearer the light and average their distance. This reads the atlas a second time as raw depths, since a comparison only says yes or no.
2. **Size** the penumbra from the formula.
3. **Average** 16 depth comparisons spread over it.

Two refinements:

- The 16 points form a **Poisson disc** (spread out with no two close, like seats taken in an empty cinema), turned by a different angle in each pixel so leftover error looks like fine grain rather than bands.
- A wide filter brings acne back: on a surface sloping away from the light, a sample a few texels over is legitimately further away and looks shadowed. So each sample is compared with the surface's own plane at that spot (**receiver plane depth bias**), the plane found from how the position changes between neighbouring pixels.

Each light's `emitterRadius` is its real glowing size for this (0.05 m for a bulb, 0.025 m for a spotlight by default), separate from `sourceRadius`, which only shapes the falloff and is tuned much larger.

## The sun: cascaded shadows

One map cannot serve the sun. Its texels are the same size throughout its box, while what the camera sees shrinks with distance: a metre of ground fills hundreds of pixels at your feet and a handful at 200 m. A box big enough for the distance is far too coarse up close.

The answer is a map book's (city, country, world): cut the view into slices by depth, **cascades**, each with its own box. The sun gets four by default, out to `ShadowSettings::directionalDistance` (200 m).

### Where to cut

Even cuts waste texels on far slices that cover little of the screen. Logarithmic cuts, each reaching the same multiple further, keep a texel's size on screen about constant but squeeze the first slice into the last metre. The **practical split scheme** (Zhang et al. 2006) blends them:

```
split = λ × logarithmic + (1 − λ) × even
```

With λ = 0.95 a typical view splits at about 3, 9, 36 and 200 m (`cascadeSplits`).

### How big each box is

The tightest box round a slice changes shape whenever the camera turns, so texels change size every frame and edges swim. Instead each slice is wrapped in its smallest **bounding sphere**, and the box is that sphere's shadow, a square as wide as the sphere. A sphere looks the same from every side, so turning never resizes the box.

The centre lies on the view axis where the slice's near and far corners are equally far. With corners `d·t` off the axis at distance `d` (t combining the tangents of half the view's width and height), equal distances give

```
z = (f + n)(1 + t²) / 2
```

or the far end's centre if that lands beyond it (`cascadeSphere`). The box's centre is then snapped to whole texels across the sun's direction, so as the camera walks the box jumps a texel at a time and edges stay put.

Stability costs size: for a 70° view the sphere's radius is about 1.43 times the slice's far distance, so a slice ending at 36 m gets a box about 103 m across, 2.5 cm texels on a 4096 tile. The full table: `docs/architecture/sun-shadows.md`.

### Which cascade a pixel reads

The boxes overlap, and near a slice's end the finer box often still holds the point, so the shader takes the **finest box that holds it**, not merely the slice its distance falls in. Over a blend band (10% of the box's radius) near that box's edge it looks up the next cascade too and mixes them, so no seam shows. A dithered switch is a setting: each pixel takes one cascade, the next more often towards the edge, one lookup instead of two, grainy until anti-aliasing averages it.

### Bias along the light

The normal offset above moves a point along its normal. Under a low sun most of that push is sideways as the sun sees it, which moves where the edge is found; with 1 cm texels in one cascade and 6 cm in the next, a building's edge landed up to 18 cm apart in the two, and the seam showed as a step.

Pushing straight towards the sun changes only depth in the map, never where the point lands, so every cascade finds every edge in the same place. Surfaces the sun grazes need more depth bias, so the push grows with the tangent of the angle between normal and sun, capped at 10 (`sunShadowBias`).

### Soft edges from the sun's size

The sun's disc spans about half a degree (`DirectionalLight::angularRadius`, 0.267° either side). Behind a blocker the penumbra widens by tan(angular radius) per metre of gap, about 4.7 mm per metre. That is why the bright spots under a tree are blurred images of the sun. The shader runs the same three PCSS steps inside the box, working the penumbra out in metres before converting to the current cascade's texels, so softness carries across a cascade seam.

### Fitting the cascades to the screen

A slice's box depends on where the slice ends, not on what is visible, so in a room the cascades would spread their texels over 200 m mostly behind walls. **Sample distribution shadow maps** (Lauritzen 2011) measure how far the visible surfaces really reach and end the cascades there.

After the main pass, a compute shader (`depth_range.comp`) reads the depth image and keeps the farthest surface, ignoring sky. Thousands of workgroups each find their own extreme and fold it into one answer with atomic minimum and maximum, which give the same result in any finishing order. Atomics work on integers, and a positive float's bits read as an integer sort in the same order as the number, because the exponent sits above the mantissa: like numbers in scientific notation, the power of ten decides first.

In the light lab's room the farthest surface is about 10 m away, so the cascades end near 12 m and every texel shrinks 7 to 17 times. Three choices keep it calm:

1. **A frame or two late.** The CPU plans shadows before the GPU has drawn that frame's depth, so it uses the last finished measurement.
2. **A ladder with hysteresis.** The distance is grown 10% for the lateness and rounded up to fixed steps, each 1.25 times the last. It climbs a step at once but comes down only when the need has fallen two steps, like a thermostat that switches on at 19° and off at 21° so it never chatters at 20°. Following every wobble would resize every texel each frame, making edges shimmer and defeating the cache.
3. **Only the far end is fitted.** Starting the cuts at the nearest surface would push every logarithmic cut outwards and put the ground at your feet in a coarser box.

### Far away, and taking turns

- **The far map.** Past the cascades, one more box reaching 5 km (`farShadowDistance`) lets mountains and towers cast. Its texels are metres wide, which suffices: the sun's own blur behind a caster 300 m up is several metres. It moves only when the camera crosses a grid cell, so it stays cached, and small casters are left out of it so the player walking by does not redraw kilometres of land. The cascades also read it for casters high above that their own boxes miss (distant occluders).
- **Staggered cascades.** Turning the camera moves every box and redraws its casters. The far half of the cascades take turns (`cascadeStagger`, 2), one a frame, keeping last frame's picture meanwhile, so each frame costs about the same.
- **A mirror's row.** A flat mirror shows mostly what is behind the camera, outside its boxes, so the mirror's reflected camera gets cascades of its own.

### Cost and presets

Each cascade is one more drawing of the casters in its box, but a box that has not moved by a texel keeps its cached picture. The Ultra and High presets use four cascades of 4096 (texels from about 0.2 cm by the camera to 14 cm at 200 m) in the 384 MB atlas; Medium three of 2048 reaching 120 m, Low three of 2048 reaching 60 m, both in the 128 MB atlas. Changing the resolution rebuilds the atlas on the spot. Everything about the sun's row: `docs/architecture/sun-shadows.md`.

## Contact shadows: small things touching the ground

Map texels are centimetres wide and lookups are biased, so a pen on a table casts no visible shadow where it touches; it seems to float. **Contact shadows** fill the gap from the camera's own depth image:

- Each pixel marches 0.3 m towards the sun in 32 steps through the depth image.
- A step that crossed a surface since the last one (in front of it then, just behind it now) is in shadow. Testing the crossing rather than "behind by some thickness" keeps a thin object's shadow no wider than one step, which is what lets a 1 cm pen cast correctly.
- Cut-outs (leaves with holes) are passed over, since their own maps already draw their holes.
- The four shadowed lamps lighting the view most brightly get the same, each march stopping short of the lamp so its own glowing bulb casts nothing.

The limit is the screen: something off screen or hidden casts no contact shadow. Settings (`ContactShadowSettings`, off on Low) and the pass: `docs/architecture/sun-shadows.md`.

## Coloured shadows through glass

With ray tracing on, a shadow ray through stained glass simply multiplies by each pane's colour. Without it, each shadow tile holding glass gets a companion at half resolution storing two things:

- the **product** of the share of light every pane lets through, as the light sees them (multiply blending forms the product as panes are drawn);
- the **nearest glass depth**.

A point behind the glass takes the tint, one in front does not, and the soft shadow filters tint each of their samples, so the coloured patch's edge softens with distance like a shadow's. Glass hidden from the light behind something solid is thrown away, so a basement window cannot colour the floor above it. The fog reads the same maps, giving coloured shafts in mist. Details and limits: `docs/architecture/glass.md`.

## More lights than seats

A light the shader uses has a **seat** in the light buffers. Clustered shading makes a light cost only where it shines, so by default every light the buffers hold is seated: 1024 point and 256 spotlights. Shadows remain scarce: at most 64 point and 8 spotlights, what the atlas holds. A level with a torch every few metres has more than that, so each frame `prioritizeLights` (`render/gpu/light_priority`) decides who gets a seat and, above all, a shadow.

Taking the first lights in the list would be arbitrary and unstable: removing a component moves the last one in its storage into the hole, so switching off the flashlight could hand a shadow to a light across the map. The rules:

1. **Out of view, out of the running.** A light whose range sphere lies wholly outside the camera's frustum cannot change a visible pixel, so it takes no seat (the same sphere test as mesh culling).
2. **Ranked by brightness at the focus.** Each light scores the irradiance it delivers at one point, the **focus**: `intensity × brightest channel / (d² + r²)`. The range window is left out on purpose: it is zero for every light out of reach of the focus, which would tie them all; without it, far lights still rank by distance. Directional lights rank by brightness alone.
3. **Shadows to the top casters.** Of the seated lights with `castsShadows`, the most important get shadows up to the caps (`LightPrioritySettings`; a game may lower them, and the rest still light, unshadowed).
4. **Hysteresis.** Walk between two similar torches and their scores cross; a plain top 64 would flick the shadow back and forth every frame near the midpoint. So last frame's holders count 1.25 times as important (`incumbentAdvantage`): a rival must be clearly brighter, about 11% closer for an equal light. Same idea as the thermostat, or a Schmitt trigger. Each light carries its `Entity` so the renderer can recognise it next frame.
5. **Fading.** Hysteresis stops flicker, but a handover would still make a pool of light or a pillar's shadow vanish in one frame. So the loser keeps its seat or shadow while fading out over a quarter second, and only then does the winner fade in; the limits hold even mid-handover. Two cases skip the fade: a light not on screen last frame starts at full strength (nothing showed it, so nothing can pop, and the flashlight switches on at once), and a light leaving the view drops at once.
6. **Manual priority.** Only the game knows that the lamp on the objective must never go dark. Point and spot lights have an integer `priority` (0 by default); higher always wins, and brightness and hysteresis decide only between equals. It does not revive a light out of view.

**Where is the focus?** In first person the camera is down among the lights, so its position works (the default, `Game::getLightFocus`). A MOBA camera floats about 16 m above the ground it shows, further than a torch reaches, so every torch on screen would score almost nothing there; the demo's MOBA mode passes the ground point in the middle of the screen instead (`MobaCameraController::getLookPoint`). It is judging street lamps by how they light the street, not the satellite photographing it.

**Watching it work.** Press **L** in the demo for 100 coloured lamps, each circling a stone pillar like a firefly (`Firefly`: a circle whose radius, height and speed wobble as sums of sines with mismatched frequencies, a Lissajous-like path that never quite repeats). The pillars' shadows sweep round like sundial hands, and since moving lights invalidate their cached tiles, this is the caching's worst case: F4 shows over 200 tiles redrawn a frame. There are more lamps than the 64 point shadows, so every lamp lights the ground, the ones nearest the focus also shadow their pillars, and shadows fade from lamp to lamp as you move. Priority's exact rules: `docs/architecture/lights-and-shadow-atlas.md`.
