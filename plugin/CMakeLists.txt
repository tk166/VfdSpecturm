# JUCE VST2 插件（EAPO 侧的频谱数据生产者）
#
# 由顶层 CMakeLists 在 -DVFD_BUILD_PLUGIN=ON 时引入： tools\build.bat plugin
#
# 只产出 VST2：EAPO 的加载器是 GetProcAddress(module,"VSTPluginMain")，
# 整个 1.4.0 树里没有任何 VST3 符号（VST3 只存在于第三方 fork）。

set(VFD_JUCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/JUCE)
set(VFD_VST2_SDK ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/vstsdk2)

if(NOT EXISTS ${VFD_JUCE_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "JUCE submodule 缺失：git submodule update --init --depth 1 third_party/JUCE")
endif()
if(NOT EXISTS ${VFD_VST2_SDK}/pluginterfaces/vst2.x/aeffect.h)
    message(FATAL_ERROR "VST2 SDK 缺失：需要 pluginterfaces/vst2.x/aeffect.h")
endif()

# ---------------------------------------------------------------------------
# 静态 CRT（/MT）—— 必须。
#
# 原因（实测踩到）：EAPO 自己的安装目录里带着一份 MSVCP140.DLL (14.29)，而 audiodg
# 加载 EAPO 的 APO 时，DLL 搜索路径会先命中那个目录。如果我们的插件用动态运行时而
# 构建工具链是更新的 MSVC(14.50)，就会出现 CRT 混用 —— 事件日志里能看到
# "audiodg.exe / MSVCP140.dll / 0xc0000005"，以及 "Plugin crashed during
# initialization"。参考仓库(WindowsCoaxVolumeControllerVST)也正是为此强制 /MT，
# 它 CMakeLists 里的注释是"静态链接，防止缺 dll"。
#
# 注意：这两个变量要在 add_subdirectory(JUCE) 与 juce_add_plugin 之前设置才生效。
# 它们只影响本目录之后创建的目标，所以顶层那两个控制台工具仍然是默认运行时。
# ---------------------------------------------------------------------------
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(JUCE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)

add_subdirectory(${VFD_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
juce_set_vst2_sdk_path(${VFD_VST2_SDK})

juce_add_plugin(vfd_eapo
    COMPANY_NAME "Vibe"
    PLUGIN_MANUFACTURER_CODE Vfd1
    PLUGIN_CODE Vfsp
    FORMATS VST
    # 产物名刻意不留空格：DLL 文件名带空格会让 schtasks 的 /tr、EAPO 的 config 行、
    # 各种脚本统统需要引号转义，是典型的埋坑设计。显示名(UI里)才允许有空格。
    PRODUCT_NAME "VfdSpectrumSender"
    VERSION 0.1.0
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(vfd_eapo PRIVATE
    PluginProcessor.cpp
    PluginProcessor.h
    PluginEditor.cpp
    PluginEditor.h
    SpectrumSender.h
    PanelConsumer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/vfd_render.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/vfd_dsp.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/vfd_fft.h)

# 直接复用传输层头文件（vfd_pipe.h）
target_include_directories(vfd_eapo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_compile_definitions(vfd_eapo PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS)

target_link_libraries(vfd_eapo PRIVATE
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
    advapi32)          # sddl.h / ConvertStringSecurityDescriptorToSecurityDescriptorW

if(MSVC)
    # VST2 SDK 头文件在 MSVC 下会刷警告（参考仓库也是这么屏蔽的）
    target_compile_options(vfd_eapo PRIVATE /wd4100 /wd4828)
endif()
