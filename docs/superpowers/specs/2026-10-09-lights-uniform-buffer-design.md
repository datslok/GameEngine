# Lights in a uniform buffer: design

Branch: `lights-uniform-buffer`. Roadmap: phase 4, first item ("Light data in a uniform buffer instead of hardcoded in the shader; several directional and point lights").

## Goal

Replace the hardcoded light in `triangle.frag` with light data the game controls. Games place directional, point and ambient lights as ECS components; each frame the engine collects them and the renderer sends them to the shader once.

Success: the demo looks identical to today with only an ambient light (0.2) and a sun (0.8), and a point light attached to the player lights the ground around it, fading smoothly to zero at its range.

## 1. Components (`scene/light.h`)

```cpp
struct DirectionalLight {
    Vec3 direction{0.0f, -1.0f, 0.0f}; // The way the light travels; need not be normalised.
    Vec3 colour{1.0f, 1.0f, 1.0f};     // 0..1 per channel.
    float intensity = 1.0f;            // Any non-negative value.
};

struct PointLight {                    // Position comes from the entity's Transform.
    Vec3 colour{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;               // The light reaches exactly zero at this distance.
};

struct AmbientLight {
    Vec3 colour{0.2f, 0.2f, 0.2f};
};
```

- Light colours are floats split into colour (tint, 0..1) and intensity (unbounded), unlike material colours (`Pixel`, a reflectance).
- A directional light stores its direction directly and needs no `Transform` (Euler angles would make aiming it awkward).
- Several `AmbientLight`s add up; none means no ambient light.

## 2. Collecting lights (`scene/lighting.h`)

```cpp
struct PlacedPointLight {
    Vec3 position;
    PointLight light;
};

struct FrameLighting {
    Vec3 ambient{0.0f, 0.0f, 0.0f};
    std::vector<DirectionalLight> directionalLights;
    std::vector<PlacedPointLight> pointLights;
};

FrameLighting collectLighting(World& world, float alpha);
```

- `ambient` is the sum of all `AmbientLight` colours.
- Directional lights are copied unchanged.
- A point light's position is `getRenderTransform(world, entity, alpha).position`, so a moving light interpolates exactly like a moving mesh. Point lights without a `Transform` are skipped.
- No GPU code; fully unit testable.

## 3. GPU side

### Uniform block (fragment uniform slot 1)

```glsl
struct DirectionalLightData { vec4 toLight; vec4 radiance; };      // xyz used
struct PointLightData       { vec4 positionRange; vec4 radiance; }; // w = range

layout(std140, set = 3, binding = 1) uniform LightData {
    vec4 ambient;
    ivec4 counts;                        // x = directional, y = point
    DirectionalLightData directional[4];
    PointLightData points[16];
};
```

Every member is a `vec4`/`ivec4`, so the `std140` layout has no hidden padding and a C++ mirror made of `float[4]`/`std::int32_t[4]` matches byte for byte. The C++ struct `LightUniformData` has `static_assert(sizeof(LightUniformData) == 672)`.

Limits: `maxDirectionalLights = 4`, `maxPointLights = 16`, shared by C++ and the shader. Lights beyond the limit are dropped (the first ones collected win). Choosing the most important lights is future work.

### Packing (`render/gpu/light_uniforms.h`)

`LightUniformData packLighting(const FrameLighting& lighting)`, a pure function:

- `radiance = colour * intensity` (alpha/w = 0).
- Directional: `toLight = -normalize(direction)`. A direction of (near) zero length is skipped and does not use a slot.
- Point: `positionRange = (position, range)`.
- `counts = (directional used, point used, 0, 0)`, capped at the limits.

### Renderer

- `GpuRenderer::setLighting(const FrameLighting&)`: packs and pushes to fragment uniform slot 1. Must be called between `beginFrame` and `endFrame` (throws `std::logic_error` otherwise, like `drawMesh`); the pushed data stays in effect for every later draw in the frame.
- The fragment shader is created with 2 uniform buffers instead of 1.
- `Application::render` calls `renderer.setLighting(collectLighting(world, alpha))` right after `beginFrame` succeeds.

### Shaders

Vertex: new output `layout(location = 2) out vec3 worldPosition = (model * vec4(position, 1.0)).xyz`.

Fragment:

```glsl
vec3 light = ambient.rgb;

for directional i < counts.x:
    light += radiance * max(0, dot(N, toLight));

for point i < counts.y:
    offset = position - worldPosition; d = length(offset);
    if d > 0:
        window = clamp(1 - (d / range)^4, 0, 1);
        light += radiance * max(0, dot(N, offset / d)) * window * window / (d * d + 1);

outputColour = vec4(albedo.rgb * light, albedo.a);
```

- `1 / (d^2 + 1)`: inverse-square falloff, with +1 so it stays finite at the light.
- `window^2`: reaches exactly 0 at `range` with zero slope, so there is no visible edge (the falloff Unreal Engine uses).
- A fragment without a usable normal (zero length) gets ambient only, as today.
- Totals above 1.0 clip to white; tone mapping is out of scope.

## 4. Demo

`DemoGame::onInit` spawns:

- `AmbientLight{{0.2, 0.2, 0.2}}`
- `DirectionalLight{direction = (1, -2, -1), colour = white, intensity = 0.8}`: the same as the old hardcoded light (`ambient + (1 - ambient) * diffuse`).
- A torch: an entity with `Transform`, `PreviousTransform` and a warm `PointLight`. There are no entity hierarchies yet, so `DemoGame::onFixedUpdate` copies the player's position (plus a small height offset) into the torch's `Transform` after `updateCharacters`.

Visual checks: first with the torch disabled, the scene must look unchanged; then with the torch, a glow follows the player in MOBA mode and fades smoothly at its range.

## 5. Tests

- `tests/lighting_test.cpp` (`testLighting`): ambients add up; directional lights are copied; point light positions are interpolated by `alpha`; point lights without a `Transform` are skipped; an empty world gives zero lighting.
- `tests/light_uniforms_test.cpp` (`testLightUniforms`): radiance is premultiplied; direction is normalised and flipped; zero-length direction is skipped; counts are capped at 4 and 16 and extra lights are dropped; point position and range are packed.
- Both registered in `tests/test_main.cpp`. The existing demo test (1000 ticks headless) must still pass with the torch.

## 6. Docs

- `ROADMAP.md`: tick the first phase 4 item.
- `CLAUDE.md`: replace the "Light" convention line; add the light components, `collectLighting`, `setLighting` and `packLighting` to the architecture section.

## Out of scope

- The software renderer keeps its hardcoded light (test-only reference path, flat shaded).
- Specular highlights (next roadmap item), shadows, tone mapping, light culling or prioritisation beyond the fixed limits.
