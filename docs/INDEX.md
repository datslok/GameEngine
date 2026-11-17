# Docs index

Every doc, one line each. A doc missing here fails the docs test.

## Reference

| Doc | What it is |
|---|---|
| `README.md` | Pitch, features, install, build, controls (the packaged demo reads its `## Controls`) |
| `ROADMAP.md` | Planned and done work by phase; the current phase; work order |
| `docs/LAYOUT.md` | `src/` folders and their layers |
| `docs/ARCHITECTURE.md` | How each system works now (being split into `docs/architecture/`) |
| `docs/architecture/build.md` | Makefile, the two builds, shaders, dependencies, packaging |
| `docs/BUILDLOG.md` | Why things were built, what was tried and measured; history by date |
| `docs/ENGINE_GUIDE.md` | Prose guide for learning the engine (for a reader with a physics background) |
| `docs/claude-md-migration.md` | Phase 19's audit of the old `CLAUDE.md`; deleted when phase 19 is done |
| `tests/golden/README.md` | Golden reference images: layout per GPU and game |
| `assets/stars/README.md` | The star catalogue's source |

## Research (read before building)

| Doc | Read before |
|---|---|
| `docs/ILLUMINATION.md` | Light probes, DDGI (phases 6d, 17) |
| `docs/LUMEN.md` | The software tracer or the Lumen-like layer (phase 14) |
| `docs/SHADOWCACHE.md` | Shadow caching for still and moving casters |
| `docs/PERSISTENTSCENE.md` | The persistent GPU scene's later stages |
| `docs/LOD.md` | Level of detail, impostors, card thinning |
| `docs/JUNGLE.md` | The jungle's layers, sizes and density (phase 18) |
| `docs/TREES.md` | Growing trees (phase 18) |
| `docs/VINES.md` | Lianas, climbers, epiphytes (phase 18) |
| `docs/WIND.md` | The wind field and swaying |
| `docs/GROUNDCOVER.md` | Grass and floor cover |
| `docs/CLOTHGPU.md` | Any cloth solver, low-frequency layer or self-collision work (phases 7, 10); yardstick numbers in Part 5 |
| `docs/RAGDOLLPLAN.md` | Active ragdolls (phase 8); check formulas marked as from memory |
| `docs/DESTRUCTION.md` | Flight and destruction (phase 15); check facts marked as from memory |
