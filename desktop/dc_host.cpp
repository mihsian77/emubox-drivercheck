// 桌面验证 harness：用 lavapipe（或任意 Vulkan 实现）跑同一份渲染内核，
// 逐像素断言图案正确性，并输出 PPM 供人眼复核。
#include "dc_render.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;

static const uint8_t* px(const std::vector<uint8_t>& b, int w, int x, int y) {
    return &b[(static_cast<size_t>(y) * w + x) * 4];
}

static void expectNear(const char* what, int got, int want, int tol) {
    const bool ok = got >= want - tol && got <= want + tol;
    if (!ok) ++failures;
    std::printf("  [%s] %-28s got=%3d want=%3d±%d\n", ok ? "PASS" : "FAIL", what, got, want, tol);
}

int main(int argc, char** argv) {
    dc::RenderOptions o;
    o.width = 512;
    o.height = 512;
    o.frames = 3;
    if (argc > 1) o.frames = std::atoi(argv[1]);
    if (argc > 3 && std::strcmp(argv[2], "--driver") == 0) o.driverPath = argv[3];

    const dc::RenderResult r = dc::render(o);

    std::printf("== steps ==\n");
    for (const auto& s : r.steps) {
        std::printf("  %-16s %s  %s\n", s.name.c_str(), s.ok ? "ok  " : "FAIL", s.detail.c_str());
    }
    std::printf("== device ==\n  %s\n  api=%s driver=%s vendor=0x%04x device=0x%04x\n",
                r.deviceName.c_str(), r.apiVersion.c_str(), r.driverVersion.c_str(), r.vendorId,
                r.deviceId);
    if (!r.ok) {
        std::printf("RENDER FAILED: %s\n", r.error.c_str());
        return 1;
    }
    std::printf("hash=%016llx repeat=%016llx consistent=%d\n",
                static_cast<unsigned long long>(r.hash),
                static_cast<unsigned long long>(r.hashRepeat), r.consistent ? 1 : 0);

    // 输出 PPM 供人眼复核
    FILE* f = std::fopen("/tmp/dc_render.ppm", "wb");
    if (f) {
        std::fprintf(f, "P6\n%d %d\n255\n", r.width, r.height);
        for (size_t i = 0; i < r.rgba.size(); i += 4) std::fwrite(&r.rgba[i], 1, 3, f);
        std::fclose(f);
        std::printf("wrote /tmp/dc_render.ppm\n");
    }

    const int W = static_cast<int>(r.width), H = static_cast<int>(r.height);
    std::printf("== pattern checks ==\n");

    // 顶部色条：红 / 绿 / 蓝（顺序错位或通道颠倒会失败）
    const int yBar = H / 20;
    const uint8_t* a = px(r.rgba, W, W / 6, yBar);
    const uint8_t* b = px(r.rgba, W, W / 2, yBar);
    const uint8_t* c = px(r.rgba, W, W * 5 / 6, yBar);
    expectNear("bar-left-r", a[0], 255, 2);
    expectNear("bar-left-g", a[1], 0, 2);
    expectNear("bar-mid-g", b[1], 255, 2);
    expectNear("bar-mid-r", b[0], 0, 2);
    expectNear("bar-right-b", c[2], 255, 2);
    expectNear("bar-right-r", c[0], 0, 2);

    // 灰阶：8 级，每级 16px（512 的 1/32）；位置在左下 → 若上下翻转会跑空
    for (int i = 0; i < 8; ++i) {
        const int x = i * 16 + 8;
        const int y = H * 85 / 100;
        const uint8_t* g = px(r.rgba, W, x, y);
        char name[32];
        std::snprintf(name, sizeof(name), "gray-step%d", i);
        expectNear(name, g[0], static_cast<int>(i * 255 / 7), 3);
        if (g[0] != g[1] || g[1] != g[2]) {
            ++failures;
            std::printf("  [FAIL] %-28s not neutral: %d,%d,%d\n", name, g[0], g[1], g[2]);
        }
    }

    // 纯白 / 纯黑块
    const uint8_t* w = px(r.rgba, W, W * 85 / 100, H * 21 / 100);
    const uint8_t* k = px(r.rgba, W, W * 85 / 100, H * 38 / 100);
    expectNear("white-block-r", w[0], 255, 2);
    expectNear("white-block-g", w[1], 255, 2);
    expectNear("white-block-b", w[2], 255, 2);
    expectNear("black-block-r", k[0], 0, 2);
    expectNear("black-block-g", k[1], 0, 2);
    expectNear("black-block-b", k[2], 0, 2);

    // 方向：顶部条区不应出现在最后一行
    const uint8_t* bottom = px(r.rgba, W, W / 2, H - 2);
    const bool barAtBottom = bottom[0] > 200 && bottom[1] < 60 && bottom[2] < 60;
    std::printf("  [%s] %-28s %s\n", barAtBottom ? "FAIL" : "PASS", "orientation-not-flipped",
                barAtBottom ? "顶部色条跑到画面底部" : "ok");
    if (barAtBottom) ++failures;

    std::printf(failures == 0 ? "\nALL CHECKS PASSED\n" : "\n%d CHECK(S) FAILED\n", failures);
    return failures == 0 ? 0 : 2;
}
