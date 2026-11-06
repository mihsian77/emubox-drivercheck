# emubox-drivercheck

EmuBox 自研的驱动实测内核。闭源，仅内部使用，不对外分发源码。

替代原先从 FauxVkTest 提取的第三方运行时（已从 EmuBox 移除），做法完全不同：

- 不自带 hook 运行时，不依赖 `libvulkan_samples.so`
- 离屏渲染一张**确定性测试图案** → 回读像素 → 人眼看图 + 程序逐像素判定
- 同一份 `dc_render.cpp` 既编进 APK，也在桌面 lavapipe 上逐像素验证

## 为什么是离屏渲染

驱动问题的典型表现是「黑屏 / 花屏 / 崩溃」，但真机上用 Surface 直呈会被窗口生命周期、
旋转、前后台切换污染，失败后分不清是驱动坏了还是窗口状态不对。离屏渲染没有这些变量：

- 画面固定、可回读、可哈希 → 「同一驱动两次结果是否一致」变成可判定事实
- 失败点精确到步骤（`create_instance` / `create_device` / `create_pipeline` / `queue_wait`）
- 不需要任何权限、不弹窗、可在子进程里跑，崩溃也不带走主进程

## 图案与判定

| 区域 | 检查的驱动缺陷 |
|---|---|
| 顶部三段纯色条（红/绿/蓝） | 通道顺序错位（BGR 当 RGB）、色域压缩 |
| 左下 8 级灰阶 | 量化/抖动异常、上下翻转（翻转会跑到左上） |
| 右上纯白/纯黑方块 | clamp 失效、值域偏移 |
| 渐变 + 8px 棋盘 | 光栅化对齐、插值精度 |

每项都有期望值和实测值，异常项直接给用户看「应为 G≈255 R/B≈0，实际 R=182 G=182」。

## 结构

```
lib/src/main/cpp/
  dc_render.cpp/.h     渲染内核（纯 C++，无 JNI/Android 依赖）
  dc_jni.cpp           JNI 薄桥，只搬结果
  dc_shaders_spv.h     由 GLSL 生成的 SPIR-V（勿手改）
  shaders/             源着色器
desktop/dc_host.cpp    桌面 harness：跑内核 + 21 项断言 + 输出 PPM
verify/run_desktop.sh  一键复核（编译着色器 → -Werror 编译 → lavapipe 实跑）
```

## 验证

```bash
./verify/run_desktop.sh
```

输出 `ALL CHECKS PASSED` 才算改动可用。当前基线：lavapipe (LLVM 20.1.2, Vulkan 1.4.318)
512×512 三帧全一致，单帧 ~14 ms。

## 集成到 EmuBox

1. CI 产出 `lib-release.aar`（artifact）
2. 放进 EmuBox `app/libs/`，`implementation(files("libs/lib-release.aar"))`
3. `DriverCheck.run()` 取 `Bitmap` + `RenderReport`，`DriverCheck.checkPattern()` 取逐项结论
4. UI 直接用 `DriverCheckSheet`（D 抽屉 + H 参考对照）

## 闭源约定

- 仓库私有，AAR 不发布到任何公共仓库
- native 用 `-fvisibility=hidden`，只导出两个 JNI 符号
- 不引用任何第三方闭源二进制；SPIR-V 由本仓库着色器生成
