# The sky and the finished image

How the engine places the sun, moon and stars, lights a sky through real air, and turns the lit scene into the picture on screen: eye adaptation, night vision, anti-aliasing, upscaling, the camera's finish and the quality presets.

## Objects in the sky

Distant things show no **parallax**: walk a hundred metres and the real moon keeps the same direction and size. The cheap way to draw something like that is to keep it at a fixed offset from the camera, recomputed every frame, so moving never brings it closer. Skyboxes rely on the same trick. Its light is then a `DirectionalLight`: parallel rays, the same direction everywhere.

The demo deliberately breaks this for its moon sphere. It sits 64 m from the free camera's start, so in F3 mode you can fly there. Close to, it moves and swells as anything near would, while the moonlight (a `DirectionalLight`) keeps one direction, so the sphere stops lining up with where the light seems to come from. A point light inside the sphere would fix that, but a lamp 64 m away fades across the scene, needs an enormous intensity and casts blurry shadows: the wrong physics for a light source at infinity.

The light lab's hand-made sun disc (shown only when its real sky is off; with the sky on, the renderer draws the discs itself) uses the fixed offset. `skyObjectPose` puts a glowing sphere at 90% of the far plane, straight towards the sun, and scales it to an angular radius θ: a sphere of radius r at distance d spans asin(r/d), so r = d sin θ. The real sun's θ is 0.267°; the lab draws it 5 times larger by default (Sun disc size in F5), for looks only, since shadows keep the real size.

**The disc's brightness** is fixed by the light it must deliver. A disc of radiance L covering a solid angle Ω gives a surface facing it the illuminance E = L Ω, so

  L = E / Ω, with Ω = 4π sin²(θ/2) ≈ π θ²

(`discSolidAngle`; the half-angle form, because 1 − cos θ cancels away most of a float's digits for tiny θ). The engine's light values fold in Lambert's 1/π (light of value v falling square-on onto white paint makes it show exactly v), so E = π v and L = π v / Ω (`discRadiance`). For the real sun that is roughly 1/θ², tens of thousands of times brighter than the white wall it lights. A bigger disc glows dimmer by exactly as much, so the total light is unchanged, and at sunset the disc reddens and dims with the light itself.

One concession: full sunlight asks for a radiance well past what a half-precision float holds (65,504), and the overflow would become infinities smeared by bloom. So the radiance is capped at 30,000 by scaling all three channels together, which keeps its colour. It still burns to white through tone mapping, with bloom's glow round it. Details: `docs/architecture/sun-moon-stars.md`.

## Where the sun stands: a sundial in reverse

The labs and the forest place the sun with a sundial's spherical trigonometry (`scene/sun.h`), so one time-of-day slider moves it along a real arc: low in the east in the morning, highest at noon, setting in the west, higher in summer and lower towards the poles.

Earth spins once a day about an axis tilted 23.44° from its orbit. From the ground, the sun circles the **celestial pole** once a day, at an angle from the celestial equator set by the tilt and the season. Three angles fix it:

- **Declination** δ, the sun's angle from the celestial equator: +23.44° at the June solstice, −23.44° at the December one, 0 at the equinoxes. A cosine is close enough for lighting: δ ≈ −23.44° cos(2π (day − 355) / 365).
- **Hour angle** H, how far Earth has turned since local noon: 15° an hour.
- **Latitude** φ, where you stand.

In Earth's turning frame the sun points at (cos δ cos H, cos δ sin H, sin δ). Tilting by latitude into local east, north and up:

| Component | Axis |
|---|---|
| −cos δ sin H | east (negative after noon: the sun is in the west) |
| sin δ cos φ − cos δ cos H sin φ | north |
| sin δ sin φ + cos δ cos H cos φ | up: the sine of the sun's height |

The squares sum to 1, so it is a unit vector without normalising. Two checks in your head: at noon (H = 0) the height is 90° − |φ − δ|, so the equinox sun stands overhead at the equator; at the north pole (φ = 90°) the height is δ all day, the midnight sun. The tests check these, and sunrise and sunset at cos H = −tan φ tan δ.

Two simplifications, both invisible in lighting: the clock is local **solar** time (12:00 when the sun crosses the meridian, which differs from a wall clock by the time zone and the "equation of time", up to 16 minutes), and the atmosphere's refraction, which lifts the sun about half a degree at the horizon, is ignored.

The time of day is a setting the ticks advance (`advanceSolarHours`), so it depends on game time, never on frame rate. The light lab's clock stands still by default (Hours per second 0; 0.025 runs a day in 16 minutes) at 15:30 on the March equinox at 45° north.

**Without a real sky** the light lab picks its daylight from the sun's height by eye: the sun fades in over the last degrees of sunrise and reddens when low, and a uniform blue sky dims through dusk. The background is a `SkyBackground` component, the colour the picture is cleared to wherever nothing is drawn. It lights nothing by itself, but light probe rays escaping to the sky bring its colour back. The lab's flat ambient light is set to the same value, since a uniform sky of brightness B gives a surface facing it exactly the irradiance B (the next section's units), so ambient and probes agree.

## Light through the air: a real sky

Sunlight reaches the ground through about 100 km of air, which does two things to it.

- It **dims** it. Each thin slice removes a fixed share, so what survives falls off exponentially with the air crossed, Beer-Lambert's law:

  T = exp(−∫ β ds),

  the same law as radioactive decay along a path. β, the **extinction coefficient**, is the share lost per kilometre; its integral is the **optical depth**.
- It **scatters** what it removes into other directions, and that scattered light is the sky's glow.

Who scatters:

| Particle | Size against a wavelength | Behaviour | Scale height |
|---|---|---|---|
| Air molecules (Rayleigh) | a thousand times smaller | each a tiny dipole antenna driven by the light's field, strength ∝ 1/λ⁴: blue (450 nm) about five times red (700 nm) | 8 km |
| Aerosols: dust, droplets, haze (Mie) | about equal | every colour alike, mostly forwards: the whitish glow round the sun | 1.2 km |
| Ozone, high up | | absorbs a little orange and red, which keeps the twilight sky blue rather than grey | a layer near 25 km |

The 1/λ⁴ explains both the blue sky and the red sunset: a low sun's light crosses up to forty times more air than at noon, and the blue is scattered out of the beam on the way. Density thins exponentially with height (the barometric formula), falling by e every scale height.

Where a scattered photon goes is its **phase function**, a probability per steradian. Rayleigh's is 3/(16π) (1 + cos²θ), as much backwards as forwards; the aerosols' is a Henyey-Greenstein lobe with g = 0.8, strongly forwards.

**Single scattering.** The sky's light along one view direction is a sum along the line of sight. At each point: the sunlight arriving there (the sun times its transmittance down to that point), times the share scattered towards the eye (β times the phase function at the angle from the sun), times what survives the rest of the way to the eye. The computer adds it up in steps, a **ray march**, integrating each step's contribution exactly over its length so long steps lose nothing.

**Multiple scattering** keeps a sunset sky bright and the sky blue after sundown. Following every order is costly, but Hillaire's 2020 method notes that after one bounce the light in the air is nearly the same in all directions, so each further order is roughly a fixed fraction f of the one before. All of them together form a geometric series,

  L₂ (1 + f + f² + …) = L₂ / (1 − f),

the same series as the light probes' infinite bounces.

**Three small tables**, filled by compute shaders:

| Table | Holds | Remade |
|---|---|---|
| Transmittance | from any height in any direction out to space | only when the air changes |
| Multiple scattering | for every height and sun angle | only when the air changes |
| Sky view | the sky's light in every direction round the camera, rows packed towards the horizon where colour changes fastest | every frame |

What reads them:

- The sky drawn behind everything, and probe rays that escape to the sky, so a west window lets in the bright western sky at dusk.
- Every directional light, multiplied by the transmittance on its way down, so sunsets redden and dim by themselves. The brightest light as it reaches the ground gets the shadow cascades, which after sunset is the moon.
- The flat ambient light, which gains the sky's cosine-weighted average over the upper half.

Night falls naturally too: twilight is sunlight still lighting the upper air after the ground has gone into shadow.

**No fudge factors.** Pixels store π times the radiance, and every light is measured by the irradiance on a surface turned towards it, so the sun alone sets how bright the sky is. The light lab's sun value (4) is measured above the air; in the default mid-afternoon, with the sun about 25° up, roughly three quarters of it reaches the ground.

**Floats at a planet's scale.** At 6,360 km from the planet's centre a float's last digit is half a metre, so distances to the ground are written from the radii directly, and in the thin air at the top 1 − exp(−x) uses its series, since exp(−x) itself rounds to exactly 1 there. Details: `docs/architecture/sky.md`.

## The night sky: the moon and the stars

**Where things are.** The stars are so far away that they form a fixed pattern on a sphere round us, the **celestial sphere**, with coordinates named like Earth's: **declination** is its latitude, **right ascension** its longitude, counted east from where the sun crosses the equator in March. Earth's turning makes it spin about the celestial pole, which stands above the north horizon as high as your latitude, once a **sidereal day** (23 h 56 min). That is four minutes short of a solar day because Earth also orbits the sun: over a year it turns 366 times against the stars but 365 against the sun.

So one angle, how far the sky has turned (the **sidereal angle**), plus your latitude give one rotation matrix from the sphere's frame into the world (`equatorialToWorld`). All 9,096 catalogue stars turn by that one matrix in the shader, not one update each on the CPU. The engine derives the sidereal angle from the sun (its hour angle plus its right ascension), which keeps stars and sun on the same clock.

**The moon** circles Earth in 27.3 days on an orbit tilted 5° from the sun's path. Its position comes from **mean orbital elements** (angles growing steadily with time, like the phase of a circular orbit) plus the largest corrections for the ellipse and the sun's tug (Meeus's series cut to its biggest terms, good to about 0.3°). The tests check it against the two eclipses of early 2026, when the moon lines up with the sun, within half a degree.

**Phases for free.** The moon is a ball lit by the sun, so its phase is geometry. Each pixel of the drawn disc maps onto the near half of a sphere (straight across, since the moon is far) and is shaded by **Lommel-Seeliger's law**, brightness ∝ μ₀ / (μ₀ + μ), with μ₀ and μ the cosines from the surface normal to the sun and to the viewer. The law suits dusty ground, where light enters a little way, scatters and comes back out. Its striking property: at full moon (μ₀ = μ everywhere) the disc is evenly bright to its edge, as the real one is, while a matte Lambertian ball would darken towards the rim.

**How bright.** A half moon gives not half but about 9% of the full moon's light: at full moon every crater's shadow hides behind the rock casting it (the **opposition surge**), while away from full the rough ground shades itself. The moonlight follows the measured phase curve (Allen's). The renderer then spreads that light over the lit part of the disc by dividing by the shading's mean over the disc, which has a closed form, so the disc's light sums back to exactly the moonlight.

**The moonlit sky.** Scattering is **linear** in the light, like superposed waves: two lights' skies simply add. So the moon's sky is the sun's calculation run again with the moon's direction and brightness. The transmittance and multiple-scattering tables do not depend on where the light is, so both skies share them; only the sky view table gets a second copy. Probe rays escaping to the sky see both, so moonlight bounces into rooms by itself.

**The stars.** Magnitude is logarithmic: five magnitudes is a factor of 100, so a star's light goes as 10^(−0.4 m). Its colour comes from its **B − V index** (how much fainter it looks through a blue filter than a yellow one): Ballesteros's formula turns that into a temperature, and Planck's law for a blackbody, integrated against the eye's colour-matching curves, into a colour. Betelgeuse comes out orange, Vega white, Rigel blue-white.

A star is far smaller than a pixel. Drawn as one pixel it would blink as the camera turned and it crossed pixel borders. Instead its light is shared among the four nearest pixels by a tent weight,

  (1 − |x|)(1 − |y|),

whose shares sum to exactly 1 wherever the star falls, so its total never changes. The air dims and reddens each star through the same transmittance table as the sun, so stars fade towards the horizon and vanish below it.

**The catch: brightness.** Real moonlight is about a 400,000th of sunlight, and starlight far less. Even with eye adaptation (next), which makes up only part of a difference, a true-to-life night would be nearly black. So the moon and stars are **artistic dials** (Moon intensity and Star brightness in the light lab's settings), thousands of times brighter than real. One side effect: the day sky no longer hides the boosted stars, so the light lab fades them in by the sun's depth below the horizon.

## Eye adaptation: an exposure that follows the light

Between noon and moonlight the light falls about 400,000 times, some 18.6 **stops** (a stop is a doubling, so stops are log₂ of a ratio, as decibels are for sound). The eye copes by adapting: the pupil opens and, over minutes, the retina grows more sensitive. A camera does the same with its exposure.

Without it, a night sits in the tone curve's dark **toe**, the foot of the S-shaped curve turning scene light into screen brightness. Everything there is squeezed together and loses colour, so lit and shaded sides look alike.

**Measuring the scene.** Each frame a compute pass sorts every pixel's luminance into a **histogram** of 256 bins, each a tenth of a stop. Working in stops (logarithms) matters: brightness spans orders of magnitude, and a plain average would be ruled by the few brightest pixels. The mean is taken over the middle of the histogram only, from the 50th to the 90th percentile (darkest first). That leaves out a black sky full of tiny stars, which would push the exposure up into noise, and the sun, moon and bulbs, which would pull it down. Unreal measures the same way.

**Counting on a GPU.** Thousands of threads add to the same 256 counters, and two adding to one counter at the same instant could lose a count. **Atomic** additions make each add indivisible. Each workgroup of 256 threads first counts its own pixels in fast **shared memory** (on-chip memory a workgroup shares), then adds its totals to the frame's histogram, so the slow global atomics happen once per bin per group rather than per pixel. Addition ignores order, so the result is the same however the groups are scheduled.

**Adapting over time.** The exposure eases towards its target like a capacitor charging through a resistor: each frame the gap shrinks by e^(−dt/τ). Frames of any length trace the same curve, since the factors multiply.

- **Into the light** τ is short (0.7 s): a sunlit doorway glares for a moment.
- **Into the dark** the real eye is slow, and slowest at first: you are nearly blind just after a light goes out. A plain exponential does the opposite, taking its biggest step at once, so the room would brighten straight away and the light would seem to fade slowly. So the eye first **holds** for a second, then adapts with τ = 1 s. A brighter frame ends the wait, so a flickering light never sets it off.

**Keeping night dark.** Full compensation would turn moonlight into daylight, which looks as wrong as black: your eye adapts at night but still sees it as dark. So only half the difference from a reference brightness is made up (strength 0.5: a night five stops darker shows two and a half stops darker), and the exposure rises at most one stop (it may fall by up to four), so the dark keeps its mood and a frame of black sky is not boosted into noise. The reference is chosen so the light lab's daylit towers measure exactly at it: the day keeps its tuned look.

Everything stays on the GPU: tone mapping reads the adapted exposure the same frame, and a copy comes back to the CPU a frame or two later only for the F4 readout. Captures snap the exposure straight to its target, so each golden image depends only on its own scene. Details: `docs/architecture/image-pipeline.md`.

## Night vision: seeing with the rods

The retina has two kinds of sensor:

- **Cones**, in three kinds tuned to long, middle and short wavelengths; comparing them gives colour. They work fully above about 3 cd/m² (a dim room).
- **Rods**, a hundred times more sensitive but all alike, so colour-blind. They take over below about 0.005 cd/m² (moonlight). In between both work and blend: **mesopic** vision.

That is why a moonlit night looks grey and cool. In physical terms the moonlit sky is just the day's sky turned down some 400,000 times, even the orange glow round a low moon; rendered as a camera would record it, it reads as a sunrise.

**What the rods see.** Rods peak at 507 nm (blue-green), cones at 555 nm, so in the dark blue things look relatively brighter and red ones nearly black: the **Purkinje shift**. The engine estimates the rods' signal with Ward Larson's fit from the pixel's CIE XYZ colour,

  Y (1.33 (1 + (Y + Z) / X) − 1.68),

divided by white's value, so white keeps its brightness and only colours shift. Rods carry no colour, but the brain reads moonlit scenes as bluish (films' "day for night"), so the grey gets a slight blue tint.

**Where the rods take over.** A pixel goes grey only while both it and the scene the eye is set for are dim. The eye's state is the scene's mean brightness, eased with the same hold-then-adapt timing as the exposure, so colour drains a moment after a light goes out. A lamp in a dark street is bright, so it keeps its warm colour; a dark corner of a sunlit room keeps its colour too, since the eye is set for the window. It all acts on the light before the exposure, since receptors see light, not an exposed picture.

**The honest catch.** The thresholds should be real luminances, but the engine's light has no physical units yet and its moon is boosted. So they are placed by measurement, between the light lab's moonlit field and its sunlit room. Night vision is therefore off by default (Night vision in F5).

## Temporal anti-aliasing: many samples over time

**The problem.** Each pixel shows the scene at one point, its centre. That is sampling, and the sampling theorem says detail finer than two pixels cannot be shown faithfully: it **aliases**. Edges become stair steps; thin things, like the light lab's fence, break up and shimmer as the camera moves, because whether a wire covers a pixel's centre flips on and off. More samples per pixel cure it, at a multiple of the shading cost.

**Spreading samples over time.** Each frame the whole picture shifts by a different fraction of a pixel, the **jitter**. The offsets follow a Halton sequence, a **low-discrepancy** sequence whose every new point lands in the biggest gap left by the earlier ones, so a run of 16 covers the pixel evenly. Each pixel keeps a running average, its **history**, giving each new frame a weight of 0.1:

    history ← history + 0.1 × (this frame − history)

That exponential memory spans about the last ten frames: some ten samples per pixel for the price of one, like a long exposure from a camera that shakes by exactly a pixel.

**When things move**, the history at a pixel shows something else, and averaging would smear. So each pixel finds where its surface was last frame, **reprojection**:

- Still things and the sky: the pixel's depth gives its position in space, and last frame's camera says where that was on screen. One matrix does it, last frame's view-projection times the inverse of this frame's.
- Moving objects: a small extra pass redraws only them, writing each pixel's **motion vector** (where its surface is now minus where it was). The engine remembers every part's matrix from the frame before for exactly this.

**When the history is wrong**: a surface that was hidden last frame, or a lamp just switched on. Each pixel's 3 × 3 neighbours this frame give a box of plausible colours, their mean plus or minus one standard deviation; a history inside is kept, one outside is pulled back to the box's edge. A true history passes; a stale one is mostly removed.

**Two refinements:**

- A very bright sample (a sparkle off a metal edge) would dominate the average and flash, so each is weighed by 1 / (1 + its brightness), bloom's trick against fireflies.
- Reading the history between pixels bilinearly would blur it a little more every frame; a Catmull-Rom cubic, which passes exactly through the pixel values, keeps it sharp.

Glass and blended surfaces change pixels in ways motion vectors cannot follow, so a **reactive mask** tells the blend to trust this frame more there. The resolve runs after the main pass and the glass, so everything after (eye adaptation, bloom, tone mapping) sees the smoothed picture. Details: `docs/architecture/anti-aliasing.md`.

## Upscaling: drawing small, showing big

**The idea.** The costliest passes (the main pass with its lighting, shadows and light reads) do roughly the same work per pixel. Draw the scene at a render scale s of the screen's size on each side and they do s² of it: 0.67 draws 44% of the pixels, 0.5 a quarter. The history stays at the screen's full size, the **output size**, and the small frames are blended into it.

**Why it can work.** The jitter shifts the small grid by a different fraction of its pixel each frame, so over a cycle its samples land all over each big pixel: a coarse camera taking several shifted exposures and combining them. A still scene fills in to nearly full resolution; where things move or appear, the picture is softer until the history catches up.

**What changes in the blend.** Each output pixel rebuilds this frame's colour from the nearest small pixels, each weighed by how close its jittered sample landed (a Gaussian, exp(−2.29 d²), d in output pixels). Where no sample landed close this frame, the new frame counts for less and the history carries the pixel. The engine's own resolve keeps its 16-frame jitter cycle at any scale; for the vendors' upscalers the cycle grows with the upscaling, ceil(8 n²) frames for an upscaling of n, as AMD advises, so each big pixel still gets samples spread across it.

**The mip bias.** The GPU picks texture mips for the size of a render pixel, which at half scale is two output pixels: a level blurrier than the output deserves. A bias of log₂ s (one level more for FSR and DLSS, as their makers advise) picks sharper mips, and the history averages away the extra detail.

**One anti-aliasing choice**, four values (F5's Anti-aliasing): Off, the engine's own TAA, AMD's **FSR 3.1** (any GPU) and NVIDIA's **DLSS** (RTX GPUs, a trained network), the default. At render scale 1, the default, DLSS is DLAA: anti-aliasing with no upscaling. FSR and DLSS replace only the blend; they take the same jittered frames, depth and motion vectors, but need a motion in *every* pixel, so a small pass first fills in the camera's own motion from each pixel's depth. Each vendor has its own conventions (which way a motion vector points, in what units, how matrices are laid out), and each conversion is a single tested function. Named modes (Quality, Balanced, Performance…) just set the scale.

**Calling another compiler's library.** Both vendors build with Microsoft's compiler; the engine uses GCC. C++ compilers disagree on how they name functions inside a library and how they lay out classes and exceptions, so C++ libraries rarely mix. C has none of that: a function is a name and a calling convention, and 64-bit Windows has only one. Both vendors offer a C interface, so the engine loads them while running (`SDL_LoadObject`), looks each function up by name and calls it through a pointer. If a library is missing, the game carries on and falls back to TAA.

**Licences shape the repository.** AMD's FSR is MIT-licensed, so its DLL is committed. NVIDIA allows shipping DLSS inside an application but not handing it out alone, and it is large, so a script fetches it (`tools/fetch_streamline.py`, checking the download's fingerprint) and git ignores it, while NVIDIA's MIT-licensed headers are committed so the engine always builds.

## The camera's finish

A real camera adds its own character. Every effect here is a setting under F5's Camera group, off or neutral by default, and costs nothing while off. Details: `docs/architecture/camera-effects.md`.

- **White balance.** Light has a colour temperature, the colour of a blackbody at that temperature (Planck's law). The eye rescales its three cone responses so the light looks white (von Kries adaptation); white balance does the same, showing the chosen light as the engine's neutral 6500 K.
- **Grade.** Contrast (stretching stops about mid grey), saturation (each channel's distance from grey), and lift, gamma and gain (shadows, midtones, highlights).
- **Lens effects.** Vignetting darkens the corners by cos⁴ of the angle off axis, the natural fall-off of light through a lens; chromatic aberration splits red from blue towards the edges, as glass bends each wavelength differently; film grain adds a fine moving noise.
- **Motion blur.** A shutter open for part of a frame sees things move. Each pixel is smeared along its own screen motion (the velocity image anti-aliasing already keeps) times the shutter's share, 180° (half the frame) by default. Samples are weighed by depth, so a moving thing in front smears over the background while a still post in front of a moving background stays sharp (McGuire 2012). It needs anti-aliasing on.
- **Depth of field.** A thin lens brings one distance to a point; nearer or farther points spread into a disc, the **circle of confusion**, whose size grows linearly in 1/distance (the lens equation). The f-number sets the aperture, and autofocus eases onto whatever is under the crosshair. At half resolution, each pixel gathers the discs that reach it, with far and near layers kept apart so a blurred foreground spreads over what lies behind it (Jimenez 2014). Frosted glass reuses this gather: a point behind the pane spreads by its own distance behind the glass.

Order after anti-aliasing: depth of field, motion blur, eye adaptation, bloom, then tone mapping (where white balance, vignetting, the grade and grain apply). The sharp picture is kept aside for next frame's reflections and gathered light.

## Judging on a typical GPU

The defaults (the Ultra preset, DLSS at full size) suit the strongest GPUs. **Quality presets** (Low, Medium, High, Ultra) trade looks for speed in one choice: shadow resolution and reach, probe rays, reflection and ambient occlusion detail, the fog's grid. They never touch taste (exposure, bloom's strength, the camera's finish, upscaling) or ray tracing, which is its own switch, off by default. The full table: `docs/architecture/settings-and-commands.md`.

The **typical GPU setup** is one switch (F5's checkbox, the `typical-gpu` command, or `--typical-gpu` at launch) that draws the engine as a mid-range PC would: the High preset, ray tracing off, and the Quality upscaling mode (two thirds of the pixels on each side) through DLSS, else FSR, else the engine's TAA. Switching it off restores exactly the settings it found. Judge a feature with it on: that is how most players will see it.

Settings that need hardware ray tracing are marked where their struct lists its fields, so on a GPU without ray queries F5 greys them out and the `set` command says they do nothing there.
