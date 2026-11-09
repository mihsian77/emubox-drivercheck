# emubox-drivercheck 设计：驱动准入判定器

> 2026-11-09。M1 已交付（离屏渲染 + 13 项断言 + 换驱动），本文定义 M2–M4。

## 一句话定位

不是「Vulkan 跑分工具」，是**驱动准入判定器**：拿到一个驱动包，回答三件事——能不能装、能不能跑、跑出来对不对。

和 DriverLab 的区别不在功能多少，在**判定对象**：它回答「这个驱动在 CTS 里过了多少」，我们回答「这个驱动能不能进 EmuBox 的下载列表」。

## 三层判定

| 层 | 判什么 | 手段 | 输出 |
|---|---|---|---|
| **L1 准入** | 包装得上吗 | 静态，不执行 | ICD 符号 / 依赖闭包 / ABI / md5 |
| **L2 渲染** | 跑得出图吗 | 离屏渲染 + 逐像素断言 | 13 项 expected/actual + 像素哈希 |
| **L3 画像** | 能跑什么游戏 | 能力查询 + DXVK/VKD3D 必要条件 | 格式覆盖 / 特性位 / 帧时间 / 适用场景 |

### L1 准入（本次实施）

| 检查 | 判据 | 失败表现 |
|---|---|---|
| ICD 入口 | 导出 `vk_icdGetInstanceProcAddr` 或 `vkGetInstanceProcAddr` | 不是 Vulkan 实现 |
| 依赖闭包 | ELF `DT_NEEDED` 逐个解析，按驱动自身目录查找 | 缺 `libgsl.so` / `libllvm-glnext.so` |
| ABI | ELF class / machine = AArch64、minSdk ≤ 设备 | 装不上或加载即崩 |
| 完整性 | 与镜像清单 `md5` 比对 | 包被截断或替换 |

第三条依赖闭包是当前最大缺口：高通跨机型驱动包是一组 `.so`，直接 `dlopen` 主文件时，
动态链接器按**默认搜索路径**找依赖，找不到就 `open_driver` 失败。DriverLab 的做法是
「zip 里每个文件都保留，从驱动自己文件夹加载自己的库」——我们要补的就是这一步。

### L2 渲染（已有，补两项）

已实现：512×512 离屏、3 帧、哈希一致、13 项图案断言（色条 / 8 级灰阶 / 白黑块 / 棋盘）、
步骤级错误（`open_driver` → `create_instance` → … → `queue_wait`）。

补：
1. **重复一致性**：同一驱动跑 N 次，哈希集合大小为 1 才算稳（现在只比 2 帧）。
2. **A/B 并列**：同一图案、两个驱动，输出「哈希是否相同 / 哪几项断言不同」。

### L3 画像（M3）

- 250 项格式覆盖（`VkFormatProperties2` + 64 位标志，补历史债）
- DXVK / VKD3D 必要条件：geometryShader、tessellationShader、independentBlend、
  shaderStorageImageWriteWithoutFormat、samplerAnisotropy、robustBufferAccess、
  `VK_EXT_robustness2`、`VK_KHR_imageless_framebuffer`
- 单帧耗时
- 结论一句话：「适合 DXVK 11 / VKD3D 12 / 仅基础模拟器」

## 独家点

| 点 | 为什么别人没有 |
|---|---|
| **双端同源** | 桌面与 APK 编同一份 `dc_render.cpp`，CI 里用 lavapipe 逐像素回归；DriverLab 是端上专用 |
| **判定可解释** | 输出 expected/actual，不是通过率。用户能拿着「应为 G≈255，实际 R=182」去找驱动作者 |
| **驱动源图谱** | 52 个仓库 + PanVK 13 条 + panDXVK，能给驱动打「适配谁」的标签；DriverLab 不管来源 |
| **三级指纹闭环** | 包 md5 → 加载成功 → 渲染哈希，三段落任意一处不符都能定位 |
| **Mali 侧覆盖** | 我们独有 PanVK / panDXVK；DriverLab 与 FauxVkTest 都是 Adreno 为主 |

## 不做

| 项 | 原因 |
|---|---|
| Khronos CTS 320 万用例 | 体积与耗时远超收益，且 DriverLab 无 license 不能抄 |
| `.rd` 命令流录制 | 需要 freedreno libwrap，端上权限与体积代价大 |
| 驱动反编译 / 函数名映射 | 属于逆向工具，不是准入判定 |
| 速度跑分 | App 已有独立跑分，重复 |

## 路线

| 阶段 | 内容 | 验收 |
|---|---|---|
| M1 ✅ | 渲染 + 断言 + 换驱动 | 桌面 lavapipe 21 项全过，系统/直载哈希相同 |
| **M2（本次）** | 依赖闭包预加载 + A/B 对比 | 带 `libgsl.so` 的包能过 `open_driver`；缺失时能报出缺哪个 |
| M3 | L3 能力画像 + 64 位属性 | 与 `vulkaninfo --json` 比对 0 差异 |
| M4 | 接注册表，驱动页显示实测结论 | 列表里直接看到「实测通过 / 渲染异常」 |

## 边界

- 只读驱动，不改系统、不需要 root。
- 不下载驱动，由调用方传入路径。
- 不把测试结果上传到任何服务器。
