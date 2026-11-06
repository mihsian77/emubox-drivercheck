// DriverCheck 渲染实测内核 —— 公共接口
// 设计原则：不依赖 JNI / Android / 任何 UI 框架，纯 C++ 即可编译到桌面，
// 这样同一份渲染代码能在 lavapipe 上逐像素验证，再交叉编译进 APK。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dc {

struct RenderOptions {
    uint32_t width = 512;
    uint32_t height = 512;
    uint32_t frames = 2;      // 重复渲染帧数，用于像素一致性判定
    int deviceIndex = -1;     // -1 = 自动选择
    // 非空：直接 dlopen 这个 .so 当 Vulkan 实现（换驱动实测）；空：走系统 loader
    const char* driverPath = nullptr;
};

struct Step {
    std::string name;         // 步骤名，失败时直接给用户看
    bool ok = false;
    std::string detail;       // 成功填关键值，失败填 VkResult / 原因
};

struct RenderResult {
    bool ok = false;
    std::string error;        // 首个致命错误（人读）
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba; // width*height*4，行优先，自上而下

    uint64_t hash = 0;        // 首帧 FNV-1a 64
    uint64_t hashRepeat = 0;  // 末帧
    bool consistent = false;  // 所有帧像素完全相同

    std::string deviceName;
    std::string driverVersion; // Vulkan 编码的整数转可读
    std::string apiVersion;
    std::string driverInfo;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;

    double renderMs = 0;      // 单帧平均耗时
    std::vector<Step> steps;
};

// 执行一次渲染实测。线程安全（每次调用自建 instance/device）。
RenderResult render(const RenderOptions& opts);

// FNV-1a 64，供上层比对指纹
uint64_t hashBytes(const uint8_t* data, size_t size);

// 把 Vulkan 版本整数转 "1.3.280"
std::string versionString(uint32_t v);

}  // namespace dc
