# CLAUDE.md migration (phase 19 audit)

Where each part of `CLAUDE.md` (103 KB, Nov 17 2026) goes. Deleted from this file when phase 19 is done.

**Buckets:** ROOT (every task) · MODULE (`src/<x>/CLAUDE.md`, `tests/CLAUDE.md`) · RULE (`.claude/rules/`, by path) · SKILL (`.claude/skills/`) · DOCS (`docs/`, `README.md`) · CHECK (a test enforces it) · DELETE (duplicate or stale, reason given).

**Spot checks of duplication** (CLAUDE.md term → found in): `thinnedShare`, `forestGroundCells`, `ClimbHost`, `chooseContactLights`, `glassMapSquare`, `keepStillDraws`, `backgroundShare`, `hashSimulationState`, `maxUniformBlockBytes`, `VK_ADD_LAYER_PATH` → ARCHITECTURE. `rwildcard`, `runParkedTests`, `-ffast-math`, `visible_tests` → nowhere but CLAUDE.md (README names `visible_tests` once).

## Intro

| Text | Bucket | Destination | Notes |
|---|---|---|---|
| Goal: FPS + MOBA, genre-agnostic core, later dressmaking, solar system, superhero | ROOT | `CLAUDE.md` overview | 3 lines |
| Phase-by-phase status (6a-6e, 16, 18, 7 sub-items, dates) | DELETE | — | Duplicates `ROADMAP.md`; root keeps "current phase: see ROADMAP.md" |

## Build and run

| Text | Bucket | Destination | Notes |
|---|---|---|---|
| `make`, `make run`, `make release`, `make run-release`, `GAME=` | ROOT | `CLAUDE.md` build table | |
| Shaders compiled by glslc, `.vert/.frag/.comp`, `-MD` deps | DOCS | `docs/architecture/build.md` | |
| List of every `*_shared.h` header and its contents (~8 KB) | DELETE | — | Each header is named in its system's architecture section; the folder listing is the index |
| `*_shared.h` hold only `#define`s of plain numbers behind a guard; C++ builds typed constants from them | RULE | `.claude/rules/shader-mirror.md` | |
| `make test`, `--times`, `make test-slow` (when to run), `make test-parked` (do not run) | MODULE | `tests/CLAUDE.md` | Root keeps one line: routine run command |
| Yardstick targets and what they measure | MODULE | `src/cloth/CLAUDE.md` | Numbers stay in `docs/CLOTHGPU.md` |
| `make golden`, `golden-save`, bit-exact references, only on request | ROOT + SKILL | root hard rule (only on request); `.claude/skills/golden/` | |
| `make gputest`, only on request; new library function needs case, number, branch | ROOT + SKILL | root hard rule; `.claude/skills/add-shader-function/` | |
| Makefile mechanics: rwildcard, one object per source, `-MMD -MP`, `-j`, rebuild timings | DOCS | `docs/architecture/build.md` | Timings → BUILDLOG if missing |
| Debug vs release flags, `-O2` folders in debug, no `-DNDEBUG`, no `-ffast-math` | DOCS + ROOT | `build.md`; root: never `-DNDEBUG` | |
| Tests through `tools/visible_tests.ps1`; `make test-release` only before commit and after merge; build debug too for warnings | ROOT | hard rules | Also in memory: deduplicate |
| Inlined tiny maths in headers, `-O2`/inlining/`-flto` measurements | DOCS | `build.md` (rule) + BUILDLOG (numbers) | |
| Dependencies (SDL3, glslc, cgltf, MikkTSpace, meshoptimizer, ImGui, volk, vk-bootstrap, VMA, FidelityFX, Streamline, validation layers) | DOCS | `docs/architecture/build.md` + README Requirements | README has most |
| Fresh Windows machine (winget, pacman) | DELETE | — | Already in README Install |
| MSYS2 `PATH` prefix for PowerShell | ROOT | build table | |
| `build/`, `*.exe`, `*.spv` git-ignored | DELETE | — | `.gitignore` says it |
| `package_demo.py`, `--anonymous` | DOCS | README "Sharing the demo" | Rule "keep README Controls current, the zip reads it" → root workflow |
| Source files are CRLF | ROOT + CHECK | hard rule; docs check test | |

## Source layout

| Text | Bucket | Destination | Notes |
|---|---|---|---|
| Layer order and one-line purpose per folder | DOCS | `docs/LAYOUT.md` | Root keeps the order in one line |
| Per-folder file and function inventories (~45 KB) | DELETE | — | Already in ARCHITECTURE (spot-checked); missing items move there during the split |
| Dependencies point downward only; games never include `platform/`, `render/`, SDL; `render/` never includes `input/`, `platform/`, `assets/` | ROOT + CHECK | hard rule; layer check test | |
| Includes from `src` root with folder, never `../` | ROOT + CHECK | hard rule; layer check test | |

## Architecture summary

| Text | Bucket | Destination | Notes |
|---|---|---|---|
| Full reference is `docs/ARCHITECTURE.md`; read a system's section before changing it, update it in the same branch | ROOT | "where to look" | Points to `docs/architecture/` after the split |
| Live path, Game hooks, game declared before engine | DOCS | `architecture/application.md` | Rule "declare the game before the engine" → `src/engine/CLAUDE.md` |
| Main loop: 120 Hz ticks, budget, never read `Input` in a tick, interpolation | ROOT (rule) + DOCS | hard rule: Input never in a tick; rest in architecture | Duplicated under "Notes for upcoming work": merge |
| Settings: one struct per system, `visit()` naming every field | ROOT | hard rule: new tunable → its struct's `visit()` | Detail in architecture |
| Commands: socket, `--send`, `--hidden --port 47620` copies | ROOT + SKILL | root rule; `.claude/skills/drive-game/` (start hidden, send, screenshot, quit, check) | |
| Always close every game you start | ROOT (IMPORTANT) | hard rule | Also in memory: deduplicate |
| Every feature with settings controllable from the command line | ROOT | hard rule | Also in memory |
| Game → renderer via `FrameDescription`; no GPU types in games | DOCS | architecture | Covered by layering rule |
| "Renderer today" (~20 KB of passes and numbers) | DELETE | — | Duplicates ARCHITECTURE renderer sections |
| Uniform blocks ≤ 4096 bytes, `static_assert` every uniform struct | MODULE | `src/render/CLAUDE.md` | |
| Shader maths mirrored on CPU and tested; maths goes in a `.glsl` library of pure functions, not a pass's `main` | RULE | `.claude/rules/shader-mirror.md` | List of mirrored libraries → DELETE (folder is the list) |
| Assets: typed handles, append-only, encodings | DOCS | architecture | |
| ECS: `each` rules (rarest first, queued destroy, `addLater`) | MODULE | `src/ecs/CLAUDE.md` | |
| Physics summary; character tuning approved, ask before retuning | MODULE | `src/physics/CLAUDE.md` + `src/gameplay/CLAUDE.md` | |
| Games: demo, light lab, cloth lab contents | DELETE | — | In ARCHITECTURE "Games and the light lab" and README |
| Software renderer: reference, tests only | DOCS | architecture | |

## Conventions

| Text | Bucket | Destination | Notes |
|---|---|---|---|
| Math: row-major `Mat4`, column vectors, right-handed, -Z, quaternions, Euler only for poses | ROOT | conventions (4 bullets) | |
| Reversed-Z, camera-relative main pass | MODULE | `src/render/CLAUDE.md` | |
| GPU matrices: column-major via `writeColumnMajor`, normal matrix | MODULE | `src/render/CLAUDE.md` | |
| UVs: bottom-left, `uvOrigin`, glTF `flipVertically = false` | MODULE | `src/assets/CLAUDE.md` | |
| Light units: linear, sRGB material bytes | MODULE | `src/render/CLAUDE.md` | |
| Style: names, block comment above each function, braces, RAII, exceptions | ROOT | conventions | |
| No duplication; tunables in one place; performance exception | ROOT | hard rule | Also in memory |
| Branches: one per feature, `--no-ff`, push, delete | ROOT | hard rule | |

## Tests

| Text | Bucket | Destination | Notes |
|---|---|---|---|
| assert-based, one `testXxx()` a file; shared helpers in `test_levels.h`, `demo_helpers.h`, `test_jobs.h`, `forest_helpers.h`, `cloth_helpers.h` | MODULE | `tests/CLAUDE.md` | |
| Register in `routineTests`; parked and slow lists | SKILL + MODULE | `.claude/skills/add-test/`; `tests/CLAUDE.md` | |
| Tests need no GPU; GPU tests are their own program | MODULE | `tests/CLAUDE.md` | |
| Never `-DNDEBUG` | ROOT | (merged with build rule) | Duplicate inside CLAUDE.md |
| glTF temp directory issue | MODULE | `tests/CLAUDE.md` | |

## Planned work and documentation habits

| Text | Bucket | Destination | Notes |
|---|---|---|---|
| `ROADMAP.md` is the plan; tick items in the branch; story → BUILDLOG; how → ARCHITECTURE | ROOT | "where new information goes" | Replaced by phase 19's table |
| Research before making anything new; save research to `docs/` | ROOT | hard rule | Also in memory |
| README per branch; ENGINE_GUIDE at phase end | ROOT + SKILL | workflow line; `.claude/skills/docs-review/` | |
| Determinism rules; new ticked component → `hashSimulationState` | ROOT | hard rule | |
| Parent/child hierarchies not built | DELETE | — | In ROADMAP phase 9 |
| Vulkan: barriers by hand per pass via `ImageState`, validation layers must report nothing, golden only on request | MODULE | `src/render/vulkan/CLAUDE.md` | |
| Read `docs/X.md` before building Y (ILLUMINATION, LUMEN, CLOTHGPU, RAGDOLLPLAN, PERSISTENTSCENE, DESTRUCTION) | DOCS + MODULE | `docs/INDEX.md` ("read before") + the matching module `CLAUDE.md` | |
| Latency: frames in flight, Mailbox | DELETE | — | In ARCHITECTURE |

## Contradictions found

None in the spot checks: the default game (light lab), the test tables (`routineTests`, `runSlowTests`, `runParkedTests`), `maxUniformBlockBytes` and the presets match the code. Stale but not wrong: the intro's phase status (phase 19 now goes ahead of 18) and the timing "about 100 s, measured Oct 29". Every claim gets checked against the code as its section moves.

## Memory files duplicating CLAUDE.md

Project rules leave memory once they are in `CLAUDE.md` or a rule: `close-games-after-use`, `tests-only-at-commit`, `visible-test-runs`, `test-release-default`, `golden-runs-sparingly`, `command-line-rule`, `research-before-new-work`, `no-duplication`, `roadmap-before-work`, `merge-push-authorized`. Personal preferences stay: `user-background`, `explain-before-building`, `choices-with-tradeoffs`, `docs-readability`, `manual-visual-checks`, `live-simulation-checks`, `tooling-gotchas`, the progress notes.

## New files

| File | Holds |
|---|---|
| `CLAUDE.md` | ≤ 8 KB: overview, build table, ~12 hard rules, conventions, where to look, where new information goes |
| `src/render/CLAUDE.md`, `src/render/vulkan/CLAUDE.md` | Uniform limit, matrices, reversed-Z, units; barriers, validation |
| `src/cloth/CLAUDE.md`, `src/ecs/CLAUDE.md`, `src/physics/CLAUDE.md`, `src/assets/CLAUDE.md`, `src/engine/CLAUDE.md`, `src/games/forest/CLAUDE.md`, `tests/CLAUDE.md` | As in the tables |
| `.claude/rules/shader-mirror.md` | `assets/shaders/**`, `src/render/gpu/**`, `tests/gpu/**`, `**/*_shared.h` |
| `.claude/skills/`: `add-test`, `add-game`, `add-shader-function`, `golden`, `drive-game`, `docs-review` | Procedures |
| `docs/INDEX.md`, `docs/LAYOUT.md`, `docs/architecture/*.md` | Index, layer order, the split reference (incl. `build.md`) |
