#pragma once

#include "render/gpu/gpu_scene.h"
#include "render/gpu/bone_poses_shared.h"
#include "render/gpu/liana_trees_shared.h"
#include "render/gpu/mesh_cluster_culling_shared.h"
#include "render/gpu/placed_drawing_shared.h"
#include "render/gpu/twig_drawing_shared.h"
#include "render/gpu/virtual_shadows_shared.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/owned_handle.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_memory.h"

#include "vma/vk_mem_alloc.h"
#include "volk/volk.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/*
* The descriptor layout of the scene's pipelines (the main pass, the shadow pass and its tile reset, the cluster pass),
* which left SDL_GPU's four sets behind with the GPU scene: set 0 is every texture (BindlessTextures, made once), set 1
* the frame's scene (made once per frame and bound once per pass): its uniform block, the storage buffers of instances,
* materials, lights, shadow tiles, the draw list, the clusters' light masks and the lights probe rays' hits may need,
* and the shadow atlas, the BRDF table and the light probes' three atlases.
* Nothing is bound per draw; each shadow tile pushes its view (GpuShadowView) as a push constant, a few bytes recorded
* straight into the command buffer; the main pass's motion vectors read each instance's model matrix from last frame
* (binding 21).
*/
inline constexpr std::uint32_t bindlessTextureSet = 0;
inline constexpr std::uint32_t sceneFrameSet = 1;

// Set 1's bindings, as triangle.vert, triangle.frag, shadow.vert and cluster_lights.comp declare them.
enum SceneBinding : std::uint32_t {
    FrameDataBinding = 0,
    InstancesBinding = 1,
    MaterialsBinding = 2,
    DirectionalLightsBinding = 3,
    PointLightsBinding = 4,
    SpotLightsBinding = 5,
    ShadowTilesBinding = 6,
    ShadowAtlasBinding = 7,
    ShadowDepthsBinding = 8,
    BrdfTableBinding = 9,
    DrawListBinding = 10,
    ClusterMasksBinding = 11,
    ProbeIrradianceBinding = 12,
    ProbeDistanceBinding = 13,
    ProbeHitLightsBinding = 14,
    ProbeFastIrradianceBinding = 15,
    ProbeStatesBinding = 16,
    SkyLightBinding = 17,
    SkyViewBinding = 18,
    AtmosphereBinding = 19,
    ProbeCascadesBinding = 20,
    PreviousModelsBinding = 21,
    MirroredLightBinding = 22,
    MirroredDirectionBinding = 23,
    MirrorPairsBinding = 24,
    AmbientOcclusionBinding = 25,
    GlassBoundsBinding = 26,
    FogBinding = 27,
    FogVolumeBinding = 28,
    SkyVisibilityBinding = 29,
    SkyVisibilityCascadesBinding = 30,
    ContactShadowsBinding = 31,
    LightContactShadowsBinding = 32,
    GlassTintBinding = 33,
    GlassDepthsBinding = 34,
    TerrainHeightsBinding = 35,
    TerrainPatchesBinding = 36,
    WindFieldBinding = 37,
    WindSpringsBinding = 38,
    WindPreviousSpringsBinding = 39,
    HangingVerticesBinding = 40,
    LianaShapesBinding = 41,
    ThinningCardsBinding = 42,
    PlacedPlantsBinding = 43,
    PlacedPartsBinding = 44,
    PlacedEntriesBinding = 45,
    TwigEntriesBinding = 46,
    TwigsBinding = 47,
    SwayBindingsBinding = 48,
    TwigBindingsBinding = 49,
    BoneTreesBinding = 50,
    BonePlantTreesBinding = 51,
    BonePosesBinding = 52,
    LianaTreePlantsBinding = 53,
    FarClustersBinding = 54,
    FarLeavesBinding = 55,
    FarShapesBinding = 56,
    LeafPyramidEarlyBinding = 57,
    LeafPyramidWholeBinding = 58,
    VirtualShadowFrameBinding = 59,
    VirtualShadowTableBinding = 60,
    SceneBindingCount = 61
};

static_assert(FarClustersBinding == SCENE_FAR_CLUSTERS_BINDING && FarLeavesBinding == SCENE_FAR_LEAVES_BINDING && FarShapesBinding == SCENE_FAR_SHAPES_BINDING,
              "The far leaves' bindings must match drawn_objects.glsl's");
static_assert(LeafPyramidEarlyBinding == SCENE_LEAF_PYRAMID_EARLY_BINDING && LeafPyramidWholeBinding == SCENE_LEAF_PYRAMID_WHOLE_BINDING,
              "The far leaves' pyramids' bindings must match mesh_cluster_draw.glsl's");

static_assert(BoneTreesBinding == SCENE_BONE_TREES_BINDING && BonePlantTreesBinding == SCENE_BONE_PLANT_TREES_BINDING &&
                  BonePosesBinding == SCENE_BONE_POSES_BINDING && SwayBindingsBinding == SCENE_SWAY_BINDINGS_BINDING,
              "The bones' bindings must match bone_vertex_data.glsl's");
static_assert(VirtualShadowFrameBinding == SCENE_VIRTUAL_SHADOW_FRAME_BINDING && VirtualShadowTableBinding == SCENE_VIRTUAL_SHADOW_TABLE_BINDING,
              "The virtual shadow maps' bindings must match scene_surface.glsl's");
static_assert(LianaTreePlantsBinding == SCENE_LIANA_TREE_PLANTS_BINDING, "The lianas' trees' binding must match liana_data.glsl's");

static_assert(PlacedPlantsBinding == SCENE_PLACED_PLANTS_BINDING && PlacedPartsBinding == SCENE_PLACED_PARTS_BINDING &&
                  PlacedEntriesBinding == SCENE_PLACED_ENTRIES_BINDING,
              "The placed bindings must match drawn_objects.glsl's");
static_assert(TwigEntriesBinding == SCENE_TWIG_ENTRIES_BINDING && TwigsBinding == SCENE_TWIGS_BINDING && TwigBindingsBinding == SCENE_TWIG_BINDINGS_BINDING,
              "The twig bindings must match drawn_objects.glsl's and triangle_vertex.glsl's");

// Set 1's layout and the pipeline layout of both sets and the shadow view's push constant, shared by every scene pipeline,
// so the sets bound for one stay bound for the next.
class SceneLayout {
public:
    // meshShaders: the GPU draws mesh clusters by mesh shaders, which read what the vertex shaders read and push a style.
    SceneLayout(VkDevice device, VkDescriptorSetLayout bindlessLayout, bool meshShaders);

    SceneLayout(const SceneLayout&) = delete;
    SceneLayout& operator=(const SceneLayout&) = delete;

    VkDescriptorSetLayout getFrameLayout() const;
    VkPipelineLayout getPipelineLayout() const;

private:
    OwnedHandle<VkDescriptorSetLayout> frameLayout;
    OwnedHandle<VkPipelineLayout> pipelineLayout;
};

/*
* A buffer in upload memory that grows to fit what a frame writes into it, for one frame in flight: the CPU writes it
* while the GPU reads that frame's earlier use of it is long finished (the frame's fence). Growing replaces the buffer
* with one twice as big (or as big as needed), so a busy scene settles at its size after a frame or two.
*/
class UploadArray {
public:
    UploadArray(VmaAllocator allocator, VkBufferUsageFlags usage);

    // Replace the contents (growing first if needed) and make them visible to the GPU.
    void write(const void* data, std::size_t bytes);

    template<typename Element>
    void write(const std::vector<Element>& elements) {
        write(elements.data(), elements.size() * sizeof(Element));
    }

    // Replace the contents with first then second, one after the other (a frame's kept still records, then the rest),
    // without joining them into one array first.
    template<typename Element>
    void write(std::span<const Element> first, std::span<const Element> second) {
        writeParts(first.data(), first.size_bytes(), second.data(), second.size_bytes());
    }

    // A frame's draw list: room for all of it (growing first if needed), only its CPU runs written (DrawListLayout), the
    // rest left for the GPU.
    void writeRuns(const DrawListLayout& layout);

    // The whole buffer, for a descriptor (shaders read only as many elements as the frame says there are).
    VkDescriptorBufferInfo describe() const;

    // The buffer's GPU address (made with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT), after the last write: growing moves it.
    VkDeviceAddress getDeviceAddress() const;

private:
    void writeParts(const void* first, std::size_t firstBytes, const void* second, std::size_t secondBytes);

    // Room for a few dozen records of any kind before the first growth.
    static constexpr std::size_t smallestBytes = 4096;

    VmaAllocator allocator = VK_NULL_HANDLE;
    VkBufferUsageFlags usage = 0;
    VulkanBuffer buffer;
};

// One frame in flight's scene data, in set 1's binding order.
struct SceneBuffers {
    explicit SceneBuffers(VmaAllocator allocator);

    UploadArray frameData;
    UploadArray directionalLights;
    UploadArray pointLights;
    UploadArray spotLights;
    UploadArray shadowTiles;
    UploadArray drawList;
    UploadArray probeHitLights;

    // The flat ambient light when there is no atmosphere (with one, the atmosphere's own buffer stands in), and the
    // atmosphere's numbers (GpuAtmosphere; present or not, render/gpu/atmosphere.h).
    UploadArray skyLight;
    UploadArray atmosphere;

    // The pairs of a fast light and a shiny surface the mirrored light search runs over (GpuMirrorPairs; none when off).
    UploadArray mirrorPairs;

    // The glass a ray towards a light may cross, tested before tracing it (GpuGlassBounds; none without glass shadows).
    UploadArray glassBounds;

    // The frame's fog (GpuFog; off without one).
    UploadArray fog;

    // The sky visibility grid's cascades (GpuProbeCascade, render/gpu/sky_visibility.h; none while it is not read).
    UploadArray skyVisibilityCascades;

    // The terrain's patches this frame (GpuTerrainPatch, render/gpu/terrain_gpu.h; one unused without a terrain).
    UploadArray terrainPatches;

    // The wind force field's record (GpuWindField, render/gpu/wind.h; off without wind).
    UploadArray wind;

    // Each placed part's instance in its plant's frame (GpuInstance, render/gpu/placed_drawing.h; one unused without).
    UploadArray placedParts;
};

// The images set 1 points at: the shadow atlas read by comparison and as raw depths, the BRDF table, and the light
// probes' three atlases (or a stand-in while there are none).
struct SceneImages {
    SampledImage shadowAtlas;
    SampledImage shadowDepths;
    SampledImage brdfTable;
    SampledImage probeIrradiance;
    SampledImage probeDistance;
    SampledImage probeFastIrradiance;
    SampledImage skyView; // the atmosphere's sky view table, or a stand-in without one
    SampledImage mirroredLight;     // the light mirrors reflect onto each pixel, and the way it arrives (stand-ins without)
    SampledImage mirroredDirection;
    SampledImage ambientOcclusion;  // the visibility in creases and corners (a stand-in without)
    SampledImage fogVolume;         // the fog integrated through its grid (a clear stand-in without)
    SampledImage skyVisibility;     // the sky visibility grid's points (all "not baked" until it is read)
    SampledImage contactShadows;    // the sun's contact shadows, 1 lit (a stand-in without)
    SampledImage lightContactShadows; // the local lights' contact shadows, a channel each (a stand-in without)
    SampledImage glassTint;         // the coloured shadow maps' tint (a stand-in without; read only for tiles with glass)
    SampledImage glassDepths;       // and their nearest glass depths
    SampledImage windSprings;       // the wind field's springs, this frame's and last frame's (render/gpu/wind.h)
    SampledImage windPreviousSprings;
    SampledImage leafPyramidEarly;  // the occlusion pyramids the mesh shaders test far leaves against (stand-ins without)
    SampledImage leafPyramidWhole;
};

// The buffers set 1 points at that the frame does not write itself: the light probes' states, the flat ambient light
// and the probes' cascades (each a stand-in without probes or an atmosphere).
struct SceneBufferViews {
    // Kept from frame to frame (render/vulkan/vulkan_lasting_buffer.h): every draw's instance, the materials, and each
    // instance's model matrix last frame (column-major, its current one when it did not move) for the velocity pass.
    VkDescriptorBufferInfo instances{};
    VkDescriptorBufferInfo materials{};
    VkDescriptorBufferInfo previousModels{};
    VkDescriptorBufferInfo probeStates{};
    VkDescriptorBufferInfo skyLight{};
    VkDescriptorBufferInfo probeCascades{};
    VkDescriptorBufferInfo terrainHeights{}; // the terrain's heights (a stand-in without a terrain)
    VkDescriptorBufferInfo hangingVertices{}; // every hanging mesh's vertices' anchors and arms (a stand-in without)
    VkDescriptorBufferInfo thinningCards{};   // every thinned mesh's header and cards (a stand-in without; scene/card_thinning.h)
    VkDescriptorBufferInfo placedPlants{};    // the planting cells' slots' plants (a stand-in without; render/gpu/placed_drawing.h)
    VkDescriptorBufferInfo placedEntries{};   // the camera's placed entries (a stand-in without)
    VkDescriptorBufferInfo twigEntries{};     // the camera's twig entries and the twigs (stand-ins without; render/gpu/twig_records.h)
    VkDescriptorBufferInfo twigs{};
    VkDescriptorBufferInfo swayBindings{};    // every pool vertex's binding to its bones (a stand-in without; render/gpu/bone_records.h)
    VkDescriptorBufferInfo twigBindings{};    // each twig's binding, beside the twigs (a stand-in without)
    VkDescriptorBufferInfo boneTrees{};       // the bones pass's trees, each plant's tree and the pool (stand-ins without)
    VkDescriptorBufferInfo bonePlantTrees{};
    VkDescriptorBufferInfo bonePoses{};
    VkDescriptorBufferInfo lianaShapes{};     // the lianas' records, slot by slot (scene/liana_shape.h; one unused without)
    VkDescriptorBufferInfo lianaTreePlants{}; // each liana anchor's tree's plant (render/gpu/liana_trees.h; a stand-in without)
    VkDescriptorBufferInfo farClusters{};     // the far leaf clusters, leaves and cells' shapes (stand-ins without; render/gpu/twig_records.h)
    VkDescriptorBufferInfo farLeaves{};
    VkDescriptorBufferInfo farShapes{};
    VkDescriptorBufferInfo virtualShadowFrame{}; // the sun's virtual shadow maps' record and page table (render/vulkan/vulkan_virtual_shadows.h)
    VkDescriptorBufferInfo virtualShadowTable{};
};

// Write this frame's set 1 (from the frame's pools, so it goes when they are reset).
VkDescriptorSet writeSceneSet(FrameDescriptors& descriptors, const SceneLayout& layout, const SceneBuffers& buffers, const SceneImages& images,
                              const VulkanBuffer& clusterMasks, const SceneBufferViews& views);

// Bind both scene sets. Before a pass's draws: other passes' pipelines (the debug UI's) bind sets of their own. Graphics
// and compute pipelines each have their own bound sets, so the cluster pass binds them at the compute bind point.
void bindScene(VkCommandBuffer commands, const SceneLayout& layout, VkDescriptorSet textures, VkDescriptorSet frameSet,
               VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS);
