// vst2_host_stub.cpp —— 用 EAPO 的方式加载并驱动我们的 VST2 插件
//
// 为什么需要它：一个从没被执行过的 DLL 直接丢进 EAPO 是危险的——插件与 audiodg 同生共死，
// 加载即崩会带走整个系统音频。
//
// 这个桩**逐行照抄 EAPO 1.3 的 VSTPluginInstance::callback() 与 VSTPluginFilter::initialize()**
// 的语义（helpers/VSTPluginInstance.cpp、filters/VSTPluginFilter.cpp），包括几个容易漏掉的细节：
//   * audioMasterVersion -> 2400
//   * audioMasterCurrentId -> 'EAPO'
//   * audioMasterGetProductString -> strcpy_s((char*)ptr, 64, "Equalizer APO")
//   * audioMasterPinConnected -> index < usedChannelCount ? 0 : 1   （usedChannelCount 在 effOpen 之后才赋值！）
//   * audioMasterGetSampleRate / GetCurrentProcessLevel / GetLanguage 都要经过
//     `(VSTPluginInstance*)effect->user` —— 而 EAPO 是**在 VSTPluginMain 返回之后**才设置
//     effect->user。所以要如实模拟"user 还是原值"的那段时间。
//   * audioMasterCanDo 只对 startStopProcess / sizeWindow 返回 1；GetTime / Idle / WantMidi 一律 0
//   * 关键路径：effEditGetRect 在 effEditOpen 之前（EAPO ticket 138 的崩溃点）
//   * 关键复现：effectChannelCount = max(numInputs, numOutputs); effectCount = (channelCount +
//     effectChannelCount - 1) / effectChannelCount —— **effectChannelCount==0 就是整数除零
//     (0xc0000094)，会把 audiodg 打死**。桩会把这个算式复算一遍并明确报警。
//
// 用法: vst2_host_stub <插件.dll> [运行秒数=4]
//       set VFD_DEBUG=1      插件侧链路日志
//       set VFD_HOSTLOG=1    宿主回调逐条 opcode 日志（排查宿主契约问题用）

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// 注意：基础 opcode 在 aeffect.h，扩展项（audioMasterCanDo / effGetEffectName / ERect 等）
// 在 aeffectx.h —— 后者会自己包含前者。
#include "aeffectx.h"

namespace {
// 与 vfd_pipe.h 里的 currentSessionId() 等价，但这里不想把管道头也拉进来
unsigned vfdspec_sessionId() {
    DWORD sid = 0xFFFFFFFFu;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    return (unsigned)sid;
}

constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 512;

bool hostLogEnabled() {
    static const bool on = (std::getenv("VFD_HOSTLOG") != nullptr);
    return on;
}

// 对应 EAPO 的 VSTPluginInstance 里回调要用到的那几个成员
struct FakeInstance {
    double sampleRate = 0.0;      // EAPO 在 prepareForProcessing 之前就是 0
    int processLevel = 2;         // 音频侧是 2 = kVstProcessLevelRealtime
    int language = 1;
    int usedChannelCount = 0;     // effOpen 之后才赋值
};
FakeInstance g_inst;

VstIntPtr VSTCALLBACK hostCallback(AEffect* effect, VstInt32 opcode, VstInt32 index,
                                  VstIntPtr value, void* ptr, float opt) {
    // EAPO: VSTPluginInstance* instance = effect != NULL ? (VSTPluginInstance*)effect->user : NULL;
    FakeInstance* inst = (effect != nullptr) ? reinterpret_cast<FakeInstance*>(effect->user)
                                            : nullptr;
    if (hostLogEnabled()) {
        static int n = 0;
        if (n++ < 400) {
            std::fprintf(stderr, "[hostcb] op=%d index=%d value=%lld ptr=%p user=%p\n",
                         (int)opcode, (int)index, (long long)value, ptr,
                         effect ? effect->user : nullptr);
            std::fflush(stderr);
        }
    }
    (void)opt;

    switch (opcode) {
        // 注意：这里**用 VST 2.4 规范的数值**而不是本镜像 SDK 的枚举名。
        // 因为 aeffectx.h 用 DECLARE_VST_DEPRECATED() 把若干项从枚举里删掉了，
        // 而 "audioMasterWantMidi = audioMasterPinConnected + 2" 这种写法会让后面的
        // 枚举值整体平移（tools/vst2_opcode_probe.cpp 会把实际值和规范值对照打印）。
        // 宿主桩必须说规范方言，才能如实模拟 EAPO。
        case 1:     return 2400;                     // audioMasterVersion
        case 2:     return 0x4541504F;               // audioMasterCurrentId ('EAPO')
        case 33:                                     // audioMasterGetProductString
            if (ptr != nullptr) strcpy_s(static_cast<char*>(ptr), 64, "Equalizer APO");
            return 1;
        case 34:    return 0x01030000;               // audioMasterGetVendorVersion
        case 4:                                      // audioMasterPinConnected
            if (inst != nullptr) return index < inst->usedChannelCount ? 0 : 1;
            return 0;
        case 14:                                     // audioMasterNeedIdle
            return effect != nullptr ? effect->dispatcher(effect, 19 /*effIdle*/, 0, 0, nullptr, 0.0f) : 0;
        case 42:                                     // audioMasterUpdateDisplay
            return effect != nullptr ? effect->dispatcher(effect, 19 /*effEditIdle*/, 0, 0, nullptr, 0.0f) : 0;
        case 7:     return 0;                        // audioMasterGetTime
        case 16:                                     // audioMasterGetSampleRate
            return inst != nullptr ? (VstIntPtr)inst->sampleRate : 0;
        case 23:                                     // audioMasterGetCurrentProcessLevel
            return inst != nullptr ? inst->processLevel : 0;
        case 38:                                     // audioMasterGetLanguage
            return inst != nullptr ? inst->language : 0;
        case 22:    return 1;                        // audioMasterWillReplaceOrAccumulate
        case 15:    return 1;                        // audioMasterSizeWindow
        case 37: {                                   // audioMasterCanDo
            const char* s = static_cast<const char*>(ptr);
            if (s != nullptr && (std::strcmp(s, "startStopProcess") == 0
                                 || std::strcmp(s, "sizeWindow") == 0)) {
                return 1;
            }
            return 0;
        }
        case 0:      return 0;                       // audioMasterAutomate
        case 3:      return 0;                       // audioMasterIdle
        case 43:     return 0;                       // audioMasterBeginEdit
        case 44:     return 0;                       // audioMasterEndEdit
        default:     return 0;                       // 含 deprecated 的 wantMidi(6) 等
    }
}
}  // namespace

int main(int argc, char** argv) {
    // 可选 --log <file>：把 stdout 同时写一份到文件。跨会话测试时必须以
    // LocalService 身份运行（拿不到交互会话的控制台），只能靠这个文件收结果。
    const char* logPath = nullptr;
    std::vector<char*> positional;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            logPath = argv[++i];
        } else {
            positional.push_back(argv[i]);
        }
    }
    std::FILE* logFile = nullptr;
    if (logPath != nullptr) logFile = std::fopen(logPath, "wb");
    auto emit = [logFile](const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        std::vfprintf(stdout, fmt, args);
        std::fflush(stdout);
        if (logFile != nullptr) {
            va_list args2;
            va_start(args2, fmt);
            std::vfprintf(logFile, fmt, args2);
            std::fflush(logFile);
            va_end(args2);
        }
        va_end(args);
    };

    if (positional.empty()) {
        std::printf("usage: vst2_host_stub <plugin.dll> [seconds=4] [--log file]\n");
        return 1;
    }
    const double seconds = (positional.size() > 1) ? std::atof(positional[1]) : 4.0;
    const char* pluginPath = positional[0];

    wchar_t widePath[MAX_PATH] = {0};
    MultiByteToWideChar(CP_ACP, 0, pluginPath, -1, widePath, MAX_PATH);

    emit("[host] session id = %u\n", (unsigned)vfdspec_sessionId());
    emit("[host] LoadLibrary %s\n", pluginPath);
    HMODULE mod = LoadLibraryW(widePath);
    if (mod == nullptr) {
        emit("[host] LoadLibrary FAILED, err=%lu\n", GetLastError());
        if (logFile) std::fclose(logFile);
        return 2;
    }

    auto entry = reinterpret_cast<AEffect* (VSTCALLBACK*)(audioMasterCallback)>(
        GetProcAddress(mod, "VSTPluginMain"));
    if (entry == nullptr) {
        emit("[host] no VSTPluginMain export (EAPO requires exactly this name)\n");
        return 3;
    }
    emit("[host] VSTPluginMain found\n");

    AEffect* e = entry(hostCallback);
    if (e == nullptr || e->magic != kEffectMagic) {
        emit("[host] plugin returned an invalid AEffect (magic mismatch)\n");
        return 4;
    }
    // EAPO 的顺序：先调 VSTPluginMain，然后才 effect->user = this
    e->user = &g_inst;

    char name[128] = {0};
    e->dispatcher(e, effGetEffectName, 0, 0, name, 0);
    emit("[host] effect name: \"%s\"  uniqueID=%08X\n", name, (unsigned)e->uniqueID);

    // EAPO 的 startEditing()：先 effEditGetRect（并直接使用结果，不检查是否写入！）
    // 矩形类型是 ERect（VstRect 只出现在某些宿主侧头文件里）
    ERect* rect = nullptr;
    e->dispatcher(e, effEditGetRect, 0, 0, &rect, 0);
    if (rect != nullptr) {
        emit("[host] effEditGetRect BEFORE open -> %d,%d %dx%d  (EAPO ticket 138 path OK)\n",
                    (int)rect->left, (int)rect->top,
                    (int)(rect->right - rect->left), (int)(rect->bottom - rect->top));
    } else {
        emit("[host] effEditGetRect BEFORE open -> NULL  <-- EAPO would crash here!\n");
    }

    // ---- 对应 VSTPluginInstance::initialize() ----
    emit("[host] effOpen\n");
    e->dispatcher(e, effOpen, 0, 0, nullptr, 0.0f);
    g_inst.usedChannelCount = std::max((int)e->numInputs, (int)e->numOutputs);
    emit("[host] after effOpen: numInputs=%d numOutputs=%d -> usedChannelCount=%d\n",
                (int)e->numInputs, (int)e->numOutputs, g_inst.usedChannelCount);

    // ---- 复现 VSTPluginFilter::initialize() 里的除法（带保护，别把桩自己也打死）----
    {
        const int channelCount = 2;                     // 立体声端点
        const int effectChannelCount = g_inst.usedChannelCount;
        if (effectChannelCount == 0) {
            emit("[host] *** EAPO would do: (2 + 0 - 1) / 0  ->  EXCEPTION_INT_DIVIDE_BY_ZERO\n"
                        "          this is exactly the 0xc0000094 crash that kills audiodg ***\n");
        } else {
            const int effectCount = (channelCount + (effectChannelCount - 1)) / effectChannelCount;
            emit("[host] EAPO would do: effectCount = (2 + %d - 1) / %d = %d\n",
                        effectChannelCount, effectChannelCount, effectCount);
        }
    }

    // ---- 对应 prepareForProcessing(): effSetSampleRate / effSetBlockSize / chunk / start ----
    g_inst.sampleRate = kSampleRate;
    e->dispatcher(e, effSetSampleRate, 0, 0, nullptr, (float)kSampleRate);
    e->dispatcher(e, effSetBlockSize, 0, kBlockSize, nullptr, 0.0f);
    // EAPO 还会喂 chunk（我们写了 8 字节 mark，会被 effFlagsProgramChunks 分支处理）
    {
        static const char tag[] = "VFDSPECT1";
        e->dispatcher(e, effSetChunk, 1, (VstIntPtr)(sizeof(tag) - 1), (void*)tag, 0.0f);
    }
    emit("[host] effMainsChanged(1) + effStartProcess\n");
    e->dispatcher(e, effMainsChanged, 0, 1, nullptr, 0.0f);
    e->dispatcher(e, effStartProcess, 0, 0, nullptr, 0.0f);

    // 声道指针：VST2 契约要求宿主为插件声明的每个声道都提供有效指针
    const int numIn = e->numInputs > 0 ? (int)e->numInputs : 2;
    const int numOut = e->numOutputs > 0 ? (int)e->numOutputs : 2;
    emit("[host] providing %d input / %d output channel pointers\n", numIn, numOut);

    std::vector<float> inBuf((size_t)kBlockSize, 0.0f);
    std::vector<float> outBuf((size_t)kBlockSize, 0.0f);
    std::vector<float*> in((size_t)numIn, inBuf.data());
    std::vector<float*> out((size_t)numOut, outBuf.data());

    emit("[host] feeding %.1fs of test audio (realtime paced) ...\n", seconds);
    const long long totalBlocks = (long long)(seconds * kSampleRate / kBlockSize);
    const double blockSeconds = (double)kBlockSize / (double)kSampleRate;
    const auto t0 = std::chrono::steady_clock::now();
    double phase = 0.0;
    for (long long b = 0; b < totalBlocks; ++b) {
        for (int i = 0; i < kBlockSize; ++i) {
            phase += 2.0 * 3.14159265358979 * 1000.0 / kSampleRate;
            if (phase > 2.0 * 3.14159265358979) phase -= 2.0 * 3.14159265358979;
            const double sweep = 200.0 + 3000.0 * (0.5 + 0.5 * std::sin(2.0 * 3.14159265 * 0.3
                                        * (double)(b * kBlockSize + i) / kSampleRate));
            inBuf[(size_t)i] = (float)(0.5 * std::sin(phase)
                                       + 0.25 * std::sin(2.0 * 3.14159265 * sweep
                                                         * (double)(b * kBlockSize + i) / kSampleRate));
            outBuf[(size_t)i] = 0.0f;
        }
        e->processReplacing(e, in.data(), out.data(), kBlockSize);

        // 按真实播放速率节流。真实宿主(含 EAPO)就是这样喂的；不节流的话 8 秒音频
        // 会在十几毫秒内灌完，插件侧基于墙钟的节流/重连窗口就永远等不到。
        const auto target = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<double>((double)(b + 1) * blockSeconds));
        std::this_thread::sleep_until(target);
    }

    emit("[host] effStopProcess + effMainsChanged(0) + effClose\n");
    e->dispatcher(e, effStopProcess, 0, 0, nullptr, 0.0f);
    e->dispatcher(e, effMainsChanged, 0, 0, nullptr, 0.0f);
    e->dispatcher(e, effClose, 0, 0, nullptr, 0.0f);

    FreeLibrary(mod);
    emit("[host] DONE - check the pipe_server log for received frames\n");
    return 0;
}
