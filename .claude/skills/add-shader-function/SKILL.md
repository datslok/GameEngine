---
name: add-shader-function
description: Use when adding or changing a function in an assets/shaders/*.glsl library, or adding a new shader library, so its C++ mirror, CPU test and GPU test stay in step.
---

# Add a shader library function

Rules: `.claude/rules/shader-mirror.md`. Example to copy: `dither.glsl` ↔ `core/dither` ↔ `tests/gpu/dither_gpu_test.cpp`.

1. **Numbers** both sides need: a `#define` in the system's `*_shared.h`; a typed C++ constant built from it.
2. **C++ mirror** (usually `src/render/gpu/<library>.h/.cpp`), written first, with a CPU unit test in `tests/` (the `add-test` skill).
3. **GLSL function** in `assets/shaders/<library>.glsl`: pure, no texture reads, `gl_FragCoord` or derivatives.
4. **GPU test**, three places:
   - `tests/gpu/gpu_test_functions.h`: the next number under the library's `// <library>_test.comp` block (`GPU_TEST_<NAME>`).
   - `tests/gpu/shaders/<library>_test.comp`: a branch `if (function == GPU_TEST_<NAME>)` reading `testInput(i)`, writing `testOutput(i, ...)`.
   - `tests/gpu/<library>_gpu_test.cpp`: a case running it through `GpuFunctionRunner::run` and comparing against the mirror with `report.check(name, tolerance, expected, actual)`; tolerance as tight as rounding allows, with a comment when looser.
5. **New library only:** create its `<library>_test.comp` (copy `dither_test.comp`), its `<library>_gpu_test.cpp` with a `gpuTest<Library>(runner, report)` entry, called from `tests/gpu/gpu_test_main.cpp`; list it in `docs/ARCHITECTURE.md`, "GPU tests and shader libraries".
6. Build (`make`, `make release`). Run `make gputest` only if the user asks.
