// Vulkan 1.1, loaded at runtime through volk: a machine with no Vulkan driver starts
// fine and falls back to OpenGL. On macOS this runs on MoltenVK, which implements
// Vulkan over Metal. Render passes for the shadow map (depth only), the scene
// (floating point colour and depth), the floating-point and 8-bit post targets and
// the window; the scene pipelines share one layout and the post pipelines another
// whose sets 1 to 3 are a step's inputs; frames in flight.
// The viewport has a negative height (core in 1.1), which gives the y-up clip space
// the backend contract asks for.
//
// The SPIR-V is embedded (src/render/shaders.h): either compiled by the build
// from the shader sources when glslc is installed, or the table committed in
// shaders/spirv. That carries the hash of the sources it was built from, and the
// backend declines to start when it is not the hash of the sources this build
// embeds, rather than bind stale shaders to the wrong layouts.
#include "backend.h"
#include "shaders.h"
#include <volk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_HDR_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT

#define VK_FRAMES_IN_FLIGHT 2
#define VK_MAX_SWAPCHAIN_IMAGES 8
#define VK_TIMED_PASSES (3 + R_MAX_POST_STEPS)   // shadow, main, sky, then each post step: two timestamps each
#define VK_MAX_INSTANCE_EXTENSIONS 8

struct vk_buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
};

struct vk_mesh {
    vk_buffer vertices, indices;
    uint32_t index_count;
};

struct vk_frame_slot {
    VkCommandBuffer commands;
    VkFence in_flight;
    VkSemaphore image_acquired;
    vk_buffer frame_uniforms, light_uniforms, post_uniforms, material_uniforms;
    void *frame_mapped, *lights_mapped, *post_mapped, *materials_mapped;
    VkDescriptorSet descriptors;
    VkQueryPool timestamps;                      // 2 * VK_TIMED_PASSES, written along the command buffer
    int timed_pass[VK_TIMED_PASSES];             // r_pass of each pair, -1 when unused
    bool timed;                                  // the slot's last command buffer wrote timestamps
};

// One of backend.h's render targets: an image, drawn through its framebuffer and
// sampled through its descriptor set. The window target has only the extent; its
// framebuffers are the swapchain's.
struct vk_target {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkFramebuffer framebuffer;
    VkFramebuffer post_framebuffer;  // the scene target's for a post step: its colour alone, under the post pass
    VkDescriptorSet input;
    VkExtent2D extent;
};

struct vk_state {
    plat_window *window;
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout set_layout, input_layout;
    VkPipelineLayout pipeline_layout;            // the scene pipelines': set 0
    VkPipelineLayout post_layout;                // the post pipelines': set 0 and three inputs
    VkRenderPass render_pass, shadow_render_pass;    // the scene's, the shadow map's
    VkRenderPass hdr_pass, ldr_pass, window_pass;    // a post step's, by its target
    r_shader_set shaders;
    VkPipeline lit_pipelines[R_MAX_SHADERS][2];      // the lit pass per material shader; [1] culls nothing, for R_DRAW_TWO_SIDED
    VkPipeline cutout_pipelines[R_MAX_SHADERS][2];   // the shadow pass of a cutout material shader; null for the rest
    VkPipeline shadow_pipelines[2];                  // the shadow pass of everything else
    VkPipeline sky_pipeline;
    VkPipeline post_pipelines[R_POST_PROGRAM_COUNT];
    VkSampler target_sampler;
    VkSampler depth_sampler;                         // the depth target's: unfiltered
    vk_target targets[R_TARGET_COUNT];
    VkFormat color_format;

    // The shadow map: a depth array with a layer per cascade, drawn through a
    // framebuffer per layer and sampled as one array.
    VkImage shadow_image;
    VkDeviceMemory shadow_memory;
    VkImageView shadow_layer_views[R_SHADOW_CASCADES];
    VkImageView shadow_array_view;
    VkFramebuffer shadow_framebuffers[R_SHADOW_CASCADES];
    VkSampler shadow_sampler;
    uint32_t shadow_resolution;                  // what the map was made at

    VkSwapchainKHR swapchain;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage images[VK_MAX_SWAPCHAIN_IMAGES];
    VkImageView views[VK_MAX_SWAPCHAIN_IMAGES];
    VkFramebuffer framebuffers[VK_MAX_SWAPCHAIN_IMAGES];
    VkSemaphore render_finished[VK_MAX_SWAPCHAIN_IMAGES];
    VkImage depth_image;
    VkDeviceMemory depth_memory;
    VkImageView depth_view;

    vk_frame_slot frames[VK_FRAMES_IN_FLIGHT];
    uint32_t frame;
    float timestamp_period;                      // nanoseconds per tick; 0 when the queue cannot time
    r_timings timings;
    vk_mesh meshes[R_MAX_MESHES];     // slot 0 stays empty: mesh 0 means none
    vk_buffer capture;
    VkDeviceSize capture_size;
};

static bool vk_ok(VkResult result, const char *what) {
    if (result == VK_SUCCESS) return true;
    fprintf(stderr, "vulkan: %s failed: %d\n", what, (int)result);
    return false;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                               VkDebugUtilsMessageTypeFlagsEXT,
                                               const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) fprintf(stderr, "vulkan: %s\n", data->pMessage);
    return VK_FALSE;
}

static uint32_t find_memory(vk_state *s, uint32_t type_bits, VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(s->physical, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    return UINT32_MAX;
}

static bool buffer_create(vk_state *s, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                          vk_buffer *out) {
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!vk_ok(vkCreateBuffer(s->device, &info, NULL, &out->buffer), "vkCreateBuffer")) return false;
    VkMemoryRequirements needs;
    vkGetBufferMemoryRequirements(s->device, out->buffer, &needs);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = needs.size;
    alloc.memoryTypeIndex = find_memory(s, needs.memoryTypeBits, properties);
    if (alloc.memoryTypeIndex == UINT32_MAX) return false;
    if (!vk_ok(vkAllocateMemory(s->device, &alloc, NULL, &out->memory), "vkAllocateMemory")) return false;
    return vk_ok(vkBindBufferMemory(s->device, out->buffer, out->memory, 0), "vkBindBufferMemory");
}

static void buffer_destroy(vk_state *s, vk_buffer *b) {
    if (b->buffer) vkDestroyBuffer(s->device, b->buffer, NULL);
    if (b->memory) vkFreeMemory(s->device, b->memory, NULL);
    memset(b, 0, sizeof(*b));
}

static VkCommandBuffer one_shot_begin(vk_state *s) {
    VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = s->command_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer commands;
    vkAllocateCommandBuffers(s->device, &alloc, &commands);
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commands, &begin);
    return commands;
}

static void one_shot_end(vk_state *s, VkCommandBuffer commands) {
    vkEndCommandBuffer(commands);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    vkQueueSubmit(s->queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(s->queue);
    vkFreeCommandBuffers(s->device, s->command_pool, 1, &commands);
}

// Device-local buffer filled through a throwaway staging buffer.
static bool buffer_upload(vk_state *s, const void *data, VkDeviceSize size, VkBufferUsageFlags usage, vk_buffer *out) {
    vk_buffer staging = {};
    if (!buffer_create(s, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging)) return false;
    void *mapped;
    vkMapMemory(s->device, staging.memory, 0, size, 0, &mapped);
    memcpy(mapped, data, (size_t)size);
    vkUnmapMemory(s->device, staging.memory);
    bool ok = buffer_create(s, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out);
    if (ok) {
        VkCommandBuffer commands = one_shot_begin(s);
        VkBufferCopy region = {0, 0, size};
        vkCmdCopyBuffer(commands, staging.buffer, out->buffer, 1, &region);
        one_shot_end(s, commands);
    }
    buffer_destroy(s, &staging);
    return ok;
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------
static bool create_instance(vk_state *s) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Treasure Planet";
    app.apiVersion = VK_API_VERSION_1_1;

    uint32_t required = 0;
    const char **required_names = plat_vk_instance_extensions(&required);
    if (!required_names || required > VK_MAX_INSTANCE_EXTENSIONS - 2) {
        fprintf(stderr, "vulkan: this window system cannot present through Vulkan\n");
        return false;
    }
    const char *extensions[VK_MAX_INSTANCE_EXTENSIONS];
    uint32_t extension_count = required;
    memcpy(extensions, required_names, required * sizeof(const char *));

    // MoltenVK is a "portability" implementation: the loader hides those unless asked.
    VkInstanceCreateFlags instance_flags = 0;
    uint32_t available_extensions = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &available_extensions, NULL);
    VkExtensionProperties *extension_properties = (VkExtensionProperties *)malloc(available_extensions * sizeof(VkExtensionProperties) + 1);
    vkEnumerateInstanceExtensionProperties(NULL, &available_extensions, extension_properties);
    for (uint32_t i = 0; i < available_extensions; ++i)
        if (strcmp(extension_properties[i].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
            extensions[extension_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
            instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
    free(extension_properties);

    const char *layers[1] = {"VK_LAYER_KHRONOS_validation"};
    uint32_t layer_count = 0;
#ifndef NDEBUG
    // Validation comes with the Vulkan SDK: used when installed, silently absent otherwise.
    uint32_t available = 0;
    vkEnumerateInstanceLayerProperties(&available, NULL);
    VkLayerProperties *properties = (VkLayerProperties *)malloc(available * sizeof(VkLayerProperties) + 1);
    vkEnumerateInstanceLayerProperties(&available, properties);
    for (uint32_t i = 0; i < available; ++i)
        if (strcmp(properties[i].layerName, layers[0]) == 0) layer_count = 1;
    free(properties);
    if (layer_count) extensions[extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
#endif
    fprintf(stderr, "vulkan: validation layers %s\n", layer_count ? "on" : "off");

    VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.flags = instance_flags;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = extension_count;
    info.ppEnabledExtensionNames = extensions;
    info.enabledLayerCount = layer_count;
    info.ppEnabledLayerNames = layers;
    if (!vk_ok(vkCreateInstance(&info, NULL, &s->instance), "vkCreateInstance")) return false;
    volkLoadInstance(s->instance);

    if (layer_count) {
        VkDebugUtilsMessengerCreateInfoEXT debug = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                          | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback = vk_debug;
        vkCreateDebugUtilsMessengerEXT(s->instance, &debug, NULL, &s->messenger);
    }
    return true;
}

static bool pick_device(vk_state *s) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(s->instance, &count, NULL);
    if (!count) return false;
    VkPhysicalDevice *devices = (VkPhysicalDevice *)malloc(count * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(s->instance, &count, devices);
    int best_score = -1;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        VkFormatProperties depth;
        vkGetPhysicalDeviceFormatProperties(devices[i], VK_FORMAT_D32_SFLOAT, &depth);
        if (properties.apiVersion < VK_API_VERSION_1_1
            || !(depth.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) continue;
        // One queue family has to do both graphics and present.
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count, NULL);
        VkQueueFamilyProperties *families = (VkQueueFamilyProperties *)malloc(family_count * sizeof(VkQueueFamilyProperties));
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count, families);
        for (uint32_t f = 0; f < family_count; ++f) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], f, s->surface, &present);
            if (!present || !(families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
                      : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
            if (score > best_score) {
                best_score = score;
                s->physical = devices[i];
                s->queue_family = f;
            }
            break;
        }
        free(families);
    }
    free(devices);
    if (best_score < 0) return false;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = s->queue_family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    // A portability implementation requires its subset extension to be enabled when offered.
    const char *extensions[2] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    uint32_t extension_count = 1, offered = 0;
    vkEnumerateDeviceExtensionProperties(s->physical, NULL, &offered, NULL);
    VkExtensionProperties *offered_extensions = (VkExtensionProperties *)malloc(offered * sizeof(VkExtensionProperties) + 1);
    vkEnumerateDeviceExtensionProperties(s->physical, NULL, &offered, offered_extensions);
    for (uint32_t i = 0; i < offered; ++i)
        if (strcmp(offered_extensions[i].extensionName, "VK_KHR_portability_subset") == 0)
            extensions[extension_count++] = "VK_KHR_portability_subset";
    free(offered_extensions);

    VkDeviceCreateInfo info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = extension_count;
    info.ppEnabledExtensionNames = extensions;
    if (!vk_ok(vkCreateDevice(s->physical, &info, NULL, &s->device), "vkCreateDevice")) return false;
    volkLoadDevice(s->device);
    vkGetDeviceQueue(s->device, s->queue_family, 0, &s->queue);
    {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(s->physical, &properties);
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(s->physical, &family_count, NULL);
        VkQueueFamilyProperties *families = (VkQueueFamilyProperties *)malloc(family_count * sizeof(VkQueueFamilyProperties) + 1);
        vkGetPhysicalDeviceQueueFamilyProperties(s->physical, &family_count, families);
        bool can_time = s->queue_family < family_count && families[s->queue_family].timestampValidBits > 0;
        free(families);
        s->timestamp_period = can_time ? properties.limits.timestampPeriod : 0.0f;
    }

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->physical, s->surface, &format_count, NULL);
    if (!format_count) return false;
    VkSurfaceFormatKHR *formats = (VkSurfaceFormatKHR *)malloc(format_count * sizeof(VkSurfaceFormatKHR));
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->physical, s->surface, &format_count, formats);
    s->color_format = VK_FORMAT_UNDEFINED;
    for (uint32_t i = 0; i < format_count; ++i)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            s->color_format = formats[i].format;
            break;
        }
    free(formats);
    if (s->color_format == VK_FORMAT_UNDEFINED) {
        fprintf(stderr, "vulkan: no 8-bit non-sRGB surface format\n");
        return false;
    }
    return true;
}

static VkShaderModule create_module(vk_state *s, const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    vk_ok(vkCreateShaderModule(s->device, &info, NULL, &module), "vkCreateShaderModule");
    return module;
}

// A post target's render pass: one colour attachment, written whole (no clear),
// then left for the fragment shader; ordered after the previous reads of it.
static bool create_color_pass(vk_state *s, VkFormat format, VkImageLayout final_layout, VkRenderPass *out) {
    VkAttachmentDescription color = {};
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = final_layout;
    VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = 1;
    pass.pAttachments = &color;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = final_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ? 1 : 2;    // nothing samples the window
    pass.pDependencies = dependencies;
    return vk_ok(vkCreateRenderPass(s->device, &pass, NULL, out), "vkCreateRenderPass");
}

static bool create_render_passes(vk_state *s) {
    // The scene's: floating-point colour and depth, both cleared, both then left
    // for the post steps to sample.
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = VK_HDR_FORMAT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;   // the depth target: the steps read it
    VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;
    // Wait for the last frame's reads of the colour and the depth and its depth
    // writes before writing either; let the post steps read both after.
    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                 | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = 2;
    pass.pAttachments = attachments;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 2;
    pass.pDependencies = dependencies;
    if (!vk_ok(vkCreateRenderPass(s->device, &pass, NULL, &s->render_pass), "vkCreateRenderPass")) return false;
    if (!create_color_pass(s, VK_HDR_FORMAT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &s->hdr_pass)
        || !create_color_pass(s, s->color_format, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &s->ldr_pass)
        || !create_color_pass(s, s->color_format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, &s->window_pass)) return false;

    // The shadow map's: depth only, kept, and left readable by the lit pass. The
    // dependencies order it after the previous frame's reads and before this
    // frame's.
    VkAttachmentDescription shadow = {};
    shadow.format = VK_FORMAT_D32_SFLOAT;
    shadow.samples = VK_SAMPLE_COUNT_1_BIT;
    shadow.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    shadow.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    shadow.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    shadow.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    shadow.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    shadow.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference shadow_ref = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription shadow_subpass = {};
    shadow_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    shadow_subpass.pDepthStencilAttachment = &shadow_ref;
    VkSubpassDependency shadow_dependencies[2] = {};
    shadow_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    shadow_dependencies[0].dstSubpass = 0;
    shadow_dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    shadow_dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    shadow_dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    shadow_dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    shadow_dependencies[1].srcSubpass = 0;
    shadow_dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    shadow_dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    shadow_dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    shadow_dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    shadow_dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo shadow_pass = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    shadow_pass.attachmentCount = 1;
    shadow_pass.pAttachments = &shadow;
    shadow_pass.subpassCount = 1;
    shadow_pass.pSubpasses = &shadow_subpass;
    shadow_pass.dependencyCount = 2;
    shadow_pass.pDependencies = shadow_dependencies;
    return vk_ok(vkCreateRenderPass(s->device, &shadow_pass, NULL, &s->shadow_render_pass), "vkCreateRenderPass");
}

static bool create_layouts(vk_state *s) {
    VkDescriptorSetLayoutBinding bindings[5] = {};
    bindings[0].binding = R_BIND_FRAME;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = R_BIND_LIGHTS;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;   // declared in both, read in one
    bindings[2].binding = R_BIND_SHADOW_MAP;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[3].binding = R_BIND_POST;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[4].binding = R_BIND_MATERIALS;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 5;
    set_info.pBindings = bindings;
    if (!vk_ok(vkCreateDescriptorSetLayout(s->device, &set_info, NULL, &s->set_layout), "vkCreateDescriptorSetLayout")) return false;
    // A post step's input: one sampled target.
    VkDescriptorSetLayoutBinding input = {};
    input.binding = 0;
    input.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    input.descriptorCount = 1;
    input.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    set_info.bindingCount = 1;
    set_info.pBindings = &input;
    if (!vk_ok(vkCreateDescriptorSetLayout(s->device, &set_info, NULL, &s->input_layout), "vkCreateDescriptorSetLayout")) return false;

    // Both layouts push the same range, so set 0 stays bound across them.
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, (uint32_t)sizeof(r_push)};
    VkDescriptorSetLayout sets[4] = {s->set_layout, s->input_layout, s->input_layout, s->input_layout};
    VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = sets;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (!vk_ok(vkCreatePipelineLayout(s->device, &layout_info, NULL, &s->pipeline_layout), "vkCreatePipelineLayout")) return false;
    layout_info.setLayoutCount = 4;
    return vk_ok(vkCreatePipelineLayout(s->device, &layout_info, NULL, &s->post_layout), "vkCreatePipelineLayout");
}

struct vk_pipeline_desc {
    const r_spirv_blob *vert, *frag;
    VkRenderPass render_pass;
    VkPipelineLayout layout;
    bool mesh_input;                 // r_vertex attributes, or nothing (the sky and the post steps make their own triangle)
    VkCullModeFlags cull;
    bool depth_test;                 // the pass has a depth attachment
    VkCompareOp depth_compare;
    bool depth_write;
    bool color;                      // has a colour attachment
};

static VkPipeline build_pipeline(vk_state *s, const vk_pipeline_desc *d) {
    if (!d->vert || !d->frag) {
        fputs("vulkan: a program's SPIR-V is missing from the table\n", stderr);
        return VK_NULL_HANDLE;
    }
    VkShaderModule vert = create_module(s, d->vert->words, d->vert->bytes);
    VkShaderModule frag = create_module(s, d->frag->words, d->frag->bytes);
    if (!vert || !frag) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vertex_binding = {0, sizeof(r_vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vertex_attributes[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(r_vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(r_vertex, normal)},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (d->mesh_input) {
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &vertex_binding;
        vertex_input.vertexAttributeDescriptionCount = 2;
        vertex_input.pVertexAttributeDescriptions = vertex_attributes;
    }
    VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = d->cull;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = d->depth_test ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = d->depth_test && d->depth_write ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = d->depth_compare;
    VkPipelineColorBlendAttachmentState blend_attachment = {};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = d->color ? 1 : 0;
    blend.pAttachments = &blend_attachment;
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = d->layout;
    info.renderPass = d->render_pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline);
    vkDestroyShaderModule(s->device, vert, NULL);
    vkDestroyShaderModule(s->device, frag, NULL);
    vk_ok(result, "vkCreateGraphicsPipelines");
    return pipeline;
}

// The lit pass, per material shader, and the shadow pass: the plain one, and one
// per cutout material shader; each with a variant that culls nothing, for two-sided
// draws. Then the sky and the post steps.
static bool create_pipelines(vk_state *s) {
    const r_spirv_blob *mesh_vert = r_spirv_find("mesh.vert"), *shadow_vert = r_spirv_find("shadow.vert");
    vk_pipeline_desc lit = {mesh_vert, NULL, s->render_pass, s->pipeline_layout, true, VK_CULL_MODE_BACK_BIT, true, VK_COMPARE_OP_GREATER, true, true};
    vk_pipeline_desc shadow = {shadow_vert, r_spirv_find("shadow.frag"), s->shadow_render_pass, s->pipeline_layout, true,
                               VK_CULL_MODE_FRONT_BIT, true, VK_COMPARE_OP_GREATER, true, false};
    bool ok = true;
    for (int side = 0; side < 2; ++side) {
        shadow.cull = side ? VK_CULL_MODE_NONE : VK_CULL_MODE_FRONT_BIT;
        s->shadow_pipelines[side] = build_pipeline(s, &shadow);
        ok = ok && s->shadow_pipelines[side];
    }
    for (uint32_t i = 0; i < s->shaders.count; ++i) {
        const r_shader_info *info = &s->shaders.items[i];
        char name[96];
        snprintf(name, sizeof(name), "%s:lit", info->file);
        lit.frag = r_spirv_find(name);
        snprintf(name, sizeof(name), "%s:shadow", info->file);
        shadow.frag = r_spirv_find(name);
        for (int side = 0; side < 2; ++side) {
            lit.cull = side ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
            shadow.cull = side ? VK_CULL_MODE_NONE : VK_CULL_MODE_FRONT_BIT;
            s->lit_pipelines[i][side] = build_pipeline(s, &lit);
            ok = ok && s->lit_pipelines[i][side];
            if (info->cutout) {
                s->cutout_pipelines[i][side] = build_pipeline(s, &shadow);
                ok = ok && s->cutout_pipelines[i][side];
            }
        }
    }
    vk_pipeline_desc sky = {r_spirv_find("sky.vert"), r_spirv_find("sky.frag"), s->render_pass, s->pipeline_layout, false,
                            VK_CULL_MODE_NONE, true, VK_COMPARE_OP_GREATER_OR_EQUAL, false, true};
    s->sky_pipeline = build_pipeline(s, &sky);
    ok = ok && s->sky_pipeline;

    // The post steps: one vertex stage, a fragment stage per program, no depth. The
    // composite and the anti-aliasing write 8-bit targets: the LDR target and the
    // window have the same format, so their passes are compatible and one
    // pipeline serves both.
    static const char *const FILES[R_POST_PROGRAM_COUNT] = {
        "post_downsample.frag", "post_upsample.frag", "post_flare.frag", "post_composite.frag", "post_fxaa.frag",
        "post_outline.frag", "post_haze.frag",
    };
    for (int i = 0; i < R_POST_PROGRAM_COUNT; ++i) {
        VkRenderPass pass = i == R_POST_COMPOSITE || i == R_POST_FXAA ? s->ldr_pass : s->hdr_pass;
        vk_pipeline_desc post = {r_spirv_find("post.vert"), r_spirv_find(FILES[i]), pass, s->post_layout, false,
                                 VK_CULL_MODE_NONE, false, VK_COMPARE_OP_ALWAYS, false, true};
        s->post_pipelines[i] = build_pipeline(s, &post);
        ok = ok && s->post_pipelines[i];
    }
    return ok;
}

static void destroy_shadow_map(vk_state *s) {
    for (uint32_t c = 0; c < R_SHADOW_CASCADES; ++c) {
        if (s->shadow_framebuffers[c]) vkDestroyFramebuffer(s->device, s->shadow_framebuffers[c], NULL);
        if (s->shadow_layer_views[c]) vkDestroyImageView(s->device, s->shadow_layer_views[c], NULL);
        s->shadow_framebuffers[c] = VK_NULL_HANDLE;
        s->shadow_layer_views[c] = VK_NULL_HANDLE;
    }
    if (s->shadow_array_view) vkDestroyImageView(s->device, s->shadow_array_view, NULL);
    if (s->shadow_image) vkDestroyImage(s->device, s->shadow_image, NULL);
    if (s->shadow_memory) vkFreeMemory(s->device, s->shadow_memory, NULL);
    s->shadow_array_view = VK_NULL_HANDLE;
    s->shadow_image = VK_NULL_HANDLE;
    s->shadow_memory = VK_NULL_HANDLE;
    s->shadow_resolution = 0;
}

// The map at a resolution; the frame slots' descriptors are pointed at it by
// write_shadow_descriptors once they exist.
static bool create_shadow_map(vk_state *s, uint32_t resolution) {
    destroy_shadow_map(s);
    s->shadow_resolution = resolution;
    VkImageCreateInfo image = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_D32_SFLOAT;
    image.extent = {resolution, resolution, 1};
    image.mipLevels = 1;
    image.arrayLayers = R_SHADOW_CASCADES;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!vk_ok(vkCreateImage(s->device, &image, NULL, &s->shadow_image), "vkCreateImage")) return false;
    VkMemoryRequirements needs;
    vkGetImageMemoryRequirements(s->device, s->shadow_image, &needs);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = needs.size;
    alloc.memoryTypeIndex = find_memory(s, needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX
        || !vk_ok(vkAllocateMemory(s->device, &alloc, NULL, &s->shadow_memory), "vkAllocateMemory")) return false;
    vkBindImageMemory(s->device, s->shadow_image, s->shadow_memory, 0);

    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = s->shadow_image;
    view.format = VK_FORMAT_D32_SFLOAT;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view.subresourceRange.levelCount = 1;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.subresourceRange.layerCount = R_SHADOW_CASCADES;
    if (!vk_ok(vkCreateImageView(s->device, &view, NULL, &s->shadow_array_view), "vkCreateImageView")) return false;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.subresourceRange.layerCount = 1;
    for (uint32_t c = 0; c < R_SHADOW_CASCADES; ++c) {
        view.subresourceRange.baseArrayLayer = c;
        if (!vk_ok(vkCreateImageView(s->device, &view, NULL, &s->shadow_layer_views[c]), "vkCreateImageView")) return false;
        VkFramebufferCreateInfo framebuffer = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = s->shadow_render_pass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &s->shadow_layer_views[c];
        framebuffer.width = resolution;
        framebuffer.height = resolution;
        framebuffer.layers = 1;
        if (!vk_ok(vkCreateFramebuffer(s->device, &framebuffer, NULL, &s->shadow_framebuffers[c]), "vkCreateFramebuffer")) return false;
    }

    // Reversed depth: lit where the reference is at least as near as what is
    // stored; outside the map the depth reads as 0, the far plane, so lit.
    if (!s->shadow_sampler) {
        VkSamplerCreateInfo sampler = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampler.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        sampler.compareEnable = VK_TRUE;
        sampler.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
        sampler.maxLod = 1.0f;
        if (!vk_ok(vkCreateSampler(s->device, &sampler, NULL, &s->shadow_sampler), "vkCreateSampler")) return false;
    }

    // Put every layer in the layout the lit pass samples it in, so a frame without
    // shadows (no sun) still binds a valid image.
    VkCommandBuffer commands = one_shot_begin(s);
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = s->shadow_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = R_SHADOW_CASCADES;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 1, &barrier);
    one_shot_end(s, commands);
    return true;
}

static bool create_pools(vk_state *s) {
    VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = s->queue_family;
    if (!vk_ok(vkCreateCommandPool(s->device, &pool, NULL, &s->command_pool), "vkCreateCommandPool")) return false;

    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4 * VK_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_FRAMES_IN_FLIGHT + R_TARGET_COUNT},
    };
    VkDescriptorPoolCreateInfo descriptor_pool = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptor_pool.maxSets = VK_FRAMES_IN_FLIGHT + R_TARGET_COUNT;
    descriptor_pool.poolSizeCount = 2;
    descriptor_pool.pPoolSizes = sizes;
    if (!vk_ok(vkCreateDescriptorPool(s->device, &descriptor_pool, NULL, &s->descriptor_pool), "vkCreateDescriptorPool")) return false;

    // The targets' input sets, written whenever the targets are made; and the
    // sampler they are read through.
    for (int t = 0; t < R_TARGET_COUNT; ++t) {
        VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = s->descriptor_pool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &s->input_layout;
        if (!vk_ok(vkAllocateDescriptorSets(s->device, &alloc, &s->targets[t].input), "vkAllocateDescriptorSets")) return false;
    }
    VkSamplerCreateInfo sampler = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 1.0f;
    if (!vk_ok(vkCreateSampler(s->device, &sampler, NULL, &s->target_sampler), "vkCreateSampler")) return false;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;    // depth is not to be blended between pixels
    return vk_ok(vkCreateSampler(s->device, &sampler, NULL, &s->depth_sampler), "vkCreateSampler");
}

// Points every frame slot's descriptor at the shadow map of the moment.
static void write_shadow_descriptors(vk_state *s) {
    for (uint32_t i = 0; i < VK_FRAMES_IN_FLIGHT; ++i) {
        VkDescriptorImageInfo shadow_map = {s->shadow_sampler, s->shadow_array_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = s->frames[i].descriptors;
        write.dstBinding = R_BIND_SHADOW_MAP;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &shadow_map;
        vkUpdateDescriptorSets(s->device, 1, &write, 0, NULL);
    }
}

static bool create_frame_slots(vk_state *s) {
    for (uint32_t i = 0; i < VK_FRAMES_IN_FLIGHT; ++i) {
        vk_frame_slot *slot = &s->frames[i];
        VkCommandBufferAllocateInfo commands = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commands.commandPool = s->command_pool;
        commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commands.commandBufferCount = 1;
        VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo semaphore = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!vk_ok(vkAllocateCommandBuffers(s->device, &commands, &slot->commands), "vkAllocateCommandBuffers")
            || !vk_ok(vkCreateFence(s->device, &fence, NULL, &slot->in_flight), "vkCreateFence")
            || !vk_ok(vkCreateSemaphore(s->device, &semaphore, NULL, &slot->image_acquired), "vkCreateSemaphore")
            || !buffer_create(s, sizeof(r_gpu_frame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &slot->frame_uniforms)
            || !buffer_create(s, sizeof(r_gpu_lights), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &slot->light_uniforms)
            || !buffer_create(s, sizeof(r_gpu_post), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &slot->post_uniforms)
            || !buffer_create(s, sizeof(r_gpu_materials), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &slot->material_uniforms))
            return false;
        vkMapMemory(s->device, slot->frame_uniforms.memory, 0, sizeof(r_gpu_frame), 0, &slot->frame_mapped);
        vkMapMemory(s->device, slot->light_uniforms.memory, 0, sizeof(r_gpu_lights), 0, &slot->lights_mapped);
        vkMapMemory(s->device, slot->post_uniforms.memory, 0, sizeof(r_gpu_post), 0, &slot->post_mapped);
        vkMapMemory(s->device, slot->material_uniforms.memory, 0, sizeof(r_gpu_materials), 0, &slot->materials_mapped);
        if (s->timestamp_period > 0.0f) {
            VkQueryPoolCreateInfo queries = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queries.queryCount = 2 * VK_TIMED_PASSES;
            if (!vk_ok(vkCreateQueryPool(s->device, &queries, NULL, &slot->timestamps), "vkCreateQueryPool")) return false;
        }

        VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = s->descriptor_pool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &s->set_layout;
        if (!vk_ok(vkAllocateDescriptorSets(s->device, &alloc, &slot->descriptors), "vkAllocateDescriptorSets")) return false;
        VkDescriptorBufferInfo frame_buffer = {slot->frame_uniforms.buffer, 0, sizeof(r_gpu_frame)};
        VkDescriptorBufferInfo light_buffer = {slot->light_uniforms.buffer, 0, sizeof(r_gpu_lights)};
        VkDescriptorBufferInfo post_buffer = {slot->post_uniforms.buffer, 0, sizeof(r_gpu_post)};
        VkDescriptorBufferInfo material_buffer = {slot->material_uniforms.buffer, 0, sizeof(r_gpu_materials)};
        VkDescriptorImageInfo shadow_map = {s->shadow_sampler, s->shadow_array_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[5] = {};
        for (int k = 0; k < 5; ++k) {
            writes[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[k].dstSet = slot->descriptors;
            writes[k].descriptorCount = 1;
        }
        writes[0].dstBinding = R_BIND_FRAME;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &frame_buffer;
        writes[1].dstBinding = R_BIND_LIGHTS;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &light_buffer;
        writes[2].dstBinding = R_BIND_SHADOW_MAP;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &shadow_map;
        writes[3].dstBinding = R_BIND_POST;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[3].pBufferInfo = &post_buffer;
        writes[4].dstBinding = R_BIND_MATERIALS;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[4].pBufferInfo = &material_buffer;
        vkUpdateDescriptorSets(s->device, 5, writes, 0, NULL);
    }
    return true;
}

// ---------------------------------------------------------------------------
// The swapchain and what depends on its size
// ---------------------------------------------------------------------------
static void destroy_swapchain_targets(vk_state *s) {
    for (int t = 0; t < R_TARGET_COUNT; ++t) {
        vk_target *target = &s->targets[t];
        if (target->framebuffer) vkDestroyFramebuffer(s->device, target->framebuffer, NULL);
        if (target->post_framebuffer) vkDestroyFramebuffer(s->device, target->post_framebuffer, NULL);
        if (target->view) vkDestroyImageView(s->device, target->view, NULL);
        if (target->image) vkDestroyImage(s->device, target->image, NULL);
        if (target->memory) vkFreeMemory(s->device, target->memory, NULL);
        target->framebuffer = VK_NULL_HANDLE;
        target->post_framebuffer = VK_NULL_HANDLE;
        target->view = VK_NULL_HANDLE;
        target->image = VK_NULL_HANDLE;
        target->memory = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < s->image_count; ++i) {
        if (s->framebuffers[i]) vkDestroyFramebuffer(s->device, s->framebuffers[i], NULL);
        if (s->views[i]) vkDestroyImageView(s->device, s->views[i], NULL);
        if (s->render_finished[i]) vkDestroySemaphore(s->device, s->render_finished[i], NULL);
        s->framebuffers[i] = VK_NULL_HANDLE;
        s->views[i] = VK_NULL_HANDLE;
        s->render_finished[i] = VK_NULL_HANDLE;
    }
    if (s->depth_view) vkDestroyImageView(s->device, s->depth_view, NULL);
    if (s->depth_image) vkDestroyImage(s->device, s->depth_image, NULL);
    if (s->depth_memory) vkFreeMemory(s->device, s->depth_memory, NULL);
    s->depth_view = VK_NULL_HANDLE;
    s->depth_image = VK_NULL_HANDLE;
    s->depth_memory = VK_NULL_HANDLE;
}

// Backend.h's render targets at the window's size: the window target is the
// swapchain, the depth target is the scene's depth image read unfiltered, the
// scene target draws with that depth, every other one is a colour image. All are
// put in the sampled layout first, so that a target no step wrote is still valid
// to have bound.
static bool create_targets(vk_state *s, VkExtent2D window) {
    for (int t = 0; t < R_TARGET_COUNT; ++t) {
        r_target_desc desc = r_target_info(t);
        vk_target *target = &s->targets[t];
        target->extent.width = (window.width >> desc.shift) > 1 ? window.width >> desc.shift : 1;
        target->extent.height = (window.height >> desc.shift) > 1 ? window.height >> desc.shift : 1;
        if (t == R_TARGET_WINDOW) continue;
        if (desc.depth) {
            VkDescriptorImageInfo sampled = {s->depth_sampler, s->depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = target->input;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &sampled;
            vkUpdateDescriptorSets(s->device, 1, &write, 0, NULL);
            continue;
        }
        VkFormat format = desc.hdr ? VK_HDR_FORMAT : s->color_format;

        VkImageCreateInfo image = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = format;
        image.extent = {target->extent.width, target->extent.height, 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!vk_ok(vkCreateImage(s->device, &image, NULL, &target->image), "vkCreateImage")) return false;
        VkMemoryRequirements needs;
        vkGetImageMemoryRequirements(s->device, target->image, &needs);
        VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = needs.size;
        alloc.memoryTypeIndex = find_memory(s, needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (alloc.memoryTypeIndex == UINT32_MAX
            || !vk_ok(vkAllocateMemory(s->device, &alloc, NULL, &target->memory), "vkAllocateMemory")) return false;
        vkBindImageMemory(s->device, target->image, target->memory, 0);

        VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.image = target->image;
        view.format = format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (!vk_ok(vkCreateImageView(s->device, &view, NULL, &target->view), "vkCreateImageView")) return false;

        VkImageView attachments[2] = {target->view, s->depth_view};
        VkFramebufferCreateInfo framebuffer = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = t == R_TARGET_SCENE ? s->render_pass : desc.hdr ? s->hdr_pass : s->ldr_pass;
        framebuffer.attachmentCount = t == R_TARGET_SCENE ? 2 : 1;
        framebuffer.pAttachments = attachments;
        framebuffer.width = target->extent.width;
        framebuffer.height = target->extent.height;
        framebuffer.layers = 1;
        if (!vk_ok(vkCreateFramebuffer(s->device, &framebuffer, NULL, &target->framebuffer), "vkCreateFramebuffer")) return false;
        if (t == R_TARGET_SCENE) {
            // A post step writing the scene target (the haze, after the outline) does
            // so under the post pass, which has no depth attachment.
            framebuffer.renderPass = s->hdr_pass;
            framebuffer.attachmentCount = 1;
            if (!vk_ok(vkCreateFramebuffer(s->device, &framebuffer, NULL, &target->post_framebuffer), "vkCreateFramebuffer")) return false;
        }

        VkDescriptorImageInfo sampled = {s->target_sampler, target->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = target->input;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &sampled;
        vkUpdateDescriptorSets(s->device, 1, &write, 0, NULL);
    }

    VkCommandBuffer commands = one_shot_begin(s);
    for (int t = R_TARGET_WINDOW + 1; t < R_TARGET_COUNT; ++t) {
        if (!s->targets[t].image) continue;    // the depth target: the scene pass puts its image in the read layout
        VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = s->targets[t].image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, NULL, 0, NULL, 1, &barrier);
    }
    one_shot_end(s, commands);
    return true;
}

static bool create_swapchain(vk_state *s, int width, int height) {
    vkDeviceWaitIdle(s->device);
    destroy_swapchain_targets(s);

    VkSurfaceCapabilitiesKHR caps;
    if (!vk_ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->physical, s->surface, &caps), "surface capabilities")) return false;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = (uint32_t)width;
        extent.height = (uint32_t)height;
    }
    if (!extent.width || !extent.height) return false;    // minimised

    uint32_t wanted = caps.minImageCount + 1;
    if (caps.maxImageCount && wanted > caps.maxImageCount) wanted = caps.maxImageCount;
    if (wanted > VK_MAX_SWAPCHAIN_IMAGES) wanted = VK_MAX_SWAPCHAIN_IMAGES;
    VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = s->surface;
    info.minImageCount = wanted;
    info.imageFormat = s->color_format;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
        ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;     // the only mode every driver has
    if (!plat_vsync()) {
        VkPresentModeKHR modes[8];
        uint32_t mode_count = 8;
        vkGetPhysicalDeviceSurfacePresentModesKHR(s->physical, s->surface, &mode_count, modes);
        for (uint32_t i = 0; i < mode_count; ++i)
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) info.presentMode = modes[i];
    }
    info.clipped = VK_TRUE;
    info.oldSwapchain = s->swapchain;
    VkSwapchainKHR swapchain;
    if (!vk_ok(vkCreateSwapchainKHR(s->device, &info, NULL, &swapchain), "vkCreateSwapchainKHR")) return false;
    if (s->swapchain) vkDestroySwapchainKHR(s->device, s->swapchain, NULL);
    s->swapchain = swapchain;
    s->extent = extent;

    s->image_count = VK_MAX_SWAPCHAIN_IMAGES;
    vkGetSwapchainImagesKHR(s->device, s->swapchain, &s->image_count, s->images);

    VkImageCreateInfo depth = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    depth.imageType = VK_IMAGE_TYPE_2D;
    depth.format = VK_FORMAT_D32_SFLOAT;
    depth.extent = {extent.width, extent.height, 1};
    depth.mipLevels = 1;
    depth.arrayLayers = 1;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.tiling = VK_IMAGE_TILING_OPTIMAL;
    depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!vk_ok(vkCreateImage(s->device, &depth, NULL, &s->depth_image), "vkCreateImage")) return false;
    VkMemoryRequirements needs;
    vkGetImageMemoryRequirements(s->device, s->depth_image, &needs);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = needs.size;
    alloc.memoryTypeIndex = find_memory(s, needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!vk_ok(vkAllocateMemory(s->device, &alloc, NULL, &s->depth_memory), "vkAllocateMemory")) return false;
    vkBindImageMemory(s->device, s->depth_image, s->depth_memory, 0);

    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    view.image = s->depth_image;
    view.format = VK_FORMAT_D32_SFLOAT;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (!vk_ok(vkCreateImageView(s->device, &view, NULL, &s->depth_view), "vkCreateImageView")) return false;

    view.format = s->color_format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    for (uint32_t i = 0; i < s->image_count; ++i) {
        view.image = s->images[i];
        if (!vk_ok(vkCreateImageView(s->device, &view, NULL, &s->views[i]), "vkCreateImageView")) return false;
        VkFramebufferCreateInfo framebuffer = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = s->window_pass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &s->views[i];
        framebuffer.width = extent.width;
        framebuffer.height = extent.height;
        framebuffer.layers = 1;
        if (!vk_ok(vkCreateFramebuffer(s->device, &framebuffer, NULL, &s->framebuffers[i]), "vkCreateFramebuffer")) return false;
        VkSemaphoreCreateInfo semaphore = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!vk_ok(vkCreateSemaphore(s->device, &semaphore, NULL, &s->render_finished[i]), "vkCreateSemaphore")) return false;
    }
    return create_targets(s, extent);
}

// ---------------------------------------------------------------------------
// The backend
// ---------------------------------------------------------------------------
static void vk_mesh_destroy(void *state, r_mesh mesh) {
    vk_state *s = (vk_state *)state;
    if (!mesh || mesh >= R_MAX_MESHES || !s->meshes[mesh].index_count) return;
    vkDeviceWaitIdle(s->device);
    buffer_destroy(s, &s->meshes[mesh].vertices);
    buffer_destroy(s, &s->meshes[mesh].indices);
    s->meshes[mesh].index_count = 0;
}

static void vk_destroy(void *state) {
    vk_state *s = (vk_state *)state;
    if (s->device) {
        vkDeviceWaitIdle(s->device);
        for (r_mesh i = 1; i < R_MAX_MESHES; ++i) vk_mesh_destroy(s, i);
        destroy_swapchain_targets(s);
        if (s->swapchain) vkDestroySwapchainKHR(s->device, s->swapchain, NULL);
        buffer_destroy(s, &s->capture);
        for (uint32_t i = 0; i < VK_FRAMES_IN_FLIGHT; ++i) {
            vk_frame_slot *slot = &s->frames[i];
            if (slot->in_flight) vkDestroyFence(s->device, slot->in_flight, NULL);
            if (slot->image_acquired) vkDestroySemaphore(s->device, slot->image_acquired, NULL);
            buffer_destroy(s, &slot->frame_uniforms);
            buffer_destroy(s, &slot->light_uniforms);
            buffer_destroy(s, &slot->post_uniforms);
            buffer_destroy(s, &slot->material_uniforms);
            if (slot->timestamps) vkDestroyQueryPool(s->device, slot->timestamps, NULL);
        }
        if (s->target_sampler) vkDestroySampler(s->device, s->target_sampler, NULL);
        if (s->depth_sampler) vkDestroySampler(s->device, s->depth_sampler, NULL);
        destroy_shadow_map(s);
        if (s->shadow_sampler) vkDestroySampler(s->device, s->shadow_sampler, NULL);
        if (s->descriptor_pool) vkDestroyDescriptorPool(s->device, s->descriptor_pool, NULL);
        if (s->command_pool) vkDestroyCommandPool(s->device, s->command_pool, NULL);
        for (int i = 0; i < R_MAX_SHADERS; ++i)
            for (int side = 0; side < 2; ++side) {
                if (s->lit_pipelines[i][side]) vkDestroyPipeline(s->device, s->lit_pipelines[i][side], NULL);
                if (s->cutout_pipelines[i][side]) vkDestroyPipeline(s->device, s->cutout_pipelines[i][side], NULL);
            }
        for (int side = 0; side < 2; ++side)
            if (s->shadow_pipelines[side]) vkDestroyPipeline(s->device, s->shadow_pipelines[side], NULL);
        if (s->sky_pipeline) vkDestroyPipeline(s->device, s->sky_pipeline, NULL);
        for (int i = 0; i < R_POST_PROGRAM_COUNT; ++i)
            if (s->post_pipelines[i]) vkDestroyPipeline(s->device, s->post_pipelines[i], NULL);
        if (s->pipeline_layout) vkDestroyPipelineLayout(s->device, s->pipeline_layout, NULL);
        if (s->post_layout) vkDestroyPipelineLayout(s->device, s->post_layout, NULL);
        if (s->set_layout) vkDestroyDescriptorSetLayout(s->device, s->set_layout, NULL);
        if (s->input_layout) vkDestroyDescriptorSetLayout(s->device, s->input_layout, NULL);
        if (s->render_pass) vkDestroyRenderPass(s->device, s->render_pass, NULL);
        if (s->shadow_render_pass) vkDestroyRenderPass(s->device, s->shadow_render_pass, NULL);
        if (s->hdr_pass) vkDestroyRenderPass(s->device, s->hdr_pass, NULL);
        if (s->ldr_pass) vkDestroyRenderPass(s->device, s->ldr_pass, NULL);
        if (s->window_pass) vkDestroyRenderPass(s->device, s->window_pass, NULL);
        vkDestroyDevice(s->device, NULL);
    }
    if (s->instance) {
        if (s->surface) vkDestroySurfaceKHR(s->instance, s->surface, NULL);
        if (s->messenger) vkDestroyDebugUtilsMessengerEXT(s->instance, s->messenger, NULL);
        vkDestroyInstance(s->instance, NULL);
    }
    free(s);
}

static void *vk_create(plat_window *window, const r_shader_set *shaders) {
    if (!r_spirv_fresh()) {
        fprintf(stderr, "vulkan: the embedded SPIR-V was not built from these shaders: install the Vulkan SDK "
                        "and rebuild, or run tools/compile_shaders\n");
        return NULL;
    }
    if (!plat_vk_available()) {
        fprintf(stderr, "vulkan: no loader or driver on this machine\n");
        return NULL;
    }
    vk_state *s = (vk_state *)calloc(1, sizeof(vk_state));
    s->window = window;
    s->shaders = *shaders;
    bool ok = create_instance(s);
    if (ok && !plat_vk_create_surface(window, s->instance, &s->surface)) {
        fprintf(stderr, "vulkan: could not create a surface on the window\n");
        ok = false;
    }
    // The shadow map's first layout change needs the command pool, and the frame
    // slots' descriptors need the shadow map.
    ok = ok && pick_device(s) && create_render_passes(s) && create_layouts(s) && create_pipelines(s)
         && create_pools(s) && create_shadow_map(s, R_SHADOW_RESOLUTION_MIN) && create_frame_slots(s);
    if (!ok) {
        vk_destroy(s);
        return NULL;
    }
    int width, height;
    plat_window_size(window, &width, &height);
    if (width > 0 && height > 0) create_swapchain(s, width, height);
    return s;
}

static r_mesh vk_mesh_create(void *state, const r_vertex *vertices, uint32_t vertex_count,
                             const uint32_t *indices, uint32_t index_count) {
    vk_state *s = (vk_state *)state;
    r_mesh slot = 1;
    while (slot < R_MAX_MESHES && s->meshes[slot].index_count) ++slot;
    if (slot == R_MAX_MESHES || !index_count) return 0;
    vk_mesh *m = &s->meshes[slot];
    if (!buffer_upload(s, vertices, vertex_count * sizeof(r_vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &m->vertices)
        || !buffer_upload(s, indices, index_count * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &m->indices)) {
        buffer_destroy(s, &m->vertices);
        buffer_destroy(s, &m->indices);
        return 0;
    }
    m->index_count = index_count;
    return slot;
}

static void record_capture(vk_state *s, VkCommandBuffer commands, VkImage image) {
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {s->extent.width, s->extent.height, 1};
    vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->capture.buffer, 1, &region);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, NULL, 0, NULL, 1, &barrier);
}

// Records one pass's range of the packet: the lit pass with each material's
// shader's pipeline, or a shadow cascade with the plain one and, for a cutout
// material, its own; each in its two-sided variant when the draw asks. The pass
// is sorted by shader, so pipelines change seldom. The Draw push constants'
// first part is per draw; the cascade index after it is pushed once per shadow
// pass and persists.
static void record_pass(vk_state *s, VkCommandBuffer commands, const r_packet *packet, r_pass pass, bool lit) {
    r_mesh bound = 0;
    VkPipeline current = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < pass.count; ++i) {
        uint32_t index = packet->indices[pass.first + i];
        const r_draw *draw = &packet->draws[index];
        if (draw->mesh >= R_MAX_MESHES) continue;
        const vk_mesh *m = &s->meshes[draw->mesh];
        if (!m->index_count) continue;
        uint32_t material = draw->material < R_MAX_MATERIALS ? draw->material : 0;
        uint32_t shader = packet->material_shader[material];
        int side = (draw->flags & R_DRAW_TWO_SIDED) ? 1 : 0;
        VkPipeline wanted = lit ? s->lit_pipelines[shader][side]
                                : (packet->material_cutout[material] ? s->cutout_pipelines[shader][side] : s->shadow_pipelines[side]);
        if (wanted != current) {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, wanted);
            current = wanted;
        }
        if (draw->mesh != bound) {
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(commands, 0, 1, &m->vertices.buffer, &zero);
            vkCmdBindIndexBuffer(commands, m->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            bound = draw->mesh;
        }
        r_push push;
        push.model = draw->model;
        memcpy(push.color, draw->color, sizeof(push.color));
        push.flags = draw->flags;
        push.material = material;
        push.lights = packet->light_masks[index];
        vkCmdPushConstants(commands, s->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, R_PUSH_DRAW_SIZE, &push);
        vkCmdDrawIndexed(commands, m->index_count, 1, 0, 0, 0);
    }
}

// The timers: a timestamp before and after each pass, read back when the slot's
// fence says the frame is done, two frames later.
static void timers_collect(vk_state *s, vk_frame_slot *slot) {
    if (!slot->timed || !slot->timestamps) return;
    r_timings result = {};
    for (int i = 0; i < VK_TIMED_PASSES; ++i) {
        if (slot->timed_pass[i] < 0) continue;
        // Pair by pair: a slot this frame did not use was never written, and asking
        // for it would make the whole read "not ready", for ever.
        uint64_t ticks[2];
        if (vkGetQueryPoolResults(s->device, slot->timestamps, (uint32_t)(2 * i), 2, sizeof(ticks), ticks, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
        result.ms[slot->timed_pass[i]] += (float)((double)(ticks[1] - ticks[0]) * (double)s->timestamp_period * 1e-6);
    }
    for (int p = 0; p < R_PASS_COUNT; ++p) result.total_ms += result.ms[p];
    result.valid = true;
    s->timings = result;
}

static void timer_begin(vk_frame_slot *slot, VkCommandBuffer commands, int index, int pass) {
    slot->timed_pass[index] = pass;
    if (slot->timestamps) vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot->timestamps, (uint32_t)(2 * index));
}
static void timer_end(vk_frame_slot *slot, VkCommandBuffer commands, int index) {
    if (slot->timestamps) vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot->timestamps, (uint32_t)(2 * index + 1));
}

static r_timings vk_timings(void *state) { return ((vk_state *)state)->timings; }

static void vk_submit(void *state, const r_packet *packet, int width, int height) {
    vk_state *s = (vk_state *)state;
    if (!s->swapchain || s->extent.width != (uint32_t)width || s->extent.height != (uint32_t)height) {
        if (!create_swapchain(s, width, height)) return;
    }
    vk_frame_slot *slot = &s->frames[s->frame];
    vkWaitForFences(s->device, 1, &slot->in_flight, VK_TRUE, UINT64_MAX);
    timers_collect(s, slot);
    if (packet->cascade_count && packet->shadow_resolution != s->shadow_resolution) {
        vkDeviceWaitIdle(s->device);
        if (!create_shadow_map(s, packet->shadow_resolution)) return;
        write_shadow_descriptors(s);
    }

    uint32_t image;
    VkResult acquired = vkAcquireNextImageKHR(s->device, s->swapchain, UINT64_MAX, slot->image_acquired, VK_NULL_HANDLE, &image);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        create_swapchain(s, width, height);
        return;    // the semaphore was not signalled, so this slot is still clean
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        vk_ok(acquired, "vkAcquireNextImageKHR");
        return;
    }
    vkResetFences(s->device, 1, &slot->in_flight);
    memcpy(slot->frame_mapped, &packet->frame, sizeof(packet->frame));
    memcpy(slot->lights_mapped, &packet->lights, sizeof(packet->lights));
    memcpy(slot->post_mapped, &packet->post, sizeof(packet->post));
    memcpy(slot->materials_mapped, &packet->materials, sizeof(packet->materials));

    VkDeviceSize capture_size = (VkDeviceSize)s->extent.width * s->extent.height * 4;
    bool capturing = packet->capture_rgba != NULL;
    if (capturing && s->capture_size < capture_size) {
        vkDeviceWaitIdle(s->device);
        buffer_destroy(s, &s->capture);
        s->capture_size = capture_size;
        capturing = buffer_create(s, capture_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s->capture);
    }

    VkCommandBuffer commands = slot->commands;
    vkResetCommandBuffer(commands, 0);
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commands, &begin);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline_layout, 0, 1, &slot->descriptors, 0, NULL);
    for (int i = 0; i < VK_TIMED_PASSES; ++i) slot->timed_pass[i] = -1;
    if (slot->timestamps) vkCmdResetQueryPool(commands, slot->timestamps, 0, 2 * VK_TIMED_PASSES);
    slot->timed = true;

    // 1. The shadow map, a cascade per layer, from the sun's point of view.
    timer_begin(slot, commands, 0, R_PASS_SHADOW);
    VkClearValue depth_clear = {};
    depth_clear.depthStencil.depth = 0.0f;
    float resolution = (float)s->shadow_resolution;
    VkViewport shadow_viewport = {0.0f, resolution, resolution, -resolution, 0.0f, 1.0f};
    VkRect2D shadow_scissor = {{0, 0}, {s->shadow_resolution, s->shadow_resolution}};
    for (uint32_t c = 0; c < packet->cascade_count; ++c) {
        VkRenderPassBeginInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = s->shadow_render_pass;
        pass.framebuffer = s->shadow_framebuffers[c];
        pass.renderArea = shadow_scissor;
        pass.clearValueCount = 1;
        pass.pClearValues = &depth_clear;
        vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(commands, 0, 1, &shadow_viewport);
        vkCmdSetScissor(commands, 0, 1, &shadow_scissor);
        vkCmdPushConstants(commands, s->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           (uint32_t)offsetof(r_push, cascade), sizeof(uint32_t), &c);
        record_pass(s, commands, packet, packet->shadow[c], false);
        vkCmdEndRenderPass(commands);
    }
    timer_end(slot, commands, 0);

    // 2. The lit pass into the scene target, then 3. the sky where nothing was drawn.
    timer_begin(slot, commands, 1, R_PASS_MAIN);
    VkClearValue clears[2] = {};
    clears[0].color.float32[3] = 1.0f;
    clears[1].depthStencil.depth = 0.0f;
    VkRenderPassBeginInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = s->render_pass;
    pass.framebuffer = s->targets[R_TARGET_SCENE].framebuffer;
    pass.renderArea.extent = s->extent;
    pass.clearValueCount = 2;
    pass.pClearValues = clears;
    vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport = {0.0f, (float)s->extent.height, (float)s->extent.width, -(float)s->extent.height, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, s->extent};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);
    record_pass(s, commands, packet, packet->main, true);
    timer_end(slot, commands, 1);
    timer_begin(slot, commands, 2, R_PASS_SKY);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->sky_pipeline);
    vkCmdDraw(commands, 3, 1, 0, 0);
    vkCmdEndRenderPass(commands);
    timer_end(slot, commands, 2);

    // 4. The post steps, the last of which writes the swapchain image.
    for (uint32_t i = 0; i < packet->step_count; ++i) {
        const r_post_step *step = &packet->steps[i];
        const vk_target *target = &s->targets[step->target];
        const vk_target *input = &s->targets[step->inputs[0]];
        bool window = step->target == R_TARGET_WINDOW;
        VkRenderPassBeginInfo post = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        post.renderPass = window ? s->window_pass : r_target_info(step->target).hdr ? s->hdr_pass : s->ldr_pass;
        post.framebuffer = window ? s->framebuffers[image] : step->target == R_TARGET_SCENE ? target->post_framebuffer : target->framebuffer;
        post.renderArea.extent = target->extent;
        vkCmdBeginRenderPass(commands, &post, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport post_viewport = {0.0f, (float)target->extent.height, (float)target->extent.width, -(float)target->extent.height, 0.0f, 1.0f};
        VkRect2D post_scissor = {{0, 0}, target->extent};
        vkCmdSetViewport(commands, 0, 1, &post_viewport);
        vkCmdSetScissor(commands, 0, 1, &post_scissor);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->post_pipelines[step->program]);
        VkDescriptorSet sets[4] = {slot->descriptors, s->targets[step->inputs[0]].input, s->targets[step->inputs[1]].input,
                                   s->targets[step->inputs[2]].input};
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->post_layout, 0, 4, sets, 0, NULL);
        r_post_push push;
        push.texel[0] = 1.0f / (float)input->extent.width;
        push.texel[1] = 1.0f / (float)input->extent.height;
        push.texel[2] = 1.0f / (float)target->extent.width;
        push.texel[3] = 1.0f / (float)target->extent.height;
        memcpy(push.params, step->params, sizeof(push.params));
        vkCmdPushConstants(commands, s->post_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        timer_begin(slot, commands, 3 + (int)i, r_post_program_pass(step->program));
        vkCmdDraw(commands, 3, 1, 0, 0);
        vkCmdEndRenderPass(commands);
        timer_end(slot, commands, 3 + (int)i);
    }
    if (capturing) record_capture(s, commands, s->images[image]);
    vkEndCommandBuffer(commands);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &slot->image_acquired;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &s->render_finished[image];
    if (!vk_ok(vkQueueSubmit(s->queue, 1, &submit, slot->in_flight), "vkQueueSubmit")) return;

    if (capturing) {
        vkWaitForFences(s->device, 1, &slot->in_flight, VK_TRUE, UINT64_MAX);
        void *mapped;
        vkMapMemory(s->device, s->capture.memory, 0, capture_size, 0, &mapped);
        const uint8_t *in = (const uint8_t *)mapped;
        uint8_t *out = packet->capture_rgba;
        bool swap = s->color_format == VK_FORMAT_B8G8R8A8_UNORM;
        for (size_t i = 0; i < (size_t)capture_size; i += 4) {
            out[i] = in[i + (swap ? 2 : 0)];
            out[i + 1] = in[i + 1];
            out[i + 2] = in[i + (swap ? 0 : 2)];
            out[i + 3] = in[i + 3];
        }
        vkUnmapMemory(s->device, s->capture.memory);
    }

    VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &s->render_finished[image];
    present.swapchainCount = 1;
    present.pSwapchains = &s->swapchain;
    present.pImageIndices = &image;
    VkResult presented = vkQueuePresentKHR(s->queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) create_swapchain(s, width, height);
    else if (presented != VK_SUCCESS) vk_ok(presented, "vkQueuePresentKHR");
    s->frame = (s->frame + 1) % VK_FRAMES_IN_FLIGHT;
}

const r_backend R_BACKEND_VK_API = {
    "Vulkan 1.1", vk_create, vk_destroy, vk_mesh_create, vk_mesh_destroy, vk_submit, vk_timings,
};
