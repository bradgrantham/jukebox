// Copied from ~/trees/viewscene/vulkan_support.h and adapted for pyjuke:
//  - macOS (MoltenVK) is supported: the portability enumeration instance
//    extension and the portability_subset device extension are enabled
//    whenever the implementation advertises them (on any platform).
//  - Instance extensions come from glfwGetRequiredInstanceExtensions() rather
//    than a hardcoded per-platform surface extension.
//  - Optional VK_EXT_debug_utils messenger that prints validation messages to
//    stderr and counts them.

#ifndef PYJUKE_VULKAN_SUPPORT_H
#define PYJUKE_VULKAN_SUPPORT_H

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

#define STR(f) #f

#define CHECK_VK(f) \
{ \
    VkResult result_ = (f); \
    if(result_ != VK_SUCCESS) { \
	if(VulkanSupport::ResultToString.count(result_) > 0) { \
	    std::cerr << "VkResult from " STR(f) " was " << VulkanSupport::ResultToString[result_] << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
	} \
	else \
	    std::cerr << "VkResult from " STR(f) " was " << result_ << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
	exit(EXIT_FAILURE); \
    } \
}

#define SUCCEED_OR_RETURN_VK(f) \
{ \
    VkResult result_ = (f); \
    if(result_ != VK_SUCCESS) { \
	return result_; \
    } \
}

#if defined(_WIN32)
#define PLATFORM_WINDOWS
#elif defined(__linux__)
#define PLATFORM_LINUX
#elif defined(__APPLE__) && defined(__MACH__)
// Supported through MoltenVK (portability subset).
#define PLATFORM_MACOS
#else
#error Platform not supported.
#endif

namespace VulkanSupport
{

static constexpr uint32_t NO_QUEUE_FAMILY{0xffffffff};

extern std::map<VkResult, std::string> ResultToString;

// Creates an instance with the extensions GLFW needs for surfaces, plus
// VK_KHR_portability_enumeration (and the matching create flag) if the loader
// offers it.  With enable_validation, enables VK_LAYER_KHRONOS_validation and
// VK_EXT_debug_utils, and chains a debug messenger create info so messages
// from vkCreateInstance/vkDestroyInstance are reported too.
// glfwInit() must have been called first.
VkResult CreateInstance(const char *app_name, const char *engine_name, bool enable_validation, bool api_dump, VkInstance* instance);

// Creates a debug messenger (instance must have been created with
// enable_validation) that prints warnings and errors to stderr.
VkResult CreateDebugMessenger(VkInstance instance, VkDebugUtilsMessengerEXT* messenger);
void DestroyDebugMessenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger);

// Number of warning/error messages the debug messenger has printed so far.
uint32_t ValidationMessageCount();

VkResult ChoosePhysicalDevice(VkInstance instance, VkPhysicalDevice* physical_device, VkPhysicalDeviceMemoryProperties* memory_properties, bool be_noisy);

VkResult ChoosePhysicalDeviceQueue(VkInstance instance, VkPhysicalDevice physical_device, uint32_t& preferred_queue_family);

// Creates a device with one queue from preferred_queue_family.  In addition
// to "extensions", enables VK_KHR_portability_subset when the physical device
// advertises it, which the spec requires (VUID-VkDeviceCreateInfo-pProperties-04451).
VkResult CreateDevice(VkPhysicalDevice physical_device, const std::vector<std::string>& extensions, uint32_t preferred_queue_family, VkDevice& device);

VkResult CreateCommandPool(uint32_t preferred_queue_family, VkDevice& device, VkCommandPool& command_pool);

};

#endif // PYJUKE_VULKAN_SUPPORT_H
