#version 450
// 全屏三角形：不依赖顶点缓冲，用 gl_VertexIndex 生成，避免 VBO/绑定布局引入额外失败点
layout(location = 0) out vec3 vColor;
layout(location = 1) out vec2 vUV;

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vUV = p;
    vColor = vec3(p, 1.0);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
