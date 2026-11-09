// 可切换来源的 Vulkan 函数表。
//
// 为什么不直接链 libvulkan：那只能跑系统驱动。要测一个 .so 驱动，必须自己 dlopen
// 它、拿 vk_icdGetInstanceProcAddr，再逐级解析函数——这正是所有「换驱动」方案的根。
// 这里把它抽象成一张表，渲染代码只认表，不认来源；系统 loader 与自定义驱动走同一
// 条渲染路径，结果才可比。
#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace dc {

struct VkApi {
    void* lib = nullptr;
    PFN_vkGetInstanceProcAddr gipa = nullptr;

    // 全局
    PFN_vkCreateInstance vkCreateInstance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
    // 实例
    PFN_vkDestroyInstance vkDestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkCreateDevice vkCreateDevice = nullptr;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
    // 设备
    PFN_vkDestroyDevice vkDestroyDevice = nullptr;
    PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
    PFN_vkCreateImage vkCreateImage = nullptr;
    PFN_vkDestroyImage vkDestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory = nullptr;
    PFN_vkFreeMemory vkFreeMemory = nullptr;
    PFN_vkBindImageMemory vkBindImageMemory = nullptr;
    PFN_vkCreateImageView vkCreateImageView = nullptr;
    PFN_vkDestroyImageView vkDestroyImageView = nullptr;
    PFN_vkCreateRenderPass vkCreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass vkDestroyRenderPass = nullptr;
    PFN_vkCreateFramebuffer vkCreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer vkDestroyFramebuffer = nullptr;
    PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
    PFN_vkCreateBuffer vkCreateBuffer = nullptr;
    PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
    PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
    PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass = nullptr;
    PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
    PFN_vkCmdDraw vkCmdDraw = nullptr;
    PFN_vkCmdEndRenderPass vkCmdEndRenderPass = nullptr;
    PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer = nullptr;
    PFN_vkCreateFence vkCreateFence = nullptr;
    PFN_vkDestroyFence vkDestroyFence = nullptr;
    PFN_vkResetFences vkResetFences = nullptr;
    PFN_vkWaitForFences vkWaitForFences = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit = nullptr;
    PFN_vkMapMemory vkMapMemory = nullptr;
    PFN_vkUnmapMemory vkUnmapMemory = nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    // 来源描述，写给用户看
    std::string source;

    // 驱动包内已预加载的依赖库（绝对路径），以及 DT_NEEDED 里没找到的库名。
    // 高通跨机型驱动是一组 .so，缺 libgsl.so / libllvm-glnext.so 时 dlopen 会直接失败，
    // 这两个列表就是把「为什么打不开」说清楚的全部依据。
    std::vector<std::string> depsLoaded;
    std::vector<std::string> depsMissing;
};

/**
 * 打开 Vulkan 实现。
 * @param driverPath 非空则直接 dlopen 该 .so（绕过系统 loader）；为空则走系统 loader。
 * @param err 失败原因（含 dlerror 原文）
 */
bool openApi(VkApi& api, const char* driverPath, std::string& err);

/** 实例创建后解析实例级函数；返回失败项名字（空串表示全齐）。 */
const char* resolveInstance(VkApi& api, VkInstance instance);

/** 设备创建后解析设备级函数；返回失败项名字（空串表示全齐）。 */
const char* resolveDevice(VkApi& api, VkDevice device);

void closeApi(VkApi& api);

}  // namespace dc
