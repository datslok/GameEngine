# GPU and CPU timing

Covers: `src/render/gpu/gpu_timing`, `src/render/cpu_timing`

How the renderer measures itself for the F4 readout and the `stats` command. The readout's profilers and lines live in `engine/debug_views` (`game-interface.md`); the pass order is in `renderer.md`.

## GPU passes (`render/gpu/gpu_timing`)

API-free, tested in `gpu_timing_test`.

- `GpuPass`: every timed pass in recording order (the list is the header; names in `renderer.md`'s pass table). `gpuPassName` gives the short names (UI, TLAS, SKY, ...), also used as debug labels.
- `gpuTimestampCount` = passes + 1: one stamp before the first pass, one after each.
- `timestampTicksBetween`: the difference masked to the queue's `timestampValidBits`, so a counter that rolled over still counts right.
- `gpuTimingsFromTicks`: seconds per pass from the device's timestamp period; the passes sum to `totalSeconds`; not `valid` with zero valid bits.

### Recording

- Each frame in flight has a query pool of `gpuTimestampCount` (none when the queue has no valid bits), reset at the start of its command buffer.
- `beginGpuPass` / `endGpuPass`: `writeTimestampsThrough` writes every stamp up to the pass's start or end with `vkCmdWriteTimestamp2` at `ALL_COMMANDS`, so a stamp lands when everything before it finished and a skipped pass lasts 0; a debug label marks the pass between.
- Tone mapping, debug lines and overlay labels sit inside their one rendering block.
- `finishGpuTimings` writes any stamps left and marks the frame complete; only `render` and `capture` call it, so an abandoned frame is never read.

### Reading back

- `beginFrame`, after the fence: `vkGetQueryPoolResults` (64-bit, no wait; `VK_NOT_READY` skips the frame) into `unreportedGpuTimings`.
- `drawScene` moves them into `RenderStats::gpu`: each measured frame reported once, as many frames late as there are in flight.
- `Application`'s `gpuProfiler` (`makeGpuProfiler`, `recordGpuTimings`) shows them in three groups (setup, lighting, picture: `firstLightingPass`, `firstPicturePass`), idle passes hidden.

## CPU stages (`render/cpu_timing`)

- `CpuStage`: SKYBAKE, WAIT, SETUP, FRUSTUM (the camera's CPU part: glass and the mirror's candidates), LIGHTS, SHADOWS, CAPTURES, LINES, PACK, RECORD, SUBMIT (meanings: the header).
- `CpuStageClock`: `start`, then `lap(stage, now)` credits the time since the last lap to that stage, so a stage met twice adds up; `finish` gives `CpuStageTimings` into `RenderStats::cpu`.
- Shown as RENDER by stage (`makeCpuStageProfiler`, `recordCpuStageTimings`) beside the engine's SCENE by part (`SceneStage`). Tested in `cpu_timing_test`.

## Cloth passes

- A frame records too many cloth dispatches to give each a `GpuPass`: the whole cloth is one pass (CLOTH), and while F4 shows, `ClothPassStamps` time them by kind (`ClothPass`): `cloth-gpu.md`, Timing.
- The cloth's GPU time a tick (`GpuPass::Cloth` over `RenderStats::clothTicks`) also charges the tick budget: `engine-core.md`.
