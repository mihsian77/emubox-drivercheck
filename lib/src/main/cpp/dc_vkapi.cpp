#include "dc_vkapi.h"

#include <dlfcn.h>

#include <elf.h>
#include <sys/stat.h>

#include <cstdio>
#include <set>
#include <vector>

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

std::string dirOf(const std::string& path) {
    size_t p = path.find_last_of('/');
    return p == std::string::npos ? std::string(".") : path.substr(0, p);
}

bool isFile(const std::string& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// 系统本来就有的库，不作为「包内缺失」处理。
bool isSystemLib(const std::string& n) {
    static const char* kSys[] = {
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so",
        "libstdc++.so", "libandroid.so", "libnativewindow.so", "libsync.so",
        "libcutils.so", "libutils.so", "libhardware.so", "libhidlbase.so",
        "libEGL.so", "libGLESv2.so", "libvulkan.so", "libOpenSLES.so",
    };
    for (const char* s : kSys)
        if (n == s) return true;
    return false;
}

// 读 ELF64 的 DT_NEEDED。只支持 64 位（arm64-v8a / x86_64 都是），32 位返回空。
bool readNeeded(const std::string& path, std::vector<std::string>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    bool ok = false;
    unsigned char ident[EI_NIDENT];
    if (fread(ident, 1, EI_NIDENT, f) == EI_NIDENT &&
        ident[EI_MAG0] == ELFMAG0 && ident[EI_MAG1] == ELFMAG1 &&
        ident[EI_MAG2] == ELFMAG2 && ident[EI_MAG3] == ELFMAG3 &&
        ident[EI_CLASS] == ELFCLASS64) {
        Elf64_Ehdr eh {};
        fseek(f, 0, SEEK_SET);
        if (fread(&eh, sizeof(eh), 1, f) == 1 && eh.e_phnum > 0 &&
            eh.e_phentsize == sizeof(Elf64_Phdr)) {
            std::vector<Elf64_Phdr> ph(eh.e_phnum);
            fseek(f, static_cast<long>(eh.e_phoff), SEEK_SET);
            if (fread(ph.data(), sizeof(Elf64_Phdr), ph.size(), f) == ph.size()) {
                const Elf64_Phdr* dyn = nullptr;
                for (const auto& p : ph)
                    if (p.p_type == PT_DYNAMIC) dyn = &p;

                if (dyn && dyn->p_filesz >= sizeof(Elf64_Dyn)) {
                    size_t n = dyn->p_filesz / sizeof(Elf64_Dyn);
                    std::vector<Elf64_Dyn> d(n);
                    fseek(f, static_cast<long>(dyn->p_offset), SEEK_SET);
                    if (fread(d.data(), sizeof(Elf64_Dyn), n, f) == n) {
                        Elf64_Addr strtab = 0;
                        Elf64_Xword strsz = 0;
                        for (const auto& e : d) {
                            if (e.d_tag == DT_STRTAB) strtab = e.d_un.d_ptr;
                            else if (e.d_tag == DT_STRSZ) strsz = e.d_un.d_val;
                        }

                        // DT_STRTAB 是虚拟地址，要经 PT_LOAD 映射回文件偏移。
                        long stroff = -1;
                        if (strtab) {
                            for (const auto& p : ph) {
                                if (p.p_type == PT_LOAD && strtab >= p.p_vaddr &&
                                    strtab < p.p_vaddr + p.p_filesz) {
                                    stroff = static_cast<long>(
                                        p.p_offset + (strtab - p.p_vaddr));
                                    break;
                                }
                            }
                        }

                        if (stroff >= 0 && strsz > 0 && strsz < (1u << 20)) {
                            std::string table(strsz, '\0');
                            fseek(f, stroff, SEEK_SET);
                            if (fread(&table[0], 1, strsz, f) == strsz) {
                                for (const auto& e : d) {
                                    if (e.d_tag == DT_NEEDED && e.d_un.d_val < table.size())
                                        out.push_back(table.c_str() + e.d_un.d_val);
                                }
                                ok = true;
                            }
                        }
                    }
                }
            }
        }
    }
    fclose(f);
    return ok;
}

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += sep;
        s += v[i];
    }
    return s;
}

// 先递归加载 DT_NEEDED 指向的同目录库（GLOBAL，符号要对主驱动可见），再加载主驱动（LOCAL）。
void* loadTree(const std::string& path, const std::string& dir, bool global,
               std::set<std::string>& visited, std::vector<std::string>& loaded,
               std::vector<std::string>& missing, std::string& firstErr) {
    if (visited.count(path)) return nullptr;
    visited.insert(path);

    std::vector<std::string> needs;
    readNeeded(path, needs);

    for (const auto& n : needs) {
        if (isSystemLib(n)) continue;
        std::string dep = dir + "/" + n;
        if (isFile(dep)) {
            loadTree(dep, dir, true, visited, loaded, missing, firstErr);
        } else {
            bool seen = false;
            for (const auto& m : missing) seen = seen || (m == n);
            if (!seen) missing.push_back(n);
        }
    }

    void* h = dlopen(path.c_str(), RTLD_NOW | (global ? RTLD_GLOBAL : RTLD_LOCAL));
    if (!h) {
        if (firstErr.empty()) firstErr = dlerr();
        return nullptr;
    }
    loaded.push_back(path);
    return h;
}

}  // namespace

bool openApi(VkApi& api, const char* driverPath, std::string& err) {
    closeApi(api);

    if (driverPath && driverPath[0]) {
        const std::string p = driverPath;
        const std::string dir = dirOf(p);
        std::set<std::string> visited;
        std::string firstErr;

        api.lib = loadTree(p, dir, false, visited, api.depsLoaded, api.depsMissing, firstErr);
        if (!api.lib) {
            if (!api.depsMissing.empty()) {
                err = "驱动包缺少依赖库：" + join(api.depsMissing, ", ") +
                      "（要把驱动包内所有 .so 解压到同一目录）";
            } else {
                err = std::string("dlopen 失败：") +
                      (firstErr.empty() ? dlerr() : firstErr.c_str());
                if (!api.depsLoaded.empty())
                    err += "；已预加载 " + std::to_string(api.depsLoaded.size()) + " 个依赖";
            }
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
