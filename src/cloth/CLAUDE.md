# cloth/

Phase 7, parked (`ROADMAP.md`). Before changing a solver, the low-frequency layer or self-collision, read `docs/research/CLOTHGPU.md`.

- Cloth tests are in `runParkedTests`: do not run them; the user accepts they may break while parked. When cloth work resumes, bring them back and fix what broke.
- Compiled `-O2` in the debug build too (too hot to run unoptimised).
- The GPU port's mirror is this folder itself: `cloth.glsl` must match the CPU solver (`render/gpu/cloth_gpu`, `render/vulkan/vulkan_cloth`).
- Deterministic: `stepCloths` runs a job per cloth; colour groups solve on every core bit for bit the one-thread result.
- Self-collision on the GPU is on the unmerged `gpu-self-collision` branch, to be rebuilt with a more real-time friendly system.

## Measuring (only when the user asks)

| Command | Use |
|---|---|
| `make test-slow` | After changing the solver: vertex block descent and its levels (about 31 s) |
| `make yardstick-quick` | Judge a change: three resolutions on the CPU (about a minute) |
| `make yardstick` | Record numbers in `docs/research/CLOTHGPU.md` Part 5 (several minutes, CPU and GPU) |
| `make yardstick-dress` | The dress-sized test alone (a few minutes) |
