cmake_minimum_required(VERSION 3.22)
project(drivercheck LANGUAGES CXX)

add_library(drivercheck SHARED
        dc_render.cpp
        dc_jni.cpp)

target_compile_features(drivercheck PRIVATE cxx_std_17)
target_compile_options(drivercheck PRIVATE -Wall -Wextra -fvisibility=hidden -fvisibility-inlines-hidden)

# libvulkan 由 NDK 提供 stub，运行时由系统/自定义 loader 解析
target_link_libraries(drivercheck PRIVATE vulkan log)
