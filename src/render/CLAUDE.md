# render/

Design: `docs/architecture/renderer.md` (the pass order, each pass's file). Vulkan specifics: `vulkan/CLAUDE.md`.

- `render/gpu/` never calls the graphics API: records, packing and mirrored maths only, testable without a GPU.
- `render/` takes a raw `SDL_Window*`; never includes `input/`, `platform/` or `assets/`.
- **Uniform blocks ≤ 4096 bytes** (`maxUniformBlockBytes`): each is bound with a 4 KB range, and reading past it returns garbage silently. Every uniform struct gets a `static_assert` on its size. Anything bigger or per object goes in a std430 storage buffer with its C++ mirror's size checked.
- **Reversed-Z:** near 1, far 0; depth tests `GREATER`; clears to `gpuFarDepth`; shadow comparisons flipped. `toGpuDepthRange` converts `Mat4::perspective`'s OpenGL depth. Nothing outside `render/` knows GPU depth.
- **Camera-relative main pass:** model matrices, fragment and light positions and shadow lookups are measured from the camera (`relativeTo`, `cameraRelativeViewProjection`). Culling and the shadow pass stay in world space (the shadow cache keys on world matrices).
- **Matrices to shaders:** transposed to column-major by `writeColumnMajor`. `normalMatrix(model)` is the cofactor matrix times the determinant's sign (mirrored objects keep outward normals; zero for a flattening matrix).
- The depth prepass and the main pass both use `triangle.vert` (`gl_Position` is `invariant`); keep it so.
- **Light units:** colours, intensities and `emissive` are linear; `Material::colour` bytes are sRGB (decoded by `packMaterial`). Shading is linear HDR into a float image; tone mapping makes the screen image.
- Every ray-traced feature sits under `RenderSettings::rayTracing` (off by default) with its own switch, its `visit()` fields wrapped in `needingRayQueries`.
- Expensive features are settings that scale down to typical GPUs; presets (`quality_preset`) never touch `rayTracing`.
