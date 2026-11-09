// JNI 薄桥：只负责把 dc::render 的结果搬给 Kotlin，不含任何渲染逻辑。
// 图像走 byte[]，其余元数据走 JSON 字符串，避免复杂的对象构造。
#include <jni.h>

#include <cstdio>
#include <string>

#include "dc_render.h"

namespace {

dc::RenderResult g_last;  // 每次 nativeRun 覆盖；仅同进程同一时刻一次实测

void appendJsonString(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

}  // namespace

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_emubox_drivercheck_DriverCheck_nativeRun(JNIEnv* env, jobject /*thiz*/, jint width,
                                                 jint height, jint frames, jstring driverPath) {
    dc::RenderOptions o;
    o.width = width > 0 ? static_cast<uint32_t>(width) : 512;
    o.height = height > 0 ? static_cast<uint32_t>(height) : 512;
    o.frames = frames > 0 ? static_cast<uint32_t>(frames) : 2;
    const char* dp = nullptr;
    if (driverPath != nullptr) dp = env->GetStringUTFChars(driverPath, nullptr);
    o.driverPath = dp;
    g_last = dc::render(o);
    if (dp != nullptr) env->ReleaseStringUTFChars(driverPath, dp);
    if (!g_last.ok) return nullptr;
    const jsize n = static_cast<jsize>(g_last.rgba.size());
    jbyteArray out = env->NewByteArray(n);
    if (out == nullptr) return nullptr;
    env->SetByteArrayRegion(out, 0, n, reinterpret_cast<const jbyte*>(g_last.rgba.data()));
    return out;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_emubox_drivercheck_DriverCheck_nativeReport(JNIEnv* env, jobject /*thiz*/) {
    const dc::RenderResult& r = g_last;
    std::string j;
    j.reserve(1024);
    j += "{\"ok\":";
    j += r.ok ? "true" : "false";
    j += ",\"error\":";
    appendJsonString(j, r.error);
    j += ",\"width\":" + std::to_string(r.width);
    j += ",\"height\":" + std::to_string(r.height);
    j += ",\"hash\":\"" + std::to_string(r.hash) + "\"";
    j += ",\"hashRepeat\":\"" + std::to_string(r.hashRepeat) + "\"";
    j += ",\"consistent\":";
    j += r.consistent ? "true" : "false";
    j += ",\"renderMs\":" + std::to_string(r.renderMs);
    j += ",\"deviceName\":";
    appendJsonString(j, r.deviceName);
    j += ",\"driverVersion\":";
    appendJsonString(j, r.driverVersion);
    j += ",\"apiVersion\":";
    appendJsonString(j, r.apiVersion);
    j += ",\"vendorId\":" + std::to_string(r.vendorId);
    j += ",\"deviceId\":" + std::to_string(r.deviceId);
    j += ",\"depsLoaded\":[";
    for (size_t i = 0; i < r.depsLoaded.size(); ++i) {
        if (i) j += ',';
        appendJsonString(j, r.depsLoaded[i]);
    }
    j += "],\"depsMissing\":[";
    for (size_t i = 0; i < r.depsMissing.size(); ++i) {
        if (i) j += ',';
        appendJsonString(j, r.depsMissing[i]);
    }
    j += "],\"steps\":[";
    for (size_t i = 0; i < r.steps.size(); ++i) {
        if (i) j += ',';
        j += "{\"name\":";
        appendJsonString(j, r.steps[i].name);
        j += ",\"ok\":";
        j += r.steps[i].ok ? "true" : "false";
        j += ",\"detail\":";
        appendJsonString(j, r.steps[i].detail);
        j += '}';
    }
    j += "]}";
    return env->NewStringUTF(j.c_str());
}
