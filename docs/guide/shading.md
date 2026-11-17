# Lights and shading

What lights the engine has, how their light falls off, and how a surface turns arriving light into the colour you see: physically based materials, glow, clear coats and parallax from height maps. Shadows have their own chapter, `shadows.md`.

## Lights are components

A light is just a component on an entity, like a mesh or a collider:

| Component | Shines | Placed by |
|---|---|---|
| `DirectionalLight` | parallel rays from very far away: the sun, the moon | the direction its light travels; no `Transform` |
| `PointLight` | every way from a point: a bulb, a muzzle flash | its entity's `Transform` |
| `SpotLight` | a cone: a flashlight, a floodlight | `Transform` plus a beam direction and two cone angles |
| `AmbientLight` | evenly from everywhere, a stand-in for bounced light | nothing; several add up |

Point and spot lights carry `castsShadows` (on by default), a `priority` and a `range` beyond which they give exactly nothing.

Two kinds of colour live in the engine, and they mean different things:

- A **light's** colour is a 0..1 tint with a separate, unbounded `intensity`, so "which hue" and "how strong" are separate knobs.
- A **material's** colour is a reflectance, the fraction of light the surface sends back, which can never pass 1, so 0..255 bytes suit it.

Each frame `collectLighting` walks the `World` for lights. Point and spot positions are interpolated between ticks like meshes, so a lamp carried by something moving does not judder. `packLighting` then writes them, measured from the camera, into the storage buffers the shaders read: up to 4 directional, 1024 point and 256 spot lights, all seated by default. When a scene holds more than the shaders take, or more shadows are asked for than exist, the renderer chooses (`shadows.md`, More lights than seats). Exact fields and layouts: `docs/architecture/lights.md`.

## Diffuse light: Lambert's cosine law

A matte surface scatters what it receives evenly in every direction, so it looks equally bright from anywhere. What changes is how much it receives: a beam meeting the surface at a slant spreads over more area, as sunlight does on the ground in winter. With `N` the unit normal and `L` the unit vector towards the light:

```
diffuse = lightRadiance * max(0, N · L)
```

## Point lights: inverse square with a window

Light from a point spreads over a sphere of area 4πd², so its strength falls as 1/d². The engine changes two things:

```
falloff = window² / (d² + r²),   window = clamp(1 - (d / range)⁴, 0, 1)
```

- **The source radius `r`** (`sourceRadius`, 1 by default) keeps the light finite at the bulb. A real lamp is not a mathematical point; close to it light arrives from its whole glowing area. Far away (d ≫ r) this is plain inverse square.
- **The window.** Pure 1/d² never reaches zero, so every light would touch every pixel. The window bends it to exactly zero at `range`, with zero slope there, so no edge shows. A light ending somewhere is what lets clustered shading skip it everywhere else.

## Spotlights: a cone with a soft edge

A spotlight is a point light dimmed by the angle off its beam's axis:

```
cone = smoothstep(cos(outerAngle), cos(innerAngle), dot(-L, beamDirection))
```

Inside the inner angle it is full; past the outer angle it gives nothing; between, it fades (the penumbra). A real flashlight's edge is soft because its bulb has size: each part of the filament throws a slightly different cone, and their overlap blurs the rim. A hard cutoff would look stencilled.

Comparing cosines instead of angles (a larger cosine means nearer the axis) saves an `acos` per pixel; the two cosines are worked out once on the CPU. The cone multiplies the same distance falloff as a point light. The flashlight both games share (`gameplay/flashlight`) uses `sourceRadius` 4.5, gentle up close yet reaching across a room.

Two placement details matter:

- The flashlight sits in the right hand, a little beside and below the eye, aimed at the centre of the view. At the eye exactly, every shadow it casts would hide straight behind its caster.
- A light that follows the camera is moved every frame in `onUpdate`, not in ticks, because the camera itself moves per frame; tick-rate updates would make the beam lag behind mouse look.

## Physically based materials

Every pixel asks one question: of the light arriving along `L`, how much leaves towards the eye along `V`? The answer is the **BRDF**, the bidirectional reflectance distribution function `f(L, V)`, and one light adds `f · E · (N · L)`, with `E` the light's irradiance.

The engine uses the model glTF files are authored in, **metallic-roughness**. A material is three numbers, each optionally varied across the surface by a texture:

- a **base colour**;
- **metallic**, 0 or 1 (in between only for mixtures like dusty metal);
- **roughness**, 0 for a polished mirror up to 1 for fully matte.

### Microfacets

Zoom in far enough and any surface is a landscape of tiny perfect mirrors pointing every which way. By the law of reflection, a facet can send light from `L` into the eye only if its normal lies along the **halfway vector** `H = normalize(L + V)`. The specular part is then a product of three factors, each a physical question:

```
f_specular = D(H) · G(L, V) · F(V · H) / (4 (N · L)(N · V))
```

- **D, the distribution (GGX):** what share of facets face along `H`? A probability density over directions,

  `D = α² / (π ((N·H)²(α² − 1) + 1)²)`, with `α = roughness²`,

  normalised so the facets' projected area equals the surface's. A smooth surface has a narrow peak (a small, bright highlight), a rough one a broad one. GGX's long tails give highlights the soft glow measured materials show.
- **G, shadowing and masking (Smith, height-correlated):** of those facets, how many can both the light and the eye see? At grazing angles bumps hide each other, like a wheat field seen edge-on. The height-correlated form counts a facet hidden from both only once, since a deep valley tends to hide from both together.
- **F, Fresnel (Schlick's fit):** how much does a mirror reflect rather than let light in? Maxwell's equations at the boundary give the answer; `F = F₀ + (1 − F₀)(1 − V·H)⁵` fits it closely. `F₀`, the head-on reflectance, is about 4% for glass, plastic, stone and paint (refractive index 1.5), and everything turns into a mirror at grazing incidence: a lake at sunset, a wet road ahead.
- **The denominator** is geometry: the change of variables from facet normals to reflected directions, and the projection onto the surface.

### Diffuse, and why metals have none

Light that enters a non-metal scatters about beneath the surface and comes out in all directions: Lambert's `colour / π`. It only gets what the mirror part did not reflect, so it is weighted by one minus the specular albedo, and energy balances.

In a metal the free electrons absorb refracted light within nanometres, so **a metal has no diffuse part**; its colour lives in its reflection instead (gold's `F₀` is about 1.00, 0.77, 0.33). glTF blends the two cases:

```
F₀ = mix(0.04, baseColour, metallic)        diffuse colour = baseColour · (1 − metallic)
```

That is why a highlight on red plastic is white (4% of white light) but on gold is gold.

### Light lost between facets

The model so far counts light that leaves after one bounce. On a rough surface much of it hits a second facet before escaping, and a single-bounce model loses that: a perfectly white, fully rough metal came out returning only a third to two thirds of the light, depending on the view.

The fix (Turquin 2019) scales the specular part by `1 + F₀ (1 / E − 1)`, where `E` is the share a perfect mirror's facets return from that view: exactly the missing light for a white metal, and close for coloured ones.

`E` depends only on the view angle and roughness and has no closed form, so `buildBrdfTable` integrates it numerically at start-up into a 32 × 32 table. Each entry averages 4096 reflected directions:

- drawn more often where `D` is large (**importance sampling**: sample in proportion to the integrand and that factor cancels);
- placed along a Hammersley sequence (evenly spread points, which converge far faster than random ones);
- one entry per job on the job system, so it costs a fraction of a second.

The shader reads the table once per pixel.

### The white furnace test

The physics check: put a surface that absorbs nothing inside a furnace glowing equally in every direction. It must send back exactly what it receives, from every view at every roughness; more would create energy, less would lose it. The tests integrate the shader's BRDF over the whole sky and require a white metal and a white non-metal to return 1 within 2%. Others check that GGX integrates to 1, that swapping light and eye changes nothing (Helmholtz reciprocity), and Fresnel's two limits.

### Units

A textbook BRDF carries a 1/π in Lambert's term. The engine multiplies the whole BRDF by π instead, which amounts to defining a light's value as the irradiance it gives a surface facing it: a white matte surface lit head-on by a light of 1 shows 1. Unreal and Frostbite fold π into their light units the same way.

### Light from everywhere

Light arriving equally from all directions is the furnace situation, so a surface returns exactly its albedo of it: the weighted diffuse colour **plus** the specular albedo from the table. Without the second term, a metal lit only by ambient light would be pitch black.

Real surroundings are not uniform, so the flat ambient is only a starting point. Other passes refine it: ambient occlusion darkens creases, the sky visibility grid dims it indoors, reflections replace its specular half, and (when switched on) light probes measure the real bounce light in its place. Their chapters cover them.

### Textures that are numbers

glTF packs roughness and metallic into one texture's green and blue channels. They are plain numbers, not colours, so they must not be sRGB-decoded: they load as **linear** textures (`TextureEncoding::Linear`, plain `UNORM`). The same file asked for both ways becomes two textures.

The model's details, the table's format and normal map filtering: `docs/architecture/shading.md`.

## Emissive: surfaces that glow

Everything above is light a surface reflects. A lamp, a screen or the moon's disc also emits its own, independent of what reaches it. The material's `emissive` colour is simply added at the end. Give a glowing object a black base colour and it shows only its own light, looking the same by day and night; the engine's glowing stand-ins for real lights (bulbs, floodlight heads) are made this way, black and fully rough so other lights barely reflect off them.

Glow does not light the surroundings by itself. The light a bulb throws comes from its separate `PointLight`; the halo round a bright light comes from bloom, a post-processing pass. Where light probes run, a glowing surface does light its neighbours through them, except the stand-ins for real lights, whose light would otherwise be counted twice.

## The whole sum

```
colour = ambient · (diffuse colour + specular albedo) + Σ (diffuse + specular) per light + emissive
```

Light superposes linearly, like intensities in optics, so each light just adds its share. A surface with no usable normal gets ambient and emissive light only. The sum is done in linear light into a floating-point image, which tone mapping turns into the screen picture afterwards.

## Clear coat: a second layer of shine

Car paint, lacquered wood and varnished floors are two layers: a base under clear varnish. Light meets the varnish first, and some reflects off it as off glass (4% head-on, nearly everything at grazing): a second, sharp, white highlight over the base's own. The rest goes on into the base.

- **Exact energy.** The coat's share of light from everywhere (its directional albedo, read from the same 32 × 32 table) is exactly what the base no longer receives, so the base is dimmed by one minus it, and a coated white surface still passes the furnace test.
- **Smooth over bumps.** Varnish fills the dents a normal map draws, so the coat uses the geometry's normal: lacquered wood keeps its grain under a mirror finish.
- **One reflection per pixel.** The reflection pass follows whichever layer is smoother (the coat, on car paint); the other keeps a cheaper stand-in. Two would double its cost.

The light lab shows it with two red balls ahead of the start, one bare and one lacquered. Switch: `MaterialSettings::clearCoat` (F5 Materials). Details: `docs/architecture/shading.md`.

## Parallax occlusion mapping: depth from a height map

A normal map tilts the light but leaves the surface flat: seen at a slant, nothing hides behind anything. A **height map** says how high each texel stands. Each pixel follows its view ray down into it, as into a small landscape under the polygon, stepping until the ray first drops below the ground, and reads every texture at that spot (Tatarchuk 2006). A brick's near edge then hides the mortar behind it.

- **The geometry.** In the surface's own frame, a ray going a fraction t of the way to the deepest level moves sideways by t × depth × tan θ, with θ the view's angle from the normal. It is a pencil's shadow lengthening as the sun sinks. So bumps look tallest seen side-on, and grazing views need more steps (8 looking straight down, up to 64).
- **No stair steps.** Between the last step above the ground and the first below, the ground is taken as a straight line and the crossing solved exactly.
- **Self-shadowing.** The same walk from the found point towards the sun says whether a bump shades it: brick edges darken the mortar beside them, more as the sun sinks.
- **Mips from the unshifted place.** The shift jumps at a brick's edge, so texture detail is chosen from where the flat polygon is, not where the ray landed; otherwise edges would sparkle.
- **Limits.** The outline stays flat, and everything that reads the depth image (ambient occlusion, contact shadows, screen reflections) still sees the flat polygon. Real geometric detail at scale is a different technique, cluster-based levels of detail like Unreal's Nanite.

The light lab's low brick wall with cobbles in front shows it. Details, and the layered grounds built on it: `docs/architecture/parallax-and-grounds.md`.
