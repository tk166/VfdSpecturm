// BuildStamp.h —— 编译时刻戳：让"面板上跑的是哪一版 DLL"永远可查
//
// 为什么需要它（真实教训，2026-09-28）：
//   EAPO 并不会因为你"在编辑器里关掉再打开这个 VSTPlugin 滤波器"就把我们的 DLL
//   从 audiodg 里卸载 —— 改名/重建 DLL 之后，正在跑的实例仍然是**旧镜像**。
//   当时面板上于是同时显示着 "EAPO 254 BANDS" 与 "render 4.0 ms"（旧代码），
//   而源码里的渲染器早就是格域版本、实测只要 0.287 ms。一次纯粹由"分不清哪一版
//   在跑"引起的误判。
//
// 所以：把编译时刻打进面板状态栏与调试日志。换 DLL 之后只要看一眼状态栏上的
// build 戳，就知道 audiodg/编辑器有没有真的换成新的。

#pragma once

#include <cstdint>

#define VFD_BUILD_STAMP __DATE__ " " __TIME__   // 例: "Sep 28 2026 21:53:57"

namespace vfdspec {

// 编译时刻的 FNV-1a 32 位指纹。用途：**面板与 audiodg 是两个进程、各自持有一份
// 加载时刻的镜像**——把指纹放进音频帧，面板就能当场发现"对面跑的是另一版 DLL"，
// 而不是像 2026-09-28 那样盯着一个 4 ms 的旧数字猜半天。
constexpr uint32_t buildIdOf(const char* s) {
    uint32_t h = 2166136261u;
    while (*s != '\0') {
        h ^= (uint8_t)(*s++);
        h *= 16777619u;
    }
    return h;
}

constexpr uint32_t kVfdBuildId = buildIdOf(VFD_BUILD_STAMP);

}  // namespace vfdspec
