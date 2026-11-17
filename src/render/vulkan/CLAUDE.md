# render/vulkan/

The engine's own Vulkan 1.3 renderer (volk, VMA, vk-bootstrap, dynamic rendering, synchronization2). Design: `docs/ARCHITECTURE.md`, VulkanRenderer.

- Every Vulkan call goes through volk (`-DVK_NO_PROTOTYPES`).
- Barriers are written by hand per pass through `ImageState` (images) and `bufferBarrier` (buffers), each with a comment saying why.
- Validation layers stay on in debug builds and must report nothing. The callback skips only `rw_luma_history` (a bug in AMD's FSR DLL).
- Pipelines are compiled through `PipelineCompiler` (a thread per core; the loading screen counts them).
- Golden images: the same GPU, driver and shaders reproduce the references bit for bit, so any difference is a real change. Run or save them only when the user asks (`golden` skill).
