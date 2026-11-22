# tests/

- Plain `assert` tests, one `testXxx()` a file. Adding one: `add-test` skill.
- No GPU or window. GPU tests (`tests/gpu/`, `make gputest`) are their own program, never registered in `test_main.cpp`.
- Never compile with `-DNDEBUG` (every assert would vanish).
- `docs_test.cpp` checks the docs: layers from `docs/LAYOUT.md`, budgets, line lengths, paths, the index, repeated text. `tests.exe --docs-report` only prints warnings (dates, measurements, backticked names the code lacks), for the `docs-review` skill.

## Lists in `test_main.cpp`

| List | Holds | Run |
|---|---|---|
| `routineTests` | Everything else | `tools/visible_tests.ps1` (release; `-Build debug`); `--times` lists the 30 slowest; `tests.exe --only <name>...` runs just those (seeing a planted bug fire) |
| `runSlowTests` | Many seconds, guarding things rarely changed: cloth solvers, the golden views' scenes | `make test-slow`, only when asked or before a golden run |
| `runParkedTests` | Work on hold: light probes, cloth, signed distance field | Never; fixed when that work resumes |

## Shared helpers (add to these, don't copy)

| File | Holds |
|---|---|
| `test_levels.h` | `near`, `block`, `flatLevel`, `addRamp` |
| `demo_helpers.h` | `tickSeconds`, `mobaInput`, `firstPersonInput`, `runTicks`, `runFrame` |
| `test_jobs.h` | `testJobs`: give it to every game a test builds (`Game::useJobs`) |
| `forest_helpers.h` | `TestForest`, `FieldCounter`, `trianglesFaceTheirNormals` |
| `cloth_helpers.h` | `untetheredSettings` |
| `cull_helpers.h` | `randomCullScene`, `packCullSceneLevels` (GPU-driven drawing's scenes); `testView`, `cutView`, `placedInstance`, `stillRecord` (mesh cluster views) |
| `planting_helpers.h` | `straightMaps`, `rollingGround`, `threeLayers`, `samePlant` (planting from maps) |
| `mesh_helpers.h` | `rippledSheet` (a curved open sheet for mesh tools); cluster cuts: `positionNumbers`, `openEdges`, `enclosedVolume` |

## Known issue

The glTF tests use a fixed temp folder and throw if it exists; a failed assert leaves it behind. Delete `game_engine_gltf_*` in the system temp folder.
