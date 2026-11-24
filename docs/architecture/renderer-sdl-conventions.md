# Renderer: conventions kept from SDL_GPU

The port drew every golden image bit for bit the same, so these stay:

- **Descriptor layout** for bloom, tone mapping, debug lines and the overlay (`ShaderResources`, sets shared through `DescriptorLayouts`): set 0 vertex textures (unused), set 1 vertex uniforms, set 2 fragment combined image samplers then storage buffers, set 3 fragment uniforms. Scene pipelines use the GPU scene's (`PipelineDescription::sharedLayout`).
- **Pipelines:** depth clamp on, counter-clockwise front faces, line width 1, dynamic viewport and scissor. `setViewport` flips y (origin at `y + height`, negative height); `beginRendering` sets the whole target.
- **Formats:** swapchain `B8G8R8A8_SRGB` (else `R8G8B8A8_SRGB`), or `R16G16B16A16_SFLOAT` in `EXTENDED_SRGB_LINEAR` for HDR; bloom levels `B10G11R11_UFLOAT_PACK32` when drawable, blendable and filterable.
- **Shared samplers:** surfaces (trilinear, repeat, anisotropy 16 or the GPU's limit, `VK_LOD_CLAMP_NONE`); tables (bilinear, clamped); nearest; linear clamped; shadow compare (`GREATER_OR_EQUAL`, linear); shadow raw depth (nearest).
- **Mipmaps:** linear blits from the level above, each `max(1, size >> level)`; a cut-out's built on the CPU to keep its coverage (`coveragePreservingMips`) and copied in with level 0; a BC7 texture's every level built on the CPU and copied in (the GPU cannot write blocks: [assets.md](assets.md#texture-compression)).
