cmake_minimum_required(VERSION 3.20)
project(VfdSpectrum LANGUAGES C CXX)   # JUCE 的部分模块需要 C 编译器

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # 源码里有中文注释, 必须显式声明 UTF-8
    add_compile_options(/utf-8 /W4 /permissive-)
endif()

# ---------------------------------------------------------------------------
# 跨进程传输层 (纯 Win32, 无第三方依赖)
# ---------------------------------------------------------------------------
add_library(vfd_pipe INTERFACE)
target_include_directories(vfd_pipe INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vfd_pipe INTERFACE advapi32)
target_compile_definitions(vfd_pipe INTERFACE
    NOMINMAX                  # windows.h 的 min/max 宏会吃掉 std::min/std::max
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS)

# 消费者: 模拟 viewer 侧 (管道服务端)
add_executable(pipe_server src/pipe_server.cpp)
target_link_libraries(pipe_server PRIVATE vfd_pipe)

# 生产者: 模拟插件侧 worker 线程 (管道客户端)
add_executable(pipe_producer src/pipe_producer.cpp)
target_link_libraries(pipe_producer PRIVATE vfd_pipe)

# VST2 宿主桩: 用 EAPO 的契约加载并驱动插件 DLL (含 effEditGetRect 崩溃路径)
# 不需要 JUCE —— 直接用 VST2 SDK 头文件
add_executable(vst2_host_stub src/vst2_host_stub.cpp)
target_include_directories(vst2_host_stub PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vstsdk2/pluginterfaces/vst2.x)
target_compile_definitions(vst2_host_stub PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(vst2_host_stub PRIVATE /wd4828)   # VST2 SDK 头的非 UTF-8 警告
endif()

# 渲染器基准: 分阶段计时 + 黄金帧导出 (性能回归 / 改动前后逐字节对照)
add_executable(render_bench tools/render_bench.cpp)
target_include_directories(render_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(render_bench PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# 加窗/泄漏体检: 打印三种窗下主瓣与旁瓣的相对电平 (dBc)
add_executable(fft_window_check tools/fft_window_check.cpp)
target_include_directories(fft_window_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(fft_window_check PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# DSP 体检: 直接把音频喂给 Analyzer, 打印"每个频段到底有没有数"。
# 用途: 2026-09-29 的 bug(第 0 档负责的 495Hz~20kHz 一个频段都不更新, 屏上放音乐
# 一刀切)就是靠它一眼看出来的 —— 它同时验证"静音时会正常掉到底"。
add_executable(dsp_probe tools/dsp_probe.cpp)
target_include_directories(dsp_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(dsp_probe PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# 诊断探针: 打印镜像 VST2 SDK 里各 opcode 的实际数值, 与 VST 2.4 规范值对照。
# 用途: 若枚举值被 DECLARE_VST_DEPRECATED 宏"平移"了, 插件与 EAPO 就会说不同方言。
add_executable(vst2_opcode_probe EXCLUDE_FROM_ALL tools/vst2_opcode_probe.cpp)
target_include_directories(vst2_opcode_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vstsdk2/pluginterfaces/vst2.x)
target_compile_definitions(vst2_opcode_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(vst2_opcode_probe PRIVATE /wd4828)
endif()

# ---------------------------------------------------------------------------
# JUCE 插件 (下一步再加)
#   子模块已就位:
#     third_party/JUCE       @ 29396c22c93392d6738e021b83196283d6e4d850  (8.0.12)
#     third_party/vstsdk2    @ 6c7dc1464946537bddb3eea39eac176d02e72f68  (VST2.4 SDK)
#   接法 (参考 WindowsCoaxVolumeControllerVST):
#     add_subdirectory(third_party/JUCE)
#     juce_set_vst2_sdk_path(${CMAKE_CURRENT_SOURCE_DIR}/third_party/vstsdk2)
#     juce_add_plugin(vfd_eapo FORMATS VST ...)
#   EAPO 只认 VST2, 所以 VST3 可以不产出。
# ---------------------------------------------------------------------------
option(VFD_BUILD_PLUGIN "Build the JUCE VST2 plugin (needs JUCE submodule)" OFF)
if(VFD_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()
