// PanelConsumer.h —— 面板侧消费者（插件编辑器进程 = 用户会话）
//
// 拓扑：**编辑器进程当管道服务端，audiodg 里的插件实例当客户端连过来**。
// 这正是 EAPO 自己设备测试的拓扑（用户会话建管道、audiodg 去连），实测跨会话可通。
//
// 协议 v3 起，面板收到的不再是"算好的频段"，而是**原始单声道音频**：
// 本类只负责按顺序收下来、放进环形缓冲，由消息线程用 readNew() 取走，
// 交给 vfddsp::Analyzer 做窗/FFT/对数分频（重活全在 UI 进程，见 src/vfd_dsp.h）。
//
// 注意：管道是单实例的，"编辑器面板"和"独立 viewer(build\pipe_server.exe)"
// 同一时刻只能有一个在收；抢不到的服务端会如实报 pipeTaken。

#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstring>
#include <vector>

#include "vfd_pipe.h"

namespace vfdspec {

class PanelConsumer : private juce::Thread {
public:
    enum class State { starting, listening, receiving, pipeTaken, wrongVersion };

    PanelConsumer() : juce::Thread("vfd-panel-consumer") {
        ring_.assign((size_t)kRingFrames * vfdpipe::kAudioChannels, 0.0f);
        startThread(juce::Thread::Priority::normal);
    }

    ~PanelConsumer() override { stopThread(2000); }

    // 消息线程调用：取走"上次之后新到"的**交错立体声样本**（L,R,R,L…），
    // 最多 maxFrames 帧（即 2*maxFrames 个 float），返回实际帧数。
    // 顺序严格保持（FFT 的窗不能有空洞）。
    //
    // ⚠️ 这里**绝不丢数据**（2026-09-29 的血案）。原来写的是"待取帧数 > maxFrames
    // 就丢掉最旧的、只给最新的 maxFrames"，看着很合理，实际是屏上那条假裙边的元凶：
    // 丢掉几毫秒音频 ⇒ 分析窗里凭空多一个**相位台阶** ⇒ 频谱上出现一条 ∝1/f 的
    // 宽带裙边（低频爬得很高、到几千赫兹就沉到显示下限以下，所以只有"半边"）。
    // 听不出来 —— 声音走的是另一条路（EAPO 的音频链路），只有我们这条显示链路在丢。
    // 实测：面板 12fps @192k 每拍要取 ~16.0k 帧，而 maxFrames 是 16384，就差这一点点，
    // 约 20% 的拍子触发丢弃 → 状态栏 drop 一直涨、jump 恒为 0（因为接缝检测在
    // 管道线程里，看不见读端的丢弃）。复现工具：pipe_producer --glitch 8。
    // 现在的契约：**只少给，不丢**；调用方循环取空为止。真的要丢也只有
    // discardPending()（整段积压一次性丢弃，且必须如实计入 droppedFrames_）。
    int readNew(float* dst, int maxFrames) {
        const juce::ScopedLock sl(lock_);
        if (maxFrames <= 0) return 0;
        int frames = writePos_ - readPos_;
        if (frames < 0) frames += kRingFrames;
        if (frames > maxFrames) frames = maxFrames;      // 只少给，不丢
        const int first = juce::jmin(frames, kRingFrames - readPos_);
        const size_t chans = (size_t)vfdpipe::kAudioChannels;
        if (first > 0) {
            std::memcpy(dst, ring_.data() + (size_t)readPos_ * chans,
                        sizeof(float) * (size_t)first * chans);
        }
        if (frames > first) {
            std::memcpy(dst + (size_t)first * chans, ring_.data(),
                        sizeof(float) * (size_t)(frames - first) * chans);
        }
        readPos_ = (readPos_ + frames) % kRingFrames;
        return frames;
    }

    // 还没被取走的帧数（调用方用来判断有没有抽干）
    int pending() const {
        const juce::ScopedLock sl(lock_);
        int frames = writePos_ - readPos_;
        if (frames < 0) frames += kRingFrames;
        return frames;
    }

    // 兜底：只有 UI 被卡住超过整个环形缓冲长度时才会用到 —— 把积压一次性丢到最新。
    // **必须如实记账**（帧数计入 droppedFrames()），因为这就是真数据丢失。
    int discardPending() {
        const juce::ScopedLock sl(lock_);
        int frames = writePos_ - readPos_;
        if (frames < 0) frames += kRingFrames;
        if (frames > 0) {
            readPos_ = writePos_;
            droppedFrames_ += (uint64_t)frames;
            ++overruns_;
        }
        return frames;
    }

    State state() const { return state_.load(std::memory_order_relaxed); }
    uint64_t framesReceived() const { return frames_.load(std::memory_order_relaxed); }
    uint64_t samplesReceived() const { return samples_.load(std::memory_order_relaxed); }
    double fps() const { return fps_.load(std::memory_order_relaxed); }
    int sampleRate() const { return rate_.load(std::memory_order_relaxed); }
    int channels() const { return ch_.load(std::memory_order_relaxed); }
    int producerSession() const { return session_.load(std::memory_order_relaxed); }
    // 对面(audiodg 侧实例)那份镜像的 build 指纹；0 = 对面是没带指纹的旧 v3 DLL
    uint32_t producerBuild() const { return build_.load(std::memory_order_relaxed); }
    int overruns() const { return overruns_; }
    // **真正丢掉的音频帧数**（= 屏上假裙边的直接成因）。健康状态必须恒为 0。
    uint64_t droppedFrames() const {
        const juce::ScopedLock sl(lock_);
        return droppedFrames_;
    }
    // 收到的流里"块接缝处样本跳变"的次数。>0 ⇒ 送进分析器的音频**不连续**，
    // 频谱上会凭空多出宽带裙边（听不出来，因为声音走另一条路）。排查"假泄漏"先看它。
    uint64_t jumps() const { return jumps_.load(std::memory_order_relaxed); }
    double lastJumpAmp() const { return lastJump_.load(std::memory_order_relaxed); }

private:
    // 给足：1<<19 帧 ≈ 2.7 s @192k（交错 2ch → 4MB），换"读者永远不会落后到丢帧"
    static constexpr int kRingFrames = 1 << 19;

    void run() override {
        vfdpipe::PipeServer server;
        std::vector<uint8_t> msg(vfdpipe::kMaxMessageBytes);
        while (!threadShouldExit()) {
            if (!server.begin()) {
                // 多半是别的消费者占着这个名字（独立 viewer 正在跑）
                state_.store(State::pipeTaken, std::memory_order_relaxed);
                wait(1000);
                continue;
            }
            state_.store(State::listening, std::memory_order_relaxed);
            if (!server.listen()) {
                server.reset();
                wait(500);
                continue;
            }

            while (!threadShouldExit()) {          // 等 audiodg 侧连进来
                const int r = server.pollClient(100);
                if (r != 0) break;
            }
            if (threadShouldExit()) break;

            state_.store(State::receiving, std::memory_order_relaxed);
            unsigned long long count = 0;
            double tPrev = 0.0;
            while (!threadShouldExit()) {
                const size_t got = server.readMessage(msg.data(), msg.size(), 100);
                if (got == 0) {
                    if (server.lastError() == ERROR_TIMEOUT) continue;   // 暂时没数据
                    break;                                              // 客户端断了
                }
                const uint32_t magic = *(const uint32_t*)msg.data();
                if (magic != vfdpipe::kMagic) continue;
                const uint32_t ver = *(const uint32_t*)(msg.data() + 4);
                if (ver != vfdpipe::kVersionAudio) {
                    // 旧 DLL（协议 v2 只送 254/512 个频段）——如实报出来，
                    // 而不是让面板一直显示"在等 audiodg 实例"（那样最容易被误判成坏了）
                    state_.store(State::wrongVersion, std::memory_order_relaxed);
                    continue;
                }
                if (got != sizeof(vfdpipe::VfdAudioFrame)) continue;
                const auto& f = *reinterpret_cast<const vfdpipe::VfdAudioFrame*>(msg.data());
                if (f.n_samples == 0 || f.n_samples > vfdpipe::kAudioChunk) continue;

                const double now = juce::Time::getMillisecondCounterHiRes();
                if (tPrev > 0.0) {
                    const double dt = (now - tPrev) / 1000.0;
                    if (dt > 1e-6) {
                        const double inst = 1.0 / dt;
                        const double prev = fps_.load(std::memory_order_relaxed);
                        fps_.store(prev > 0.0 ? prev * 0.9 + inst * 0.1 : inst,
                                   std::memory_order_relaxed);
                    }
                }
                tPrev = now;

                {
                    const juce::ScopedLock sl(lock_);
                    const int n = (int)f.n_samples;
                    const size_t chans = (size_t)vfdpipe::kAudioChannels;
                    // ---- 接缝检测：这一块的第一帧 vs 上一块最后一帧 ----
                    // 门限必须是**自适应**的：高音在低采样率下每样本本来就差得多
                    // （4kHz@48k 正常差 0.39），写死阈值会把正常音频判成跳变。
                    // 这里用"本块内部正常最大斜率"的 4 倍做基准。
                    if (havePrev_) {
                        for (int c = 0; c < (int)chans; ++c) {
                            const float d = std::fabs(f.samples[(size_t)c]
                                                      - prevLast_[(size_t)c]);
                            const float thr = std::max(4.0f * blockMaxSlope_[(size_t)c], 1e-4f);
                            if (d > thr) {
                                jumps_.fetch_add(1, std::memory_order_relaxed);
                                lastJump_.store((double)d, std::memory_order_relaxed);
                            }
                        }
                    }
                    for (int c = 0; c < (int)chans; ++c) {
                        float mx = 0.0f;
                        for (int i = 1; i < n; ++i) {
                            const float d = std::fabs(
                                f.samples[(size_t)i * chans + (size_t)c]
                                - f.samples[(size_t)(i - 1) * chans + (size_t)c]);
                            if (d > mx) mx = d;
                        }
                        blockMaxSlope_[(size_t)c] = mx;
                        prevLast_[(size_t)c] =
                            f.samples[(size_t)(n - 1) * chans + (size_t)c];
                    }
                    havePrev_ = true;

                    // 环形缓冲写满 ⇒ 说明读端整整 kRingFrames 帧（2.7s@192k）没来取。
                    // 一次性把最旧的腾出来，并且**按帧记账**（原来只 readPos_+1 是错的：
                    // 那等于每来一帧就把时间轴往前抠一格，丢的量根本数不清）。
                    {
                        int pend = writePos_ - readPos_;
                        if (pend < 0) pend += kRingFrames;
                        if (pend + n > kRingFrames - 1) {
                            const int need = pend + n - (kRingFrames - 1);
                            readPos_ = (readPos_ + need) % kRingFrames;
                            droppedFrames_ += (uint64_t)need;
                            ++overruns_;
                        }
                    }
                    const int first = juce::jmin(n, kRingFrames - writePos_);
                    std::memcpy(ring_.data() + (size_t)writePos_ * chans, f.samples,
                                sizeof(float) * (size_t)first * chans);
                    if (n > first) {
                        std::memcpy(ring_.data(), f.samples + (size_t)first * chans,
                                    sizeof(float) * (size_t)(n - first) * chans);
                    }
                    writePos_ = (writePos_ + n) % kRingFrames;
                }
                rate_.store((int)f.samplerate, std::memory_order_relaxed);
                ch_.store((int)f.channels, std::memory_order_relaxed);
                session_.store((int)f.producer_session, std::memory_order_relaxed);
                build_.store(f.reserved, std::memory_order_relaxed);
                samples_.fetch_add(f.n_samples, std::memory_order_relaxed);
                frames_.store(++count, std::memory_order_relaxed);
                state_.store(State::receiving, std::memory_order_relaxed);
            }
            server.reset();
            state_.store(State::listening, std::memory_order_relaxed);
        }
        server.reset();
    }

    mutable juce::CriticalSection lock_;
    std::vector<float> ring_;
    int writePos_ = 0;
    int readPos_ = 0;
    int overruns_ = 0;
    uint64_t droppedFrames_ = 0;   // 丢掉的帧数（受 lock_ 保护）
    // 接缝检测：门限自适应用"本块内部正常最大斜率"的 4 倍（见 run() 里的说明）
    bool havePrev_ = false;
    float prevLast_[vfdpipe::kAudioChannels] = {};
    float blockMaxSlope_[vfdpipe::kAudioChannels] = {};
    std::atomic<uint64_t> jumps_{0};
    std::atomic<double> lastJump_{0.0};
    std::atomic<State> state_{State::starting};
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> samples_{0};
    std::atomic<double> fps_{0.0};
    std::atomic<int> rate_{0};
    std::atomic<int> ch_{0};
    std::atomic<int> session_{0};
    std::atomic<uint32_t> build_{0};
};

}  // namespace vfdspec
