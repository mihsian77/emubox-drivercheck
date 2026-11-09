// 依赖闭包用例：验证「驱动包内自带 .so」能被按目录找到并预加载，
// 以及缺依赖时给出的是「缺哪个库」而不是一句 generic dlopen 错误。
//
// 由 verify/run_desktop.sh 第 5 步调用，两种场景各跑一次：
//   A 依赖与驱动同目录 → 必须 ok=1，depsLoaded 含 libfake_dep.so
//   B 目录里只有驱动     → 必须 ok=0，depsMissing 含 libfake_dep.so
#include <cstdio>

#include "dc_vkapi.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <driver.so>\n", argv[0]);
        return 2;
    }
    dc::VkApi api;
    std::string err;
    const bool ok = dc::openApi(api, argv[1], err);

    std::printf("ok=%d\n", ok ? 1 : 0);
    std::printf("error=%s\n", err.c_str());
    std::printf("loaded=%zu\n", api.depsLoaded.size());
    for (const auto& s : api.depsLoaded) std::printf("  L %s\n", s.c_str());
    std::printf("missing=%zu\n", api.depsMissing.size());
    for (const auto& s : api.depsMissing) std::printf("  M %s\n", s.c_str());
    if (ok) {
        std::printf("source=%s\n", api.source.c_str());
        std::printf("gipa=%d\n", api.gipa != nullptr ? 1 : 0);
    }
    return ok ? 0 : 1;
}
