#!/usr/bin/env bash
# 桌面复核：用 lavapipe 跑与 APK 完全相同的渲染内核，逐像素断言图案。
# 任何一次改动 dc_render.cpp / 着色器后都应执行，保证「手机上看到的」和「桌面验证过的」是同一份代码。
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== 1. 编译着色器 =="
if command -v glslangValidator >/dev/null; then
  ( cd lib/src/main/cpp/shaders
    glslangValidator -V dc_test.vert -o dc_test.vert.spv
    glslangValidator -V dc_test.frag -o dc_test.frag.spv )
  python3 - <<'PY'
import pathlib
out = ["// 由 glslangValidator 生成，勿手改（见 shaders/ 与 verify/run_desktop.sh）",
       "#pragma once", "#include <cstddef>", "#include <cstdint>", ""]
for name, sym in (("dc_test.vert.spv", "kVertSpv"), ("dc_test.frag.spv", "kFragSpv")):
    data = pathlib.Path("lib/src/main/cpp/shaders/" + name).read_bytes()
    out.append(f"static const uint32_t {sym}[] = {{")
    words = [data[i:i+4] for i in range(0, len(data), 4)]
    for i in range(0, len(words), 8):
        out.append("    " + ", ".join("0x%08xu" % int.from_bytes(w, "little") for w in words[i:i+8]) + ",")
    out.append("};")
    out.append(f"static const size_t {sym}Size = sizeof({sym});")
    out.append("")
pathlib.Path("lib/src/main/cpp/dc_shaders_spv.h").write_text("\n".join(out))
PY
else
  echo "  glslangValidator 不存在，沿用已生成的 dc_shaders_spv.h"
fi

echo "== 2. 编译桌面 harness（-Wall -Wextra）=="
g++ -std=c++17 -O2 -Wall -Wextra -Werror \
    -Ilib/src/main/cpp desktop/dc_host.cpp lib/src/main/cpp/dc_render.cpp \
    -lvulkan -o /tmp/dc_host

echo "== 3. 在 lavapipe 上实跑并逐像素断言 =="
VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}" /tmp/dc_host 3
echo "== 通过：桌面复核与 APK 用的是同一份 dc_render.cpp =="
