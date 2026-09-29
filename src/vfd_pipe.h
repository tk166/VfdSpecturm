// vfd_pipe.h —— VFD 频谱显示的跨进程传输协议（Win32 命名管道，无第三方依赖）
//
// 设计要点（都是实测/调研换来的结论，改动前请先看 docs/508x302-feasibility.md §1.4-1.5）：
//   * 传输选命名管道而不是共享内存：管道命名空间是全局的，**天然跨会话**，
//     不需要 Global\ 特权。实测本机 audiodg 在 Session 0、交互用户在 Session 1，
//     而 Session 1 创建 Global\ 文件映射会被拒（err=5）。
//   * 方向：viewer（用户会话）当**服务端**，插件（audiodg, Session 0）当**客户端**。
//     这与 EAPO 自身设备测试的做法一致（DeviceSelector 建管道，audiodg 去连）。
//   * 消息模式（PIPE_TYPE_MESSAGE）：一次 WriteFile 就是一整帧，ReadFile 只会读到
//     完整帧，天然不会撕裂——不需要 seqlock。
//   * 只传"频段数据"（约 2 KB/帧），不传整帧位图：30 Hz 下只有 ~60 KB/s，
//     渲染自由度留在 viewer 侧。
//   * **绝不在音频线程里调用 connect()/writeFrame()**：管道写满时会阻塞，
//     违反 APO 的实时契约。所有调用都应在插件的 worker 线程里。

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <sddl.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace vfdpipe {

// 管道名。`\\.\pipe\` 本身就是全局命名空间，跨会话可见、无需特权。
inline constexpr wchar_t kDefaultPipeName[] = L"\\\\.\\pipe\\VfdSpectrum";

// 实际使用的管道名：允许用环境变量 VFD_PIPE_NAME 覆盖。
// 为什么需要它：管道是**单实例**的，EAPO 面板开着的时候默认名字被它占着，
// 独立 viewer / 自动化测试就连不上了（实测 err=231）。有了覆盖，就能在不
// 动用户面板的前提下另开一条同名通路做验证。
inline const wchar_t* pipeName() {
    static const std::wstring name = [] {
        wchar_t buf[256] = {};
        const DWORD n = GetEnvironmentVariableW(L"VFD_PIPE_NAME", buf, 255);
        if (n > 0 && n < 255) return std::wstring(buf);
        return std::wstring(kDefaultPipeName);
    }();
    return name.c_str();
}

inline constexpr uint32_t kMagic    = 0x46444656u;   // 'VFDF'
// 协议版本 2: 频段帧（bands[512]）。保留：兼容旧 DLL 与"只要柱状数据"的消费者。
inline constexpr uint32_t kVersionBands = 2u;
// 协议版本 4: **立体声音频帧**（v3 = 单声道）。插件(audiodg, Session 0)只做透传，
// 窗/FFT/对数分频/峰值/声量计全部搬到 UI 进程（配置编辑器面板 / pipe_server）。
// 为什么要立体声：声量计/峰值计要按 L/R 分开画（foobar2000 那种），
// 顺带把"混单"也挪到 UI 端 —— 音频进程里少做一件事是一件。
// 带宽：交错 float32 ×2ch，192kHz 时 1.54 MB/s（实测管道突发 140 MB/s，可忽略）。
inline constexpr uint32_t kVersion  = kVersionBands;   // 默认（旧）版本号，见 kVersionAudio
inline constexpr uint32_t kVersionAudio = 4u;
inline constexpr uint32_t kMaxBands = 512u;
// 音频帧携带的**每声道**样本数：48k → 42.7ms/帧，192k → 10.7ms/帧
inline constexpr uint32_t kAudioChunk = 2048u;
// 交错声道数（L/R）。更多声道以后要加的话，改这里 + 版本号。
inline constexpr uint32_t kAudioChannels = 2u;

#pragma pack(push, 1)
struct VfdFrame {
    uint32_t magic;
    uint32_t version;
    uint32_t n_bands;        // <= kMaxBands
    uint32_t samplerate;
    uint64_t frame_index;    // 生产者自增；消费者用它检测丢帧
    double   timestamp_s;    // 生产者单调时钟（秒）
    float    gain_db;
    uint16_t bits;
    uint16_t channels;
    // 生产者所在会话号(由 ProcessIdToSessionId 填)。这不是装饰：跨会话是这套传输
    // 唯一的未知数，把它放进帧里，服务端日志就能自证 "server_session=1
    // producer_session=0" —— 用两个同会话的控制台程序是测不出这件事的。
    uint16_t producer_session;
    uint16_t reserved1;
    float    bands[kMaxBands];   // 归一化 0..1，对数频段
    float    peaks[kMaxBands];   // 峰值保持（可为 0）
};
#pragma pack(pop)

static_assert(sizeof(VfdFrame) == 4140, "VfdFrame 布局变了：协议版本 kVersionBands 要一起升");

// 音频帧（协议 v4）：一次 WriteFile 就是"一段交错立体声 + 一个头"。
// 固定 n_samples = kAudioChunk（每声道样本数），消费者按固定大小收帧、拼进环形缓冲。
#pragma pack(push, 1)
struct VfdAudioFrame {
    uint32_t magic;
    uint32_t version;            // kVersionAudio
    uint32_t n_samples;          // 每声道样本数，<= kAudioChunk
    uint32_t samplerate;         // 原始采样率（可能 192000）
    uint64_t frame_index;        // 生产者自增；消费者用来发现丢帧
    double   timestamp_s;
    float    gain_db;
    uint16_t channels;           // 源声道数（我们只取前两个：L/R）
    uint16_t producer_session;
    uint32_t reserved;           // 生产端的 build 指纹（vfdspec::kVfdBuildId）
    float    samples[kAudioChunk * kAudioChannels];   // 交错: L0 R0 L1 R1 ...
};
#pragma pack(pop)

static_assert(sizeof(VfdAudioFrame) == 16428, "VfdAudioFrame 布局变了：协议版本要一起升");

// 管道消息的最大尺寸（消费者按这个开缓冲）
inline constexpr size_t kMaxMessageBytes = sizeof(VfdAudioFrame);

inline void initFrame(VfdFrame& f) {
    std::memset(&f, 0, sizeof(f));
    f.magic = kMagic;
    f.version = kVersionBands;
    f.n_bands = kMaxBands;
}

inline void initAudioFrame(VfdAudioFrame& f) {
    std::memset(&f, 0, sizeof(f));
    f.magic = kMagic;
    f.version = kVersionAudio;
    f.n_samples = kAudioChunk;
    f.channels = (uint16_t)kAudioChannels;
}

// 当前进程所在会话号（0 = 服务/音频引擎侧，>=1 = 交互用户会话）。
inline uint16_t currentSessionId() {
    DWORD sid = 0xFFFFFFFFu;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    return (uint16_t)(sid & 0xFFFF);
}

// 轻量完整性校验（FNV-1a）：消费者用来确认收到的是完整帧而不是被截断的垃圾。
inline uint32_t checksum(const VfdFrame& f) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&f);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(f); ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

// ---------------------------------------------------------------- 服务端（viewer 侧）
//
// 用重叠 I/O + 有界等待：阻塞式 ConnectNamedPipe/ReadFile 会让服务端在客户端消失后
// 永远卡住、无法按时退出，而且残留实例会让下一个服务端拿到 ERROR_PIPE_BUSY(231)。
class PipeServer {
public:
    ~PipeServer() { reset(); if (ev_) CloseHandle(ev_); }

    // permissive=true 时用宽松 SDDL（Everyone 全权），让 Session 0 / LocalService
    // 的客户端也能连上。生产环境应改成最小权限（只给 LS + 当前用户）。
    bool begin(bool permissive = true) {
        SECURITY_ATTRIBUTES sa{};
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (permissive) {
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &sd, nullptr)) {
                lastError_ = GetLastError();
                return false;
            }
            sa.nLength = sizeof(sa);
            sa.lpSecurityDescriptor = sd;
            sa.bInheritHandle = FALSE;
        }
        if (!ev_) ev_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset
        h_ = CreateNamedPipeW(pipeName(),
                              PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED
                                  | FILE_FLAG_FIRST_PIPE_INSTANCE,
                              PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                              1,                              // 同时只服务一个客户端
                              0,                              // 入缓冲由系统定
                              // 出缓冲给足：192kHz 下每条消息 16KB、94 条/秒，
                              // 消费端偶尔忙一下不该让生产者的 WriteFile 阻塞（阻塞→丢样本→
                              // 频谱上凭空出现宽带裙边，而声音那条路完全听不出来）。
                              64u * (DWORD)sizeof(VfdAudioFrame),
                              0,
                              permissive ? &sa : nullptr);
        lastError_ = GetLastError();
        if (sd) LocalFree(sd);
        return h_ != INVALID_HANDLE_VALUE;
    }

    // 开始监听。**ConnectNamedPipe 每个实例只调用一次**：如果每轮都重新发起、
    // 超时又 CancelIo，会把管道实例搞成"再也不接受连接"的状态（服务端明明在
    // listening，客户端却怎么都连不上）。
    bool listen() {
        if (h_ == INVALID_HANDLE_VALUE || !ev_) { lastError_ = ERROR_INVALID_HANDLE; return false; }
        connectPending_ = false;
        connectOv_ = OVERLAPPED{};
        connectOv_.hEvent = ev_;
        if (ConnectNamedPipe(h_, &connectOv_)) return true;
        lastError_ = GetLastError();
        if (lastError_ == ERROR_PIPE_CONNECTED) return true;      // 已经连上了
        if (lastError_ != ERROR_IO_PENDING) return false;
        connectPending_ = true;                                  // 等待中
        return true;
    }

    // 轮询客户端。返回 1=已连接, 0=还没人来(可继续等), -1=出错。
    // 内部不会重复发起 ConnectNamedPipe，只等同一个重叠操作。
    int pollClient(int sliceMs) {
        if (h_ == INVALID_HANDLE_VALUE || !ev_) { lastError_ = ERROR_INVALID_HANDLE; return -1; }
        if (!connectPending_) return 1;
        const DWORD w = WaitForSingleObject(ev_, (DWORD)sliceMs);
        if (w == WAIT_OBJECT_0) {
            DWORD dummy = 0;
            const bool ok = GetOverlappedResult(h_, &connectOv_, &dummy, FALSE) != 0;
            connectPending_ = false;
            if (ok) return 1;
            lastError_ = GetLastError();
            return -1;
        }
        return 0;                                                // 超时：继续等
    }

    // 放弃等待（仅在整个实例要关闭时调用）
    void abandonWait() {
        if (connectPending_) {
            CancelIo(h_);
            connectPending_ = false;
        }
    }

    // 有界读一整条消息（消息模式：一次 WriteFile 就是一整条，天然不会撕裂）。
    // 返回实际读到的字节数；0 = 失败（原因看 lastError()）。
    size_t readMessage(void* buf, size_t cap, int timeoutMs) {
        if (h_ == INVALID_HANDLE_VALUE || !ev_) {
            lastError_ = ERROR_INVALID_HANDLE;
            return 0;
        }
        OVERLAPPED ov{};
        ov.hEvent = ev_;
        DWORD got = 0;
        if (!ReadFile(h_, buf, (DWORD)cap, &got, &ov)) {
            lastError_ = GetLastError();
            if (lastError_ != ERROR_IO_PENDING) return 0;
            const DWORD w = WaitForSingleObject(ev_, (DWORD)timeoutMs);
            if (w != WAIT_OBJECT_0) {
                CancelIo(h_);
                lastError_ = ERROR_TIMEOUT;
                return 0;
            }
            if (!GetOverlappedResult(h_, &ov, &got, FALSE)) {
                lastError_ = GetLastError();
                return 0;
            }
        }
        return (size_t)got;
    }

    bool readFrame(VfdFrame& f, int timeoutMs) {
        return readMessage(&f, sizeof(f), timeoutMs) == sizeof(f);
    }

    bool readAudioFrame(VfdAudioFrame& f, int timeoutMs) {
        return readMessage(&f, sizeof(f), timeoutMs) == sizeof(f);
    }

    void reset() {
        if (h_ != INVALID_HANDLE_VALUE) {
            abandonWait();
            DisconnectNamedPipe(h_);
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }

    DWORD lastError() const { return lastError_; }
    static const char* describe(DWORD e) {
        switch (e) {
            case ERROR_TIMEOUT:       return "timeout (no client yet)";
            case ERROR_PIPE_BUSY:     return "another server instance is already listening";
            case ERROR_ACCESS_DENIED: return "pipe name already exists (first-instance flag)";
            case ERROR_BROKEN_PIPE:   return "client disconnected";
            case ERROR_NO_DATA:       return "client closed the pipe";
            default:                  return "see Win32 error code";
        }
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    HANDLE ev_ = nullptr;
    OVERLAPPED connectOv_{};
    bool connectPending_ = false;
    DWORD  lastError_ = 0;
};

// ------------------------------------------------------- 客户端（插件 / 生产者侧）
class PipeClient {
public:
    // 有界重试连接。**不要在音频线程调用**（WaitNamedPipe 可能阻塞）。
    // 失败时立刻返回 false，由调用方安静降级——绝不能进重试循环：
    // EAPO 的 APO 连续失败 10 次会导致整个 APO 栈被系统禁用。
    bool connect(int totalTimeoutMs = 300, int perAttemptMs = 100) {
        const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)totalTimeoutMs;
        for (;;) {
            if (WaitNamedPipeW(pipeName(), perAttemptMs)) {
                h_ = CreateFileW(pipeName(), GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
                if (h_ != INVALID_HANDLE_VALUE) {
                    DWORD mode = PIPE_READMODE_MESSAGE;
                    SetNamedPipeHandleState(h_, &mode, nullptr, nullptr);
                    return true;
                }
            }
            lastError_ = GetLastError();
            if ((long long)(deadline - GetTickCount64()) <= 0) return false;
            Sleep(20);
        }
    }

    bool writeFrame(const VfdFrame& f) {
        return writeMessage(&f, sizeof(f));
    }

    bool writeAudioFrame(const VfdAudioFrame& f) {
        return writeMessage(&f, sizeof(f));
    }

    bool writeMessage(const void* buf, size_t bytes) {
        if (h_ == INVALID_HANDLE_VALUE) return false;
        DWORD wrote = 0;
        if (!WriteFile(h_, buf, (DWORD)bytes, &wrote, nullptr)) {
            lastError_ = GetLastError();
            return false;
        }
        return wrote == (DWORD)bytes;
    }

    void close() {
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }

    bool connected() const { return h_ != INVALID_HANDLE_VALUE; }
    DWORD lastError() const { return lastError_; }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    DWORD  lastError_ = 0;
};

}  // namespace vfdpipe
