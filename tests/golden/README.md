# Golden images

Reference renders of the demo's fixed views, one folder per GPU (the name the driver reports). `make golden` renders the views again and compares them with the folder for this GPU; `make golden-save` replaces the references. See `docs/ARCHITECTURE.md`, section "Golden images".

Each view has two images:

- `<view>-scene.png`: the HDR scene image before tone mapping (half floats).
- `<view>-final.png`: the image the screen gets: 8-bit sRGB, or half floats (scRGB) for the HDR views.

Half-float images are stored losslessly in ordinary 8-bit PNGs twice as wide: each pixel's eight bytes are two RGBA pixels side by side. They look like noise in an image viewer; the engine unpacks them exactly. The 8-bit images can be viewed as they are.

References are only valid on the GPU and driver that made them: another GPU rounds texture filtering differently. Save new references only for a change meant to alter the picture, and say so in the commit.
