#version 450
// 驱动实测图案：每个区域都对应一个可判定的驱动缺陷，便于人眼与程序双重校验。
// 坐标约定：vUV.y=0 在画面顶部（Vulkan NDC y 向下），s 即左上原点的视觉坐标。
//   顶部 12% 三段纯色条  → 通道顺序错位（BGR/RGB 颠倒）会立刻看出来
//   左下 25% 8 级灰阶    → 量化/抖动异常、上下翻转（翻转后会跑到左上）
//   右上 纯白/纯黑方块    → clamp 与范围压缩异常
//   其余 渐变 + 8px 棋盘  → 光栅化对齐、插值精度
layout(location = 0) in vec3 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec2 s = vUV;  // Vulkan NDC: vUV.y=0 已在画面顶部

    vec3 c = mix(vColor, vColor * 0.55 + 0.12, float(((px.x >> 3) ^ (px.y >> 3)) & 1));

    if (s.y < 0.12) {
        int band = int(clamp(s.x * 3.0, 0.0, 2.999));
        c = band == 0 ? vec3(1.0, 0.0, 0.0)
          : band == 1 ? vec3(0.0, 1.0, 0.0)
                      : vec3(0.0, 0.0, 1.0);
    } else if (s.x < 0.25 && s.y > 0.75) {
        int step8 = clamp(int(s.x * 32.0), 0, 7);
        c = vec3(float(step8) / 7.0);
    } else if (s.x > 0.78 && s.x < 0.92 && s.y > 0.14 && s.y < 0.28) {
        c = vec3(1.0);
    } else if (s.x > 0.78 && s.x < 0.92 && s.y > 0.31 && s.y < 0.45) {
        c = vec3(0.0);
    }

    fragColor = vec4(c, 1.0);
}
