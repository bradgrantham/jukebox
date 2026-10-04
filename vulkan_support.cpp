// Copied from ~/trees/viewscene/vulkan_support.cpp; see vulkan_support.h for
// what changed.

#include <set>
#include <string>
#include <vector>
#include <memory>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <iterator>
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include "vulkan_support.h"

namespace VulkanSupport
{

std::map<VkResult, std::string> ResultToString =
{
    {VK_ERROR_OUT_OF_HOST_MEMORY, "OUT_OF_HOST_MEMORY"},
    {VK_ERROR_OUT_OF_DEVICE_MEMORY, "OUT_OF_DEVICE_MEMORY"},
    {VK_ERROR_INITIALIZATION_FAILED, "INITIALIZATION_FAILED"},
    {VK_ERROR_DEVICE_LOST, "DEVICE_LOST"},
    {VK_ERROR_MEMORY_MAP_FAILED, "MEMORY_MAP_FAILED"},
    {VK_ERROR_LAYER_NOT_PRESENT, "LAYER_NOT_PRESENT"},
    {VK_ERROR_EXTENSION_NOT_PRESENT, "EXTENSION_NOT_PRESENT"},
    {VK_ERROR_FEATURE_NOT_PRESENT, "FEATURE_NOT_PRESENT"},
    {VK_ERROR_INCOMPATIBLE_DRIVER, "INCOMPATIBLE_DRIVER"},
    {VK_ERROR_SURFACE_LOST_KHR, "SURFACE_LOST_KHR"},
    {VK_ERROR_NATIVE_WINDOW_IN_USE_KHR, "NATIVE_WINDOW_IN_USE_KHR"},
    {VK_ERROR_OUT_OF_DATE_KHR, "OUT_OF_DATE_KHR"},
    {VK_SUBOPTIMAL_KHR, "SUBOPTIMAL_KHR"},
};

namespace
{

uint32_t validation_message_count = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    [[maybe_unused]] VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    [[maybe_unused]] void* user_data)
{
    const char* level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR" : "WARNING";
    std::cerr << "[vulkan " << level << "] " << (data->pMessage ? data->pMessage : "(no message)") << "\n";
    validation_message_count++;
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT MessengerCreateInfo()
{
    VkDebugUtilsMessengerCreateInfoEXT create{};
    create.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    create.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    create.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    create.pfnUserCallback = DebugCallback;
    return create;
}

std::set<std::string> AvailableInstanceExtensions()
{
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
    std::set<std::string> names;
    for(const auto& p: props) {
        names.insert(p.extensionName);
    }
    return names;
}

};

uint32_t ValidationMessageCount()
{
    return validation_message_count;
}

VkResult CreateInstance(const char *app_name, const char *engine_name, bool enable_validation, bool api_dump, VkInstance* instance)
{
    std::set<std::string> extension_set;
    std::set<std::string> layer_set;

    // GLFW knows which surface extensions this platform needs
    // (VK_EXT_metal_surface on macOS, xcb/xlib/wayland on Linux, ...).
    uint32_t glfw_reqd_extension_count = 0;
    const char** glfw_reqd_extensions = glfwGetRequiredInstanceExtensions(&glfw_reqd_extension_count);
    if(glfw_reqd_extensions == nullptr) {
        std::cerr << "glfwGetRequiredInstanceExtensions failed; is Vulkan available to GLFW?\n";
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    for(uint32_t i = 0; i < glfw_reqd_extension_count; i++) {
        extension_set.insert(glfw_reqd_extensions[i]);
    }

    extension_set.insert(VK_KHR_SURFACE_EXTENSION_NAME);

    // Non-conformant (portability) implementations such as MoltenVK are only
    // enumerated if the application opts in with this extension and flag.
    // Only ask for it when the loader offers it; setting the flag without
    // enabling the extension is invalid (VUID-VkInstanceCreateInfo-flags-06559).
    VkInstanceCreateFlags flags = 0;
    std::set<std::string> available = AvailableInstanceExtensions();
    if(available.count(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) > 0) {
        extension_set.insert(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    if(enable_validation) {
        layer_set.insert("VK_LAYER_KHRONOS_validation");
        extension_set.insert(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    if(api_dump) {
        layer_set.insert("VK_LAYER_LUNARG_api_dump");
    }

    std::vector<const char*> extensions;
    std::vector<const char*> layers;
    // Careful - only valid for duration of sets where contents have not changed
    for(auto& s: extension_set) {
        extensions.push_back(s.c_str());
    }

    for(auto& s: layer_set) {
        layers.push_back(s.c_str());
    }

    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = app_name;
    app_info.pEngineName = engine_name;
    app_info.apiVersion = VK_API_VERSION_1_2;

    VkDebugUtilsMessengerCreateInfoEXT messenger_create = MessengerCreateInfo();

    VkInstanceCreateInfo create = {};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pNext = enable_validation ? &messenger_create : nullptr;
    create.flags = flags;
    create.pApplicationInfo = &app_info;
    create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create.ppEnabledExtensionNames = extensions.data();
    create.enabledLayerCount = static_cast<uint32_t>(layers.size());
    create.ppEnabledLayerNames = layers.data();

    return vkCreateInstance(&create, nullptr, instance);
}

VkResult CreateDebugMessenger(VkInstance instance, VkDebugUtilsMessengerEXT* messenger)
{
    auto create_fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    if(create_fn == nullptr) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    VkDebugUtilsMessengerCreateInfoEXT create = MessengerCreateInfo();
    return create_fn(instance, &create, nullptr, messenger);
}

void DestroyDebugMessenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger)
{
    if(messenger == VK_NULL_HANDLE) {
        return;
    }
    auto destroy_fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if(destroy_fn != nullptr) {
        destroy_fn(instance, messenger, nullptr);
    }
}

const char* device_types[] = {
    "other",
    "integrated GPU",
    "discrete GPU",
    "virtual GPU",
    "CPU",
    "unknown",
};

std::map<uint32_t, std::string> memory_property_bit_name_map = {
    {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "DEVICE_LOCAL"},
    {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "HOST_VISIBLE"},
    {VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "HOST_COHERENT"},
    {VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "HOST_CACHED"},
    {VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, "LAZILY_ALLOCATED"},
};

void print_memory_property_bits(VkMemoryPropertyFlags flags)
{
    bool add_or = false;
    for(auto& bit : memory_property_bit_name_map)
	if(flags & bit.first) {
	    printf("%s%s", add_or ? " | " : "", bit.second.c_str());
	    add_or = true;
	}
}

void print_device_information(VkPhysicalDevice physical_device, const VkPhysicalDeviceMemoryProperties& memory_properties)
{
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical_device, &properties);

    printf("Physical Device Information\n");
    printf("    API     %d.%d.%d\n", properties.apiVersion >> 22, (properties.apiVersion >> 12) & 0x3ff, properties.apiVersion & 0xfff);
    printf("    driver  %X\n", properties.driverVersion);
    printf("    vendor  %X\n", properties.vendorID);
    printf("    device  %X\n", properties.deviceID);
    printf("    name    %s\n", properties.deviceName);
    printf("    type    %s\n", device_types[std::min(5, (int)properties.deviceType)]);

    uint32_t ext_count;

    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &ext_count, nullptr);
    std::unique_ptr<VkExtensionProperties[]> exts(new VkExtensionProperties[ext_count]);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &ext_count, exts.get());
    printf("    extensions:\n");
    for (uint32_t i = 0; i < ext_count; i++) {
        printf("        %s\n", exts[i].extensionName);
    }

    uint32_t queue_family_count;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
    std::unique_ptr<VkQueueFamilyProperties[]> queue_families(new VkQueueFamilyProperties[queue_family_count]);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families.get());
    for(uint32_t i = 0; i < queue_family_count; i++) {
        printf("queue %d:\n", i);
        printf("    flags:                       %04X\n", queue_families[i].queueFlags);
        printf("    queueCount:                  %d\n", queue_families[i].queueCount);
        printf("    timestampValidBits:          %d\n", queue_families[i].timestampValidBits);
        printf("    minImageTransferGranularity: (%d, %d, %d)\n",
            queue_families[i].minImageTransferGranularity.width,
            queue_families[i].minImageTransferGranularity.height,
            queue_families[i].minImageTransferGranularity.depth);
    }

    for(uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        printf("memory type %d: flags ", i);
        print_memory_property_bits(memory_properties.memoryTypes[i].propertyFlags);
        printf("\n");
    }
}

VkResult ChoosePhysicalDevice(VkInstance instance, VkPhysicalDevice* physical_device, VkPhysicalDeviceMemoryProperties* memory_properties, bool be_noisy)
{
    uint32_t gpu_count = 0;

    SUCCEED_OR_RETURN_VK(vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr));

    if(be_noisy) {
        std::cerr << gpu_count << " gpus enumerated\n";
    }
    if(gpu_count == 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    std::vector<VkPhysicalDevice> physical_devices;
    physical_devices.resize(gpu_count);
    SUCCEED_OR_RETURN_VK(vkEnumeratePhysicalDevices(instance, &gpu_count, physical_devices.data()));

    for(auto dev: physical_devices) {
        vkGetPhysicalDeviceMemoryProperties(dev, memory_properties);
        if(be_noisy) {
            print_device_information(dev, *memory_properties);
        }
        if(true) { // ???
            *physical_device = dev;
        }
    }

    return VK_SUCCESS;
}

VkResult ChoosePhysicalDeviceQueue(VkInstance instance, VkPhysicalDevice physical_device, uint32_t& preferred_queue_family)
{
    uint32_t queue_family_count;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families;
    queue_families.resize(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families.data());
    preferred_queue_family = NO_QUEUE_FAMILY;
    for(uint32_t i = 0; i < queue_family_count; i++) {
        if((queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && glfwGetPhysicalDevicePresentationSupport(instance, physical_device, i)) {
            preferred_queue_family = i;
        }
    }

    return VK_SUCCESS;
}

VkResult CreateDevice(VkPhysicalDevice physical_device, const std::vector<std::string>& extensions, uint32_t preferred_queue_family, VkDevice& device)
{
    VkDeviceQueueCreateInfo create_queues[1]{};
    float queue_priorities[1] = {1.0f};
    create_queues[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    create_queues[0].pNext = NULL;
    create_queues[0].flags = 0;
    create_queues[0].queueFamilyIndex = preferred_queue_family;
    create_queues[0].queueCount = 1;
    create_queues[0].pQueuePriorities = queue_priorities;

    std::vector<std::string> all_extensions = extensions;

    // A portability (non-conformant) implementation advertises
    // VK_KHR_portability_subset and then the application must enable it.
    // The macro for the name lives in vulkan_beta.h, so spell it out.
    static constexpr const char* portability_subset = "VK_KHR_portability_subset";
    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &ext_count, exts.data());
    for(const auto& e: exts) {
        if(strcmp(e.extensionName, portability_subset) == 0) {
            if(std::find(all_extensions.begin(), all_extensions.end(), portability_subset) == all_extensions.end()) {
                all_extensions.push_back(portability_subset);
            }
        }
    }

    std::vector<const char*> extensions_c_strs;
    std::transform(all_extensions.cbegin(), all_extensions.cend(), std::back_inserter(extensions_c_strs), [](const std::string& s){return s.c_str();});

    VkDeviceCreateInfo create{};

    create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create.pNext = NULL;
    create.flags = 0;
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = create_queues;
    create.enabledExtensionCount = static_cast<uint32_t>(extensions_c_strs.size());
    create.ppEnabledExtensionNames = extensions_c_strs.data();
    return vkCreateDevice(physical_device, &create, nullptr, &device);
}

VkResult CreateCommandPool(uint32_t preferred_queue_family, VkDevice& device, VkCommandPool& command_pool)
{
    VkCommandPoolCreateInfo create_command_pool = {};
    create_command_pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    // Allow individual command buffers to be reset and re-recorded each frame.
    create_command_pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    create_command_pool.queueFamilyIndex = preferred_queue_family;
    return vkCreateCommandPool(device, &create_command_pool, nullptr, &command_pool);
}


};
