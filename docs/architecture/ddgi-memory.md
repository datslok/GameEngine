# Light probe memory: fast lights, events, sharing

Covers: nothing of its own; its code is in the files `ddgi.md` covers

How long the light probes remember, and what makes them forget sooner: fast lights, the ray budget, the adaptive memory, scene events and sharing between probes. All in `render/gpu/ddgi` (its GPU cases: `GPU_TEST_PROBE_ADAPTIVE`, `_RESTART`, `_SHARING`). On hold with the rest of the probes: `ddgi.md`. Every field's reason and the measurements behind each default: `GlobalIlluminationSettings` in the header.

## The frame's schedule

`scheduleProbeFrame(schedule, settings, frameSeconds, fastLights, fresh)` → `ProbeFrame` (`ProbeSchedule` keeps the frame number and smoothed frame rate):

- Ray counts clamped to 16..1024; every probe at the still count when no fast light is listed.
- The frame's ray rotation and phase (`ddgi.md`, Rays).
- Shares kept from each time constant (`hysteresisForTimeConstant`); 0 for fresh atlases.

**Ray budget** (`probeUpdatesPerSecond`, 60; 0 off): above that frame rate the still count is divided by `probeRayDivisor(current, fps, budget, rays)`, a power of two, about the largest not above fps / budget.

- Hysteresis: step up `rayBudgetStepUpShare` (5%) early, down only `rayBudgetStepDownShare` (10%) below; frame rate smoothed over `rayBudgetSmoothingSeconds` (0.25 s); never below `minRaysPerProbe`.
- Fast probes keep `fastRaysPerProbe`; the cycle stays 16 frames and each frame blends by its own time, so only noise changes, and noise goes with rays a second.

## Fast lights

`PointLight::fast`, `SpotLight::fast` (the flashlight): their bounce would trail over the 2 s memory, and light adds up, so it is kept apart.

- The trace writes each ray's still light (sun, sky, emissive, still lamps, still atlas's bounce) and fast light (fast lamps at the hit, fast atlas's bounce) separately; blended into two irradiance atlases with `irradianceTimeConstant` (2 s) and `fastIrradianceTimeConstant` (0.1 s); summed where read.
- Probes within reach of a fast light (`probeNearLight`: closer than its range plus `probeMaxDistance`) trace `fastRaysPerProbe` (256), the rest `raysPerProbe`.
- The fast blend counts a frame as at most `fastBlendMaxFrameSeconds` (1/30 s): a hitch cannot replace its history with one noisy frame.
- `fastLights` off: `probeHitLights(..., keepFastApart = false)` lists every light as still; the fast atlas fades out.
- Frames without fast lights leave the fast atlas exactly zero.

## Adaptive memory

`adaptiveMemory` (on), `adaptiveProbeBlend`, `AdaptiveProbeTexel`: each still irradiance texel is a running mean that restarts when its light really changes, like a denoiser's history length. Fast atlas and distances keep fixed memories.

- **Running mean:** blend factor min(longKeep, n / (n + 1)), n the history in frames (irradiance alpha, at most `DDGI_MAX_HISTORY` 2048, exact in a half float): a plain mean since the restart until the long memory keeps more.
- **Change window** (`changeWindowSeconds`, 0.2 s): its mean and the deviation of each frame's luminance (`probeLuminance`, Rec. 709) about it, in `changeStatistics`; the deviation blended by the long memory's share and kept as a deviation so a half float cannot overflow.
- **A real change:** the window's mean leaves a band of `changeThreshold` (8) x its spread sigma sqrt((1 - k) / (1 + k)) (k: the share kept a frame), plus `DDGI_CHANGE_FLOOR_SHARE` (1%) of the light. The stored light becomes the window's mean, history k / (1 - k).
- A texel with no history (fresh atlases, or the memory just switched on: the fixed blend writes alpha 0) starts from its stored light with the long memory's history, so nothing jumps.
- `GpuProbeVolume` carries the window's share in `rotation[2].w` and the threshold in `sky.w` (0: the fixed blend).
- Limits: changes too faint to tell from noise in a few frames, and slow drifts, follow only the long memory.

## Scene events

A game tells the probes about a change it knows of, which they would otherwise follow only over their memory.

- `ProbeEvent` (`scene/probe_volume.h`): `ProbeEventKind` `LightChange` or `GeometryChange`; `everywhere`, else the probes in `region`. `probeEventAround(kind, centre, reach)` (a light's range) or `(kind, centres, reach)` (lights switched together).
- One-frame component: `signalProbeEvent(world, event)` (`scene/lighting`) adds it on its own entity; `collectLighting` gathers them in order into `FrameLighting::probeEvents`; `Application::describeFrame` calls `clearProbeEvents` once the frame is built.
- Passed to `VulkanDdgi::update` while `sceneEvents` is on. Golden captures send them on the first warm-up frame only.
- Senders: the flashlight on every switch (`Flashlight::toggle`, the beam's reach), the light lab's lamps, street and sunlit room on their switches and everywhere when the clock is moved by hand, the demo's lamp fields and floodlights.

## Restarts

`GpuProbeRestart` (48 bytes; the shader's `ProbeRestart {ivec2 frames; vec2 keeps; vec4 check; ivec4 seed}`), worked out on the CPU since the update's push constants are full: frames since an event reached the irradiance and the distances (negative: none), the share of each kept this frame, `ProbeEventCheck {threshold, growth, frames}`, `fastKeep` (`check.w`), the seed slot.

**Each frame** `advanceProbeRestarts(restarts, volume, events, frame, restarted, seeded)`:

1. Moves running restarts on a frame, ending each once its ramp (counted from the end of the check) reaches the frame's share kept.
2. Sets 0 for probes in each event's region: a light change the irradiance, a geometry change the distances too; only for an atlas the frame keeps some of (fresh atlases have nothing to forget).
3. Probes that started (`ddgi-cascades.md`, Probe states) restart outright, threshold 0.
4. A volume of a new size starts with none.

**The event check** (adaptive memory; `adaptiveProbeUpdate`, `eventCheckBlend`): for `eventCheckSeconds` (0.5 s, at least `minEventCheckFrames` 16, `eventCheckFrames`) each texel compares the mean since the event with its stored light.

- Restarts from that mean past `eventChangeThreshold` (4) x the noise measured before the event, shrinking as 1 / sqrt(frames + 1), plus the 1% floor; otherwise keeps its long memory.
- A big change restarts at once, a smaller one after (threshold x noise / change)^2 frames; barely touched light never flickers. Threshold 0 restarts every texel.
- A new event restarts a still-marked texel at once, unchecked: its noise over a few frames is no basis for a check, and a light toggled faster than the check decides would otherwise never restart.

**A restarted texel** (`restartProbeTexel`): history marked negative while the restart runs.

- `restartedProbeBlend` while its frames are fewer than the window holds (`historyForShare(windowKeep)`): the window's mean is the stored light, noise measured afresh, no change looked for (looking at once let the lit room's own noise pass for change after change). Then `adaptiveProbeBlend`.
- Memory grows back: `restartedProbeKeep(longKeep, 0, growth, n)` = min(longKeep, exp(-1 / (growth n))), so an earlier frame's weight falls as (its time / now)^(1 / growth). `eventMemoryGrowth` 0.5: growth 1, near the plain mean, kept a switched-off lamp's light for seconds.

**Without the check** (fixed memory, and the distances): restart outright with `restartedProbeKeep(keep, shortKeep, growth, n)`: nothing kept on the event's frame, never less than a memory of `eventMemorySeconds` (0.05 s) keeps (the soft restart, `ProbeFrame::restartHysteresis`), so first frames are averages. Not applied with the check: keeping a first frame longer only kept its noise.

**The fast atlas** restarts with the still irradiance, outright (no noise measured, short memory anyway), keeping `fastKeep` with no soft restart (it would keep a switched-off light's bounce).

## Sharing between probes

`probeSharing` (0 by default, Ultra; High 0.1, Medium 0.3, Low 0.5) and `fastProbeSharing` (0.5, every preset; the fast atlas's own weight, against a flashlight's flicker at low frame rates). Either above 0 runs the pass and turns the rays.

- `sharedProbeValue`: own light weight 1, each of the 6 face neighbours (`DDGI_SHARE_NEIGHBOURS`) the sharing weight, over the weights. A neighbour counts only if the probe's mean distance towards it (`probeDirectionTexel`) reaches `DDGI_SHARE_VISIBLE_SHARE` (0.9) of the way (`probesSeeEachOther`).
- `ddgi_share.comp` (one group a probe, push constants `GpuProbeShare` 32 bytes, after both updates): bindings 0 restarts, 1 states (untraced probes keep their shared copy and are no one's neighbour; separations between relocated positions), 2 cascades, 3-5 the atlases, 6-7 the shared copies. `VulkanDdgi::prepare` points the scene set, balls and next frame's bounce at the shared copies while sharing is on; the probes keep accumulating their own light, so the blur never feeds back.
- **Own turns:** `probeRayTurn(probe)` (`randomProbeRotation` of 2^62 + probe, a number no ray cycle reaches), applied before the frame's rotation, so neighbours' errors are independent; with one shared turn sharing removes nothing. `GpuProbeTurn` rows (48 bytes, `packProbeTurns`): identity while sharing is off (rays bit for bit unchanged), rewritten after `waitIdle` when switched (they live in upload memory).
- **During a restart:** `eventProbeSharingWeight(weight, eventWeight, n, longKeep)`: `eventProbeSharing` (0.5) on the event's frame, fading linearly to `probeSharing` over `historyForShare(longKeep)` frames, never below it; carried in `GpuProbeShare::weight` z and w. Needs the pass running.
