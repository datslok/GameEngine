# Mewton

Mewton: a from-scratch C++23 engine. A genre-agnostic core for **FPS** and **MOBA** games, with genre code in modules on top; later a dressmaking game with real cloth (phases 7, 10), a real-scale solar system (13) and a flying superhero bringing down buildings by real physics (15).

Current phase and work order: `ROADMAP.md` (phase numbers are names; the file's order is the work order).

## Build and test

From PowerShell, prefix build commands with `$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;" + $env:PATH;` (MSYS2 is not on the permanent `PATH`; the game needs `ucrt64\bin` for its DLLs). Run from the repo root: asset paths are relative.

| Command | Does |
|---|---|
| `make` / `make release` | Debug `build/game.exe` / `-O2` `build/release/game.exe`, plus shaders |
| `make run` / `make run-release` | Build and run; `GAME=<name>` picks the game (light lab by default) |
| `powershell -File tools/visible_tests.ps1` | The routine suite, release, in a window the user sees (`-Build debug`, `-Arguments --times`) |
| `make test-slow`, `test-parked`, `yardstick*`, `golden*`, `gputest` | Only when the user asks: `tests/CLAUDE.md`, `src/cloth/CLAUDE.md`, `golden` skill |

Makefile, flags, dependencies, packaging: `docs/architecture/build.md`.

## Hard rules

1. **IMPORTANT: close every game copy you start, in the same step** (`--port 47620 --send quit`, then `Get-Process game`). Never end a turn with one running; never touch the user's copy. How: `drive-game` skill.
2. **IMPORTANT: research before building anything new:** how shipped games, engines and papers do it (sources, numbers), saved in `docs/` when substantial; explain the plan; build after the user's go. New work goes on `ROADMAP.md` before any code.
3. **Tests first, and each seen to fail:** plant a bug in the code it guards, watch the test catch it, undo the bug (`add-test` skill). Run the suite only before committing finished work, and after merging into `main` only if `main` moved since branching. While iterating, build debug and release (each shows warnings the other hides) and let the user check by eye. Never `-DNDEBUG` or `-ffast-math`.
4. **Layers:** a `src/` folder includes only from lower layers (`docs/LAYOUT.md`). Games never include `platform/`, `render/` or SDL (they run headless). Includes from the `src` root with the folder, never `../`.
5. **Deterministic simulation:** inside ticks no clock reads, no containers ordered by address, job results in a fixed order. A component ticks change goes into `hashSimulationState`. Never read `Input` in a tick: edges reach ticks as commands.
6. **Settings and commands:** every tunable lives in its system's settings struct, named once in its `visit()` (F5, `settings.cfg` and command line follow). Actions that are not values get a command, tested through `CommandRegistry::execute`. A feature's plan says how the command line reaches it.
7. **No duplication:** search before writing; merge near-copies into one function; each tuning value in one named place. Exception: speed (keep the fast form, say why in a comment).
8. **Shader maths** lives in `.glsl` libraries mirrored in C++ and tested (`.claude/rules/shader-mirror.md`).
9. **Branches:** one per feature, descriptive commits. Merge into `main` with `--no-ff`, push, delete the branch locally and on GitHub.
10. Source files use CRLF; keep them.
11. **Build for scale and performance, not ease:** pick the design whose cost grows with what is seen or changed, not with what exists, even when it is harder to build. Each plan says what its cost grows with; say plainly where a gain is negligible, and stage hard work so each step stays testable.
    - Offering choices: recommend that design and say what current engines do; ease is a pro, never the reason for a pick.
12. **IMPORTANT: two failed tries, then stop and consult the user:** say what failed and why it might, and suggest research (how others solve it) or an outside-the-box approach. Work smarter, not harder: never a third blind attempt.

## Conventions

- `Mat4` is row-major (`values[row][column]`) and multiplies column vectors (`M * v`). Right-handed; the camera looks down -Z.
- Rotations are unit `Quat`s (x y z w as glTF; `a * b` turns by b first). Euler angles only describe poses; characters and cameras keep yaw and pitch as angles.
- Style: descriptive names, a block comment above each function saying why, braces on every branch, RAII, exceptions for errors (constructors clean up and rethrow). Match the code around you.

## Where to look

- `docs/INDEX.md`: every doc, and which to read before building what.
- `docs/LAYOUT.md`: folders and layers. `docs/architecture/`: how each system works, a file a system (listed in `docs/INDEX.md`); read a system's file before changing it.
- Loaded on their own when you touch matching files: `src/<folder>/CLAUDE.md`, `tests/CLAUDE.md`, `.claude/rules/*.md`. Procedures: `.claude/skills/`.

## Where new information goes

Pick the narrowest home. One home per fact; elsewhere, link to it.

| Kind | Home |
|---|---|
| A step, planned or done: one short line (120 characters of prose, `testDocs`), its branch, the doc with its plan; choices, numbers and findings go in the research doc and the build log | `ROADMAP.md` |
| How it works now | `docs/architecture/<system>.md` |
| Why, what was tried and measured (at most 10 bullets a branch) | the current phase's last part in `docs/buildlog/` (listed in `docs/BUILDLOG.md`) |
| Research and sources | `docs/research/<TOPIC>.md`, listed in `docs/INDEX.md` |
| Learning the engine (prose) | a chapter in `docs/guide/` (listed in `docs/ENGINE_GUIDE.md`), at the end of a phase |
| Features, controls (the packaged demo reads `## Controls`) | `README.md` |
| A rule for almost every task | this file |
| A rule for one folder | `src/<folder>/CLAUDE.md` |
| A rule across folders | `.claude/rules/<name>.md` with `paths:` |
| A procedure | `.claude/skills/<name>/SKILL.md` |
| A check a tool can make | a test, not a rule |

- Dense: bullets and tables, no history or dates outside `BUILDLOG.md` and `ROADMAP.md`.
- When behaviour changes, rewrite its section; do not append. A new rule removes or merges what it replaces.
- Budgets: this file 8 KB; each folder `CLAUDE.md` and rule 4 KB; `README.md` 20 KB; each architecture file 20 KB, past which it splits into narrower topics.
- **Compact, never lose meaning:** shorter wording is fine; every fact, number, reason and source survives, moved to its home when over budget. Delete only what is no longer true, or a duplicate (name the home kept). Before each commit the `commands` hook names what vanished from every doc; tell the user what was dropped and why.
- Each feature branch: tick its `ROADMAP.md` line, add its `BUILDLOG.md` entry, update its architecture section, and `README.md` if players see it. Before each commit, `tools/docs_reminder.ps1` names code whose architecture doc did not change; an architecture file claims folders with a `Covers:` line.
- End of a phase: `docs-review` skill.
