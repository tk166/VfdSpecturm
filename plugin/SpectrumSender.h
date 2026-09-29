// SpectrumSender.h —— 音频线程与管道之间的搬运工（**只搬运，不分析**）
//
// 协议 v3 起，这个类退化成一根"音频透传管"：
//   * processBlock（音频线程）：多声道混单 + 写无锁环形缓冲，绝不阻塞/分配/碰管道。
//   * worker 线程：把样本攒成 kAudioChunk 一段，写进命名管道。
//
// 为什么把 FFT/分频/峰值全部搬走（这是本轮最大的架构改动）：
//   1) 分辨率只由 **FFT 窗长** 决定，而 audiodg 是系统音频引擎进程，里面不该做重活；
//   2) UI 端（配置编辑器面板 / pipe_server）算，才能把窗长开大 —— 192kHz 上 32768 点
//      (bin 5.9Hz) / 16385 个频点，而不是被 254/512 个"频段"卡死；
//   3) audiodg 里少一段 DSP，就少一个把系统音频搞崩的机会。
// 带宽代价：单声道 float32，48kHz ≈ 190 KB/s、192kHz ≈ 770 KB/s —— 管道实测突发
// 140 MB/s，占不到 1%。
//
// 稳定性契约（照抄调研结论，别改）：
//   * 绝不在音频线程里 connect()/write()：管道写满时会阻塞（实测最坏 144 µs）。
//   * 连接失败只记一次日志、安静降级 —— EAPO 的 APO 连续失败 10 次会把整个 APO 栈禁用。
//   * 只认"真的收到过音频"的实例再尝试连接：EAPO 会在配置编辑器进程里再实例化一个
//     插件（那个实例永远没有音频），靠这道闸门它就不会去抢那个单实例管道。

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "BuildStamp.h"
#include "vfd_pipe.h"

// 调试日志：只在设置了环境变量 VFD_DEBUG 时输出。
// 为什么需要它：插件跑在 audiodg 里（无控制台、看不到任何输出），一旦"没有数据"
// 你根本不知道卡在 processBlock、攒块还是管道上。用它可以把这条链逐段点亮。
namespace vfdspec {
inline bool debugEnabled() {
    static const bool on = (std::getenv("VFD_DEBUG") != nullptr);
    return on;
}
}  // namespace vfdspec

#define VFD_LOG(...)                                        \
    do {                                                    \
        if (vfdspec::debugEnabled()) {                      \
            std::fprintf(stderr, "[vfd] " __VA_ARGS__);     \
            std::fputc('\n', stderr);                       \
            std::fflush(stderr);                            \
        }                                                   \
    } while (0)

namespace vfdspec {

class SpectrumSender : private juce::Thread {
public:
    SpectrumSender() : juce::Thread("vfd-audio-pump") {
        ring_.assign((size_t)kRingSize * vfdpipe::kAudioChannels, 0.0f);   // 交错，长度 = 帧数×2
        stage_.reserve((size_t)vfdpipe::kAudioChunk * 8 * vfdpipe::kAudioChannels);
        startThread(juce::Thread::Priority::low);
    }

    ~SpectrumSender() override { stopThread(2000); }

    // ---- 采样率变更 ----
    // 不在调用线程（prepareToPlay 是 EAPO 的协商线程）里做任何重活，只记一个待处理值；
    // 这里连 FFT 表都不用建了，worker 只把它塞进帧头。
    void requestSampleRate(double sampleRate) {
        pendingRate_.store(sampleRate > 0 ? sampleRate : 48000.0, std::memory_order_relaxed);
    }

    // ---- 音频线程调用：必须无阻塞、无分配 ----
    // 取前两个声道（L/R）**交错**写入环形缓冲；混单交给 UI 端做（协议 v4）。
    void pushAudio(const float* const* channels, int numChannels, int numSamples) noexcept {
        if (numChannels <= 0 || numSamples <= 0) return;

        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo_.prepareToWrite(numSamples, start1, size1, start2, size2);
        const int writable = size1 + size2;
        if (writable < numSamples) {
            // 环形缓冲满了（几乎不该发生，缓冲是 1.37s）。这里**记账**而不是装没事：
            // 丢掉一段会在频谱上变成一条宽带裙边，而声音那条路完全听不出来 ——
            // 这种事必须能被看见（面板状态栏/日志里都有 droppedFrames）。
            dropped_.fetch_add((uint64_t)(numSamples - writable), std::memory_order_relaxed);
        }
        if (writable <= 0) return;                       // 全满：整块丢，已记账
        const int toWrite = juce::jmin(numSamples, writable);

        const float* left = channels[0];
        const float* right = (numChannels > 1) ? channels[1] : channels[0];   // 单声道就复制一份
        int written = 0;
        while (written < toWrite) {
            const int chunk = juce::jmin(toWrite - written,
                                         written < size1 ? size1 - written
                                                         : size2 - (written - size1));
            const int pos = (written < size1) ? start1 + written : start2 + (written - size1);
            float* dst = ring_.data() + (size_t)pos * 2;
            const int base = written;
            for (int i = 0; i < chunk; ++i) {
                dst[2 * i] = left[base + i];
                dst[2 * i + 1] = right[base + i];
            }
            written += chunk;
        }
        fifo_.finishedWrite(toWrite);
        samplesSeen_.fetch_add((uint64_t)toWrite, std::memory_order_relaxed);
        blocksSeen_.fetch_add(1, std::memory_order_relaxed);   // 音频线程只累加，不打日志
        lastChannels_.store(numChannels, std::memory_order_relaxed);
    }

    // ---- 编辑器/状态查询 ----
    bool isPipeConnected() const noexcept { return connected_.load(std::memory_order_relaxed); }
    uint64_t framesSent() const noexcept { return framesSent_.load(std::memory_order_relaxed); }
    uint64_t samplesSeen() const noexcept { return samplesSeen_.load(std::memory_order_relaxed); }
    // 丢了多少帧。**只要不为 0，频谱上就会出现凭空多出来的宽带裙边**（声音听不出来，
    // 因为声音走的是另一条路）。所以它必须可观测，绝不能静默丢。
    uint64_t droppedFrames() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    double lastSendHz() const noexcept { return lastSendHz_.load(std::memory_order_relaxed); }
    int chunkSamples() const noexcept { return (int)vfdpipe::kAudioChunk; }
    double sampleRate() const noexcept { return appliedRate_; }

private:
    // 环形缓冲给足：192kHz 下也就 1.37 s，代价 2MB，换"永不满/永不丢"。
    static constexpr int kRingSize = 1 << 18;      // 262144 帧

    void run() override {
        VFD_LOG("audio pump started (build %s, session=%u, pipe=%ls, chunk=%u samples)",
                VFD_BUILD_STAMP, (unsigned)vfdpipe::currentSessionId(), vfdpipe::pipeName(),
                (unsigned)vfdpipe::kAudioChunk);
        // 初值回拨 1 秒：让"第一次收到音频"就能立刻尝试连接，而不是白等 500 ms
        double lastReconnectAttempt = juce::Time::getMillisecondCounterHiRes() - 1000.0;
        double lastSend = 0.0;
        double lastTrace = 0.0;
        bool loggedConnectFail = false;
        uint64_t iter = 0;

        while (!threadShouldExit()) {
            wait(3);                                // 3 ms 一轮，音频线程那边完全无感知
            ++iter;
            const bool traceIt = (iter <= 200);

            // 0) 采样率（只登记，帧头里带出去）
            const double wantRate = pendingRate_.load(std::memory_order_relaxed);
            if (wantRate > 0.0 && wantRate != appliedRate_) {
                appliedRate_ = wantRate;
                stage_.clear();                     // 采样率变了，别把旧数据混进去
                VFD_LOG("rate registered: %.0f Hz", appliedRate_);
            }

            // 1) 从无锁环形缓冲搬到 stage_（两边的单位都是**交错 float**）
            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            const int ready = fifo_.getNumReady();
            fifo_.prepareToRead(ready, start1, size1, start2, size2);
            if (size1 > 0) {
                stage_.insert(stage_.end(), ring_.begin() + (size_t)start1 * 2,
                              ring_.begin() + (size_t)(start1 + size1) * 2);
            }
            if (size2 > 0) {
                stage_.insert(stage_.end(), ring_.begin() + (size_t)start2 * 2,
                              ring_.begin() + (size_t)(start2 + size2) * 2);
            }
            fifo_.finishedRead(size1 + size2);

            const double now = juce::Time::getMillisecondCounterHiRes();
            if (now - lastTrace >= 500.0) {          // 诊断：每 500 ms 点亮一次链路状态
                lastTrace = now;
                VFD_LOG("trace: blocks=%llu samples=%llu ch=%d rate=%.0f stage=%zu "
                        "connected=%d frames=%llu",
                        (unsigned long long)blocksSeen_.load(std::memory_order_relaxed),
                        (unsigned long long)samplesSeen_.load(std::memory_order_relaxed),
                        lastChannels_.load(std::memory_order_relaxed),
                        appliedRate_, stage_.size(),
                        connected_.load(std::memory_order_relaxed) ? 1 : 0,
                        (unsigned long long)framesSent_.load(std::memory_order_relaxed));
            }

            // 2) 连接管理（没连上就不攒数据，免得一连上吐出一堆陈旧音频）
            if (!client_.connected()) {
                stage_.clear();
                if (samplesSeen_.load(std::memory_order_relaxed) == 0) continue;
                if (now - lastReconnectAttempt >= 500.0) {
                    lastReconnectAttempt = now;
                    if (client_.connect(150)) {
                        connected_.store(true, std::memory_order_relaxed);
                        VFD_LOG("pipe connected");
                    } else if (!loggedConnectFail) {
                        loggedConnectFail = true;
                        VFD_LOG("pipe connect failed (err=%lu) - retrying every 500ms",
                                client_.lastError());
                    }
                }
                continue;
            }

            // 3) 每凑够一段就写一条音频帧（不做限速：节奏由音频自身的速率决定）
            const size_t chunkFloats = (size_t)vfdpipe::kAudioChunk * vfdpipe::kAudioChannels;
            while (stage_.size() >= chunkFloats) {
                vfdpipe::VfdAudioFrame f;
                vfdpipe::initAudioFrame(f);
                f.samplerate = (uint32_t)(appliedRate_ > 0.0 ? appliedRate_ : 48000.0);
                f.channels = (uint16_t)juce::jmax(1, lastChannels_.load(std::memory_order_relaxed));
                f.gain_db = 0.0f;
                f.producer_session = vfdpipe::currentSessionId();
                // 把本镜像的 build 指纹带给面板：两边版本不一致时面板会当场报警
                f.reserved = vfdspec::kVfdBuildId;
                f.frame_index = framesSent_.load(std::memory_order_relaxed);
                f.timestamp_s = now / 1000.0;
                std::memcpy(f.samples, stage_.data(), sizeof(float) * chunkFloats);
                stage_.erase(stage_.begin(), stage_.begin() + (long)chunkFloats);

                if (!client_.writeAudioFrame(f)) {
                    VFD_LOG("write failed (err=%lu) -> disconnect", client_.lastError());
                    client_.close();
                    connected_.store(false, std::memory_order_relaxed);
                    stage_.clear();
                    break;
                }
                const uint64_t n = framesSent_.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n == 1) VFD_LOG("first audio frame written");
                if (lastSend > 0.0 && now > lastSend) {
                    lastSendHz_.store(1000.0 / (now - lastSend), std::memory_order_relaxed);
                }
                lastSend = now;
                if (traceIt) VFD_LOG("it%llu frame %llu written", (unsigned long long)iter,
                                     (unsigned long long)n);
            }

            // 4) 防积压：暂存里堆太多（消费者刚重启等）时**只丢弃整段并记账**，
            //    绝不静默丢 —— 静默丢就是频谱上凭空出现的宽裙边。
            const size_t cap = chunkFloats * 64;
            if (stage_.size() > cap) {
                const size_t dropFloats = stage_.size() - cap;
                const size_t dropFrames = dropFloats / vfdpipe::kAudioChannels;
                dropped_.fetch_add((uint64_t)dropFrames, std::memory_order_relaxed);
                VFD_LOG("stage overflow: dropping %llu frames (total %llu)",
                        (unsigned long long)dropFrames,
                        (unsigned long long)dropped_.load(std::memory_order_relaxed));
                stage_.erase(stage_.begin(), stage_.begin() + (long)(dropFrames * vfdpipe::kAudioChannels));
            }
        }

        client_.close();
    }

    juce::AbstractFifo fifo_{kRingSize};
    std::vector<float> ring_, stage_;
    vfdpipe::PipeClient client_;
    double appliedRate_ = 0.0;

    std::atomic<double> pendingRate_{0.0};

    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> framesSent_{0};
    std::atomic<uint64_t> samplesSeen_{0};
    std::atomic<uint64_t> dropped_{0};          // 丢掉的帧数（>0 = 频谱会出现假裙边）
    std::atomic<uint64_t> blocksSeen_{0};
    std::atomic<int> lastChannels_{0};
    std::atomic<double> lastSendHz_{0.0};
};

}  // namespace vfdspec
