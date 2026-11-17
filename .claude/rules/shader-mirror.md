---
paths:
  - "assets/shaders/**"
  - "src/render/gpu/**"
  - "tests/gpu/**"
  - "src/**/*_shared.h"
---

# Shader maths and its C++ mirrors

- Shader maths lives in `assets/shaders/*.glsl` libraries of pure functions: no texture reads, `gl_FragCoord` or derivatives inside, so compute shaders can run them. Not in a pass's `main`.
- Each library has a C++ mirror (mostly `src/render/gpu/`; cloth's is `src/cloth/` itself) with CPU unit tests in `tests/`.
- Numbers both sides use live once, in a macro-only `*_shared.h`: only `#define`s of plain numbers behind an include guard (GLSL reads it). C++ builds typed constants from them.
- A new library function also gets a GPU test: `add-shader-function` skill. Run `make gputest` only when the user asks.
- Change a function and its mirror in the same commit.
