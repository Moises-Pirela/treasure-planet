// Vulkan 1.1, loaded at runtime through volk: a machine with no Vulkan driver starts
// fine and falls back to OpenGL. One render pass, one pipeline, frames in flight.
// The viewport has a negative height (core in 1.1), which gives the y-up clip space
// the backend contract asks for.
#include "backend.h"
#include <volk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t MESH_VERT_SPV[] =
#include "shaders/mesh.vert.spv.inl"
    ;
static const uint32_t MESH_FRAG_SPV[] =
#include "shaders/mesh.frag.spv.inl"
    ;

#define VK_FRAMES_IN_FLIGHT 2
#define VK_MAX_MESHES 256
#define VK_MAX_SWAPCHAIN_IMAGES 8
// What the shader's Draw block holds: r_draw up to its mesh handle.
#define VK_PUSH_SIZE ((uint32_t)offsetof(r_draw, mesh))

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
    vk_buffer uniforms;
    void *uniforms_mapped;
    VkDescriptorSet descriptors;
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
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout pipeline_layout;
    VkRenderPass render_pass;
    VkPipeline pipeline;
    VkFormat color_format;

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
    vk_mesh meshes[VK_MAX_MESHES];    // slot 0 stays empty: mesh 0 means none
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

    const char *extensions[3] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    uint32_t extension_count = 2;
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
    const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;
    if (!vk_ok(vkCreateDevice(s->physical, &info, NULL, &s->device), "vkCreateDevice")) return false;
    volkLoadDevice(s->device);
    vkGetDeviceQueue(s->device, s->queue_family, 0, &s->queue);

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

static bool create_pipeline(vk_state *s) {
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = s->color_format;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;
    // Wait for the acquire before writing colour, and for the last frame's depth writes.
    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = 2;
    pass.pAttachments = attachments;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 1;
    pass.pDependencies = &dependency;
    if (!vk_ok(vkCreateRenderPass(s->device, &pass, NULL, &s->render_pass), "vkCreateRenderPass")) return false;

    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 1;
    set_info.pBindings = &binding;
    if (!vk_ok(vkCreateDescriptorSetLayout(s->device, &set_info, NULL, &s->set_layout), "vkCreateDescriptorSetLayout")) return false;

    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, VK_PUSH_SIZE};
    VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &s->set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (!vk_ok(vkCreatePipelineLayout(s->device, &layout_info, NULL, &s->pipeline_layout), "vkCreatePipelineLayout")) return false;

    VkShaderModule vert = create_module(s, MESH_VERT_SPV, sizeof(MESH_VERT_SPV));
    VkShaderModule frag = create_module(s, MESH_FRAG_SPV, sizeof(MESH_FRAG_SPV));
    if (!vert || !frag) return false;
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
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = vertex_attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_GREATER;
    VkPipelineColorBlendAttachmentState blend_attachment = {};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
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
    info.layout = s->pipeline_layout;
    info.renderPass = s->render_pass;
    VkResult result = vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &info, NULL, &s->pipeline);
    vkDestroyShaderModule(s->device, vert, NULL);
    vkDestroyShaderModule(s->device, frag, NULL);
    return vk_ok(result, "vkCreateGraphicsPipelines");
}

static bool create_frame_slots(vk_state *s) {
    VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = s->queue_family;
    if (!vk_ok(vkCreateCommandPool(s->device, &pool, NULL, &s->command_pool), "vkCreateCommandPool")) return false;

    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo descriptor_pool = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptor_pool.maxSets = VK_FRAMES_IN_FLIGHT;
    descriptor_pool.poolSizeCount = 1;
    descriptor_pool.pPoolSizes = &size;
    if (!vk_ok(vkCreateDescriptorPool(s->device, &descriptor_pool, NULL, &s->descriptor_pool), "vkCreateDescriptorPool")) return false;

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
            || !buffer_create(s, sizeof(r_frame_uniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &slot->uniforms))
            return false;
        vkMapMemory(s->device, slot->uniforms.memory, 0, sizeof(r_frame_uniforms), 0, &slot->uniforms_mapped);

        VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = s->descriptor_pool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &s->set_layout;
        if (!vk_ok(vkAllocateDescriptorSets(s->device, &alloc, &slot->descriptors), "vkAllocateDescriptorSets")) return false;
        VkDescriptorBufferInfo buffer = {slot->uniforms.buffer, 0, sizeof(r_frame_uniforms)};
        VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = slot->descriptors;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &buffer;
        vkUpdateDescriptorSets(s->device, 1, &write, 0, NULL);
    }
    return true;
}

// ---------------------------------------------------------------------------
// The swapchain and what depends on its size
// ---------------------------------------------------------------------------
static void destroy_swapchain_targets(vk_state *s) {
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
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
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
    depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
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
        VkImageView targets[2] = {s->views[i], s->depth_view};
        VkFramebufferCreateInfo framebuffer = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = s->render_pass;
        framebuffer.attachmentCount = 2;
        framebuffer.pAttachments = targets;
        framebuffer.width = extent.width;
        framebuffer.height = extent.height;
        framebuffer.layers = 1;
        if (!vk_ok(vkCreateFramebuffer(s->device, &framebuffer, NULL, &s->framebuffers[i]), "vkCreateFramebuffer")) return false;
        VkSemaphoreCreateInfo semaphore = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!vk_ok(vkCreateSemaphore(s->device, &semaphore, NULL, &s->render_finished[i]), "vkCreateSemaphore")) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The backend
// ---------------------------------------------------------------------------
static void vk_mesh_destroy(void *state, r_mesh mesh) {
    vk_state *s = (vk_state *)state;
    if (!mesh || mesh >= VK_MAX_MESHES || !s->meshes[mesh].index_count) return;
    vkDeviceWaitIdle(s->device);
    buffer_destroy(s, &s->meshes[mesh].vertices);
    buffer_destroy(s, &s->meshes[mesh].indices);
    s->meshes[mesh].index_count = 0;
}

static void vk_destroy(void *state) {
    vk_state *s = (vk_state *)state;
    if (s->device) {
        vkDeviceWaitIdle(s->device);
        for (r_mesh i = 1; i < VK_MAX_MESHES; ++i) vk_mesh_destroy(s, i);
        destroy_swapchain_targets(s);
        if (s->swapchain) vkDestroySwapchainKHR(s->device, s->swapchain, NULL);
        buffer_destroy(s, &s->capture);
        for (uint32_t i = 0; i < VK_FRAMES_IN_FLIGHT; ++i) {
            vk_frame_slot *slot = &s->frames[i];
            if (slot->in_flight) vkDestroyFence(s->device, slot->in_flight, NULL);
            if (slot->image_acquired) vkDestroySemaphore(s->device, slot->image_acquired, NULL);
            buffer_destroy(s, &slot->uniforms);
        }
        if (s->descriptor_pool) vkDestroyDescriptorPool(s->device, s->descriptor_pool, NULL);
        if (s->command_pool) vkDestroyCommandPool(s->device, s->command_pool, NULL);
        if (s->pipeline) vkDestroyPipeline(s->device, s->pipeline, NULL);
        if (s->pipeline_layout) vkDestroyPipelineLayout(s->device, s->pipeline_layout, NULL);
        if (s->set_layout) vkDestroyDescriptorSetLayout(s->device, s->set_layout, NULL);
        if (s->render_pass) vkDestroyRenderPass(s->device, s->render_pass, NULL);
        vkDestroyDevice(s->device, NULL);
    }
    if (s->instance) {
        if (s->surface) vkDestroySurfaceKHR(s->instance, s->surface, NULL);
        if (s->messenger) vkDestroyDebugUtilsMessengerEXT(s->instance, s->messenger, NULL);
        vkDestroyInstance(s->instance, NULL);
    }
    free(s);
}

static void *vk_create(plat_window *window) {
    if (volkInitialize() != VK_SUCCESS) {
        fprintf(stderr, "vulkan: no loader on this machine\n");
        return NULL;
    }
    vk_state *s = (vk_state *)calloc(1, sizeof(vk_state));
    s->window = window;
    bool ok = create_instance(s);
    if (ok) {
        VkWin32SurfaceCreateInfoKHR surface = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surface.hinstance = (HINSTANCE)plat_instance_native();
        surface.hwnd = (HWND)plat_window_native(window);
        ok = vk_ok(vkCreateWin32SurfaceKHR(s->instance, &surface, NULL, &s->surface), "vkCreateWin32SurfaceKHR");
    }
    ok = ok && pick_device(s) && create_pipeline(s) && create_frame_slots(s);
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
    while (slot < VK_MAX_MESHES && s->meshes[slot].index_count) ++slot;
    if (slot == VK_MAX_MESHES || !index_count) return 0;
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

static void vk_submit(void *state, const r_frame *frame, int width, int height) {
    vk_state *s = (vk_state *)state;
    if (!s->swapchain || s->extent.width != (uint32_t)width || s->extent.height != (uint32_t)height) {
        if (!create_swapchain(s, width, height)) return;
    }
    vk_frame_slot *slot = &s->frames[s->frame];
    vkWaitForFences(s->device, 1, &slot->in_flight, VK_TRUE, UINT64_MAX);

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
    memcpy(slot->uniforms_mapped, &frame->uniforms, sizeof(frame->uniforms));

    VkDeviceSize capture_size = (VkDeviceSize)s->extent.width * s->extent.height * 4;
    bool capturing = frame->capture_rgba != NULL;
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

    VkClearValue clears[2] = {};
    clears[0].color.float32[0] = frame->clear[0];
    clears[0].color.float32[1] = frame->clear[1];
    clears[0].color.float32[2] = frame->clear[2];
    clears[0].color.float32[3] = 1.0f;
    clears[1].depthStencil.depth = 0.0f;
    VkRenderPassBeginInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = s->render_pass;
    pass.framebuffer = s->framebuffers[image];
    pass.renderArea.extent = s->extent;
    pass.clearValueCount = 2;
    pass.pClearValues = clears;
    vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {0.0f, (float)s->extent.height, (float)s->extent.width, -(float)s->extent.height, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, s->extent};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline_layout, 0, 1, &slot->descriptors, 0, NULL);
    r_mesh bound = 0;
    for (size_t i = 0; i < frame->draw_count; ++i) {
        const r_draw *draw = &frame->draws[i];
        if (!draw->mesh || draw->mesh >= VK_MAX_MESHES) continue;
        const vk_mesh *m = &s->meshes[draw->mesh];
        if (!m->index_count) continue;
        if (draw->mesh != bound) {
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(commands, 0, 1, &m->vertices.buffer, &zero);
            vkCmdBindIndexBuffer(commands, m->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            bound = draw->mesh;
        }
        vkCmdPushConstants(commands, s->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, VK_PUSH_SIZE, draw);
        vkCmdDrawIndexed(commands, m->index_count, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(commands);
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
        uint8_t *out = frame->capture_rgba;
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
    "Vulkan 1.1", vk_create, vk_destroy, vk_mesh_create, vk_mesh_destroy, vk_submit,
};
