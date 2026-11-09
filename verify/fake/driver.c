// 假驱动：依赖 libfake_dep.so，并导出 ICD 入口。
// 不实现真正的 Vulkan，只用于验证 dlopen 依赖闭包这条路径。
extern int dc_fake_dep(void);
typedef void (*PFN)(void);

static PFN fake_gipa(void* inst, const char* name) {
    (void)inst;
    (void)name;
    return (PFN)(void*)&dc_fake_dep;
}

__attribute__((visibility("default")))
PFN vk_icdGetInstanceProcAddr(void* inst, const char* name) {
    return fake_gipa(inst, name);
}
