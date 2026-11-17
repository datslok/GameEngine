# assets/

- `AssetManager` loads each file once and is append-only; `Application` uploads whatever it gained before each frame.
- `MeshHandle`, `TextureHandle`: typed indices, default = none. Textures carry a `TextureEncoding` (sRGB colour or linear data).
- **UVs:** bottom-left origin (OBJ). `Mesh::uvOrigin` says which a mesh uses (glTF: `TopLeft`); tangents count v up the picture either way.
- `loadImage` flips rows by default; glTF textures need `flipVertically = false`. Textures are cached by (path, flip) or by embedded image buffer.
- `addDrawnMesh` keeps a mesh only in the render format (big generated scenery).
- `scene/` and `render/` never include this folder: `ImageData` lives in `scene/image_data.h` so they need not.
