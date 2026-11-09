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
    -Ilib/src/main/cpp desktop/dc_host.cpp \
    lib/src/main/cpp/dc_render.cpp lib/src/main/cpp/dc_vkapi.cpp \
    -ldl -o /tmp/dc_host

echo "== 3. 在 lavapipe 上实跑并逐像素断言 =="
VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}" /tmp/dc_host 3

echo "== 4. 系统 loader 与直载驱动必须得到同一结果 =="
LVP="$(ldconfig -p | awk '/libvulkan_lvp\.so/{print $NF; exit}')"
if [ -n "$LVP" ]; then
  H1=$(VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}" /tmp/dc_host 2 | grep '^hash=' | cut -d' ' -f1)
  H2=$(/tmp/dc_host 2 --driver "$LVP" | grep '^hash=' | cut -d' ' -f1)
  echo "  system: $H1"
  echo "  direct: $H2"
  [ "$H1" = "$H2" ] || { echo "  FAIL：两条加载路径结果不一致"; exit 1; }
  echo "  PASS：换驱动路径与系统路径像素完全一致"
else
  echo "  跳过：未找到 libvulkan_lvp.so"
fi
echo "== 5. 依赖闭包（驱动包自带 .so 能否被按目录找到）=="
gcc -shared -fPIC -Wl,-soname,libfake_dep.so \
    verify/fake/dep.c -o /tmp/libfake_dep.so
gcc -shared -fPIC verify/fake/driver.c -L/tmp -lfake_dep -o /tmp/libfake_driver.so
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Ilib/src/main/cpp \
    verify/dep_test.cpp lib/src/main/cpp/dc_vkapi.cpp -ldl -o /tmp/dep_test

mkdir -p /tmp/dc_case_ok /tmp/dc_case_miss
cp /tmp/libfake_dep.so /tmp/libfake_driver.so /tmp/dc_case_ok/
cp /tmp/libfake_driver.so /tmp/dc_case_miss/

if /tmp/dep_test /tmp/dc_case_miss/libfake_driver.so >/tmp/dc_miss.txt 2>&1; then
  echo "  FAIL：缺依赖时不应成功"; cat /tmp/dc_miss.txt; exit 1
fi
grep -q 'M libfake_dep.so' /tmp/dc_miss.txt || {
  echo "  FAIL：缺依赖没有报出库名"; cat /tmp/dc_miss.txt; exit 1; }
echo "  缺依赖 → 报出 libfake_dep.so"

if ! /tmp/dep_test /tmp/dc_case_ok/libfake_driver.so >/tmp/dc_ok.txt 2>&1; then
  echo "  FAIL：同目录有依赖时应加载成功"; cat /tmp/dc_ok.txt; exit 1
fi
grep -q 'L /tmp/dc_case_ok/libfake_dep.so' /tmp/dc_ok.txt || {
  echo "  FAIL：依赖没有被预加载"; cat /tmp/dc_ok.txt; exit 1; }
echo "  同目录 → 依赖已预加载，驱动可打开"

echo "== 通过：桌面复核与 APK 用的是同一份 dc_render.cpp =="
