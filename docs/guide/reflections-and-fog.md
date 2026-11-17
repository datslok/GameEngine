# Reflections and fog

How shiny surfaces get their reflections (from the screen, a probe at the camera, and a flat mirror drawn twice), how mirrors throw light onto other surfaces, and how the air scatters light as volumetric fog.

## Reflections

**What a shiny surface needs.** A glossy surface sends the eye mostly the light arriving from one direction, its mirror direction, blurred over a cone that widens with roughness. The light from all around (the flat ambient dimmed by sky visibility, or the probes, [Light that bounces](bounce-light.md)) is an average over a whole hemisphere: right in brightness but as blurred as the roughest surface. A polished floor then shows a soft glow where it should show the room.

**The split sum.** Specular light from all around is an integral of incoming light times the BRDF. Real-time engines split it in two (Karis 2013): the BRDF's part, which depends only on view angle and roughness and comes from a precomputed table shared with the rest of the shading, and the light's part, the incoming light blurred to match the roughness. Everything in this section is about that second half.

### What runs by default

Reflections are on by default (off on the Low preset) and need anti-aliasing on, since they read last frame's smoothed picture. The main pass first adds a **stand-in** for each pixel's reflection (the light from all around, read along the mirror direction) and writes three small extra targets: the pixel's normal, its specular colour and roughness, and the stand-in itself. A reflection pass then swaps the stand-in for something better, as far as each source trusts itself:

| Order | Source | Good at |
|---|---|---|
| 1 | the reflection probe at the camera | what is off screen or behind the camera |
| 1b | on a flat mirror: the mirror's own picture, instead of 1 | a head-on mirror, sharp |
| 2 | last frame's picture, marched in screen space and laid over the rest (skipped on the flat mirror) | detail on screen, contact with the surface |

Each source returns a **confidence** from 0 to 1, and the pixel moves from the stand-in towards the source by that much. Where no source is trusted the pixel stays exactly as the main pass drew it, so the fallbacks never flicker against each other. Because last frame's picture already holds last frame's reflections, reflections in reflections build up one bounce a frame.

Glossy here means roughness below 0.6, fading in from 0.3; rougher surfaces keep the stand-in, which is already right for them.

### Screen-space reflections: the picture already drawn

Each visible pixel already knows its colour, so a reflection of something on screen can be read off last frame's picture. A glossy pixel follows its mirror ray across the screen until the ray passes behind the surface the depth image holds there: that surface is what it reflects.

- **Straight on screen.** A straight line in space stays straight on screen, and the stored reversed-Z depth is linear in 1/distance, as screen x and y are, so the ray's pixel and depth all change linearly with one parameter. The march works in screen space.
- **A depth pyramid** (Hi-Z). Stepping a pixel at a time would take hundreds of steps. The depth image is reduced into a mip chain where each texel keeps the nearest depth of the 2 x 2 below it: a quadtree over the screen. Where the ray is in front of everything in a big cell, nothing there can stop it, so it skips the whole cell and climbs a level; near surfaces it descends. A few dozen steps cross the screen (at most 64 by default).
- **Thickness.** Depth holds only front faces, so a ray passing behind a pole would look like a hit. A hit counts only if the ray is less than 0.3 m behind the surface.
- **Blur from the colour pyramid.** A rough surface reads the colour pyramid (successive 2 x 2 averages of the picture) at the level as wide as its reflected cone has grown by the time it lands.
- **Trust fades** near the screen's edge, at the reach's end, on rough surfaces, on surfaces hit from behind, and for rays heading back towards the camera, which can only really hit what lies between the surface and the eye yet tend to pass just behind near surfaces. A mirror seen head-on is left to the other sources.

Exact pass, fades and settings: [reflections.md](../architecture/reflections.md).

### The reflection probe: what the screen does not show

Behind the camera, past the screen's edges and behind other things, the screen has nothing to offer. A **reflection probe** is a 360-degree photograph from one point: six square pictures on the faces of a cube (a **cubemap**). The engine takes it at the camera, one face a frame by default (all six from the same spot, finished every six frames), so it is always current, flashlight included, and no level places one by hand.

- **Mirror images.** A cube's faces are stored mirrored relative to an ordinary camera's picture (a convention from early graphics APIs), so drawing one turns every triangle round, and the capture culls the other side.
- **Parallax.** A photograph from one point is wrong seen from anywhere else, as a window's reflection shifts while you walk. So each texel also stores how far away its surface was. A shiny point P elsewhere wants what its mirror ray P + R t hits: the first t where the ray's point lies farther from the probe than the distance the probe stored in that point's own direction, meaning it has gone behind a surface. It must be the *first* such point: solving the equation by any other means can land on the room behind a doorway, where the line of sight happens to agree again.
- **Walking the cube.** The search walks the ray cell by cell through a pyramid of the stored distances (each level keeping the nearest distance over a patch of directions, as the depth pyramid does), so a long step is taken only when nothing in the cone of directions it sweeps is nearer than the ray. This keeps it from jumping over a wall the probe sees edge-on, as a sliver. A few halvings of the last step pin the crossing down.
- **Self-doubt.** At a real surface the stored distances agree on both sides of the crossing; a jump means the ray slipped behind an edge the probe saw past, where it cannot know what lies there, and trust drops.
- **Blur by pre-integration.** The cube is copied into smaller levels, each the light averaged over a rougher surface's lobe (**prefiltering**). The average is a Monte Carlo integral with samples drawn where the lobe is strongest (**importance sampling**), each reading a blurrier level of the photograph as the lobe spreads, so 64 samples give smooth light instead of sparkle (**filtered importance sampling**).
- **Shadows behind the camera.** The sun's cascades fit what the camera sees, so the faces behind it would be sunlit through every roof. Without rays the sky visibility grid stands in for that shadow, keeping rooms dark in the probe's picture too.

Exact search and numbers: [reflection-probe.md](../architecture/reflection-probe.md).

### Planar mirrors: drawing the world twice

A screen-space reflection shows only what is already on screen, and the probe sees from the camera's place, so a mirror seen head-on, showing at full size what the probe never saw, looks jagged. For a flat mirror there is an exact answer: the reflection is the world seen from the camera's image behind the glass.

- **Choosing it.** Each frame the engine picks one flat face of smooth metal (roughness at most 0.4) filling the most solid angle on screen, its area times the cosine to the camera over distance cubed. Last frame's choice keeps its place unless another beats it clearly, so the picture does not pop. Nothing is marked by hand, and every face on the chosen plane shares the picture.
- **Drawing it.** The scene is drawn a second time through the reflected camera, at half the render size by default. The reflection x → x − 2(n·x − d)n is folded into the view-projection, so each reflected object is drawn exactly at the pixel where the mirror shows it, and the picture is read with no search.
- **Clipping at the glass.** Whatever sits behind the mirror's plane must not show. Every pipeline clamps depth rather than clipping at the near plane, so the vertex shader writes a clip distance against the mirror's plane, lifted 2 mm so the mirror's own face does not flicker into its picture.
- **Its own sun shadows.** Most of what a mirror shows lies outside the camera's cascades, so it has shadow cascades of its own, fitted to its view.
- Rough mirrors read blurrier levels of the picture. With ray-traced reflections on, the planar picture is skipped, since rays are exact anyway.

Exact details: [planar-reflections.md](../architecture/planar-reflections.md).

### With ray tracing

With the master switch on, on GPUs with ray queries, and on the Ultra preset (`rayTraced`), each glossy pixel traces its mirror ray instead of searching the probe. The hit is exact. Its colour comes from the probe's picture where the probe saw that point (checked by comparing distances), and elsewhere it is lit as light probe rays light what they hit. Glass on the way tints it. Screen-space reflections still go on top.

## Light that arrives by way of a mirror

Reflections let you *see* a lamp in a mirror; they do not let the mirror *throw* the lamp's light onto the floor. That light is a **caustic**: light gathered by a shiny or clear surface onto another, like the bright ripples on a pool's floor. Direct lighting runs in straight lines from each lamp, and bounce light sees a metal mirror as black (metals have no matte colour), so neither carries it. Two ways fill the gap.

### By default: image lights for flat mirrors

Light from a lamp off a flat mirror is exactly light from the lamp's **mirror image**, as far behind the glass as the lamp stands in front, its light leaving through the mirror's outline and scaled by how much the metal reflects. The path's length is the image's distance, so the falloff is right, and reflection keeps angles, so a spotlight's cone carries over.

- Each such image becomes one more spotlight with a **window** shaped like the mirror, so everything that handles spotlights (clustered shading, shadows, fog) handles it with no new code beyond "does this line pass through the window?".
- Its shadow map is drawn from the image and clipped at the mirror's plane, since from behind the glass the first thing seen would be the wall the mirror hangs on.
- The engine picks up to four images a frame from the lamps and the flat metal faces their light reaches, ranked by how bright the reflected light is, and never makes an image of an image.
- Left out: curved mirrors (they have no single image), Fresnel's change with angle and the soft beam edge of a rough mirror.

### With ray tracing: solving for the mirror point

For any mirror shape, and for a point light, a randomly fired ray never finds the one path from the bulb by way of the mirror to a given floor point: even path tracers struggle with this case. The engine solves for it instead (**manifold next event estimation**, Hanika and others, 2015). It runs for the lights marked fast (the flashlight) paired with shiny surfaces found from their materials, on the Ultra preset with the master switch on.

- **Fermat's principle as an equation.** For a floor point x and a light at L, the light arrives by way of the mirror point y where the path x → y → L is stationary: the law of reflection. Its half vector, unit(x − y) + unit(L − y), lies along the mirror's normal, so its two components along the surface vanish: two equations in two unknowns, the position of y on the surface.
- **Newton's method on the surface.** Guess y, work out how the two components change as y slides (their 2 x 2 Jacobian, from the two directions and the surface's curvature there), and step to where that linear estimate says they vanish. The step leaves the curved surface, so a ray from x through the new guess puts it back on the mirror, and the same ray proves nothing blocks x's view of it. For a flat mirror the first step is the textbook image construction, exact at once; a ball takes a few. "Manifold" because the unknown lives on a surface.
- **Brightness from the wavefront's curvature.** The bulb sends intensity I into each small cone of directions, and the irradiance where the cone lands is I times its solid angle over the area it covers. A flat mirror keeps the wavefront's curvature, so the light falls off as from the image, over the whole unfolded path squared. A convex ball adds its own curvature (the mirror equation, 1/u + 1/v = 2/R) and spreads the beam, so its reflected light is faint; a concave mirror focuses it. All of this comes out of the same Jacobian, by implicit differentiation, as one number, the spread area, which replaces distance squared in the usual falloff. The bulb's size keeps a focus finite.
- **Rough mirrors: a blurred footprint.** Brushed steel is a landscape of tiny facets tilted every way. Near y, the tilt a facet needs to send light to x grows in proportion to its distance from y, and tilts are spread like a bell curve, so the light comes from a bell-shaped patch round y. On a mirror bigger than that footprint the total equals the sharp mirror's, because the bell curve integrates to one: roughness softens the spot's edges, it does not dim it. Each pixel samples four points of the footprint a frame and anti-aliasing averages the frames.
- **Where it runs.** A compute pass between the depth prepass and the main pass solves each pixel's path, and the main pass adds the result as one more light from the mirror's direction. The same search runs where light probe rays land (so the lit spot bounces round the room) and where ray-traced reflections land (so a mirror shows the spot). It sits in passes of its own: inside the probes' tracing, its mere presence slowed every probe ray, because a GPU reserves registers for the code a shader holds whether it runs or not.

Exact maths and passes: [mirrored-lights.md](../architecture/mirrored-lights.md).

## Volumetric fog: light in the air

Without fog the air does nothing: a surface's light reaches the eye untouched. Air with mist or haze is a **participating medium**: droplets take light out of every ray and put light into it. That is why a flashlight shows as a cone, sun through a window as shafts, a street lamp has a halo, and far things fade into a pale veil. A game turns fog on by placing a `Fog` component (density, how fast it thins with height, albedo, anisotropy); the light lab's ground mist is one.

### The physics

Three rates per metre, like the attenuation coefficient of a beam through a material:

- **scattering**: light knocked into other directions;
- **absorption**: light turned to heat (water mist hardly absorbs; smoke absorbs a lot);
- **extinction**, their sum: everything a ray loses. Its integral along the ray is the **optical depth** τ, and the share that survives is the **transmittance** T = exp(−τ), Beer–Lambert's law. The **albedo** is scattering over extinction.

What reaches the eye along a ray to a surface at distance d is

  L_eye = T(d) L_surface + ∫₀ᵈ T(s) · extinction(s) · albedo · S(s) ds

the surface's light dimmed by the fog, plus light scattered into the ray at every point s on the way, dimmed by the fog between s and the eye. S sums every light arriving at s, each weighted by:

- **its shadow at s**: air in shadow gets nothing from that lamp, air in the beam does, and the difference is the visible shaft;
- **the phase function** p(θ), how droplets share out scattered light by the angle θ it turns through. It plays the part of a normalised differential cross-section: it integrates to 1 over the sphere, since scattering moves light without making any. The engine uses **Henyey–Greenstein**, set by one number g, the mean cosine of the scattering angle: 0 scatters evenly, positive values mostly forward, as water droplets do. Forward scattering is why a lamp you look towards has a bright halo.

This keeps **single scattering**: each photon is scattered once on its way to the eye. In the engine's units (a light's value is the irradiance E it gives, a pixel is π times radiance) a light adds π p(θ) E per unit of optical depth, checked by a furnace test: a uniform sky of radiance L, summed over all directions, gives back π L for any g. Multiple scattering, which would fill shadows a little, is left to the light from all around.

### Froxels: a grid of the air

Marching every pixel's ray through the fog costs too much. Instead the GPU fills a 3D grid shaped like the view: the screen cut into 160 x 90 tiles, each cut in depth into 64 slices out to 100 m (fewer on lower presets). Each cell is a small truncated pyramid of air, a **froxel** (frustum voxel). Slices are spaced evenly in log(depth + a constant): a few centimetres deep at the camera, about 7 m deep at 100 m, so each froxel is roughly as deep as it is wide.

Three compute passes run each frame:

1. **Light each froxel**: its extinction at its height, and the light it scatters towards the eye. The sun with one lookup in its shadow cascades; point and spot lights from the froxel's cluster lists (the same lists clustered shading built for pixels), each with its shadow; and the light from all around: the flat ambient times the sky visibility grid's open share by default, so a closed room's air is not lit like outdoors, or the probes' light read up and down when they run.
2. **Integrate each column** front to back, slice by slice, keeping the light gathered and the share let through so far.
3. **Fog the picture**: each pixel, at its depth, becomes colour × transmittance + gathered light.

**The slab's exact answer.** A slice is a slab of constant fog. Adding "scattered light × length" (a Riemann sum) overshoots for a thick slab. The engine adds scattered × (1 − exp(−τ)) and lets exp(−τ) through, the slab's exact solution, so one thick slab gives exactly what many thin ones do and energy is conserved however coarse the slices (Hillaire, Frostbite, 2015).

**Many samples over time.** Each froxel samples one point a frame, jittered round its middle on a Halton sequence and turned differently per froxel so neighbours do not move in step. It keeps 90% of last frame's result, read where the froxel's middle was last frame, as temporal anti-aliasing does for pixels. The grid averages dozens of samples and the shafts' edges come out smooth.

**Near a surface.** The slice a surface sits in may have its jittered sample behind the surface (sunlit air past a wall), so the grid is read only up to the slice before, and the last stretch uses that slice's medium carried forward.

### Past the grid

Beyond 100 m a formula replaces the grid. Density falls by e every scale height up, like an atmosphere, and along a straight ray an exponential integrates in closed form: the density where the stretch starts × its length × (1 − e^(−x))/x, with x the change in the exponent along it. For a level ray x is 0 and the factor's limit is 1, so near 0 the formula switches to its series, where computing (1 − e^(−x))/x directly would lose every digit. The far fog is lit by the directional lights and the sky, ending at the fog's farthest distance so the sky beyond still shows.

Its sun shadows come from a few samples a pixel along the stretch, placed at equal shares of scattered light rather than equal metres (the thin far fog would otherwise be over-counted), each looked up in the cascades and the far shadow map, which reaches kilometres. The noise in their placement is averaged by anti-aliasing.

### Glass in fog

Glass is drawn after the picture is fogged, over a picture already holding the fog up to the surface behind. So glass adds its own light dimmed by the fog in front of it, and the fog's light only for the share the glass does not let through. Worked through, that is exactly the glass and the surface behind fogged one after the other.

**Left out:** lights without shadow maps light the fog through walls; light reaching a froxel is not dimmed by the fog it crosses first; the reflection probe and probe rays see no fog.

Exact passes, grid and settings: [fog.md](../architecture/fog.md).
