#include "dc_render.h"
#include "dc_shaders_spv.h"
#include "dc_vkapi.h"

#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace dc {

uint64_t hashBytes(const uint8_t* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::string versionString(uint32_t v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u", VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v),
                  VK_VERSION_PATCH(v));
    return std::string(buf);
}

namespace {

const char* resName(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        default: break;
    }
    static thread_local char buf[32];
    std::snprintf(buf, sizeof(buf), "VkResult(%d)", static_cast<int>(r));
    return buf;
}

uint32_t findMemoryType(const VkApi& api, VkPhysicalDevice dev, uint32_t bits,
                        VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    api.vkGetPhysicalDeviceMemoryProperties(dev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return UINT32_MAX;
}

// 所有 Vulkan 句柄集中持有，出错时按序销毁
struct Handles {
    VkApi api;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory bufMem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    void destroy() {
        if (device != VK_NULL_HANDLE) {
            if (fence) api.vkDestroyFence(device, fence, nullptr);
            if (pool) api.vkDestroyCommandPool(device, pool, nullptr);
            if (buf) api.vkDestroyBuffer(device, buf, nullptr);
            if (bufMem) api.vkFreeMemory(device, bufMem, nullptr);
            if (pipeline) api.vkDestroyPipeline(device, pipeline, nullptr);
            if (layout) api.vkDestroyPipelineLayout(device, layout, nullptr);
            if (vert) api.vkDestroyShaderModule(device, vert, nullptr);
            if (frag) api.vkDestroyShaderModule(device, frag, nullptr);
            if (fb) api.vkDestroyFramebuffer(device, fb, nullptr);
            if (pass) api.vkDestroyRenderPass(device, pass, nullptr);
            if (view) api.vkDestroyImageView(device, view, nullptr);
            if (image) api.vkDestroyImage(device, image, nullptr);
            if (imageMem) api.vkFreeMemory(device, imageMem, nullptr);
            api.vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) api.vkDestroyInstance(instance, nullptr);
        closeApi(api);
    }
};

}  // namespace

RenderResult render(const RenderOptions& opts) {
    RenderResult r;
    const uint32_t W = opts.width ? opts.width : 512;
    const uint32_t H = opts.height ? opts.height : 512;
    const uint32_t frames = opts.frames ? opts.frames : 1;
    r.width = W;
    r.height = H;

    Handles h;
    VkApi& api = h.api;
    auto stepOk = [&r](const char* name, std::string detail) {
        r.steps.push_back({name, true, std::move(detail)});
    };
    auto fail = [&r, &h](const char* name, std::string detail) -> RenderResult& {
        r.steps.push_back({name, false, detail});
        r.error = std::string(name) + ": " + detail;
        r.ok = false;
        h.destroy();
        return r;
    };

    const auto t0 = std::chrono::steady_clock::now();

    // ---- 1. instance ----
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "DriverCheck";
    app.pEngineName = "DriverCheck";
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    std::string openErr;
    // 依赖闭包要在 fail() 之前取出：fail() 会 closeApi 并清空 api。
    const bool opened = openApi(h.api, opts.driverPath, openErr);
    r.depsLoaded = h.api.depsLoaded;
    r.depsMissing = h.api.depsMissing;
    if (!opened) return fail("open_driver", openErr);
    stepOk("open_driver", h.api.source);

    VkResult vr = api.vkCreateInstance(&ici, nullptr, &h.instance);
    if (vr != VK_SUCCESS) return fail("create_instance", resName(vr));
    {
        const char* miss = resolveInstance(h.api, h.instance);
        if (miss[0]) return fail("resolve_instance", std::string("缺函数 ") + miss);
    }
    stepOk("create_instance", "ok");

    // ---- 2. physical device ----
    uint32_t devCount = 0;
    vr = api.vkEnumeratePhysicalDevices(h.instance, &devCount, nullptr);
    if (vr != VK_SUCCESS) return fail("enumerate_devices", resName(vr));
    if (devCount == 0) return fail("enumerate_devices", "no Vulkan device (驱动未安装或加载失败)");
    std::vector<VkPhysicalDevice> devs(devCount);
    api.vkEnumeratePhysicalDevices(h.instance, &devCount, devs.data());

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    uint32_t gfxFamily = UINT32_MAX;
    for (uint32_t i = 0; i < devCount; ++i) {
        if (opts.deviceIndex >= 0 && static_cast<uint32_t>(opts.deviceIndex) != i) continue;
        uint32_t qCount = 0;
        api.vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qCount);
        api.vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qCount, qs.data());
        for (uint32_t q = 0; q < qCount; ++q) {
            if (qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                phys = devs[i];
                gfxFamily = q;
                break;
            }
        }
        if (phys != VK_NULL_HANDLE) break;
    }
    if (phys == VK_NULL_HANDLE) return fail("pick_device", "没有支持图形队列的 Vulkan 设备");

    VkPhysicalDeviceProperties dp{};
    api.vkGetPhysicalDeviceProperties(phys, &dp);
    r.deviceName = dp.deviceName;
    r.apiVersion = versionString(dp.apiVersion);
    r.driverVersion = versionString(dp.driverVersion);
    r.vendorId = dp.vendorID;
    r.deviceId = dp.deviceID;
    stepOk("pick_device", r.deviceName + " / " + r.apiVersion);

    // 格式必须是 R8G8B8A8_UNORM：规范要求所有实现支持它作 color attachment，
    // 不支持说明驱动有缺陷——这本身就是结论，不做格式降级来掩盖。
    VkFormatProperties fp{};
    api.vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_R8G8B8A8_UNORM, &fp);
    const VkFormatFeatureFlags need =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if ((fp.optimalTilingFeatures & need) != need) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "R8G8B8A8_UNORM optimal=0x%x 缺 color/transfer 位",
                      fp.optimalTilingFeatures);
        return fail("check_format", buf);
    }
    stepOk("check_format", "R8G8B8A8_UNORM ok");

    // ---- 3. device + queue ----
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = gfxFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    vr = api.vkCreateDevice(phys, &dci, nullptr, &h.device);
    if (vr != VK_SUCCESS) return fail("create_device", resName(vr));
    {
        const char* miss = resolveDevice(h.api, h.device);
        if (miss[0]) return fail("resolve_device", std::string("缺函数 ") + miss);
    }
    VkQueue queue = VK_NULL_HANDLE;
    api.vkGetDeviceQueue(h.device, gfxFamily, 0, &queue);
    stepOk("create_device", "queue family " + std::to_string(gfxFamily));

    // ---- 4. image ----
    VkImageCreateInfo imgc{};
    imgc.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgc.imageType = VK_IMAGE_TYPE_2D;
    imgc.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgc.extent = {W, H, 1};
    imgc.mipLevels = 1;
    imgc.arrayLayers = 1;
    imgc.samples = VK_SAMPLE_COUNT_1_BIT;
    imgc.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imgc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vr = api.vkCreateImage(h.device, &imgc, nullptr, &h.image);
    if (vr != VK_SUCCESS) return fail("create_image", resName(vr));

    VkMemoryRequirements mr{};
    api.vkGetImageMemoryRequirements(h.device, h.image, &mr);
    uint32_t mt = findMemoryType(api, phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) return fail("image_memory", "无 DEVICE_LOCAL 内存类型");
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mt;
    vr = api.vkAllocateMemory(h.device, &mai, nullptr, &h.imageMem);
    if (vr != VK_SUCCESS) return fail("image_memory", resName(vr));
    vr = api.vkBindImageMemory(h.device, h.image, h.imageMem, 0);
    if (vr != VK_SUCCESS) return fail("bind_image", resName(vr));

    VkImageViewCreateInfo ivc{};
    ivc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivc.image = h.image;
    ivc.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivc.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vr = api.vkCreateImageView(h.device, &ivc, nullptr, &h.view);
    if (vr != VK_SUCCESS) return fail("create_view", resName(vr));
    stepOk("create_target", std::to_string(W) + "x" + std::to_string(H));

    // ---- 5. render pass ----
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_R8G8B8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpc{};
    rpc.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpc.attachmentCount = 1;
    rpc.pAttachments = &att;
    rpc.subpassCount = 1;
    rpc.pSubpasses = &sub;
    rpc.dependencyCount = 1;
    rpc.pDependencies = &dep;
    vr = api.vkCreateRenderPass(h.device, &rpc, nullptr, &h.pass);
    if (vr != VK_SUCCESS) return fail("create_renderpass", resName(vr));

    VkFramebufferCreateInfo fbc{};
    fbc.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbc.renderPass = h.pass;
    fbc.attachmentCount = 1;
    fbc.pAttachments = &h.view;
    fbc.width = W;
    fbc.height = H;
    fbc.layers = 1;
    vr = api.vkCreateFramebuffer(h.device, &fbc, nullptr, &h.fb);
    if (vr != VK_SUCCESS) return fail("create_framebuffer", resName(vr));
    stepOk("create_renderpass", "ok");

    // ---- 6. pipeline ----
    auto makeModule = [&](const uint32_t* code, size_t bytes, VkShaderModule* out) {
        VkShaderModuleCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        sci.codeSize = bytes;
        sci.pCode = code;
        return api.vkCreateShaderModule(h.device, &sci, nullptr, out);
    };
    vr = makeModule(kVertSpv, kVertSpvSize, &h.vert);
    if (vr != VK_SUCCESS) return fail("shader_vert", resName(vr));
    vr = makeModule(kFragSpv, kFragSpvSize, &h.frag);
    if (vr != VK_SUCCESS) return fail("shader_frag", resName(vr));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = h.vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = h.frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{0, 0, static_cast<float>(W), static_cast<float>(H), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_FALSE;
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkPipelineLayoutCreateInfo plc{};
    plc.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    vr = api.vkCreatePipelineLayout(h.device, &plc, nullptr, &h.layout);
    if (vr != VK_SUCCESS) return fail("pipeline_layout", resName(vr));

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.layout = h.layout;
    gp.renderPass = h.pass;
    gp.subpass = 0;
    vr = api.vkCreateGraphicsPipelines(h.device, VK_NULL_HANDLE, 1, &gp, nullptr, &h.pipeline);
    if (vr != VK_SUCCESS) return fail("create_pipeline", resName(vr));
    stepOk("create_pipeline", "fullscreen triangle");

    // ---- 7. readback buffer ----
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(W) * H * 4;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vr = api.vkCreateBuffer(h.device, &bci, nullptr, &h.buf);
    if (vr != VK_SUCCESS) return fail("create_buffer", resName(vr));

    VkMemoryRequirements bmr{};
    api.vkGetBufferMemoryRequirements(h.device, h.buf, &bmr);
    uint32_t bt = findMemoryType(api, phys, bmr.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (bt == UINT32_MAX) return fail("buffer_memory", "无 HOST_VISIBLE|COHERENT 内存类型");
    VkMemoryAllocateInfo bmai{};
    bmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bmai.allocationSize = bmr.size;
    bmai.memoryTypeIndex = bt;
    vr = api.vkAllocateMemory(h.device, &bmai, nullptr, &h.bufMem);
    if (vr != VK_SUCCESS) return fail("buffer_memory", resName(vr));
    vr = api.vkBindBufferMemory(h.device, h.buf, h.bufMem, 0);
    if (vr != VK_SUCCESS) return fail("bind_buffer", resName(vr));

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = gfxFamily;
    vr = api.vkCreateCommandPool(h.device, &pci, nullptr, &h.pool);
    if (vr != VK_SUCCESS) return fail("command_pool", resName(vr));

    VkCommandBufferAllocateInfo cba2{};
    cba2.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cba2.commandPool = h.pool;
    cba2.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba2.commandBufferCount = 1;
    vr = api.vkAllocateCommandBuffers(h.device, &cba2, &h.cmd);
    if (vr != VK_SUCCESS) return fail("command_buffer", resName(vr));

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vr = api.vkCreateFence(h.device, &fci, nullptr, &h.fence);
    if (vr != VK_SUCCESS) return fail("fence", resName(vr));

    // ---- 8. 渲染 frames 次 ----
    std::vector<uint8_t> firstFrame;
    uint64_t lastHash = 0;
    bool allSame = true;
    std::vector<uint8_t> pixels(static_cast<size_t>(bytes));
    auto renderStart = std::chrono::steady_clock::now();

    for (uint32_t f = 0; f < frames; ++f) {
        vr = api.vkResetCommandBuffer(h.cmd, 0);
        if (vr != VK_SUCCESS) return fail("reset_command", resName(vr));

        VkCommandBufferBeginInfo cbbi{};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vr = api.vkBeginCommandBuffer(h.cmd, &cbbi);
        if (vr != VK_SUCCESS) return fail("begin_command", resName(vr));

        VkClearValue clear{};
        clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderPassBeginInfo rpb{};
        rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpb.renderPass = h.pass;
        rpb.framebuffer = h.fb;
        rpb.renderArea = {{0, 0}, {W, H}};
        rpb.clearValueCount = 1;
        rpb.pClearValues = &clear;
        api.vkCmdBeginRenderPass(h.cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
        api.vkCmdBindPipeline(h.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, h.pipeline);
        api.vkCmdDraw(h.cmd, 3, 1, 0, 0);
        api.vkCmdEndRenderPass(h.cmd);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {W, H, 1};
        api.vkCmdCopyImageToBuffer(h.cmd, h.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, h.buf, 1,
                               &region);

        vr = api.vkEndCommandBuffer(h.cmd);
        if (vr != VK_SUCCESS) return fail("end_command", resName(vr));

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &h.cmd;
        vr = api.vkQueueSubmit(queue, 1, &si, h.fence);
        if (vr != VK_SUCCESS) return fail("queue_submit", resName(vr));

        vr = api.vkWaitForFences(h.device, 1, &h.fence, VK_TRUE, 5000000000ull);
        if (vr == VK_TIMEOUT) return fail("queue_wait", "5s 超时（驱动挂起或 GPU hang）");
        if (vr != VK_SUCCESS) return fail("queue_wait", resName(vr));
        api.vkResetFences(h.device, 1, &h.fence);

        void* mapped = nullptr;
        vr = api.vkMapMemory(h.device, h.bufMem, 0, bytes, 0, &mapped);
        if (vr != VK_SUCCESS) return fail("map_memory", resName(vr));
        std::memcpy(pixels.data(), mapped, static_cast<size_t>(bytes));
        api.vkUnmapMemory(h.device, h.bufMem);

        const uint64_t hh = hashBytes(pixels.data(), pixels.size());
        if (f == 0) {
            firstFrame = pixels;
            r.hash = hh;
        } else if (hh != r.hash) {
            allSame = false;
        }
        lastHash = hh;
    }
    const auto renderEnd = std::chrono::steady_clock::now();
    r.renderMs =
        std::chrono::duration<double, std::milli>(renderEnd - renderStart).count() / frames;
    r.hashRepeat = lastHash;
    r.consistent = allSame;
    stepOk("render", std::to_string(frames) + " 帧 / " + std::to_string(static_cast<int>(r.renderMs)) +
                         " ms·帧");

    // ---- 9. 基础自检：画面不应全黑 / 全同色 ----
    {
        uint64_t sum = 0, sumSq = 0;
        const size_t n = pixels.size() / 4;
        for (size_t i = 0; i < n; ++i) {
            const uint32_t v = pixels[i * 4] + pixels[i * 4 + 1] + pixels[i * 4 + 2];
            sum += v;
            sumSq += static_cast<uint64_t>(v) * v;
        }
        const double mean = static_cast<double>(sum) / n;
        const double var = static_cast<double>(sumSq) / n - mean * mean;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "mean=%.1f var=%.1f", mean, var);
        if (mean < 1.0) return fail("selfcheck", std::string("全黑画面 (") + buf + ")");
        if (var < 100.0) return fail("selfcheck", std::string("画面无差异 (") + buf + ")");
        stepOk("selfcheck", buf);
    }

    r.rgba = std::move(firstFrame);
    r.ok = true;
    const auto t1 = std::chrono::steady_clock::now();
    r.steps.push_back({"total", true,
                       std::to_string(static_cast<int>(
                           std::chrono::duration<double, std::milli>(t1 - t0).count())) +
                           " ms"});
    h.destroy();
    return r;
}

}  // namespace dc
