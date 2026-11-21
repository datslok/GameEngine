# Build

Covers: `makefile`, `tools/package_demo.py`

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
