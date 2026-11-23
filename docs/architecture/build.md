# Build

Covers: `makefile`, `tools/package_demo.py`, `tools/hooks/`

## Makefile

- Finds `src/**/*.cpp` recursively (`rwildcard`), `tests/*.cpp` and `tests/gpu/*.cpp`; new files need no makefile change.
- One object per source in `build/obj/` (mirroring the folders); the game and the tests link the same engine objects.
- Header dependencies from GCC's `-MMD -MP` `.d` files; a makefile change rebuilds everything.
- Parallel on every core: `MAKEFLAGS += -j$(nproc) --output-sync=target` (`make JOBS=1` to override). A one-file change rebuilds in about 2 s.
- External C: MikkTSpace with `gcc -std=c99`; Dear ImGui with `-Warray-bounds` off for its files only.

## Two builds

| | `make` (debug) | `make release` |
|---|---|---|
| Folder | `build/` | `build/release/` |
| Flags | `-std=c++23 -Wall -Wextra -g -O0` | same with `-O2` |
| Exceptions | `src/cloth/`, the signed distance field and the sky visibility bake (with the level's ray code) at `-O2`: too hot to run unoptimised | — |

- `BUILD=debug|release` picks `OPTIMISATION` and `BUILD_DIR`; release targets re-run make with `BUILD=release`, so switching never rebuilds the other.
- Both keep `-g`. Never `-DNDEBUG` (tests rely on `assert`), never `-ffast-math`. Planting's files (`scene/planting`, `planting_maps`, `terrain_heights`) add `-ffp-contract=off`: they must round as the GPU's `precise` copy does (`planting.md`).
- Tiny maths (`Vec2`, `Vec3`, `Vec4`, `Mat4 * Vec4`, `transformPoint`, `transformDirection`) is defined in headers so it inlines. `-flto` is not used: a one-file rebuild took 20 s instead of 3 for 3-9% speed.

## Shaders

- `glslc --target-env=vulkan1.3 -Isrc`: `.vert`, `.frag`, `.comp`, `.mesh` in `assets/shaders/` → `.spv`.
- glslc's `-MD` files in `build/obj/shaders/` rebuild a shader when an included file changes.
- Shaders include the macro-only `*_shared.h` headers they share with C++ (rule: `.claude/rules/shader-mirror.md`).

## Dependencies

| Library | Where | Note |
|---|---|---|
| SDL3, SDL3_image, glslc (`shaderc`), Vulkan headers, validation layers | MSYS2 | Install: `README.md` |
| cgltf, MikkTSpace, meshoptimizer v1.3 (with `external/meshoptimizer/clusterlod.h`), Dear ImGui 1.92.9b (SDL3 and Vulkan backends) | `external/`, unmodified | |
| bc7enc (BC7 encoder, C; decoder, C++; MIT or public domain) | `external/bc7enc`, unmodified | Commit in `VERSION.txt`; `-O2` in debug too |
| volk, vk-bootstrap, VMA | `external/`, unmodified | Versions in each `VERSION.txt`, matched to MSYS2's Vulkan headers |
| FidelityFX FSR 3.1 (SDK v1.1.4, last with Vulkan) | `external/fidelityfx`, DLL committed | Loaded at run time (`SDL_LoadObject`); without it FSR is unavailable |
| NVIDIA Streamline (DLSS) | MIT headers in `external/streamline/include`; DLLs fetched by `python tools/fetch_streamline.py` | Signature-checked at load (`-lwintrust -lcrypt32`); started before the device |

- Every Vulkan call goes through volk (`-DVK_NO_PROTOTYPES`); nothing links against the loader.
- The makefile copies the FSR and Streamline DLLs beside each build's exe.
- The validation layers live in `C:\msys64\ucrt64\bin`, where the loader does not look: the makefile exports `VK_ADD_LAYER_PATH`; set it yourself to start `game.exe` another way.
- The validation callback skips one warning, `rw_luma_history` (a bug in AMD's DLL).

## Packaging

`python tools/package_demo.py` → `build/GameEngineDemo.zip` for a PC without MSYS2:

- The stripped release exe, DLLs found by following import tables with MSYS2 `objdump`, plus SDL3_image's decoder DLLs for the formats the assets use (loaded at run time, so invisible in import tables). Fails if an imported DLL is neither packaged nor part of Windows.
- `assets/` without shader sources, and a README whose controls come from `README.md`'s `## Controls` (`readme_controls`).
- `--anonymous`: `build/Demo.zip`, controls cut to their first phrase (`brief_action`), entries dated 1980-01-01, the PE timestamp zeroed (`clear_build_time`).
- Needs a real Python (`winget install Python.Python.3.13`); the `WindowsApps` one is a Store placeholder.

## Claude Code hooks

`tools/hooks/claude_hooks.py` holds the checks Claude Code runs around Claude's tool calls, registered in `.claude/settings.json`. Each enforces a hard rule from `CLAUDE.md` that a script can check, so the rule holds even when Claude forgets it.

| Check | When | Does |
|---|---|---|
| `commands` | Before every Bash or PowerShell command | Blocks a build with `-DNDEBUG` or `-ffast-math`; blocks committing code (any non-Markdown change) straight onto `main` outside a merge, and merges into `main` without `--no-ff`; asks the user before golden, GPU, slow, parked and yardstick runs |
| `makefile` | Before an Edit or Write of the Makefile | Blocks the two flags outside comments |
| `line-endings` | After an Edit or Write | Turns bare LF into CRLF in `src/`, `tests/`, `assets/shaders/` and `tools/` (root files such as `README.md` keep theirs) |
| `games` | When Claude ends a turn | Keeps the turn going while a game copy with `--port 47620` (Claude's, never the user's) runs |

- **Blocking:** exit code 2, the reason on stderr for Claude; a question is the PreToolUse `ask` permission decision.
- **Cost:** about 80 ms a shell command (150 ms a `git commit`); the games check lists processes with `tasklist` and asks PowerShell for command lines only when a `game.exe` runs.
- **Tests:** `python -I tools/hooks/test_claude_hooks.py`, each check's cases plus whole runs fed hook JSON.
- The commit attribution check lives in the user's own Claude settings folder, outside the repo, since it covers every project.
