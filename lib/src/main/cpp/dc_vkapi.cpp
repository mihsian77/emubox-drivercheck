#include "dc_vkapi.h"

#include <dlfcn.h>

namespace dc {
namespace {

template <typename T>
T cast(PFN_vkVoidFunction p) {
    return reinterpret_cast<T>(p);
}

PFN_vkVoidFunction globalSym(VkApi& api, const char* name) {
    if (api.gipa) {
        if (PFN_vkVoidFunction p = api.gipa(nullptr, name)) return p;
    }
    return reinterpret_cast<PFN_vkVoidFunction>(dlsym(api.lib, name));
}

const char* dlerr() {
    const char* e = dlerror();
    return e ? e : "unknown dlopen error";
}

}  // namespace

bool openApi(VkApi& api, const char* driverPath, std::string& err) {
    closeApi(api);

    if (driverPath && driverPath[0]) {
        api.lib = dlopen(driverPath, RTLD_NOW | RTLD_LOCAL);
        if (!api.lib) {
            err = std::string("dlopen 失败：") + dlerr();
            return false;
        }
        api.source = std::string("driver:") + driverPath;
    } else {
        // 系统 loader：Android 上是 libvulkan.so，桌面上通常只有带版本号的 .so.1
        api.lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!api.lib) api.lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!api.lib) {
            err = std::string("dlopen libvulkan 失败：") + dlerr();
            return false;
        }
        api.source = "system loader";
    }

    // 驱动 ICD 通常只导 vk_icdGetInstanceProcAddr；桌面 loader 导 vkGetInstanceProcAddr
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(api.lib, "vkGetInstanceProcAddr"));
    if (!gipa) {
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            dlsym(api.lib, "vk_icdGetInstanceProcAddr"));
    }
    if (!gipa) {
        err = "驱动未导出 vkGetInstanceProcAddr / vk_icdGetInstanceProcAddr，不是有效 Vulkan 实现";
        closeApi(api);
        return false;
    }
    api.gipa = gipa;

    api.vkCreateInstance =
        cast<PFN_vkCreateInstance>(globalSym(api, "vkCreateInstance"));
    api.vkEnumerateInstanceExtensionProperties = cast<PFN_vkEnumerateInstanceExtensionProperties>(
        globalSym(api, "vkEnumerateInstanceExtensionProperties"));
    if (!api.vkCreateInstance) {
        err = "驱动未提供 vkCreateInstance";
        closeApi(api);
        return false;
    }
    return true;
}

const char* resolveInstance(VkApi& api, VkInstance instance) {
    api.instance = instance;
    auto g = [&](const char* n) {
        PFN_vkVoidFunction p = api.gipa(instance, n);
        return p ? p : reinterpret_cast<PFN_vkVoidFunction>(dlsym(api.lib, n));
    };

#define DC_INST(n)                                     \
    api.n = cast<PFN_##n>(g(#n));                      \
    if (!api.n) return #n;

    DC_INST(vkDestroyInstance)
    DC_INST(vkEnumeratePhysicalDevices)
    DC_INST(vkGetPhysicalDeviceProperties)
    DC_INST(vkGetPhysicalDeviceFormatProperties)
    DC_INST(vkGetPhysicalDeviceMemoryProperties)
    DC_INST(vkGetPhysicalDeviceQueueFamilyProperties)
    DC_INST(vkCreateDevice)
    DC_INST(vkGetDeviceProcAddr)
#undef DC_INST
    return "";
}

const char* resolveDevice(VkApi& api, VkDevice device) {
    api.device = device;
    auto g = [&](const char* n) {
        if (api.vkGetDeviceProcAddr) {
            if (PFN_vkVoidFunction p = api.vkGetDeviceProcAddr(device, n)) return p;
        }
        PFN_vkVoidFunction p = api.gipa(api.instance, n);
        return p ? p : reinterpret_cast<PFN_vkVoidFunction>(dlsym(api.lib, n));
    };

#define DC_DEV(n)                                      \
    api.n = cast<PFN_##n>(g(#n));                      \
    if (!api.n) return #n;

    DC_DEV(vkDestroyDevice)
    DC_DEV(vkGetDeviceQueue)
    DC_DEV(vkCreateImage)
    DC_DEV(vkDestroyImage)
    DC_DEV(vkGetImageMemoryRequirements)
    DC_DEV(vkAllocateMemory)
    DC_DEV(vkFreeMemory)
    DC_DEV(vkBindImageMemory)
    DC_DEV(vkCreateImageView)
    DC_DEV(vkDestroyImageView)
    DC_DEV(vkCreateRenderPass)
    DC_DEV(vkDestroyRenderPass)
    DC_DEV(vkCreateFramebuffer)
    DC_DEV(vkDestroyFramebuffer)
    DC_DEV(vkCreateShaderModule)
    DC_DEV(vkDestroyShaderModule)
    DC_DEV(vkCreatePipelineLayout)
    DC_DEV(vkDestroyPipelineLayout)
    DC_DEV(vkCreateGraphicsPipelines)
    DC_DEV(vkDestroyPipeline)
    DC_DEV(vkCreateBuffer)
    DC_DEV(vkDestroyBuffer)
    DC_DEV(vkGetBufferMemoryRequirements)
    DC_DEV(vkBindBufferMemory)
    DC_DEV(vkCreateCommandPool)
    DC_DEV(vkDestroyCommandPool)
    DC_DEV(vkAllocateCommandBuffers)
    DC_DEV(vkResetCommandBuffer)
    DC_DEV(vkBeginCommandBuffer)
    DC_DEV(vkEndCommandBuffer)
    DC_DEV(vkCmdBeginRenderPass)
    DC_DEV(vkCmdBindPipeline)
    DC_DEV(vkCmdDraw)
    DC_DEV(vkCmdEndRenderPass)
    DC_DEV(vkCmdCopyImageToBuffer)
    DC_DEV(vkCreateFence)
    DC_DEV(vkDestroyFence)
    DC_DEV(vkResetFences)
    DC_DEV(vkWaitForFences)
    DC_DEV(vkQueueSubmit)
    DC_DEV(vkMapMemory)
    DC_DEV(vkUnmapMemory)
#undef DC_DEV
    return "";
}

void closeApi(VkApi& api) {
    if (api.lib) dlclose(api.lib);
    api = VkApi{};
}

}  // namespace dc
