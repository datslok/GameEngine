# Lights in a Uniform Buffer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the hardcoded shader light with directional, point and ambient light components that the engine collects each frame and sends to the fragment shader in a uniform block.

**Architecture:** Light components live in `scene/`. `collectLighting(world, alpha)` turns them into a GPU-free `FrameLighting`; `packLighting` turns that into a byte-exact mirror of the shader's `std140` block; `GpuRenderer::setLighting` pushes it once per frame to fragment uniform slot 1.

**Tech Stack:** C++20, SDL3 GPU API (Vulkan, SPIR-V via `glslc`), GLSL 450, plain `assert` tests.

**Spec:** `docs/superpowers/specs/2026-10-09-lights-uniform-buffer-design.md`

## Global Constraints

- Limits: `maxDirectionalLights = 4`, `maxPointLights = 16`; extra lights are dropped, first collected wins.
- `static_assert(sizeof(LightUniformData) == 672)`; every shader block member is `vec4`/`ivec4`.
- Uniform block binding: `layout(std140, set = 3, binding = 1)`, fragment uniform slot 1.
- Demo lights: `AmbientLight{0.2, 0.2, 0.2}`, `DirectionalLight{direction (1, -2, -1), white, intensity 0.8}`.
- Falloff: `window = clamp(1 - (d/range)^4, 0, 1)`, contribution `radiance * max(0, N·L) * window² / (d² + 1)`.
- Project style (CLAUDE.md): includes from the `src` root, block comment above each function explaining why, braces on every branch, CRLF line endings, layered dependencies (`scene/` and `render/` never include `assets/`, `games/` never includes `render/`).
- Tests: one `testXxx()` per file, registered in `tests/test_main.cpp`. Run with `make test` from the repo root via MSYS2 (see memory: build through PowerShell).
- `Vec3` has no unary minus; negate with `v * -1.0f`.

## Review Focus

1. A point light with `range <= 0` would make the shader divide by zero (`d / range`) and produce NaN pixels; `packLighting` must skip it (test in Task 2).
2. A frame that draws without calling `setLighting` would read whatever was in slot 1; `beginFrame` must push empty lighting so such a frame is well defined (Task 3).
3. A fragment exactly at a point light's position (d = 0) must skip that light rather than divide by zero (Task 3 shader).
4. A directional light with a zero direction must not take a slot or produce NaN (test in Task 2).
5. The torch must not follow a destroyed player into a stale entity: only copy when `world.isAlive(player)` (Task 4).

---

### Task 1: Light components and `collectLighting`

**Files:**
- Create: `src/scene/light.h`
- Create: `src/scene/lighting.h`, `src/scene/lighting.cpp`
- Test: `tests/lighting_test.cpp`; register `testLighting` in `tests/test_main.cpp`

**Interfaces:**
- Produces: `DirectionalLight{Vec3 direction{0,-1,0}; Vec3 colour{1,1,1}; float intensity = 1.0f;}`, `PointLight{Vec3 colour{1,1,1}; float intensity = 1.0f; float range = 10.0f;}`, `AmbientLight{Vec3 colour{0.2f,0.2f,0.2f};}` in `scene/light.h`.
- Produces: `struct PlacedPointLight { Vec3 position; PointLight light; };`, `struct FrameLighting { Vec3 ambient{0,0,0}; std::vector<DirectionalLight> directionalLights; std::vector<PlacedPointLight> pointLights; };`, `FrameLighting collectLighting(World& world, float alpha);` in `scene/lighting.h`.

- [ ] **Step 1: Write the failing test** `testLighting()` in `tests/lighting_test.cpp`, with these cases:
  - Empty `World` → `ambient` is (0,0,0), both vectors empty.
  - Two entities with `AmbientLight{0.1,0.2,0.3}` and `AmbientLight{0.1,0.1,0.1}` → `ambient` ≈ (0.2, 0.3, 0.4).
  - One `DirectionalLight{direction (1,-2,-1), colour (1,0.5,0.25), intensity 0.8}` → one entry with identical fields.
  - Point light entity with `Transform` at (2,0,0), `PreviousTransform` at (0,0,0), `PointLight{range 5}`; `collectLighting(world, 0.25f)` → one entry, position ≈ (0.5,0,0), range 5.
  - Point light entity with no `Transform` → not in `pointLights` (count stays at the previous case's count).
- [ ] **Step 2: Register and run** — add `void testLighting();` and the call to `tests/test_main.cpp`. Run `make test`. Expected: compile error (`scene/lighting.h` not found).
- [ ] **Step 3: Implement** `scene/light.h` and `collectLighting`. Use `world.each<AmbientLight>`, `world.each<DirectionalLight>`, and `world.each<PointLight, Transform>` with `getRenderTransform(world, entity, alpha).position`. A block comment above `collectLighting` says why positions are interpolated.
- [ ] **Step 4: Run** `make test`. Expected: `All tests passed!`
- [ ] **Step 5: Commit** — `git add src/scene/light.h src/scene/lighting.* tests/lighting_test.cpp tests/test_main.cpp`, message "added light components and collected them each frame".

### Task 2: `packLighting` and `LightUniformData`

**Files:**
- Create: `src/render/gpu/light_uniforms.h`, `src/render/gpu/light_uniforms.cpp`
- Test: `tests/light_uniforms_test.cpp`; register `testLightUniforms`

**Interfaces:**
- Consumes: `FrameLighting`, `PlacedPointLight`, `DirectionalLight`, `PointLight` (Task 1).
- Produces, in `render/gpu/light_uniforms.h`:
  ```cpp
  inline constexpr int maxDirectionalLights = 4;
  inline constexpr int maxPointLights = 16;

  struct DirectionalLightUniform { float toLight[4]; float radiance[4]; };
  struct PointLightUniform { float positionRange[4]; float radiance[4]; };

  // Mirrors the LightData block in triangle.frag byte for byte (std140, all vec4).
  struct LightUniformData {
      float ambient[4];
      std::int32_t counts[4];   // x = directional, y = point
      DirectionalLightUniform directional[maxDirectionalLights];
      PointLightUniform points[maxPointLights];
  };
  static_assert(sizeof(LightUniformData) == 672);

  LightUniformData packLighting(const FrameLighting& lighting);
  ```

- [ ] **Step 1: Write the failing test** `testLightUniforms()`, with these cases:
  - Empty `FrameLighting` with `ambient (0.2,0.2,0.2)` → `ambient` = {0.2,0.2,0.2,0}, `counts` = {0,0,0,0}.
  - Directional `{direction (0,-2,0), colour (1,0.5,0.5), intensity 2}` → `toLight` ≈ {0,1,0,0}, `radiance` ≈ {2,1,1,0}, `counts[0] == 1`.
  - Directional with direction (0,0,0) followed by a valid one → `counts[0] == 1` and slot 0 holds the valid light.
  - Point at (1,2,3), `{colour (1,1,1), intensity 3, range 6}` → `positionRange` = {1,2,3,6}, `radiance` = {3,3,3,0}, `counts[1] == 1`.
  - Point lights with `range` 0 and -1 → skipped (`counts[1] == 0`).
  - 6 directional and 20 point lights (all valid) → `counts` = {4,16,…}; `points[15]` holds the 16th light's position.
- [ ] **Step 2: Register and run** `make test`. Expected: compile error (`light_uniforms.h` not found).
- [ ] **Step 3: Implement** `packLighting`. Zero-initialise the result (`LightUniformData data{};`). Skip a directional light when `direction.lengthSquared()` is below `1e-12f`; `toLight = direction.normalized() * -1.0f`. Skip a point light when `range <= 0.0f`. Stop filling each kind at its limit.
- [ ] **Step 4: Run** `make test`. Expected: `All tests passed!`
- [ ] **Step 5: Commit** — message "packed lights into the shader's uniform layout".

### Task 3: Renderer, shaders and engine wiring (scene looks unchanged)

**Files:**
- Modify: `src/render/gpu/gpu_renderer.h` (add `setLighting`), `src/render/gpu/gpu_renderer.cpp` (fragment shader `loadShader` call ~line 61: uniform buffers `1` → `2`; `beginFrame` ~line 278; new `setLighting`)
- Modify: `assets/shaders/triangle.vert`, `assets/shaders/triangle.frag`
- Modify: `src/engine/application.cpp` (`render`, after `beginFrame` succeeds ~line 110)
- Modify: `src/games/demo/demo_game.cpp` (`createScene`)

**Interfaces:**
- Consumes: `FrameLighting`, `collectLighting` (Task 1); `LightUniformData`, `packLighting`, limits (Task 2).
- Produces: `void GpuRenderer::setLighting(const FrameLighting& lighting);` — throws `std::logic_error("setLighting requires an active frame")` outside a frame; packs and calls `SDL_PushGPUFragmentUniformData(commands, 1, &data, sizeof(data))`.

- [ ] **Step 1: Shaders.** Vertex: add `layout(location = 2) out vec3 worldPosition;` set to `(model * vec4(position, 1.0)).xyz`. Fragment: add `layout(location = 2) in vec3 worldPosition;` and the `LightData` block from the spec (`set = 3, binding = 1`, arrays sized 4 and 16); replace the hardcoded light and ambient with the loop from the spec. Keep the existing zero-length-normal guard (ambient only). In the point loop, skip when `d <= 0.0`.
- [ ] **Step 2: Renderer.** Add `setLighting`. At the end of a successful `beginFrame`, call `setLighting(FrameLighting{})` so a frame that never sets lighting reads zeros rather than stale data. Change the fragment shader's uniform buffer count to 2.
- [ ] **Step 3: Engine.** In `Application::render`, call `renderer.setLighting(collectLighting(world, alpha));` right after `beginFrame` returns true.
- [ ] **Step 4: Demo lights.** In `DemoGame::createScene`, create one entity with `AmbientLight{Vec3{0.2f, 0.2f, 0.2f}}` and one with `DirectionalLight{Vec3{1.0f, -2.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}, 0.8f}`, with a comment that these reproduce the old hardcoded light.
- [ ] **Step 5: Run** `make test`. Expected: `All tests passed!` (shaders compile as part of the target; a `glslc` error fails here).
- [ ] **Step 6: Visual check 1.** `make run`, compare with `main`: lighting must look identical (same shading on cubes, duck and ground). Also check the terminal for SDL GPU validation errors (debug mode is on).
- [ ] **Step 7: Commit** — message "sent lights to the shader in a uniform buffer instead of hardcoding one".

### Task 4: Player torch, docs

**Files:**
- Modify: `src/games/demo/demo_game.h` (member `Entity playerTorch;`, getter `Entity getPlayerTorch() const;`)
- Modify: `src/games/demo/demo_game.cpp` (`createScene`, `onFixedUpdate`)
- Modify: `tests/demo_game_test.cpp`
- Modify: `ROADMAP.md` (tick phase 4 item 1), `CLAUDE.md`

**Interfaces:**
- Consumes: `PointLight` (Task 1).
- Produces: `DemoGame::getPlayerTorch()`; torch height offset `torchHeight = 1.0f` above the player's position.

- [ ] **Step 1: Write the failing test.** In `testDemoGame`, after the existing movement ticks: `const Entity torch = game.getPlayerTorch();` assert `world.isAlive(torch)`, `world.has<PointLight>(torch)`, `world.has<PreviousTransform>(torch)`, and that the torch `Transform` position x/z equal the player's and y equals the player's y + 1.0.
- [ ] **Step 2: Run** `make test`. Expected: compile error (`getPlayerTorch` missing).
- [ ] **Step 3: Implement.** In `createScene`, after spawning the player, create the torch with `Transform` at player position + (0, 1, 0), an equal `PreviousTransform`, and `PointLight{Vec3{1.0f, 0.6f, 0.3f}, 3.0f, 6.0f}` (warm, intensity 3, range 6). In `onFixedUpdate`, right after `updateCharacters` and only if `world.isAlive(player)`, copy the player's position + (0, torchHeight, 0) into the torch's `Transform`. Comment: no entity hierarchy yet (ROADMAP phase 9).
- [ ] **Step 4: Run** `make test`. Expected: `All tests passed!`
- [ ] **Step 5: Visual check 2.** `make run`, switch to MOBA (F2), right-click to walk: a warm glow on the ground follows the duck smoothly and fades to nothing about 6 units away, with no visible edge.
- [ ] **Step 6: Docs.** `ROADMAP.md`: `- [x]` on "Light data in a uniform buffer…" with ` (`lights-uniform-buffer`)`. `CLAUDE.md`: replace the "Light" convention line with: lights are `DirectionalLight`, `PointLight`, `AmbientLight` components; `collectLighting` → `GpuRenderer::setLighting` → fragment slot 1 (`LightData`, mirrored by `LightUniformData`, limits 4/16); the software renderer still hardcodes `(-1, 2, 1)`. Add the light components to the "Components today" list, `scene/light` and `scene/lighting` to the source layout, and remove "light" from anything that says it is hardcoded in `triangle.frag`.
- [ ] **Step 7: Commit** — message "gave the demo player a torch and documented the light components".
