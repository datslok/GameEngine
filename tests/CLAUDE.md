# tests/

- Plain `assert` tests, one `testXxx()` a file. Adding one: `add-test` skill.
- No GPU or window. GPU tests (`tests/gpu/`, `make gputest`) are their own program, never registered in `test_main.cpp`.
- Never compile with `-DNDEBUG` (every assert would vanish).

## Lists in `test_main.cpp`

| List | Holds | Run |
|---|---|---|
| `routineTests` | Everything else | `tools/visible_tests.ps1` (release; `-Build debug`); `--times` lists the 30 slowest |
| `runSlowTests` | Many seconds, guarding things rarely changed | `make test-slow`, only when asked |
| `runParkedTests` | Work on hold: light probes, cloth, signed distance field | Never; fixed when that work resumes |

## Shared helpers (add to these, don't copy)

| File | Holds |
|---|---|
| `test_levels.h` | `near`, `block`, `flatLevel`, `addRamp` |
| `demo_helpers.h` | `tickSeconds`, `mobaInput`, `firstPersonInput`, `runTicks`, `runFrame` |
| `test_jobs.h` | `testJobs`: give it to every game a test builds (`Game::useJobs`) |
| `forest_helpers.h` | `TestForest`, `FieldCounter`, `trianglesFaceTheirNormals` |
| `cloth_helpers.h` | `untetheredSettings` |

## Known issue

The glTF tests use a fixed temp folder and throw if it exists; a failed assert leaves it behind. Delete `game_engine_gltf_*` in the system temp folder.
