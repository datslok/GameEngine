#include "render/vulkan/vulkan_renderer.h"
#include "render/gpu/brdf.h"
#include "render/gpu/camera_relative.h"
#include "render/gpu/depth_shared.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/column_major.h"
#include "render/gpu/material_uniforms.h"
#include "render/gpu/shadow_cache.h"
#include "render/gpu/tone_map_uniforms.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_check.h"
#include "render/window_display.h"
#include "scene/culling.h"

#include "imgui/imgui.h"
#include "imgui/imgui_impl_vulkan.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
    // The lit scene: 16-bit floats keep about three significant digits from a millionth up to 65504, plenty for light
    // far brighter than white, at half the memory of 32-bit floats. Alpha is unused, but there is no 3-channel format.
    constexpr VkFormat sceneColourFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    // The most textures the bindless array holds (see createSceneLayout).
    constexpr std::uint32_t maxBindlessTextures = 16384;

    // Golden captures write what the screen would get: 8-bit sRGB (as the SDR swapchain) or scRGB half floats.
    VkFormat captureFormat(bool hdr) {
        return hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_SRGB;
    }

    VkSamplerCreateInfo samplerInfo(VkFilter filter, VkSamplerMipmapMode mipmaps, VkSamplerAddressMode address) {
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = filter;
        info.minFilter = filter;
        info.mipmapMode = mipmaps;
        info.addressModeU = address;
        info.addressModeV = address;
        info.addressModeW = address;
        info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        return info;
    }

    // The mesh vertex as triangle.vert reads it: position, normal, texture coordinates and tangent.
    VertexLayout meshVertices() {
        VertexLayout layout;
        layout.stride = sizeof(MeshVertex);
        layout.attributes = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<std::uint32_t>(offsetof(MeshVertex, position))},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<std::uint32_t>(offsetof(MeshVertex, normal))},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<std::uint32_t>(offsetof(MeshVertex, uv))},
            {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<std::uint32_t>(offsetof(MeshVertex, tangent))},
        };
        return layout;
    }

    RawImage readImage(const VulkanBuffer& buffer, VkDeviceSize offset, std::uint32_t width, std::uint32_t height, PixelFormat format) {
        RawImage image;
        image.width = width;
        image.height = height;
        image.format = format;
        image.bytes.resize(static_cast<std::size_t>(width) * height * bytesPerPixel(format));
        std::memcpy(image.bytes.data(), static_cast<const std::byte*>(buffer.getMapped()) + offset, image.bytes.size());
        return image;
    }
}

VulkanRenderer::VulkanRenderer(SDL_Window* window, bool validation, std::uint32_t requestedFramesInFlight):
    window(window),
    device(window, validation),
    layouts(device.getDevice()),
    uploads(device),
    swapchain(device, window),
    framesInFlight(validFramesInFlight(requestedFramesInFlight))
{
    checkFormats();
    createFrames();
    createSamplers();
    createSceneLayout();
    createPipelines();
    createShadowAtlas();
    createFixedTextures();

    bloom = std::make_unique<VulkanBloom>(device, layouts, linearSampler.get());
    debugLines = std::make_unique<VulkanDebugLines>(device, layouts, framesInFlight);
    overlay = std::make_unique<VulkanOverlay>(device, layouts, nearestSampler.get());

    if (swapchain.rebuild(false, presentMode, framesInFlight)) {
        swapchainFramesInFlight = framesInFlight;
        createScreenPipelines(swapchain.getFormat());
    }
}

// Nothing may be destroyed while the GPU might still use it, so the destructor waits; the members then go in reverse order.
VulkanRenderer::~VulkanRenderer() {
    device.waitIdle();

    if (debugUiStarted) {
        ImGui_ImplVulkan_Shutdown();
        debugUiStarted = false;
    }
}

// The formats every GPU worth supporting has, checked once so a missing one fails clearly at start-up.
void VulkanRenderer::checkFormats() const {
    if (!device.supportsFormat(depthFormat, VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
                                            VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        throw std::runtime_error("This GPU cannot sample a 32-bit depth image with filtering, which shadows need");
    }

    if (!device.supportsFormat(sceneColourFormat, VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT)) {
        throw std::runtime_error("This GPU cannot draw into a 16-bit float image, which HDR rendering needs");
    }

    if (!device.supportsFormat(VK_FORMAT_R16G16_UNORM, VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        throw std::runtime_error("This GPU cannot sample a 16-bit two-channel image, which physically based shading needs");
    }
}

/*
* Each frame in flight gets its own command buffer, fence, acquire semaphore, descriptor sets, uniform buffer, scene
* buffers and timestamp pool, so the CPU can fill one while the GPU still reads another. The fence starts signalled: no frame is
* pending yet. The scene buffers grow to fit the busiest frame they have held. Images only the GPU writes (the
* scene, depth, bloom, the shadow atlas, the overlay) are shared: the GPU runs submissions in order, and each pass's
* barriers also wait for the previous frame's use of them.
*/
void VulkanRenderer::createFrames() {
    frames.clear();
    frameIndex = 0;

    for (std::uint32_t index = 0; index < framesInFlight; ++index) {
        Frame frame;
        frame.commandPool = device.createCommandPool();
        frame.finished = device.createFence(true);
        frame.imageAcquired = device.createSemaphore();
        frame.descriptors = std::make_unique<FrameDescriptors>(device.getDevice());
        frame.uniforms = std::make_unique<FrameUniforms>(device, layouts);
        frame.scene = std::make_unique<SceneBuffers>(device.getAllocator());

        VkCommandBufferAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        info.commandPool = frame.commandPool.get();
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        vulkanCheck(vkAllocateCommandBuffers(device.getDevice(), &info, &frame.commands), "Frame command buffer allocation failed");
        device.nameObject(VK_OBJECT_TYPE_COMMAND_BUFFER, reinterpret_cast<std::uint64_t>(frame.commands), "Frame commands");

        if (device.getTimestampValidBits() > 0) {
            frame.timestamps = device.createTimestampPool(gpuTimestampCount);
        }

        frames.push_back(std::move(frame));
    }
}

/*
* The same filtering SDL_GPU's samplers had. Surfaces: blended between the four nearest texels and the two nearest mip
* levels (trilinear), repeating past 0..1, and sampled along the strip a grazing view covers (anisotropic, up to 16
* samples or the GPU's limit). Any number of levels may be read (SDL set each texture's own count; the GPU clamps to
* what an image has anyway). The shadow sampler compares instead of returning depth: with reversed-Z a point is lit
* when its depth is at least the stored one, and linear filtering blends the four nearest comparisons.
*/
void VulkanRenderer::createSamplers() {
    VkSamplerCreateInfo surface = samplerInfo(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    surface.anisotropyEnable = VK_TRUE;
    surface.maxAnisotropy = std::min(16.0f, device.getLimits().maxSamplerAnisotropy);
    surface.maxLod = VK_LOD_CLAMP_NONE;
    surfaceSampler = device.createSampler(surface);

    tableSampler = device.createSampler(samplerInfo(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE));
    nearestSampler = device.createSampler(samplerInfo(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE));
    linearSampler = device.createSampler(samplerInfo(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE));

    VkSamplerCreateInfo shadow = samplerInfo(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    shadow.compareEnable = VK_TRUE;
    shadow.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    shadowSampler = device.createSampler(shadow);

    shadowDepthSampler = device.createSampler(samplerInfo(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE));
}

/*
* Room for 16,384 textures (or the GPU's limit, if lower): the demo loads a few dozen, so a level with hundreds of
* materials still fits. Empty slots cost only their descriptors (a few dozen bytes each).
*/
void VulkanRenderer::createSceneLayout() {
    bindlessTextures = std::make_unique<BindlessTextures>(device, maxBindlessTextures);
    sceneLayout = std::make_unique<SceneLayout>(device.getDevice(), bindlessTextures->getLayout());
}

/*
* The main pass: back faces culled (counter-clockwise is the front, as SDL_GPU set it with its flipped viewport), depth
* tested and written with GREATER (reversed-Z: nearer is larger). The shadow pass draws depth only, both sides of every
* triangle so open meshes still cast shadows, with no depth bias (the shader's normal offset handles acne). The tile
* reset draws the full-screen triangle at the far depth with a test that always passes, overwriting one tile.
*/
void VulkanRenderer::createPipelines() {
    PipelineDescription main;
    main.vertexShader = "assets/shaders/triangle.vert.spv";
    main.fragmentShader = "assets/shaders/triangle.frag.spv";
    main.sharedLayout = sceneLayout->getPipelineLayout(); // everything from the scene's sets
    main.vertices = meshVertices();
    main.cullMode = VK_CULL_MODE_BACK_BIT;
    main.colourFormat = sceneColourFormat;
    main.depthFormat = depthFormat;
    main.depthTest = true;
    main.depthWrite = true;
    main.depthCompare = VK_COMPARE_OP_GREATER;
    mainPipeline = std::make_unique<VulkanPipeline>(device, layouts, main);

    PipelineDescription shadow;
    shadow.vertexShader = "assets/shaders/shadow.vert.spv";
    shadow.fragmentShader = "assets/shaders/shadow.frag.spv";
    shadow.sharedLayout = sceneLayout->getPipelineLayout();
    shadow.vertices.stride = sizeof(MeshVertex);
    shadow.vertices.attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<std::uint32_t>(offsetof(MeshVertex, position))}};
    shadow.depthFormat = depthFormat;
    shadow.depthTest = true;
    shadow.depthWrite = true;
    shadow.depthCompare = VK_COMPARE_OP_GREATER;
    shadowPipeline = std::make_unique<VulkanPipeline>(device, layouts, shadow);

    PipelineDescription reset = shadow;
    reset.vertexShader = "assets/shaders/fullscreen_triangle.vert.spv";
    reset.vertices = {};
    reset.depthCompare = VK_COMPARE_OP_ALWAYS;
    shadowClearPipeline = std::make_unique<VulkanPipeline>(device, layouts, reset);
}

// The pipelines that draw into the screen image are built for its format, which changes with HDR output.
void VulkanRenderer::createScreenPipelines(VkFormat format) {
    if (format == screenFormat && toneMapPipeline) {
        return;
    }

    toneMapPipeline = createToneMapPipeline(format);
    debugLines->setTargetFormat(format);
    overlay->setTargetFormat(format);
    screenFormat = format;
}

std::unique_ptr<VulkanPipeline> VulkanRenderer::createToneMapPipeline(VkFormat format) {
    PipelineDescription description;
    description.vertexShader = "assets/shaders/fullscreen_triangle.vert.spv";
    description.fragmentShader = "assets/shaders/tone_map.frag.spv";
    description.resources.fragmentSamplers = 2;
    description.resources.fragmentUniforms = 1;
    description.colourFormat = format;
    return std::make_unique<VulkanPipeline>(device, layouts, description);
}

/*
* The atlas is drawn into as a depth target and read as a texture, and kept between frames. It starts cleared to the
* far depth ("nothing in the way") and readable, which is the state every frame leaves it in.
*/
void VulkanRenderer::createShadowAtlas() {
    ImageDescription description;
    description.format = depthFormat;
    description.width = shadowAtlasWidth;
    description.height = shadowAtlasHeight;
    description.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    description.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    description.name = "Shadow atlas";
    shadowAtlas = VulkanImage{device, description};

    uploads.run([&](VkCommandBuffer commands) {
        shadowAtlas.transition(commands, ImageState::TransferDestination, ImageContents::Discard);

        const VkClearDepthStencilValue far{gpuFarDepth, 0};
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearDepthStencilImage(commands, shadowAtlas.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1, &range);

        shadowAtlas.transition(commands, ImageState::DepthFragmentRead);
    });
}

/*
* White stands in for a missing texture (it multiplies a colour or factor by 1). The specular albedo table of
* physically based shading is integrated on the CPU (render/gpu/brdf.h), two 16-bit channels per texel.
*/
void VulkanRenderer::createFixedTextures() {
    const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
    whiteTexture = std::make_unique<VulkanTexture>(device, uploads, 1, 1, white, surfaceTexture(TextureEncoding::Srgb));
    bindlessTextures->write(whiteTextureSlot, whiteTexture->getView(), surfaceSampler.get());

    const BrdfTable table = buildBrdfTable();
    const std::vector<std::uint16_t> texels = brdfTableTexels(table);

    VulkanTextureDescription description;
    description.format = VK_FORMAT_R16G16_UNORM;
    description.bytesPerTexel = 2 * sizeof(std::uint16_t);
    description.surface = false;

    brdfTable = std::make_unique<VulkanTexture>(
        device, uploads, table.size, table.size,
        std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(texels.data()), texels.size() * sizeof(std::uint16_t)},
        description);
}

/*
* The scene image and its depth follow the frame's size. The GPU may still read the old pair, so it is waited for
* before they go (only on a resize, or around a golden capture).
*/
void VulkanRenderer::ensureFrameTextures(std::uint32_t width, std::uint32_t height) {
    if (depth && frameWidth == width && frameHeight == height) {
        return;
    }

    ImageDescription depthDescription;
    depthDescription.format = depthFormat;
    depthDescription.width = width;
    depthDescription.height = height;
    depthDescription.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthDescription.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthDescription.name = "Scene depth";
    VulkanImage depthReplacement{device, depthDescription};

    ImageDescription sceneDescription;
    sceneDescription.format = sceneColourFormat;
    sceneDescription.width = width;
    sceneDescription.height = height;
    sceneDescription.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sceneDescription.name = "Scene colour";
    VulkanImage sceneReplacement{device, sceneDescription};

    device.waitIdle();
    depth = std::move(depthReplacement);
    sceneColour = std::move(sceneReplacement);
    frameWidth = width;
    frameHeight = height;
}

/*
* Windows reports HDR per monitor, and it changes while the game runs, so it is read every frame (a cheap property
* lookup). Whether the swapchain can be scRGB is asked of the driver only when HDR comes on.
*/
void VulkanRenderer::updateDisplayOutput(const DisplaySettings& settings) {
    DisplayCapabilities capabilities = readWindowDisplay(window);

    if (capabilities.hdrEnabled == displayCapabilities.hdrEnabled) {
        capabilities.hdrSwapchainSupported = displayCapabilities.hdrSwapchainSupported;
    }
    else if (capabilities.hdrEnabled) {
        capabilities.hdrSwapchainSupported = swapchain.supportsHdr();
    }

    displayCapabilities = capabilities;
    displayOutput = chooseDisplayOutput(settings, displayCapabilities);
}

/*
* Before a frame: rebuild the swapchain if the window was resized, it went out of date, HDR output turned on or off, or
* the present mode changed, and remake the pipelines that draw into it if its format changed. The GPU must be idle
* first, since the old images may still be in use. If the driver refuses an HDR swapchain, HDR is marked unsupported
* (so it is not tried every frame) and the frame goes out in SDR. False, drawing nothing, while minimised.
*/
bool VulkanRenderer::prepareSwapchain() {
    const bool wantHdr = displayOutput.hdr;

    if (swapchain.exists() && !swapchain.needsRebuild() && swapchain.isHdr() == wantHdr && swapchain.getPresentMode() == presentMode &&
        swapchainFramesInFlight == framesInFlight) {
        return true;
    }

    const bool hdrChanged = swapchain.exists() && swapchain.isHdr() != wantHdr;
    device.waitIdle();
    bool built = false;

    try {
        built = swapchain.rebuild(wantHdr, presentMode, framesInFlight);
    }
    catch (const std::runtime_error& error) {
        if (!wantHdr) {
            throw;
        }

        SDL_Log("HDR output unavailable: %s", error.what());
        displayCapabilities.hdrSwapchainSupported = false;
        displayOutput = DisplayOutput{};
        built = swapchain.rebuild(false, presentMode, framesInFlight);
    }

    if (!built) {
        return false;
    }

    swapchainFramesInFlight = framesInFlight;
    createScreenPipelines(swapchain.getFormat());

    if (hdrChanged) {
        if (displayOutput.hdr) {
            SDL_Log("HDR output on: peak %.2f times SDR white, SDR white at %.2f (%.0f nits)",
                    displayOutput.peakBrightness, displayOutput.whiteLevel, displayOutput.whiteLevel * scrgbWhiteNits);
        }
        else {
            SDL_Log("HDR output off");
        }
    }

    return true;
}

/*
* Waits for the GPU to finish the last frame that used this frame's resources, so they can be reused: its command
* buffer, its descriptor sets and its uniform data. The fence is reset only when the next submission is certain.
*/
FrameContext VulkanRenderer::beginFrame() {
    if (recording) {
        throw std::logic_error("Finish the current frame before beginning another");
    }

    Frame& frame = frames[frameIndex];
    const VkFence fence = frame.finished.get();
    vulkanCheck(vkWaitForFences(device.getDevice(), 1, &fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()),
                "Waiting for the previous frame failed");

    readGpuTimings(frame);
    frame.descriptors->reset();
    frame.uniforms->reset();
    vulkanCheck(vkResetCommandPool(device.getDevice(), frame.commandPool.get(), 0), "Frame command pool reset failed");

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vulkanCheck(vkBeginCommandBuffer(frame.commands, &begin), "Frame command buffer start failed");
    recording = true;

    // Queries must be reset before they are written again; outside any pass, so first.
    if (frame.timestamps) {
        vkCmdResetQueryPool(frame.commands, frame.timestamps.get(), 0, gpuTimestampCount);
    }

    frame.timestampsWritten = 0;
    frame.timestampsComplete = false;

    return FrameContext{frame.commands, *frame.uniforms, *frame.descriptors, frameIndex};
}

/*
* Ends the command buffer and submits it. A frame for the screen waits for its image's acquire semaphore at the colour
* output stage (nothing before that touches the image, so shadows and the main pass start at once) and signals the
* image's render-finished semaphore, which presentation waits for. The fence tells the CPU when it is all done.
*/
void VulkanRenderer::submitFrame(FrameContext& context, bool toScreen) {
    Frame& frame = frames[frameIndex];
    recording = false;
    vulkanCheck(vkEndCommandBuffer(context.commands), "Frame command buffer end failed");
    frame.uniforms->flush();

    VkCommandBufferSubmitInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandInfo.commandBuffer = context.commands;

    VkSemaphoreSubmitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait.semaphore = frame.imageAcquired.get();
    wait.stageMask = acquireWaitStage;

    VkSemaphoreSubmitInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &commandInfo;

    if (toScreen) {
        signal.semaphore = swapchain.getRenderFinished(*acquiredImage);
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
    }

    const VkFence fence = frame.finished.get();
    vulkanCheck(vkResetFences(device.getDevice(), 1, &fence), "Frame fence reset failed");
    vulkanCheck(vkQueueSubmit2(device.getQueue(), 1, &submit, fence), "Frame submission failed");

    if (toScreen) {
        const std::uint32_t image = *acquiredImage;
        acquiredImage.reset();
        swapchain.present(device.getQueue(), image);
    }

    frameIndex = (frameIndex + 1) % framesInFlight;
}

/*
* After an error mid-frame: close what is open, and still submit and present an acquired image (with whatever was drawn),
* since the swapchain lent it and expects it back. Errors here are ignored: the first one is being reported.
*/
void VulkanRenderer::abandonFrame() noexcept {
    if (!recording) {
        return;
    }

    Frame& frame = frames[frameIndex];

    try {
        if (renderingOpen) {
            vkCmdEndRendering(frame.commands);
            renderingOpen = false;
        }

        FrameContext context{frame.commands, *frame.uniforms, *frame.descriptors, frameIndex};
        const bool toScreen = acquiredImage.has_value();

        if (toScreen) {
            transitionImage(frame.commands, swapchain.getImage(*acquiredImage), VK_IMAGE_ASPECT_COLOR_BIT,
                            swapchain.getState(*acquiredImage), ImageState::Present);
        }

        submitFrame(context, toScreen);
    }
    catch (...) {
        recording = false;
        acquiredImage.reset();
    }
}

/*
* Once the frame's fence has passed, the GPU has written every timestamp it was given, so they are read without
* waiting. Only a frame that wrote them all is read (not an abandoned one, or one that found no swapchain image);
* VK_NOT_READY would mean a stamp is missing, and that frame is simply not reported.
*/
void VulkanRenderer::readGpuTimings(Frame& frame) {
    if (!frame.timestamps || !frame.timestampsComplete) {
        return;
    }

    frame.timestampsComplete = false;
    std::array<std::uint64_t, gpuTimestampCount> ticks{};
    const VkResult result = vkGetQueryPoolResults(device.getDevice(), frame.timestamps.get(), 0, gpuTimestampCount, sizeof(ticks), ticks.data(),
                                                  sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);

    if (result == VK_NOT_READY) {
        return;
    }

    vulkanCheck(result, "Reading the GPU timestamps failed");
    unreportedGpuTimings = gpuTimingsFromTicks(ticks, device.getTimestampPeriod(), device.getTimestampValidBits());
}

/*
* Writes the frame's timestamps up to and including this one. Each is written once everything recorded before it has
* finished on the GPU (the all-commands stage), so the gap between two is how much later the frame finished because of
* the work between them. Stamps a skipped pass would have written go in at the same moment, so it takes no time.
*/
void VulkanRenderer::writeTimestampsThrough(FrameContext& context, std::uint32_t index) {
    Frame& frame = frames[context.frameIndex];

    if (!frame.timestamps) {
        return;
    }

    while (frame.timestampsWritten <= index && frame.timestampsWritten < gpuTimestampCount) {
        vkCmdWriteTimestamp2(context.commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, frame.timestamps.get(), frame.timestampsWritten);
        ++frame.timestampsWritten;
    }
}

// Pass number p starts at stamp p and ends at stamp p + 1, and is labelled in between for RenderDoc and Nsight.
void VulkanRenderer::beginGpuPass(FrameContext& frame, GpuPass pass) {
    writeTimestampsThrough(frame, static_cast<std::uint32_t>(pass));
    device.beginLabel(frame.commands, gpuPassName(pass));
}

void VulkanRenderer::endGpuPass(FrameContext& frame, GpuPass pass) {
    device.endLabel(frame.commands);
    writeTimestampsThrough(frame, static_cast<std::uint32_t>(pass) + 1);
}

// After the last pass: any stamps of passes not drawn are written, and the frame is marked for reading next time round.
void VulkanRenderer::finishGpuTimings(FrameContext& context) {
    writeTimestampsThrough(context, gpuTimestampCount - 1);
    Frame& frame = frames[context.frameIndex];
    frame.timestampsComplete = static_cast<bool>(frame.timestamps);
}

/*
* The whole frame arrives at once, so the renderer decides how to draw it: the shadow views into the atlas, the camera's
* view into the scene image (reading the atlas), bloom, then tone mapping into the swapchain image with the debug lines
* and the UI on top. The image goes from "acquired" to a colour target (its old contents discarded, since every pixel is
* written) and, at the end, to the layout presentation needs.
*/
bool VulkanRenderer::render(const FrameDescription& description, const RenderSettings& settings, ImDrawData* ui) {
    updateDisplayOutput(settings.display);

    if (!prepareSwapchain()) {
        SDL_Delay(10);
        return false;
    }

    FrameContext frame = beginFrame();

    try {
        acquiredImage = swapchain.acquire(frames[frameIndex].imageAcquired.get());

        if (!acquiredImage) {
            // Out of date (a resize mid-frame): nothing to draw into. Submit the empty buffer and rebuild next frame.
            submitFrame(frame, false);
            return false;
        }

        const VkExtent2D extent = swapchain.getExtent();
        ensureFrameTextures(extent.width, extent.height);

        const BloomComposite bloomMix = drawScene(frame, description, settings, ui);
        const std::uint32_t image = *acquiredImage;
        ImageState& imageState = swapchain.getState(image);

        transitionImage(frame.commands, swapchain.getImage(image), VK_IMAGE_ASPECT_COLOR_BIT, imageState, ImageState::ColourTarget,
                        ImageContents::Discard);

        const ColourAttachment colour{swapchain.getView(image), VK_ATTACHMENT_LOAD_OP_DONT_CARE, {}};
        beginRendering(frame.commands, extent, &colour, nullptr);
        renderingOpen = true;

        // Labels begin and end inside the one rendering block, so none crosses its boundary.
        beginGpuPass(frame, GpuPass::ToneMap);
        drawToneMap(frame, *toneMapPipeline, settings.toneMapping, bloomMix);
        endGpuPass(frame, GpuPass::ToneMap);

        // Last, so they are drawn over the finished image in exactly their own colours, and the UI over them.
        beginGpuPass(frame, GpuPass::DebugLines);
        debugLines->draw(frame, viewProjection, frameWidth, frameHeight);
        endGpuPass(frame, GpuPass::DebugLines);

        if (ui != nullptr) {
            beginGpuPass(frame, GpuPass::Overlay);
            overlay->composite(frame, displayOutput.whiteLevel);
            endGpuPass(frame, GpuPass::Overlay);
        }

        vkCmdEndRendering(frame.commands);
        renderingOpen = false;

        transitionImage(frame.commands, swapchain.getImage(image), VK_IMAGE_ASPECT_COLOR_BIT, imageState, ImageState::Present);
        finishGpuTimings(frame);
        submitFrame(frame, true);
    }
    catch (...) {
        // The shadow pass may have stopped part-way, so no cached tile can be trusted.
        shadowMemory.forgetAll();
        abandonFrame();
        throw;
    }

    return true;
}

/*
* Everything up to tone mapping, into the frame textures (already the frame's size): lights chosen, shadows, the main
* pass and bloom, with the UI drawn into its own image on the way. Leaves the scene image readable and no pass open,
* and returns how tone mapping should mix in the bloom.
*/
BloomComposite VulkanRenderer::drawScene(FrameContext& frame, const FrameDescription& description, const RenderSettings& settings,
                                         ImDrawData* ui) {
    stats = RenderStats{};
    stats.gpu = std::exchange(unreportedGpuTimings, GpuTimings{});

    Camera camera = description.camera;
    camera.setAspectRatio(getFrameAspectRatio());

    viewProjection = toGpuDepthRange(camera.getProjectionMatrix()) * camera.getViewMatrix();
    relativeViewProjection = cameraRelativeViewProjection(camera);
    cameraOrigin = camera.getPosition();
    const Frustum cameraFrustum = Frustum::fromClipMatrix(viewProjection, ClipDepth::OneToZero);

    const PrioritizedLighting prioritized = prioritizeLights(description.lighting, description.lightFocus, cameraFrustum, lightHistory,
                                                             description.frameSeconds, settings.lights);
    lightHistory = prioritized.history;

    stats.pointLights = description.lighting.pointLights.size();
    stats.pointLightsInView = prioritized.pointLightsInView;
    stats.pointLightsLit = prioritized.lighting.pointLights.size();
    stats.pointLightsShadowed = static_cast<std::size_t>(std::count_if(
        prioritized.lighting.pointLights.begin(), prioritized.lighting.pointLights.end(),
        [](const PlacedPointLight& placed) { return placed.light.castsShadows; }));

    GpuLights lights = packLighting(prioritized.lighting, cameraOrigin);
    const ShadowPlan shadows = planShadows(prioritized.lighting, camera, shadowLayout, settings.shadows);
    attachShadows(lights, shadows);

    // What every view draws is settled before anything is recorded: the camera's visible draws and each changed shadow
    // tile's casters, batched by mesh, so the scene buffers can be written once and the passes only draw.
    const std::vector<std::size_t> visible = visibleDraws(description.draws, cameraFrustum);
    const BatchedDraws mainDraws = batchByMesh(description.draws, visible);
    stats.drawn = visible.size();
    stats.culled = description.draws.size() - visible.size();

    const ShadowDraws shadowDraws = prepareShadowDraws(description, shadows.tiles);

    // Written straight into this frame's upload memory: no copy pass, since the GPU reads them where they are.
    debugLines->upload(frame.frameIndex, description.debugLines, description.debugScreenLines, displayOutput.whiteLevel);
    const VkDescriptorSet sceneSet = writeScene(frame, description, lights, shadows, mainDraws, shadowDraws);

    if (ui != nullptr) {
        beginGpuPass(frame, GpuPass::DebugUi);
        overlay->drawUi(frame, ui, frameWidth, frameHeight);
        endGpuPass(frame, GpuPass::DebugUi);
    }

    if (!shadows.tiles.empty()) {
        beginGpuPass(frame, GpuPass::Shadows);
        drawShadows(frame, shadowDraws, sceneSet);
        endGpuPass(frame, GpuPass::Shadows);
    }

    beginGpuPass(frame, GpuPass::Main);
    drawMainPass(frame, mainDraws, sceneSet);
    endGpuPass(frame, GpuPass::Main);

    // The scattered light, from the finished scene image, before tone mapping mixes it in.
    beginGpuPass(frame, GpuPass::Bloom);
    const BloomComposite bloomMix = bloom->draw(frame, sceneColour, frameWidth, frameHeight, settings.bloom);
    endGpuPass(frame, GpuPass::Bloom);
    return bloomMix;
}

/*
* The frame's GPU scene, into its upload buffers: the frame's uniform block, an instance per visible draw (in the main
* pass's batch order, so a batch's instances are consecutive) with its material numbered in the frame's material
* table, the lights with their shadows, the shadow tiles and the shadow casters' instances. Returns the scene's set 1,
* pointing at them and at the shadow atlas and BRDF table, bound once per pass.
*/
VkDescriptorSet VulkanRenderer::writeScene(FrameContext& frame, const FrameDescription& description, const GpuLights& lights,
                                           const ShadowPlan& shadows, const BatchedDraws& mainDraws, const ShadowDraws& shadowDraws) {
    MaterialTable materials;
    std::vector<GpuInstance> instances;
    instances.reserve(mainDraws.order.size());

    for (std::size_t index : mainDraws.order) {
        const DrawItem& draw = description.draws[index];
        checkTexturesUploaded(draw.material);
        instances.push_back(packInstance(draw, relativeViewProjection, cameraOrigin, materials.add(draw.material)));
    }

    SceneBuffers& buffers = *frames[frame.frameIndex].scene;
    const GpuFrameData frameData = makeFrameData(lights);
    buffers.frameData.write(&frameData, sizeof(frameData));
    buffers.instances.write(instances);
    buffers.materials.write(materials.getEntries());
    buffers.directionalLights.write(lights.directional);
    buffers.pointLights.write(lights.points);
    buffers.spotLights.write(lights.spots);
    buffers.shadowTiles.write(shadows.tileData);
    buffers.shadowInstances.write(shadowDraws.instances);

    const SceneImages images{
        SampledImage{shadowAtlas.getView(), shadowSampler.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        SampledImage{shadowAtlas.getView(), shadowDepthSampler.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        sampled(*brdfTable),
    };

    return writeSceneSet(frame.descriptors, *sceneLayout, buffers, images);
}

/*
* Which shadow tiles to redraw, and what goes into each. A tile whose view and casters are what was drawn into it last
* time keeps its contents (shadow caching). Casters are those inside the tile's frustum. Each redrawn tile's casters
* are batched by mesh, their instances (the tile's view times the caster's model matrix) one tile after another.
*/
VulkanRenderer::ShadowDraws VulkanRenderer::prepareShadowDraws(const FrameDescription& description, const std::vector<ShadowTile>& tiles) {
    ShadowDraws prepared;

    for (const ShadowTile& tile : tiles) {
        if (tile.size == 0) {
            ++stats.shadowTilesSkipped;
            continue;
        }

        const std::vector<std::size_t> casters = shadowCasters(description.draws, Frustum::fromClipMatrix(tile.matrix, ClipDepth::OneToZero));
        const std::uint64_t signature = shadowCasterSignature(description.draws, casters);

        if (shadowMemory.isCurrent(tile, signature)) {
            ++stats.shadowTilesCached;
            continue;
        }

        BatchedDraws batched = batchByMesh(description.draws, casters, static_cast<std::uint32_t>(prepared.instances.size()));

        for (std::size_t index : batched.order) {
            prepared.instances.push_back(packShadowInstance(tile.matrix, description.draws[index].model));
        }

        stats.shadowDrawn += casters.size();
        stats.shadowCulled += description.draws.size() - casters.size();
        prepared.tiles.push_back(ShadowTileDraw{&tile, std::move(batched), signature});
    }

    return prepared;
}

/*
* The shadow maps, one tile per view (the viewport picks the tile), depth only: each redrawn tile is first reset to the
* far depth, then its casters drawn, one instanced draw per mesh. When nothing changed there is no pass at all.
*
* Barriers: the atlas goes from being read by last frame's fragment shaders to being a depth target (its contents kept:
* only some tiles are redrawn), and back to being read once the tiles are drawn, before the main pass samples it.
*/
void VulkanRenderer::drawShadows(FrameContext& frame, const ShadowDraws& shadowDraws, VkDescriptorSet sceneSet) {
    if (shadowDraws.tiles.empty()) {
        return;
    }

    const VkCommandBuffer commands = frame.commands;
    shadowAtlas.transition(commands, ImageState::DepthTarget);

    const DepthAttachment atlas{shadowAtlas.getView(), VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, gpuFarDepth};
    beginRendering(commands, {shadowAtlasWidth, shadowAtlasHeight}, nullptr, &atlas);
    renderingOpen = true;

    // The reset and the shadow pipelines share the scene's layout, so the sets stay bound across both.
    bindScene(commands, *sceneLayout, bindlessTextures->getSet(), sceneSet);

    for (const ShadowTileDraw& drawing : shadowDraws.tiles) {
        const ShadowTile& tile = *drawing.tile;
        ++stats.shadowTilesDrawn;

        setViewport(commands, static_cast<float>(tile.x), static_cast<float>(tile.y), static_cast<float>(tile.size), static_cast<float>(tile.size));

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowClearPipeline->get());
        drawFullScreenTriangle(commands);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline->get());

        stats.shadowDrawCalls += drawBatches(commands, drawing.casters.batches);
        shadowMemory.remember(tile, drawing.signature);
    }

    vkCmdEndRendering(commands);
    renderingOpen = false;
    shadowAtlas.transition(commands, ImageState::DepthFragmentRead);
}

/*
* The camera's view into the scene image: one instanced draw per mesh, every object's matrices, material, textures and
* lights read from the scene's buffers. Both targets start cleared, so their old contents are discarded (the barrier
* still waits for last frame's reads of the scene image to finish first). Afterwards the scene image becomes readable
* for bloom and tone mapping.
*/
void VulkanRenderer::drawMainPass(FrameContext& frame, const BatchedDraws& mainDraws, VkDescriptorSet sceneSet) {
    const VkCommandBuffer commands = frame.commands;
    sceneColour.transition(commands, ImageState::ColourTarget, ImageContents::Discard);
    depth.transition(commands, ImageState::DepthTarget, ImageContents::Discard);

    const ColourAttachment colour{sceneColour.getView(), VK_ATTACHMENT_LOAD_OP_CLEAR, {0.0f, 0.0f, 0.0f, 1.0f}};
    const DepthAttachment depthTarget{depth.getView(), VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, gpuFarDepth};
    beginRendering(commands, {frameWidth, frameHeight}, &colour, &depthTarget);
    renderingOpen = true;

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, mainPipeline->get());
    bindScene(commands, *sceneLayout, bindlessTextures->getSet(), sceneSet);
    stats.drawCalls = drawBatches(commands, mainDraws.batches);

    vkCmdEndRendering(commands);
    renderingOpen = false;
    sceneColour.transition(commands, ImageState::FragmentRead);
}

/*
* One instanced draw per batch: the mesh's buffers bound, and every instance of it drawn at once (gl_InstanceIndex counts
* from firstInstance, the batch's first record in the instance buffer). Returns how many draw calls that took.
*/
std::size_t VulkanRenderer::drawBatches(VkCommandBuffer commands, const std::vector<DrawBatch>& batches) const {
    for (const DrawBatch& batch : batches) {
        std::uint32_t indexCount = 0;
        bindMesh(commands, batch.mesh, indexCount);
        vkCmdDrawIndexed(commands, indexCount, batch.instanceCount, 0, 0, batch.firstInstance);
    }

    return batches.size();
}

/*
* The tone mapping pass, into the open pass: the full-screen triangle reads the scene texel for texel and mixes in the
* bloom (half the screen's size, filtered). Without bloom the shader never reads it, but the slot still needs an image,
* so the scene stands in.
*/
void VulkanRenderer::drawToneMap(FrameContext& frame, const VulkanPipeline& pipeline, const ToneMappingSettings& settings,
                                 const BloomComposite& bloomMix) {
    const VkCommandBuffer commands = frame.commands;
    const ToneMapUniformData toneMapping = packToneMapping(settings, displayOutput, bloomMix);

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.get());
    frame.uniforms.push(VK_SHADER_STAGE_FRAGMENT_BIT, 0, &toneMapping, sizeof(toneMapping));
    frame.uniforms.bind(commands, pipeline);

    const VulkanImage* bloomResult = bloom->getResult();
    const std::array<SampledImage, 2> images{
        SampledImage{sceneColour.getView(), nearestSampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        SampledImage{bloomResult != nullptr ? bloomResult->getView() : sceneColour.getView(), linearSampler.get(),
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    bindFragmentResources(commands, frame.descriptors, pipeline, images);
    drawFullScreenTriangle(commands);
}

/*
* The same frame drawn off screen at a fixed size for a fixed display, and read back: the scene image as the main pass
* left it and the tone mapped image as the screen would get it. The images are copied into read-back memory, and a
* final barrier makes the copies visible to the CPU once the fence says they are done. Nothing is presented; the
* display output is restored afterwards. Debug lines and the UI are not drawn. Slow (it waits): for golden images.
*/
CapturedFrame VulkanRenderer::capture(const FrameDescription& description, const RenderSettings& settings, Uint32 width, Uint32 height,
                                      const DisplayOutput& output) {
    const DisplayOutput screenOutput = displayOutput;
    const PixelFormat finalFormat = output.hdr ? PixelFormat::RgbaHalf : PixelFormat::Rgba8;
    const VkDeviceSize sceneBytes = VkDeviceSize{width} * height * bytesPerPixel(PixelFormat::RgbaHalf);
    const VkDeviceSize finalBytes = VkDeviceSize{width} * height * bytesPerPixel(finalFormat);

    ImageDescription targetDescription;
    targetDescription.format = captureFormat(output.hdr);
    targetDescription.width = width;
    targetDescription.height = height;
    targetDescription.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    targetDescription.name = "Golden capture";
    VulkanImage target{device, targetDescription};

    const std::unique_ptr<VulkanPipeline> capturePipeline = createToneMapPipeline(targetDescription.format);
    VulkanBuffer readback{device.getAllocator(), sceneBytes + finalBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback};

    ensureFrameTextures(width, height);
    FrameContext frame = beginFrame();
    displayOutput = output;

    try {
        const BloomComposite bloomMix = drawScene(frame, description, settings, nullptr);

        target.transition(frame.commands, ImageState::ColourTarget, ImageContents::Discard);
        const ColourAttachment colour{target.getView(), VK_ATTACHMENT_LOAD_OP_DONT_CARE, {}};
        beginRendering(frame.commands, {width, height}, &colour, nullptr);
        renderingOpen = true;
        beginGpuPass(frame, GpuPass::ToneMap);
        drawToneMap(frame, *capturePipeline, settings.toneMapping, bloomMix);
        endGpuPass(frame, GpuPass::ToneMap);
        vkCmdEndRendering(frame.commands);
        renderingOpen = false;
        finishGpuTimings(frame);

        sceneColour.transition(frame.commands, ImageState::TransferSource);
        target.transition(frame.commands, ImageState::TransferSource);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(frame.commands, sceneColour.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.get(), 1, &copy);

        copy.bufferOffset = sceneBytes;
        vkCmdCopyImageToBuffer(frame.commands, target.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.get(), 1, &copy);

        VkMemoryBarrier2 toHost{};
        toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        toHost.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &toHost;
        vkCmdPipelineBarrier2(frame.commands, &dependency);

        submitFrame(frame, false);
        device.waitIdle();
        readback.invalidate();
    }
    catch (...) {
        shadowMemory.forgetAll();
        abandonFrame();
        device.waitIdle();
        displayOutput = screenOutput;
        throw;
    }

    displayOutput = screenOutput;

    CapturedFrame captured;
    captured.scene = readImage(readback, 0, width, height, PixelFormat::RgbaHalf);
    captured.final = readImage(readback, sceneBytes, width, height, finalFormat);
    return captured;
}

std::string VulkanRenderer::getDeviceName() const {
    return device.getName();
}

/*
* Handles are indices into the same numbering the AssetManager uses, so finding the GPU copy is one array access.
*/
void VulkanRenderer::bindMesh(VkCommandBuffer commands, MeshHandle meshHandle, std::uint32_t& indexCount) const {
    if (!meshHandle.isValid() || meshHandle.index >= meshes.size()) {
        throw std::out_of_range("Mesh handle has not been uploaded");
    }

    const VulkanMesh& mesh = *meshes[meshHandle.index];
    const VkBuffer vertexBuffer = mesh.getVertexBuffer();
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, &vertexBuffer, &offset);
    vkCmdBindIndexBuffer(commands, mesh.getIndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
    indexCount = mesh.getIndexCount();
}

// The shader would read an empty slot of the texture array for a texture that was never uploaded; better to stop here.
void VulkanRenderer::checkTexturesUploaded(const Material& material) const {
    for (TextureHandle handle : {material.texture, material.metallicRoughnessTexture, material.normalTexture}) {
        if (handle.isValid() && handle.index >= textures.size()) {
            throw std::out_of_range("Material texture handle has not been uploaded");
        }
    }
}

SampledImage VulkanRenderer::sampled(const VulkanTexture& texture) const {
    return SampledImage{texture.getView(), texture.isSurface() ? surfaceSampler.get() : tableSampler.get(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

// Uploads wait for the GPU with their own command buffer, so they must happen between frames. A texture takes its slot
// in the bindless array at once (slots in-flight frames do not read may change while they run).
void VulkanRenderer::uploadMesh(const IndexedMesh& mesh) {
    if (recording) {
        throw std::logic_error("Upload meshes before beginning a frame");
    }

    meshes.push_back(std::make_unique<VulkanMesh>(device, uploads, mesh));
}

void VulkanRenderer::uploadTexture(Uint32 width, Uint32 height, std::span<const Uint8> pixels, TextureEncoding encoding) {
    if (recording) {
        throw std::logic_error("Upload textures before beginning a frame");
    }

    textures.push_back(std::make_unique<VulkanTexture>(device, uploads, width, height, pixels, surfaceTexture(encoding)));
    const TextureHandle handle{static_cast<std::uint32_t>(textures.size() - 1)};
    bindlessTextures->write(bindlessTextureSlot(handle), textures.back()->getView(), surfaceSampler.get());
}

std::size_t VulkanRenderer::getMeshCount() const {
    return meshes.size();
}

std::size_t VulkanRenderer::getTextureCount() const {
    return textures.size();
}

/*
* ImGui's Vulkan backend draws into the overlay image, whose format never changes, with dynamic rendering. It makes its
* own descriptor pool for its font atlas, and keeps one set of vertex buffers per frame it may have in flight (at least two).
*/
void VulkanRenderer::startDebugUi() {
    if (debugUiStarted) {
        return;
    }

    static constexpr VkFormat overlayFormat = vulkanOverlayFormat;

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = device.getInstance();
    info.PhysicalDevice = device.getPhysicalDevice();
    info.Device = device.getDevice();
    info.QueueFamily = device.getQueueFamily();
    info.Queue = device.getQueue();
    info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
    info.MinImageCount = 2;
    info.ImageCount = std::max<std::uint32_t>(2, maxFramesInFlight);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &overlayFormat;

    if (!ImGui_ImplVulkan_Init(&info)) {
        throw std::runtime_error("Debug UI renderer start-up failed");
    }

    debugUiStarted = true;
}

void VulkanRenderer::beginDebugUiFrame() {
    ImGui_ImplVulkan_NewFrame();
}

void VulkanRenderer::stopDebugUi() {
    if (!debugUiStarted) {
        return;
    }

    device.waitIdle();
    ImGui_ImplVulkan_Shutdown();
    debugUiStarted = false;
}

/*
* Not every GPU and driver supports every mode, so check first and fall back to Vsync, which Vulkan guarantees. The
* swapchain is rebuilt with the new mode before the next frame.
*/
PresentMode VulkanRenderer::setPresentMode(PresentMode requested) {
    if (recording) {
        throw std::logic_error("Change the present mode between frames");
    }

    presentMode = swapchain.supportsPresentMode(requested) ? requested : PresentMode::Vsync;
    return presentMode;
}

PresentMode VulkanRenderer::getPresentMode() const {
    return presentMode;
}

/*
* The GPU may still be reading the frames' own buffers, so it is waited for before they go. Timings already read stay
* to be reported; those of frames not read yet are dropped with their pools. The swapchain is rebuilt before the next
* frame, since its image count depends on the number of frames in flight.
*/
std::uint32_t VulkanRenderer::setFramesInFlight(std::uint32_t requested) {
    if (recording) {
        throw std::logic_error("Change the frames in flight between frames");
    }

    const std::uint32_t count = validFramesInFlight(requested);

    if (count == framesInFlight) {
        return framesInFlight;
    }

    device.waitIdle();
    framesInFlight = count;
    createFrames();
    debugLines->setFramesInFlight(count);
    return framesInFlight;
}

std::uint32_t VulkanRenderer::getFramesInFlight() const {
    return framesInFlight;
}

const RenderStats& VulkanRenderer::getLastFrameStats() const {
    return stats;
}

const DisplayOutput& VulkanRenderer::getDisplayOutput() const {
    return displayOutput;
}

float VulkanRenderer::getFrameAspectRatio() const {
    if (frameWidth == 0 || frameHeight == 0) {
        throw std::logic_error("No frame dimensions are available");
    }

    return static_cast<float>(frameWidth) / static_cast<float>(frameHeight);
}
