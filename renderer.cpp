// Vulkan implementation of gfx::Renderer.  See renderer.h for the contract.
//
// Structure follows ~/trees/cinematic-volume/main.cpp (submissions in flight
// with a fence each, per-swapchain-image framebuffers, destroy-and-recreate
// swapchain on resize), with these differences:
//  - the semaphore signalled for present is per swapchain image rather than
//    per submission, because a present may still be using it after the
//    submission's fence signals (the validation layer reports reuse otherwise);
//  - the acquire semaphore is per submission, so it is known to be unsignalled
//    when its submission's fence has been waited on;
//  - VK_SUBOPTIMAL_KHR from acquire still renders and presents the acquired
//    image (its semaphore is signalled) and recreates afterwards;
//  - zero-sized framebuffers (minimised windows) skip the frame.

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include "renderer.h"
#include "font.h"
#include "vulkan_support.h"

#include "ui_vert_spv.h"
#include "ui_frag_spv.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>

namespace gfx
{

namespace
{

static constexpr uint64_t DEFAULT_FENCE_TIMEOUT = 100000000000;
static constexpr int SUBMISSIONS_IN_FLIGHT = 2;

uint32_t GetMemoryTypeIndex(const VkPhysicalDeviceMemoryProperties& memory_properties, uint32_t type_bits, VkMemoryPropertyFlags properties)
{
    for(uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        if(type_bits & (1u << i)) {
            if((memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
    }
    return UINT32_MAX;
}

// Host-visible, persistently mapped buffer.
struct Buffer
{
    VkDevice device{VK_NULL_HANDLE};
    VkBuffer buf{VK_NULL_HANDLE};
    VkDeviceMemory mem{VK_NULL_HANDLE};
    void* mapped{nullptr};
    VkDeviceSize size{0};
    bool coherent{true};

    void Create(VkDevice device_, const VkPhysicalDeviceMemoryProperties& memory_properties, VkDeviceSize size_, VkBufferUsageFlags usage, bool prefer_cached)
    {
        Release();
        device = device_;
        size = size_;

        VkBufferCreateInfo create {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        CHECK_VK(vkCreateBuffer(device, &create, nullptr, &buf));

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, buf, &req);

        uint32_t type = UINT32_MAX;
        if(prefer_cached) {
            type = GetMemoryTypeIndex(memory_properties, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        }
        if(type == UINT32_MAX) {
            type = GetMemoryTypeIndex(memory_properties, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        if(type == UINT32_MAX) {
            throw std::runtime_error("no host-visible coherent memory type for buffer");
        }
        VkMemoryAllocateInfo allocate {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size,
            .memoryTypeIndex = type,
        };
        CHECK_VK(vkAllocateMemory(device, &allocate, nullptr, &mem));
        CHECK_VK(vkBindBufferMemory(device, buf, mem, 0));
        CHECK_VK(vkMapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, &mapped));
    }

    void Release()
    {
        if(mapped) {
            vkUnmapMemory(device, mem);
            mapped = nullptr;
        }
        if(buf != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, buf, nullptr);
            buf = VK_NULL_HANDLE;
        }
        if(mem != VK_NULL_HANDLE) {
            vkFreeMemory(device, mem, nullptr);
            mem = VK_NULL_HANDLE;
        }
        size = 0;
    }
};

// Per-quad data, one instance each.  Must match the attributes in ui.vert.
struct QuadInstance
{
    float rect[4];      // x, y, w, h in framebuffer pixels
    float params[4];    // radius px, border width px, kind, unused
    float fill[4];
    float border[4];
    float uv[4];        // atlas texels u0, v0, u1, v1
};
static_assert(sizeof(QuadInstance) == 80);

struct PushConstants
{
    float framebuffer_size[2];
    float encode_srgb;
    float pad;
};

enum QuadKind { KIND_SHAPE = 0, KIND_GLYPH = 1 };

struct PixelRect
{
    int x0{0}, y0{0}, x1{0}, y1{0};
};

PixelRect Intersect(const PixelRect& a, const PixelRect& b)
{
    PixelRect r{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    if(r.x1 < r.x0) r.x1 = r.x0;
    if(r.y1 < r.y0) r.y1 = r.y0;
    return r;
}

bool IsSRGBFormat(VkFormat f)
{
    return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

};

struct Renderer::Impl
{
    GLFWwindow* window;
    float design_w, design_h;
    bool validation;

    // Instance, device, queue
    VkInstance instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT messenger{VK_NULL_HANDLE};
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device{VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    uint32_t queue_family{VulkanSupport::NO_QUEUE_FAMILY};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    VkCommandPool command_pool{VK_NULL_HANDLE};

    // Swapchain
    VkSurfaceFormatKHR surface_format{};
    bool encode_srgb{false};
    VkSwapchainKHR swapchain{VK_NULL_HANDLE};
    VkExtent2D swapchain_extent{0, 0};
    int swapchain_fb_w{0}, swapchain_fb_h{0};   // glfw framebuffer size it was made for
    bool swapchain_dirty{false};
    bool swapchain_transfer_src{false};
    struct PerSwapchainImage {
        VkImage image{VK_NULL_HANDLE};
        VkImageView image_view{VK_NULL_HANDLE};
        VkFramebuffer framebuffer{VK_NULL_HANDLE};
        VkSemaphore render_finished{VK_NULL_HANDLE};
    };
    std::vector<PerSwapchainImage> per_swapchainimage;

    // Pipeline
    VkRenderPass render_pass{VK_NULL_HANDLE};
    VkDescriptorSetLayout descriptor_set_layout{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
    VkPipeline pipeline{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool{VK_NULL_HANDLE};
    VkDescriptorSet descriptor_set{VK_NULL_HANDLE};
    VkSampler sampler{VK_NULL_HANDLE};

    // Glyph atlas on the GPU
    VkImage atlas_image{VK_NULL_HANDLE};
    VkDeviceMemory atlas_memory{VK_NULL_HANDLE};
    VkImageView atlas_view{VK_NULL_HANDLE};
    int atlas_gpu_w{0}, atlas_gpu_h{0};
    Buffer atlas_staging;

    // In flight rendering
    struct Submission {
        VkCommandBuffer command_buffer{VK_NULL_HANDLE};
        VkFence draw_completed_fence{VK_NULL_HANDLE};
        bool draw_completed_fence_submitted{false};
        VkSemaphore image_acquired{VK_NULL_HANDLE};
        Buffer instances;
    };
    std::array<Submission, SUBMISSIONS_IN_FLIGHT> submissions;
    int submission_index{0};
    uint32_t swapchain_index{0};
    bool frame_active{false};

    // Frame contents
    struct Batch {
        PixelRect scissor;
        uint32_t first;
        uint32_t count;
    };
    std::vector<QuadInstance> quads;
    std::vector<Batch> batches;
    std::vector<PixelRect> clip_stack;

    // Design space mapping
    float scale{1};
    float origin_x{0}, origin_y{0};
    int target_w{0}, target_h{0};

    // Text
    std::shared_ptr<const FontFile> font_file;
    GlyphAtlas atlas;
    float fonts_scale{0};
    std::map<long, std::unique_ptr<Font>> fonts;   // key: pixel size in 1/64 px

    // Screenshot
    bool capture_requested{false};
    bool capture_ready{false};
    Buffer readback;
    Capture capture;

    Impl(GLFWwindow* window_, const std::string& font_path, float design_w_, float design_h_, bool validation_);
    ~Impl();

    void CreateDeviceAndQueue();
    void ChooseSurfaceFormat();
    void CreateRenderPass();
    void CreatePipeline();
    void CreateDescriptors();
    void CreatePerSubmissionData();
    void WaitForAllDrawsCompleted();
    bool CreateSwapchainData();
    void DestroySwapchainImages(std::vector<PerSwapchainImage>& images);
    bool RecreateSwapchain();
    void UploadAtlas();
    void SubmitOneShot(const std::function<void(VkCommandBuffer)>& record);

    void SetTargetSize(int w, int h);
    void RefreshScaleFromWindow();
    Font& FontFor(float design_size);

    void Emit(const QuadInstance& q);
    void StartBatch();
    PixelRect DesignToPixels(const Rect& r) const;
};

//----------------------------------------------------------------------------

Renderer::Impl::Impl(GLFWwindow* window_, const std::string& font_path, float design_w_, float design_h_, bool validation_) :
    window(window_),
    design_w(design_w_),
    design_h(design_h_),
    validation(validation_)
{
    // Font first: it is the most likely thing to be misconfigured.
    std::string path = font_path;
    if(path.empty()) {
        path = default_font_path();
        if(path.empty()) {
            throw std::runtime_error("no default UI font found on this system; set the `font` key in ~/.pyjuke/config to the path of a TrueType font");
        }
    }
    try {
        font_file = std::make_shared<const FontFile>(path);
    } catch(const std::exception& e) {
        throw std::runtime_error(std::string(e.what()) + "; set the `font` key in ~/.pyjuke/config to the path of a TrueType font");
    }

    CHECK_VK(VulkanSupport::CreateInstance("pyjuke", "pyjuke", validation, false, &instance));
    if(validation) {
        CHECK_VK(VulkanSupport::CreateDebugMessenger(instance, &messenger));
    }
    CHECK_VK(glfwCreateWindowSurface(instance, window, nullptr, &surface));

    CreateDeviceAndQueue();
    CHECK_VK(VulkanSupport::CreateCommandPool(queue_family, device, command_pool));

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    atlas.set_max_dim(static_cast<int>(std::min<uint32_t>(4096, properties.limits.maxImageDimension2D)));

    ChooseSurfaceFormat();
    CreateRenderPass();
    CreateDescriptors();
    CreatePipeline();
    CreatePerSubmissionData();

    RefreshScaleFromWindow();
}

Renderer::Impl::~Impl()
{
    if(device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);

        fonts.clear();

        DestroySwapchainImages(per_swapchainimage);
        if(swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        }

        for(auto& s: submissions) {
            s.instances.Release();
            vkDestroySemaphore(device, s.image_acquired, nullptr);
            vkDestroyFence(device, s.draw_completed_fence, nullptr);
            if(s.command_buffer != VK_NULL_HANDLE) {
                vkFreeCommandBuffers(device, command_pool, 1, &s.command_buffer);
            }
        }
        readback.Release();
        atlas_staging.Release();

        vkDestroyImageView(device, atlas_view, nullptr);
        vkDestroyImage(device, atlas_image, nullptr);
        vkFreeMemory(device, atlas_memory, nullptr);
        vkDestroySampler(device, sampler, nullptr);

        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
        vkDestroyRenderPass(device, render_pass, nullptr);
        vkDestroyCommandPool(device, command_pool, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    if(instance != VK_NULL_HANDLE) {
        if(surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
        }
        VulkanSupport::DestroyDebugMessenger(instance, messenger);
        vkDestroyInstance(instance, nullptr);
    }
}

void Renderer::Impl::CreateDeviceAndQueue()
{
    uint32_t gpu_count = 0;
    CHECK_VK(vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr));
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    CHECK_VK(vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data()));

    // Pick the first device with a graphics queue that can present to our
    // surface, preferring a discrete or integrated GPU over anything else.
    int best_rank = -1;
    for(auto gpu: gpus) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu, &props);
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, families.data());
        for(uint32_t i = 0; i < family_count; i++) {
            VkBool32 present = VK_FALSE;
            CHECK_VK(vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &present));
            if((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                int rank = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 2 :
                           (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) ? 1 : 0;
                if(rank > best_rank) {
                    best_rank = rank;
                    physical_device = gpu;
                    queue_family = i;
                }
                break;
            }
        }
    }
    if(physical_device == VK_NULL_HANDLE) {
        std::cerr << "no Vulkan device can present to this window\n";
        exit(EXIT_FAILURE);
    }
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    CHECK_VK(VulkanSupport::CreateDevice(physical_device, {VK_KHR_SWAPCHAIN_EXTENSION_NAME}, queue_family, device));
    vkGetDeviceQueue(device, queue_family, 0, &queue);
}

void Renderer::Impl::ChooseSurfaceFormat()
{
    uint32_t count = 0;
    CHECK_VK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &count, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(count);
    CHECK_VK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &count, formats.data()));
    if(count == 0) {
        std::cerr << "surface reports no formats\n";
        exit(EXIT_FAILURE);
    }

    // Prefer UNORM: colours are sRGB-encoded values and we blend in that
    // space.  Accept an _SRGB format if that is all there is (the fragment
    // shader then decodes so the stored values match).
    const VkFormat preferred[] = {
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
    };
    for(VkFormat want: preferred) {
        for(const auto& f: formats) {
            if(f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                surface_format = f;
                encode_srgb = IsSRGBFormat(f.format);
                return;
            }
        }
    }
    std::cerr << "warning: none of the preferred swapchain formats is available; colours may be wrong\n";
    surface_format = formats[0];
    encode_srgb = IsSRGBFormat(formats[0].format);
}

void Renderer::Impl::CreateRenderPass()
{
    VkAttachmentDescription color_attachment {
        .flags = 0,
        .format = surface_format.format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference color_reference { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass {
        .flags = 0,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_reference,
    };

    // The layout transition from UNDEFINED waits for the acquire semaphore,
    // which the submit waits on at COLOR_ATTACHMENT_OUTPUT.
    VkSubpassDependency dependency {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
        .dependencyFlags = 0,
    };

    VkRenderPassCreateInfo create {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    CHECK_VK(vkCreateRenderPass(device, &create, nullptr, &render_pass));
}

void Renderer::Impl::CreateDescriptors()
{
    VkDescriptorSetLayoutBinding binding {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = nullptr,
    };
    VkDescriptorSetLayoutCreateInfo layout_create {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    CHECK_VK(vkCreateDescriptorSetLayout(device, &layout_create, nullptr, &descriptor_set_layout));

    VkDescriptorPoolSize pool_size { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pool_create {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    CHECK_VK(vkCreateDescriptorPool(device, &pool_create, nullptr, &descriptor_pool));

    VkDescriptorSetAllocateInfo allocate {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptor_set_layout,
    };
    CHECK_VK(vkAllocateDescriptorSets(device, &allocate, &descriptor_set));

    // Glyph quads map atlas texels 1:1 at whole-pixel positions, so nearest
    // filtering is exact and never blurs.
    VkSamplerCreateInfo sampler_create {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .minLod = 0.0f,
        .maxLod = 0.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    CHECK_VK(vkCreateSampler(device, &sampler_create, nullptr, &sampler));
}

void Renderer::Impl::CreatePipeline()
{
    VkPushConstantRange push_range {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    VkPipelineLayoutCreateInfo layout_create {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    };
    CHECK_VK(vkCreatePipelineLayout(device, &layout_create, nullptr, &pipeline_layout));

    auto make_module = [&](const uint32_t* code, size_t size) {
        VkShaderModuleCreateInfo create {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = size,
            .pCode = code,
        };
        VkShaderModule module;
        CHECK_VK(vkCreateShaderModule(device, &create, nullptr, &module));
        return module;
    };
    VkShaderModule vert = make_module(ui_vert_spv, ui_vert_spv_size);
    VkShaderModule frag = make_module(ui_frag_spv, ui_frag_spv_size);

    VkPipelineShaderStageCreateInfo stages[2] {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag,
            .pName = "main",
        },
    };

    VkVertexInputBindingDescription binding {
        .binding = 0,
        .stride = sizeof(QuadInstance),
        .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
    };
    VkVertexInputAttributeDescription attributes[5] {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(QuadInstance, rect)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(QuadInstance, params)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(QuadInstance, fill)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(QuadInstance, border)},
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(QuadInstance, uv)},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input_state {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(std::size(attributes)),
        .pVertexAttributeDescriptions = attributes,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly_state {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rasterization_state {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multisample_state {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    // Premultiplied-alpha "over".
    VkPipelineColorBlendAttachmentState blend_attachment {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo color_blend_state {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };

    VkDynamicState dynamic_states[] { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<uint32_t>(std::size(dynamic_states)),
        .pDynamicStates = dynamic_states,
    };

    VkGraphicsPipelineCreateInfo create {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input_state,
        .pInputAssemblyState = &input_assembly_state,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization_state,
        .pMultisampleState = &multisample_state,
        .pDepthStencilState = nullptr,
        .pColorBlendState = &color_blend_state,
        .pDynamicState = &dynamic_state,
        .layout = pipeline_layout,
        .renderPass = render_pass,
        .subpass = 0,
    };
    CHECK_VK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline));

    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
}

void Renderer::Impl::CreatePerSubmissionData()
{
    for(auto& submission: submissions) {
        VkCommandBufferAllocateInfo allocate {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        CHECK_VK(vkAllocateCommandBuffers(device, &allocate, &submission.command_buffer));

        VkFenceCreateInfo fence_create {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = 0,
        };
        CHECK_VK(vkCreateFence(device, &fence_create, nullptr, &submission.draw_completed_fence));
        submission.draw_completed_fence_submitted = false;

        VkSemaphoreCreateInfo sema_create {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .flags = 0,
        };
        CHECK_VK(vkCreateSemaphore(device, &sema_create, nullptr, &submission.image_acquired));

        submission.instances.Create(device, memory_properties, 1024 * sizeof(QuadInstance), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
    }
}

void Renderer::Impl::WaitForAllDrawsCompleted()
{
    for(auto& submission: submissions) {
        if(submission.draw_completed_fence_submitted) {
            CHECK_VK(vkWaitForFences(device, 1, &submission.draw_completed_fence, VK_TRUE, DEFAULT_FENCE_TIMEOUT));
            CHECK_VK(vkResetFences(device, 1, &submission.draw_completed_fence));
            submission.draw_completed_fence_submitted = false;
        }
    }
}

void Renderer::Impl::DestroySwapchainImages(std::vector<PerSwapchainImage>& images)
{
    for(auto& per_image: images) {
        vkDestroyFramebuffer(device, per_image.framebuffer, nullptr);
        vkDestroyImageView(device, per_image.image_view, nullptr);
        vkDestroySemaphore(device, per_image.render_finished, nullptr);
    }
    images.clear();
}

// Creates a swapchain for the current surface size, retiring the old one.
// Returns false (leaving no swapchain) if the surface has zero size.
bool Renderer::Impl::CreateSwapchainData()
{
    int fb_w, fb_h;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);

    VkSurfaceCapabilitiesKHR surfcaps;
    CHECK_VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &surfcaps));

    VkExtent2D extent = surfcaps.currentExtent;
    if(extent.width == UINT32_MAX) {
        // The surface size is determined by the swapchain (e.g. Wayland).
        extent.width = std::clamp(static_cast<uint32_t>(std::max(fb_w, 0)), surfcaps.minImageExtent.width, surfcaps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(std::max(fb_h, 0)), surfcaps.minImageExtent.height, surfcaps.maxImageExtent.height);
    }

    VkSwapchainKHR old_swapchain = swapchain;
    std::vector<PerSwapchainImage> old_images;
    old_images.swap(per_swapchainimage);

    if(extent.width == 0 || extent.height == 0 || fb_w == 0 || fb_h == 0) {
        DestroySwapchainImages(old_images);
        if(old_swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, old_swapchain, nullptr);
        }
        swapchain = VK_NULL_HANDLE;
        swapchain_extent = {0, 0};
        return false;
    }

    uint32_t min_image_count = std::max<uint32_t>(surfcaps.minImageCount, 3);
    if(surfcaps.maxImageCount != 0) {
        min_image_count = std::min(min_image_count, surfcaps.maxImageCount);
    }

    swapchain_transfer_src = (surfcaps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if(swapchain_transfer_src) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if(!(surfcaps.supportedCompositeAlpha & composite_alpha)) {
        for(VkCompositeAlphaFlagBitsKHR bit: {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
            if(surfcaps.supportedCompositeAlpha & bit) {
                composite_alpha = bit;
                break;
            }
        }
    }

    // FIFO is the only present mode guaranteed to exist, and it is what an
    // on-demand redraw wants anyway.
    VkSwapchainCreateInfoKHR create {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = min_image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = usage,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .preTransform = surfcaps.currentTransform,
        .compositeAlpha = composite_alpha,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = old_swapchain,
    };
    CHECK_VK(vkCreateSwapchainKHR(device, &create, nullptr, &swapchain));

    DestroySwapchainImages(old_images);
    if(old_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, old_swapchain, nullptr);
    }

    swapchain_extent = extent;
    swapchain_fb_w = fb_w;
    swapchain_fb_h = fb_h;

    uint32_t image_count = 0;
    CHECK_VK(vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr));
    std::vector<VkImage> images(image_count);
    CHECK_VK(vkGetSwapchainImagesKHR(device, swapchain, &image_count, images.data()));

    per_swapchainimage.resize(image_count);
    for(uint32_t i = 0; i < image_count; i++) {
        auto& per_image = per_swapchainimage[i];
        per_image.image = images[i];

        VkImageViewCreateInfo view_create {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = surface_format.format,
            .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        CHECK_VK(vkCreateImageView(device, &view_create, nullptr, &per_image.image_view));

        VkFramebufferCreateInfo fb_create {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = render_pass,
            .attachmentCount = 1,
            .pAttachments = &per_image.image_view,
            .width = extent.width,
            .height = extent.height,
            .layers = 1,
        };
        CHECK_VK(vkCreateFramebuffer(device, &fb_create, nullptr, &per_image.framebuffer));

        VkSemaphoreCreateInfo sema_create {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .flags = 0,
        };
        CHECK_VK(vkCreateSemaphore(device, &sema_create, nullptr, &per_image.render_finished));
    }
    return true;
}

bool Renderer::Impl::RecreateSwapchain()
{
    // Everything that might reference the old images (command buffers,
    // present semaphores) must be finished before they are destroyed.
    CHECK_VK(vkDeviceWaitIdle(device));
    WaitForAllDrawsCompleted();
    swapchain_dirty = false;
    return CreateSwapchainData();
}

void Renderer::Impl::SubmitOneShot(const std::function<void(VkCommandBuffer)>& record)
{
    VkCommandBufferAllocateInfo allocate {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cb;
    CHECK_VK(vkAllocateCommandBuffers(device, &allocate, &cb));
    VkCommandBufferBeginInfo begin {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK_VK(vkBeginCommandBuffer(cb, &begin));
    record(cb);
    CHECK_VK(vkEndCommandBuffer(cb));

    VkFenceCreateInfo fence_create { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK_VK(vkCreateFence(device, &fence_create, nullptr, &fence));
    VkSubmitInfo submit {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    CHECK_VK(vkQueueSubmit(queue, 1, &submit, fence));
    CHECK_VK(vkWaitForFences(device, 1, &fence, VK_TRUE, DEFAULT_FENCE_TIMEOUT));
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, command_pool, 1, &cb);
}

// Copy the whole CPU atlas to the GPU, recreating the image if its size
// changed.  Rare (new glyphs or a rescale), so it simply waits for the GPU to
// stop using the old contents.
void Renderer::Impl::UploadAtlas()
{
    CHECK_VK(vkQueueWaitIdle(queue));

    int w = atlas.width();
    int h = atlas.height();

    if(w != atlas_gpu_w || h != atlas_gpu_h) {
        vkDestroyImageView(device, atlas_view, nullptr);
        vkDestroyImage(device, atlas_image, nullptr);
        vkFreeMemory(device, atlas_memory, nullptr);

        VkImageCreateInfo image_create {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8_UNORM,
            .extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        CHECK_VK(vkCreateImage(device, &image_create, nullptr, &atlas_image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, atlas_image, &req);
        uint32_t type = GetMemoryTypeIndex(memory_properties, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if(type == UINT32_MAX) {
            type = GetMemoryTypeIndex(memory_properties, req.memoryTypeBits, 0);
        }
        VkMemoryAllocateInfo allocate {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size,
            .memoryTypeIndex = type,
        };
        CHECK_VK(vkAllocateMemory(device, &allocate, nullptr, &atlas_memory));
        CHECK_VK(vkBindImageMemory(device, atlas_image, atlas_memory, 0));

        VkImageViewCreateInfo view_create {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = atlas_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8_UNORM,
            .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        CHECK_VK(vkCreateImageView(device, &view_create, nullptr, &atlas_view));

        VkDescriptorImageInfo image_info {
            .sampler = sampler,
            .imageView = atlas_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet write {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descriptor_set,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_info,
        };
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

        atlas_gpu_w = w;
        atlas_gpu_h = h;
    }

    VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h;
    if(atlas_staging.size < bytes) {
        atlas_staging.Create(device, memory_properties, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
    }
    memcpy(atlas_staging.mapped, atlas.pixels().data(), bytes);

    SubmitOneShot([&](VkCommandBuffer cb) {
        // Whole image is overwritten, so the old contents can be discarded.
        VkImageMemoryBarrier to_transfer {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = atlas_image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_transfer);

        VkBufferImageCopy copy {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1},
        };
        vkCmdCopyBufferToImage(cb, atlas_staging.buf, atlas_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        VkImageMemoryBarrier to_shader {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = atlas_image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_shader);
    });

    atlas.mark_clean();
}

//----------------------------------------------------------------------------
// Design space and text

void Renderer::Impl::SetTargetSize(int w, int h)
{
    if(w <= 0 || h <= 0) {
        return;     // keep the previous mapping while minimised
    }
    target_w = w;
    target_h = h;
    scale = std::min(w / design_w, h / design_h);
    // Whole-pixel origin so integer design coordinates at integer scales
    // land on pixel boundaries.
    origin_x = std::round((w - design_w * scale) * 0.5f);
    origin_y = std::round((h - design_h * scale) * 0.5f);

    if(scale != fonts_scale) {
        // Re-bake every size at the new scale.  Only the CPU atlas changes
        // here; the GPU copy is replaced in end_frame() after the GPU is idle.
        fonts.clear();
        atlas.clear();
        fonts_scale = scale;
    }
}

void Renderer::Impl::RefreshScaleFromWindow()
{
    if(frame_active) {
        return;     // never invalidate glyphs already emitted this frame
    }
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    SetTargetSize(w, h);
}

Font& Renderer::Impl::FontFor(float design_size)
{
    float px = std::max(1.0f, design_size * scale);
    long key = std::lround(px * 64.0f);
    auto found = fonts.find(key);
    if(found != fonts.end()) {
        return *found->second;
    }
    auto font = std::make_unique<Font>(font_file, static_cast<float>(key) / 64.0f, atlas);
    Font& ref = *font;
    fonts.emplace(key, std::move(font));
    return ref;
}

PixelRect Renderer::Impl::DesignToPixels(const Rect& r) const
{
    float x0 = origin_x + r.x * scale;
    float y0 = origin_y + r.y * scale;
    float x1 = origin_x + (r.x + r.w) * scale;
    float y1 = origin_y + (r.y + r.h) * scale;
    return PixelRect{
        static_cast<int>(std::floor(x0)), static_cast<int>(std::floor(y0)),
        static_cast<int>(std::ceil(x1)), static_cast<int>(std::ceil(y1))};
}

void Renderer::Impl::StartBatch()
{
    const PixelRect& clip = clip_stack.back();
    if(!batches.empty() && batches.back().count == 0) {
        batches.back().scissor = clip;
        return;
    }
    batches.push_back(Batch{clip, static_cast<uint32_t>(quads.size()), 0});
}

void Renderer::Impl::Emit(const QuadInstance& q)
{
    quads.push_back(q);
    batches.back().count++;
}

//----------------------------------------------------------------------------
// Renderer

Renderer::Renderer(GLFWwindow* window, const std::string& font_path, float design_w, float design_h, bool validation) :
    impl_(std::make_unique<Impl>(window, font_path, design_w, design_h, validation))
{
}

Renderer::~Renderer() = default;

bool Renderer::begin_frame(Color clear)
{
    Impl& m = *impl_;
    assert(!m.frame_active);

    int fb_w, fb_h;
    glfwGetFramebufferSize(m.window, &fb_w, &fb_h);
    if(fb_w == 0 || fb_h == 0) {
        return false;
    }

    if(m.swapchain == VK_NULL_HANDLE || m.swapchain_dirty || fb_w != m.swapchain_fb_w || fb_h != m.swapchain_fb_h) {
        if(!m.RecreateSwapchain()) {
            return false;
        }
    }

    auto& submission = m.submissions[m.submission_index];
    if(submission.draw_completed_fence_submitted) {
        CHECK_VK(vkWaitForFences(m.device, 1, &submission.draw_completed_fence, VK_TRUE, DEFAULT_FENCE_TIMEOUT));
        CHECK_VK(vkResetFences(m.device, 1, &submission.draw_completed_fence));
        submission.draw_completed_fence_submitted = false;
    }

    VkResult result = VK_ERROR_OUT_OF_DATE_KHR;
    for(int attempt = 0; attempt < 2; attempt++) {
        result = vkAcquireNextImageKHR(m.device, m.swapchain, UINT64_MAX, submission.image_acquired, VK_NULL_HANDLE, &m.swapchain_index);
        if(result != VK_ERROR_OUT_OF_DATE_KHR) {
            break;
        }
        // Semaphore was not signalled; rebuild and try again.
        if(!m.RecreateSwapchain()) {
            return false;
        }
    }
    if(result == VK_SUBOPTIMAL_KHR) {
        // The image was acquired and the semaphore will be signalled, so use
        // it; rebuild at the next opportunity.
        m.swapchain_dirty = true;
    } else if(result == VK_ERROR_OUT_OF_DATE_KHR) {
        m.swapchain_dirty = true;
        return false;
    } else {
        CHECK_VK(result);
    }

    m.SetTargetSize(static_cast<int>(m.swapchain_extent.width), static_cast<int>(m.swapchain_extent.height));

    m.frame_active = true;
    m.quads.clear();
    m.batches.clear();
    m.clip_stack.clear();
    m.clip_stack.push_back(m.DesignToPixels(Rect{0, 0, m.design_w, m.design_h}));
    m.StartBatch();

    rounded_rect(Rect{0, 0, m.design_w, m.design_h}, 0, clear);
    return true;
}

void Renderer::rounded_rect(Rect r, float radius, Color fill, float border_width, Color border)
{
    Impl& m = *impl_;
    assert(m.frame_active);
    if(r.w <= 0 || r.h <= 0) {
        return;
    }
    if(border_width <= 0 && fill.a <= 0) {
        return;
    }
    QuadInstance q {
        .rect = {m.origin_x + r.x * m.scale, m.origin_y + r.y * m.scale, r.w * m.scale, r.h * m.scale},
        .params = {std::max(0.0f, radius) * m.scale, std::max(0.0f, border_width) * m.scale, static_cast<float>(KIND_SHAPE), 0},
        .fill = {fill.r, fill.g, fill.b, fill.a},
        .border = {border.r, border.g, border.b, border.a},
        .uv = {0, 0, 0, 0},
    };
    m.Emit(q);
}

void Renderer::text(float x, float y, std::string_view utf8, float size, Color c)
{
    Impl& m = *impl_;
    assert(m.frame_active);
    Font& font = m.FontFor(size);
    float pen_x = std::round(m.origin_x + x * m.scale);
    float baseline = std::round(m.origin_y + y * m.scale + font.ascent());
    font.layout(utf8, [&](const Glyph& g, int pen) {
        if(g.region.w == 0 || g.region.h == 0) {
            return;
        }
        float gx = pen_x + static_cast<float>(pen + g.bearing_x);
        float gy = baseline - static_cast<float>(g.bearing_y);
        float u0 = static_cast<float>(g.region.x);
        float v0 = static_cast<float>(g.region.y);
        QuadInstance q {
            .rect = {gx, gy, static_cast<float>(g.region.w), static_cast<float>(g.region.h)},
            .params = {0, 0, static_cast<float>(KIND_GLYPH), 0},
            .fill = {c.r, c.g, c.b, c.a},
            .border = {0, 0, 0, 0},
            .uv = {u0, v0, u0 + g.region.w, v0 + g.region.h},
        };
        m.Emit(q);
    });
}

float Renderer::text_width(std::string_view utf8, float size)
{
    impl_->RefreshScaleFromWindow();
    return static_cast<float>(impl_->FontFor(size).text_width(utf8)) / impl_->scale;
}

float Renderer::line_height(float size)
{
    impl_->RefreshScaleFromWindow();
    return impl_->FontFor(size).line_height() / impl_->scale;
}

float Renderer::ascent(float size)
{
    impl_->RefreshScaleFromWindow();
    return impl_->FontFor(size).ascent() / impl_->scale;
}

float Renderer::descent(float size)
{
    impl_->RefreshScaleFromWindow();
    return impl_->FontFor(size).descent() / impl_->scale;
}

void Renderer::push_clip(Rect r)
{
    Impl& m = *impl_;
    assert(m.frame_active);
    m.clip_stack.push_back(Intersect(m.clip_stack.back(), m.DesignToPixels(r)));
    m.StartBatch();
}

void Renderer::pop_clip()
{
    Impl& m = *impl_;
    assert(m.frame_active);
    if(m.clip_stack.size() <= 1) {
        std::cerr << "Renderer::pop_clip without matching push_clip\n";
        return;
    }
    m.clip_stack.pop_back();
    m.StartBatch();
}

bool Renderer::end_frame()
{
    Impl& m = *impl_;
    assert(m.frame_active);
    m.frame_active = false;

    if(m.clip_stack.size() != 1) {
        std::cerr << "Renderer::end_frame: " << (m.clip_stack.size() - 1) << " push_clip() without pop_clip()\n";
    }

    if(m.atlas.dirty()) {
        m.UploadAtlas();
    }

    auto& submission = m.submissions[m.submission_index];
    auto& per_image = m.per_swapchainimage[m.swapchain_index];

    // This submission's previous use finished in begin_frame(), so its
    // instance buffer can be rewritten or replaced.
    VkDeviceSize bytes = m.quads.size() * sizeof(QuadInstance);
    if(submission.instances.size < bytes) {
        VkDeviceSize size = submission.instances.size;
        while(size < bytes) {
            size *= 2;
        }
        submission.instances.Create(m.device, m.memory_properties, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
    }
    memcpy(submission.instances.mapped, m.quads.data(), bytes);

    bool capture = m.capture_requested && m.swapchain_transfer_src;
    m.capture_requested = false;
    VkExtent2D extent = m.swapchain_extent;
    VkDeviceSize capture_bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
    if(capture && m.readback.size < capture_bytes) {
        m.readback.Create(m.device, m.memory_properties, capture_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    }

    auto cb = submission.command_buffer;
    VkCommandBufferBeginInfo begin {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK_VK(vkResetCommandBuffer(cb, 0));
    CHECK_VK(vkBeginCommandBuffer(cb, &begin));

    const VkClearValue clear_value {.color {.float32 {0.0f, 0.0f, 0.0f, 1.0f}}};
    VkRenderPassBeginInfo begin_render_pass {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = m.render_pass,
        .framebuffer = per_image.framebuffer,
        .renderArea = {{0, 0}, extent},
        .clearValueCount = 1,
        .pClearValues = &clear_value,
    };
    vkCmdBeginRenderPass(cb, &begin_render_pass, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m.pipeline_layout, 0, 1, &m.descriptor_set, 0, nullptr);

    VkViewport viewport {
        .x = 0, .y = 0,
        .width = static_cast<float>(extent.width),
        .height = static_cast<float>(extent.height),
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    vkCmdSetViewport(cb, 0, 1, &viewport);

    PushConstants push {
        .framebuffer_size = {static_cast<float>(extent.width), static_cast<float>(extent.height)},
        .encode_srgb = m.encode_srgb ? 1.0f : 0.0f,
        .pad = 0,
    };
    vkCmdPushConstants(cb, m.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &submission.instances.buf, &offset);

    const PixelRect framebuffer_rect{0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height)};
    for(const auto& batch: m.batches) {
        if(batch.count == 0) {
            continue;
        }
        PixelRect s = Intersect(batch.scissor, framebuffer_rect);
        if(s.x1 <= s.x0 || s.y1 <= s.y0) {
            continue;
        }
        VkRect2D scissor {
            .offset = {s.x0, s.y0},
            .extent = {static_cast<uint32_t>(s.x1 - s.x0), static_cast<uint32_t>(s.y1 - s.y0)},
        };
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdDraw(cb, 6, batch.count, 0, batch.first);
    }

    vkCmdEndRenderPass(cb);

    if(capture) {
        VkImageMemoryBarrier to_transfer {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = per_image.image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_transfer);

        VkBufferImageCopy copy {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent.width, extent.height, 1},
        };
        vkCmdCopyImageToBuffer(cb, per_image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m.readback.buf, 1, &copy);

        VkImageMemoryBarrier to_present {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = per_image.image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        VkBufferMemoryBarrier to_host {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = m.readback.buf,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &to_host, 1, &to_present);
    }

    CHECK_VK(vkEndCommandBuffer(cb));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &submission.image_acquired,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &per_image.render_finished,
    };
    CHECK_VK(vkQueueSubmit(m.queue, 1, &submit, submission.draw_completed_fence));
    submission.draw_completed_fence_submitted = true;

    VkPresentInfoKHR present {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &per_image.render_finished,
        .swapchainCount = 1,
        .pSwapchains = &m.swapchain,
        .pImageIndices = &m.swapchain_index,
        .pResults = nullptr,
    };
    VkResult result = vkQueuePresentKHR(m.queue, &present);

    if(capture) {
        // Read back before any swapchain recreation resets the fence state.
        if(submission.draw_completed_fence_submitted) {
            CHECK_VK(vkWaitForFences(m.device, 1, &submission.draw_completed_fence, VK_TRUE, DEFAULT_FENCE_TIMEOUT));
        }
        const uint8_t* src = static_cast<const uint8_t*>(m.readback.mapped);
        bool bgra = m.surface_format.format == VK_FORMAT_B8G8R8A8_UNORM || m.surface_format.format == VK_FORMAT_B8G8R8A8_SRGB;
        m.capture.width = static_cast<int>(extent.width);
        m.capture.height = static_cast<int>(extent.height);
        size_t pixels = static_cast<size_t>(extent.width) * extent.height;
        m.capture.rgb.resize(pixels * 3);
        for(size_t i = 0; i < pixels; i++) {
            const uint8_t* p = src + i * 4;
            m.capture.rgb[i * 3 + 0] = bgra ? p[2] : p[0];
            m.capture.rgb[i * 3 + 1] = p[1];
            m.capture.rgb[i * 3 + 2] = bgra ? p[0] : p[2];
        }
        m.capture_ready = true;
    }

    m.submission_index = (m.submission_index + 1) % SUBMISSIONS_IN_FLIGHT;

    if(result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        m.RecreateSwapchain();
        return false;
    }
    CHECK_VK(result);
    return !m.swapchain_dirty;
}

void Renderer::window_to_design(double wx, double wy, float& dx, float& dy) const
{
    const Impl& m = *impl_;
    int win_w, win_h, fb_w, fb_h;
    glfwGetWindowSize(m.window, &win_w, &win_h);
    glfwGetFramebufferSize(m.window, &fb_w, &fb_h);
    double px = (win_w > 0) ? wx * fb_w / win_w : wx;
    double py = (win_h > 0) ? wy * fb_h / win_h : wy;
    dx = static_cast<float>((px - m.origin_x) / m.scale);
    dy = static_cast<float>((py - m.origin_y) / m.scale);
}

float Renderer::scale() const
{
    return impl_->scale;
}

void Renderer::design_origin(float& x, float& y) const
{
    x = impl_->origin_x;
    y = impl_->origin_y;
}

bool Renderer::capture_next_frame()
{
    if(impl_->swapchain != VK_NULL_HANDLE && !impl_->swapchain_transfer_src) {
        return false;
    }
    impl_->capture_requested = true;
    return true;
}

bool Renderer::take_capture(Capture& out)
{
    if(!impl_->capture_ready) {
        return false;
    }
    out = std::move(impl_->capture);
    impl_->capture = Capture{};
    impl_->capture_ready = false;
    return true;
}

};
