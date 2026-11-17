# Build log: Phase 6: Rendering foundations, lighting and global illumination (Oct 18-24 2026) (part 2 of 5)

## Phase 6: Rendering foundations, lighting and global illumination (Oct 18-24 2026) (part 2 of 5)
### 6d: global illumination (Oct 19 2026 onwards) (continued)

- `ultra-gi-defaults`, then `sun-clock-slower` (Oct 20 2026):
  - The light lab's sun clock runs by default, 0.025 h/s (a day in 16 minutes; was 0), and golden views stop it (`prepareGoldenView`), so their 0.5 s of ticks leave the sun at its hour.
  - The first branch also made 256 rays a probe and a 0.5 s irradiance time constant the defaults (the same rays times time constant as 64 at 2 s, so the same noise at a quarter of the lag, for four times the rays).
  - The user then went back to 64 rays and 2 s, so the golden images are the ones from before.
- `ddgi-ray-budget` (Oct 20 2026): the roadmap's optional cap on probe updates a second, built as a ray budget instead of probes taking turns.
  - **The two were weighed:**
    - Probes updating in turns (16 interleaved groups, each blending with its own time since its last update) would have needed a ray cycle per group (a probe updated every 4th frame of a global 16-frame cycle sees only 4 of its 16 phases) and a special case for probes near fast lights, whose 0.05 s memory cannot wait.
    - Tracing fewer rays a frame keeps every probe updating every frame with today's blend, for at most a 4x saving (16 rays is the least) and the per-probe fixed costs kept.
    - The user took the simpler one.
  - **Measured before building:** `ddgi_test`'s dark-room texel model (a 15 degree bright patch, tau 2 s, 200 s per run, two seeds), the crawl over 0.1 s:

    | Rays and frame rate | 15 degree patch | 8 degree patch |
    |---|---|---|
    | 64 rays at 60 fps | 0.94-0.96% | 2.0% |
    | 64 at 240 fps | 0.32-0.33% | 0.72% |
    | 16 at 240 fps, the 16-frame cycle | 0.93-0.95% | 1.96-1.98% |
    | 16 at 240 fps, 32 | 0.91-0.93% | 2.27-2.31% |
    | 16 at 240 fps, 64 | 0.93-0.99% | 2.04% |

    So noise goes with rays a second as expected, and stretching the cycle to keep the long spiral at 1,024 points (the first plan) buys nothing: the cycle stays 16 and no shader changed.
  - **A bug on the way:** the ladder's first form stepped up only at the whole step and failed its own test, since a steady 1/240 s comes back as 239.99998 fps. A real 240 jitters both sides, so a step up is taken 5% short of it.
  - Off by default (a budget of 60 at 240 fps crawls like 60 fps, three times today's 240), so the golden images are unchanged.
- `lightlab-default` (Oct 20 2026): the light lab is the default game (first in the registry), so a plain start, `make run` and double-clicking the packaged exe open it. The zip's `Demo.bat` starts the demo (it had `LightLab.bat` the other way round). The user's choice.

#### Adaptive memory, sharing and the quality presets

- `ddgi-adaptive-memory` (Oct 20 2026): with the ray budget at 60 the user saw a fair bit more noise, which is what 60 fps looks like without one: the 2 s / 64 rays were judged at 240 fps.
  - **Reading the probes differently was weighed first and set aside.** Every probe traces the same directions each frame and neighbouring texels share their rays, so their noise is shared and blurring them averages little of it away. TAA's memory (under 0.1 s) is far shorter than the crawl.
  - **The noise is in each texel's memory, so that is where the change went.** A scratch experiment on `ddgi_test`'s dark-room texel (200-320 s runs, steps between states 40 s apart) came first.
  - **A first version** kept an 8 s memory and pulled the stored light towards a 0.2 s window's mean while they differed by more than 4 of the window's standard deviations. It was as calm as a fixed 8 s, never firing falsely, but a doubling took 10-17 s, since it stopped pulling while most of the gap was left.
  - **Restarting the history instead** (a running mean from the window's mean, as denoisers' history lengths do) fixed that for clear changes: a lamp switched on or off at 60 fps came in within about 1 s instead of 4.6, crawling a quarter as much with 8 s.
  - **But near the information limit nothing helps:** an 8 degree patch at 60 fps is seen by about 0.3 rays a frame, so its doubling cannot be told from luck in a fraction of a second, and the filter fell back to its long memory (17-18 s). Slow drifts lag by it too (ramp error 15-18% against 5-6%).
  - **Hence:**
    - a 4 s long memory in the measurements (crawl halved, switches within 0.5-1.1 s, the worst case 9 s)
    - the noise kept with the long memory's share, starting from the stored light (the fixed blend now writes alpha 0, so switching on mid-run does not jump)
    - off by default for the user to judge on screen, so the golden images are unchanged; switched on, the light lab's probe views moved by at most 5 of 255 and the validation layers reported nothing.
  - `probeLuminance` replaced the luminance `hitLightStrength` computed itself, on both sides.
  - Sharing light between neighbouring probes (independent rays per probe, then a visibility-aware blend) is on the roadmap as the next step, since only more information beats the dark room's limit.
- `ddgi-probe-sharing` (Oct 20 2026): at 30 fps with 64 rays and 2 s the user still saw a lot of noise in the dark back room, and found a longer memory's lag unacceptable even with the adaptive memory, so the cure had to be more information, not more memory.
  - **More rays measured poorly at 30 fps** (`ddgi_test`'s dark-room model, 2 s): 64 rays 1.41%, 128 0.98%, 256 0.85%, against 0.58% for 64 at 120 fps with the same rays a second. Each update is a big step at 30 fps however good its estimate.
  - **A suspected leak, before building:** the user's screenshot of a warm glow along the back room's ceiling against the partition raised a leak; dense probes made it go away.
    - A temporary capture of that view with each suspect changed apart measured the strip along the partition: the visibility floor at 0, the self-shadow bias at 0 or 0.6, and no probes above the roof changed nothing (30.9 red against 20.4 further away, unchanged within 1).
    - With the doorway closed the strip became the ceiling's darkest part (4.5 against 6.9), as an inside corner should.
    - So there is no leak through the wall: the probes beside the doorway, 35 cm from the partition and 50 cm below the ceiling, see the sunlit floor through the doorway at an angle the corner above cannot, and the corner reads mostly them (a parallax error, on the roadmap as finer probes at openings).
  - **Sharing was then built as planned:**
    - First each probe's own turn of its rays. With one turn for all, neighbours err alike and averaging removes nothing: seven probes in the model at 30 fps, weight 0.5, 1.41% with one turn, 0.54% with their own, theory 0.40 of it.
    - Then a pass averaging each probe with its six face neighbours it can see, into separate atlases so the blur never feeds back into the probes' history.
    - The turns went in a per-probe buffer of identity rows when off, rather than a flag in the full push constants, so sharing off is bit for bit the old picture.
  - **Measured** in the light lab's back room with temporary capture overrides (frame time 1/30 s, warm-up 600-1203 frames, half-float captures 3 frames apart):
    - crawl 1.61% RMS without sharing, 0.77% at 0.5, 0.73% at 1, against 0.88% at 120 fps without, so 30 fps with sharing is calmer than 120 fps was
    - the room's mean light rose 7% at 0.5 (light borrowed from brighter neighbours it can see)
    - the back room's floor seen through the doorway brightened most (up to 31 of 255 in the sunlit-room view).
  - Off by default for the user to judge; validation layers clean with it on.
- `ddgi-fast-light-toggle` (Oct 20 2026): a setting to stop keeping fast lights apart (`fastLights`, on by default), asked for by the user now that the adaptive memory exists.
  - Off, `probeHitLights` lists every light as still, so the flashlight's bounce goes into the still atlas under its memory, which with the adaptive memory restarts where the change stands out from the noise.
  - Golden images unchanged.
- `gi-defaults` (Oct 20 2026): the user's refined settings became the defaults, taken from their `settings.cfg` without further measuring:

  | Setting | New default | Was |
  |---|---|---|
  | Rays a probe | 128 | 64 |
  | Ray budget | 60 updates a second | off |
  | Adaptive memory | on, with a change threshold of 8 standard deviations | off, 5 |
  | Probe sharing | 0.5 | off |
  | Fast irradiance time constant | 0.1 s | 0.05 |
  | The light lab's sun clock | still by default (0 h/s) | 0.025 |

  - The clock-pinning test now sets it moving itself.
  - The tests of the schedule's own rules (CPU, `plainProbeSettings`) and the GPU's real-pass checks now set the plain probes (64 rays every frame, the fixed memory, no sharing) explicitly.
  - The light lab's views with probes were saved anew.
- `gi-quality-presets` (Oct 20 2026): the quality presets set the light probes' rays too, the same for still and fast probes, as the user asked: Low 32, Medium 64, High 128, Ultra 256, the rest of global illumination as tuned.
  - That clashed with Ultra being the defaults (128 still, 256 fast rays then).
  - The user chose to make the defaults High, keeping the 4096 cascades (so High, the default, no longer saves shadow memory for typical GPUs; Medium's 2048 cascades do), with the fast rays down to 128 (only the flashlight golden view changed, by at most 1 of 255).
  - Ultra is now High with 256 rays.
- `low-preset-cascades` (Oct 20 2026): on Low the user saw light seeming to clip through the light lab's walls, a ragged bright fringe where the sunlit room's partition meets the ceiling and stair steps on the doorway jamb.
  - It was first taken for the probes (32 rays), but a longer distance memory changed nothing, and Medium with Low's probe rays did not show it.
  - It was the sun's shadows: 2 cascades of 1024 over 60 m, leaving 30 cm walls a texel or two thick.
  - 1024 cascades saved no memory (the atlas is 128 MB for any cascade up to 2048), so Low's cascades went to 2048. Low stays cheaper than Medium in cascade count and reach, lamp shadows, bloom and probe rays.
  - Low is not the default, so the golden images are unchanged.
- `low-preset-cascade-count` (Oct 20 2026): with 2048 cascades Low still clipped a little; the user found 3 cascades (Medium's count) fixed it, so Low has three, reaching 60 m against Medium's 120.
- `gi-preset-sharing` (Oct 20 2026): the quality presets set sharing between probes too, as the user asked: Low 0.5, Medium 0.3, High 0.1 (so the default drops from 0.5 to 0.1), Ultra 0, more sharing making up for fewer rays. The light lab's views with probes were saved anew.
- `ddgi-fast-sharing` (Oct 20 2026): the only flicker left for the user was a flashlight at 30 fps (the fast atlas's 0.1 s memory averages three frames there).
  - So the fast atlas got its own sharing weight, `fastProbeSharing`, 0.5 by default and untouched by the presets; the pass already averaged the two atlases apart, so it was a second weight in its push constants.
  - Only the flashlight golden view changed (at most 5 of 255). Not measured; judged by the user.

#### Scene events

- `ddgi-scene-events` (Oct 20 2026): the roadmap's scene events, with a ceiling lamp in the light lab's back room (L) to try them on, as the user asked. Decisions:
  - **Restart, not the paper's multipliers.**
    - 2021 scales every probe's hysteresis by 0.85 for 4 frames or 0.5 for 10.
    - The adaptive memory already has a restart (a running mean, n / (n + 1)), and a game's event says outright what the adaptive memory has to prove from its noise, so an event restarts the probes it covers and they average the frames since.
    - The fixed memory and the distances have no history of their own, so the CPU counts frames since each probe's event (`GpuProbeRestart`, a per-probe buffer in each frame in flight's upload memory, since `GpuProbeVolume`'s 128 bytes of push constants were full) and they keep min(keep, n / (n + 1)).
  - **Regions**, as the user chose from the two options (and as the paper suggests): a restarted probe is noisy for a few frames (one frame's rays at first), so an event carries a box (the lamp's reach) and only probes inside restart; a time-of-day jump uses everywhere.
  - **The game signals**, as the user chose over the engine spotting light switches itself (which could not see geometry changes): a `ProbeEvent` component lasting one frame, gathered with the lights and cleared by the engine after the frame is built.
  - **The first version failed:** it restarted only the stored light and kept the change detector running with the old noise.
    - In the dark room model a lamp then came in in 0.27 s at 60 fps, not the fixed memory's 0.044: the dark room's tiny noise made the lit room's own noise pass for change after change, each restarting from a window that still held the darkness.
    - Taking the noise as at least the light did not help (the first frame's estimate is often the darkness itself, with no ray on the lamp).
    - What worked: for the change window's length after the event, the window's mean is the plain mean too, the noise is measured afresh and no change is looked for (`restartedProbeBlend`, chosen by `adaptiveProbeUpdate`, which the shader calls).
    - Measured: 0.044 s at 60 fps, 0.008 at 240 (0.81 and 0.96 s with the adaptive memory alone), the settled crawl unchanged.
  - **Golden captures** repeat one frame description 960 times; its events now go to the first warm-up frame only, or every repeat would restart the probes.
  - **The lamp:** warm, intensity 2, range 7 m, picked by eye in a new golden view (`back-room-lamp`); its panel casts no shadow so it blocks neither its light nor probe rays.
    - Off at the start, but the panel is still drawn, so the `flashlight` view, which shows it at the top, changed.
    - Two views with the building in sight changed by a hair (overview: 36 pixels past tolerance, at most 4 half-float steps; sun-disc within tolerance): the acceleration structure holds one more instance, so a few probe rays came out a hair different, though they skip the panel.
    - With the panel hidden, every reference matched bit for bit, so the events themselves change nothing until signalled. The light lab's references were saved anew.
  - **Moving the clock by hand** (the slider) past 0.1 h signals a light change everywhere; the clock running by itself does not, its sun moving smoothly.
  - **More sharing during a restart:** the user asked for sharing to rise to 0.5 while a restart runs, against the noise of averaging few frames, and chose a fade over a step back (a step would sharpen the bounce in one frame).
    - A restarted probe's still weight fades linearly from `eventProbeSharing` (0.5) back to the preset's over the restart's period, about 2 s, only for the probes in the region.
    - Building it showed that an event on a frame with fresh atlases (the `back-room-lamp` view switches its lamp on before the first frame) started a restart that ran on, sharing more, through the warm-up. Such frames keep nothing, so they now start no restart, and that view's reference was saved again (unchanged to the eye).
  - **The soft restart:** at 30 fps on Low the user saw a lot of noise in the sunlit room when switching the lamp. The event's region, a cube of the lamp's 7 m reach, covered the whole sunlit room, whose probes restarted though the lamp hardly changes their light (Low already shares 0.5, so the boost added nothing there).
    - Offered: restarting only the back room, more event sharing, or a softer restart. The user chose the soft restart, with a slider (`eventMemorySeconds`): through a restart a probe keeps at least the share a short memory keeps each frame.
    - Measured in the dark room model at 30 fps:
      - a restarted probe whose light did not change errs 22.5% over the first half second (1.4% left alone), 16.9% at 0.05 s, 13.4% at 0.1 s, 8.1% at 0.3 s
      - while a switch takes 0.05, 0.14, 0.26 and 1.45 s to reach 90% (a running mean keeps its first samples, so the old light fades with a tail).
    - 0.05 s is the default. Even softly a restarted probe is far noisier than one left alone, so the light lab's region is the larger lever.
    - Golden images unchanged. The user preferred to keep the general mechanism rather than tune the light lab's region.
  - **A growing memory:** the lamp then went dark far slower than the flashlight.
    - A restart averaged the frames since the event as a plain mean, so its first frames, holding lamp light (the soft restart's share, and the bounce of last frame's probes, which probe rays read), faded only as 1 / time: at 30 fps 1.7% of the lamp was left after a second, as bright as the dark room itself.
    - Now a restart's memory grows with the time since the event, its time constant `eventMemoryGrowth` (0.5) times that time, so an early frame's weight falls as the square of the time ratio instead.
    - Measured in the dark room model at 30 fps: within 1% of darkness after 0.30 s instead of 1.32 (the flashlight 0.46 s), for a restarted probe whose light stays 20.6% instead of 16.8% over the first half second and 4.5% instead of 3.3% to 4 s; 0.25 was 0.20 s for 26.3% and 7.2%.
    - A slider (`eventMemoryGrowth`) lets the user judge. Golden images unchanged.
  - **The change check:** dim areas the lamp barely touches still flickered at each switch, since every texel in the region restarted, however little its light changed. The user's idea: change quickly where the change is big, slowly where it is not.
    - Built as a check (adaptive memory only): for 0.5 s after an event each texel compares the mean of the frames since with its stored light in units of its own noise (shrinking as 1 / sqrt(frames)) and restarts only past 4 of them.
    - A restarted texel is marked by a negative history while the restart runs, so the growing memory follows its own frames since its restart.
    - In the dark room model at 30 fps, a texel the lamp makes 15% brighter or leaves alone now flickers 0.74%, as with no events, instead of 13.9% and 14.9%. The lamp itself comes on in 0.16 s and goes dark in 0.28 s (3 gave 0.09 and 0.13 s; 5, 1.3 and 4.9 s, a sparsely lit texel's noise taking too many frames to tell apart).
    - Measuring it showed the soft restart now made restarted texels noisier (24.2% against 22.8%: keeping a restart's single first frame longer keeps its noise), so it applies only where texels still restart outright: the fixed memory and the distances.
    - Golden images unchanged.
  - **The flashlight's switch as an event,** as the user asked.
    - Its bounce lives in the fast atlas, which the check (on the still atlas) never touched, so the fast atlas now restarts with the still one, outright (no noise measured there, and its 0.1 s memory makes a restart cheap).
    - `Flashlight::toggle` signals a light change over the beam's 50 m reach, which the check makes harmless for still light it hardly changes.
    - Dark room model: on within 0.02 s instead of 0.17 at 30 fps, dark at once instead of 0.43 s, no more flicker.
    - Golden images unchanged (the flashlight view's switch falls on its fresh first frame).
- `ddgi-event-spam` (Oct 20 2026): spamming L, at low frame rates especially, left the lamp to dim very slowly.
  - **Reproduced** in the dark room model (the switching harness gained short states and the renderer's own rule for ending restarts, `advanceRestartFrames`, made public for it).
    - A sparsely lit texel needs about 13 frames to tell its light going out from its noise, so each switch's check was cut short by the next and never restarted it, and the light took the long memory to go dark once the switching stopped (13-16 s).
    - A single switch at 15 fps failed too, the 0.5 s check being 7 frames (18 s).
  - **Two fixes:**
    - A texel the last event restarted (still marked) restarts at once at the next, unchecked, since the same light is most likely being switched back.
    - The check lasts at least 16 frames (`minEventCheckFrames`, `eventCheckFrames`).
  - Now dark at once after switching every 0.1-1 s at 15 and 30 fps, 0.73 s after one switch at 15 fps. Golden images unchanged.

#### Relocation and the sky

- `ddgi-probe-relocation` (Oct 20 2026): probe relocation and probe states, the 2021 paper's rules with the reference code's numbers (256 fixed relocation rays, at most 5 steps, inside past 25% back faces, an offset limit of 0.4 of a cell, too close under 0.1 of the smallest spacing, nudged by up to 0.2 of it). Decisions:
  - **Relocation runs on the GPU** with ray queries against the static level, not on the CPU against the game's `StaticLevel`.
    - It sees exactly what the probes see, the renderer never sees the collision level (and a game's collision meshes may differ from what is drawn), and the dense volume (19,035 probes x 256 rays x up to 6 traces) would take seconds on the CPU in a debug build.
    - The records come back through read-back memory, read by the next frame after waiting for the GPU (a one-off hitch, only after start-up, a new volume, a geometry change or switching relocation).
    - The CPU then owns each frame's states: it knows them for the readout and for restarting woken probes, and the rules are tested directly.
  - **What is static is marked by the game** (`ModelRenderer::isStatic`, carried by `DrawItem` and the new ray mask bit `RAY_MASK_STATIC`), defaulting to moving.
    - A forgotten mark costs only probes kept awake, while guessing from components (a `PreviousTransform`, spinners, fireflies) would miss the next kind of mover and leave probes asleep beside it.
    - The light lab marks its ground and blocks, the demo its ground.
  - **A probe that sees no static surface at all sleeps** instead of being switched off, against the paper: something that moves may still come near it, and a level with nothing marked static then lights its movers as before (every unmarked draw wakes the probes round it).
  - **Vigilant** means a front-face hit inside the probe's own cells (one spacing each way round its grid point) grown by the self-shadow bias's longest reach.
    - The cell a surface's biased point falls in picks the probes, so these are exactly the probes some surface reads.
    - The pass records how far the nearest front face lies beyond the cells, and the CPU compares that with the bias every frame, so changing the bias needs no new pass.
  - **Awake:** a sleeping probe within a mover's bounds grown by the same and `wakeCells` (1) more cells, so it has settled by the time the mover arrives.
  - **A probe that starts being traced, or moves, restarts outright** (no share of its old light kept, every texel restarted unchecked) through the scene events' restarts.
  - **Readers:** sampling skips off and sleeping probes, and a point with none round it gets the flat ambient; sharing skips them and measures between moved probes.
  - **The debug balls** sit where the probes moved, off ones red, sleeping ones dark blue, awake ones with a yellow rim. The F4 readout gained a probe line (probes, traced, awake, asleep, off, moved).
  - **Measured** in the light lab (golden warm-up frames, release build, RTX 5080):
    - The 1 m volume moves and switches off no probe (none lies in a wall or within 10 cm of one), and 392 of 960 sleep, the probe pass 0.036 ms instead of 0.050.
    - The dense 20 cm volume switches off 350 probes buried in walls (only 5 could move out: the limit is 8 cm there, less than half a 30 cm wall) and puts about half to sleep, 0.25 ms instead of 1.0.
    - With relocation and states both off the light lab's views match the old references within tolerance (a few hundred pixels one half-float step apart: rounding in the reshaped shaders, the same pixels as before the level was marked static).
    - With them on its views changed by at most 3 of 255 on screen, and the light lab's references were saved anew.
- `sky-atmosphere` (Oct 20 2026): the sky and the air (Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique").
  - **What was built:** an `Atmosphere` component (Earth's air by default, in km as the paper) and three tables:
    - a transmittance table (256 x 64, made once)
    - a multiple scattering table (32 x 32, the paper's geometric series 1 / (1 - f), made once)
    - a sky view table (192 x 108, every frame).
  - **What it does:** drawn over the background, lighting probe rays that escape, dimming and reddening every directional light on its way down, and adding the light it gives an open surface to the flat ambient (computed on the GPU, the frame's own ambient plus the sky's cosine-weighted mean over the sky above).
  - The light lab switches to it by default (its hand-made day stays as the fallback, `atmosphere` off).
  - **Decisions, with the user** (recommended and accepted):
    - the sky pass draws the sun's disc (its radiance through the air, so it reddens at sunset; the light lab's disc entity only without the atmosphere)
    - the flat ambient from the sky on the GPU
    - no aerial perspective yet (in the light lab's 400 m world the air in front of the farthest tower lets about 99% through, so the haze would not show; it waits for kilometre scales, phase 13)
    - the light lab only (the demo keeps its flat night sky and its golden images).
  - **Measured and changed from the paper:**
    - Sampling each step at its midpoint, not 0.3 of the way in (straight up, 0.3 made blue's optical depth 2.5% too deep, the light kept 0.9% low against the closed form; the midpoint is within 0.05%), with steps growing as the square of their number (even 2.5 km steps put the whole 1.2 km aerosol layer in the first).
    - The distance to a sphere from the radius and the cosine rather than a position, its near root found from the far one (at 6,360 km a float's last digit is half a metre, and the squares' rounding several square kilometres, so the GPU and the CPU disagreed by a metre on rays grazing the ground).
    - 1 - exp(-depth) by its series below 1e-3 (in the thin air near the top the GPU's exp rounded to exactly 1 and the multiple scattering lost those steps).
    - The sunlit ground added where a sky view ray ends on the planet (without it the planet beyond the light lab's 400 m field showed as a dark band below the horizon). The light lab gives the planet its grass's albedo, so the land beyond the field matches it.
  - **Units with no fudge factors:** a sky pixel is pi times the sun's value times the scattering integral per unit illuminance, so the sky's brightness follows from the sun's. The light lab's sun is now its light above the air, 4 by default, so in the default mid-afternoon (the sun 25 degrees up, 71% of green getting through) the ground gets about the 3 it had.
  - **Cost:** the sky's tables and ambient cost 0.015 ms a frame on the RTX 5080 (the sky view table is the same size at any resolution).
  - **Golden images:** every light lab view changed (sky, sun colour, ambient) and its references were saved anew; the demo's matched bit for bit, its flat ambient now read from a buffer instead of the frame's uniform block.

#### Cascades for large worlds

- `ddgi-cascades` (Oct 20 2026): cascades for large worlds.
  - **What changed:** the probes no longer live in a box a game places but in nested grids round the camera, the densest 0.5 m apart and each further one twice as far apart (four by default, 21 x 13 x 21 probes each: the densest lights 3.6 m round the camera at full weight, the fourth 29 m), all moving with it. So light bounces wherever the camera goes and no level places or tunes probes.
  - **Decisions, with the user:**
    - Cascades only, no hand-placed volumes beside them: the user wants lighting emergent, with as little per-level tuning as possible. A fixed volume per room stays possible later, since the code blends any list of volumes.
    - The light lab's two hand-placed volumes (the 1 m one round the building and the 20 cm test) are gone.
    - Scrolled-in probes start from the coarser cascades' light (seeds), with a switch to compare.
  - **Scrolling:** each cascade is a window on a lattice fixed in the world (multiples of its spacing from the origin), its middle the lattice point nearest the camera.
    - When the camera crosses into another cell, the plane falling out behind is reused for the plane coming in (a circular buffer: `ProbeVolume::scroll`, slot = (grid place + scroll) mod count).
    - So every other probe keeps its slot, its light and its history, and only the new plane starts.
    - Corners, neighbours and event regions are found by grid place, never by slot (the slot next to the last plane wraps to the far side).
  - **The blend is centred on the camera**, not on the window, so it moves smoothly and nothing pops when a plane jumps: the densest first, each cascade weighted by its fade times what the denser ones left, the flat ambient last.
    - Its reach was derived rather than tuned. The camera lies up to half a cell from the window's middle, so the nearest outer plane is at least (count - 1) / 2 less half a cell away.
    - The weight ends one more cell and the self-shadow bias's reach short of that, so the cell round any point it lights (found from its biased point) never touches an outer plane, where a scroll brings in probes that are new (tested over 20,000 random cameras and points).
    - [2021]'s rule (shrink the blend by a cell) left the cell round a point touching the new plane by up to the bias's reach.
  - **New planes wait for relocation:** a plane the scroll brings in waits off for its own relocation, then starts.
    - Relocation used to be read back the next frame after waiting for the GPU (a hitch, fine once at start-up); with a cell crossed every few frames when walking that would stutter.
    - So each frame in flight now has its own read-back records, read when that frame comes round again (its fence has passed: no waiting), and each record is kept only if its probe still stands at the lattice point it was asked for at.
  - **Seeds:** a probe that starts (scrolled in, woken, newly vigilant) is seeded by a small pass (`ddgi_seed.comp`) with the coarser cascades' light where it stands, in each texel's direction (the irradiance a surface there facing that way would get), counted as 4 frames of its own history and fading as its own rays come in.
    - Measured in the dark room model (128 rays, 60 fps), the error over its first quarter second: 13.3% from its rays alone, 9.6% seeded 20% off, but 24% seeded with twice or none of the light (a coarse probe that sees through a doorway the fine one cannot); from half a second on all within half a percent.
    - Kept on, with `seedProbes` to compare by eye.
  - **One set of atlases holds every cascade**, one after another (the grid counts.z times cascades layers deep), so every pass runs one dispatch over all of them. A per-frame buffer of cascade records (origin, spacing, scroll, first probe, weight) replaces the volume's numbers in the push constants and the frame block, which lost its probe origin and spacing.
  - **Quality presets:** High (the default) and Ultra four cascades of 21 x 13 x 21 from 0.5 m, Medium four of 17 x 11 x 17, Low three of 15 x 9 x 15 from 1 m.
  - **Measured** in the light lab (golden views, debug build, RTX 5080), against the single 1 m volume:
    - the probe pass 0.22-0.50 ms instead of 0.034-0.051 (3,700-5,600 of 22,932 probes traced, the rest asleep or off)
    - the whole frame 0.47-1.11 ms instead of 0.30-0.59
    - the engine's video memory 734 MB instead of 486 (about 250 MB of probes).

    About a third of the probes are off: a window centred on a camera at eye height reaches as far below the ground as above it. From the high overview camera no cascade reaches the ground and every probe sleeps (the flat ambient lights the view, as it lit everything outside the old volume).
  - **Found by the user walking the light lab at sunset:** dark blotches and sharp bands on the ground.
    - Every cascade's lattice started at the world's origin, so each had a layer exactly on the ground (y = 0).
    - A probe exactly on a surface starts its rays on it, where they hit or slip through by rounding, and rounding in the probes' positions (measured from the camera and back) put some a hair above and some a hair below, which saw only the ground's underside and lent darkness. The old volume had sat on half-metre marks for this reason.
    - Now every lattice is offset half the densest spacing (0.25 m) from the world's origin, the same in metres for every cascade so they stay nested, which keeps probes off whole and half metres, where levels put their floors and walls; a capture of the same view showed the ground smooth.
    - The probes' debug balls also scaled with each cascade's spacing and buried the view, so they show one cascade at a time (`shownProbeCascade`).
  - **Golden images:** every light lab view changed (finer bounce light in the rooms, the grass's green bounce on the towers' shaded sides, which the old volume did not reach) and its references were saved anew; the demo's matched bit for bit.
