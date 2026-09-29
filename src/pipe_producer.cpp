// pipe_producer.cpp —— 生产者：模拟插件侧 worker 线程（管道客户端）
//
// 协议 v4 起，生产者**不再发送频谱数据，而是透传交错立体声音频**（与真实插件一致）：
// 这样它测的就是真实链路 —— 消费端拿到音频后自己算窗/FFT/对数分频与声量计。
//
// 用法: pipe_producer [运行秒数=15] [--rate 48000] [--burst N] [--amp 0.2] [--tone 1000]
//   * 默认按实时节奏投递（每 kAudioChunk 帧一条消息）
//   * --burst N: 不限速连发 N 条，用来测管道吞吐
//   * --tone F : 单音模式（做加窗/泄漏对照用），L=F, R=2F

#include "vfd_pipe.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace vfdpipe;

static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// 合成一段**交错立体声**信号（L/R 故意给不同内容，方便看声量计）：
//   默认模式：L = 14 个对数分布的乐音 + 低频 + 一层宽带底噪；R 整体低 6 dB、每第 3 根弱化
//   单音模式：L = F, R = 2F
// 底噪是刻意加的：多分辨率阶梯的档位分界(断层)只有在**宽带底**上才看得见。
static inline float hashNoise(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return (float)((int32_t)(s >> 9) % 32768) / 16384.0f - 1.0f;
}

static void synth(float* dst, int frames, double rate, double t0, float amp, double toneHz) {
    const double twoPi = 6.283185307179586;
    uint32_t rng = 0x12345678u;
    for (int i = 0; i < frames; ++i) {
        const double t = t0 + (double)i / rate;
        double l = 0.0, r = 0.0;
        if (toneHz > 1.0) {
            l = amp * std::sin(twoPi * toneHz * t);
            r = amp * 0.5 * std::sin(twoPi * toneHz * 2.0 * t);
        } else {
            for (int k = 0; k < 14; ++k) {
                const double f = 45.0 * std::pow(12000.0 / 45.0, (double)k / 13.0);
                const double a = amp * 0.35 * std::pow(0.86, (double)k);
                const double env = 0.55 + 0.45 * std::sin(twoPi * (0.3 + 0.12 * k) * t + k);
                l += a * env * std::sin(twoPi * f * t);
                const double weak = (k % 3 == 1) ? 0.25 : 1.0;
                r += a * 0.5 * weak * env * std::sin(twoPi * f * t + 0.7 * k);
            }
            l += amp * 0.02 * std::sin(twoPi * 55.0 * t);
            r += amp * 0.02 * std::sin(twoPi * 55.0 * t);
            const double n = (double)hashNoise(rng) * amp * 0.01;      // ≈ -46dB 宽带底
            l += n;
            r += n * 0.7;
        }
        dst[2 * i] = (float)l;
        dst[2 * i + 1] = (float)r;
    }
}

int main(int argc, char** argv) {
    double runSeconds = 15.0;
    long long burst = 0;
    double rate = 48000.0;
    float amp = 0.25f;
    double toneHz = 0.0;
    long long glitchEvery = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--burst") == 0 && i + 1 < argc) {
            burst = std::atoll(argv[++i]);
        } else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            rate = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--amp") == 0 && i + 1 < argc) {
            amp = (float)std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--tone") == 0 && i + 1 < argc) {
            toneHz = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--glitch") == 0 && i + 1 < argc) {
            glitchEvery = std::atoll(argv[++i]);   // 每 N 段故意少发 1 段（验证检测器/复现假裙边）
        } else if (i == 1) {
            runSeconds = std::atof(argv[i]);
        }
    }

    std::printf("[producer] pipe=%ls  audio frame=%zu bytes (%u frames x %u ch)\n", pipeName(),
                sizeof(VfdAudioFrame), (unsigned)kAudioChunk, (unsigned)kAudioChannels);
    std::printf("[producer] rate=%.0f Hz  payload=%.1f MB/s%s%s\n", rate,
                rate * 4.0 * (double)kAudioChannels / 1e6, burst ? "  (BURST mode)" : "",
                toneHz > 1.0 ? "  (single tone)" : "");
    std::fflush(stdout);

    PipeClient client;
    if (!client.connect(3000)) {
        std::printf("[producer] connect failed (err=%lu) -- is pipe_server running?\n",
                    client.lastError());
        return 2;
    }
    std::printf("[producer] connected\n");
    std::fflush(stdout);

    VfdAudioFrame f;
    initAudioFrame(f);
    f.samplerate = (uint32_t)rate;
    f.channels = (uint16_t)kAudioChannels;
    f.producer_session = currentSessionId();       // 供服务端自证跨会话
    std::printf("[producer] my session id = %u (0 = service/session-0 side)\n",
                f.producer_session);
    std::fflush(stdout);

    const double t0 = now_s();
    const double chunkSeconds = (double)kAudioChunk / rate;
    uint64_t sent = 0, failed = 0, reconnects = 0;
    double sumWriteUs = 0.0, maxWriteUs = 0.0;
    double sampleClock = 0.0;
    const double tEnd = t0 + runSeconds;

    for (;;) {
        const double t = now_s();
        if (burst) {
            if (sent >= (uint64_t)burst) break;
        } else {
            if (t >= tEnd) break;
            // 按实时节奏：一条消息 = kAudioChunk/rate 秒的音频
            const double due = t0 + (double)(sent + 1) * chunkSeconds;
            if (t < due) {
                std::this_thread::sleep_for(std::chrono::microseconds(
                    (long long)((due - t) * 1e6)));
                continue;
            }
        }

        // --glitch N: 每 N 段故意**丢掉**一段（不发送），复现"丢块 → 频谱上凭空出现宽带
        // 裙边"这一现象 —— 它同时用来验证消费端的 jump 检测器是否有效。
        if (glitchEvery > 0 && sent > 0 && (sent % (uint64_t)glitchEvery) == 0) {
            sampleClock += chunkSeconds;          // 时间前进，但这一段不发给消费者
            ++sent;
            continue;
        }

        f.frame_index = sent;
        f.timestamp_s = t - t0;
        synth(f.samples, (int)kAudioChunk, rate, sampleClock, amp, toneHz);
        sampleClock += chunkSeconds;

        const double w0 = now_s();
        const bool ok = client.writeAudioFrame(f);
        const double wus = (now_s() - w0) * 1e6;
        if (ok) {
            ++sent;
            sumWriteUs += wus;
            if (wus > maxWriteUs) maxWriteUs = wus;
            if ((sent % 200) == 0) {
                std::printf("[producer] sent=%llu  write avg=%.1f us max=%.1f us "
                            "reconnects=%llu\n",
                            (unsigned long long)sent, sumWriteUs / (double)sent, maxWriteUs,
                            (unsigned long long)reconnects);
                std::fflush(stdout);
            }
        } else {
            ++failed;
            std::printf("[producer] write failed err=%lu -> reconnect\n", client.lastError());
            std::fflush(stdout);
            client.close();
            if (client.connect(2000)) {
                ++reconnects;
                continue;
            }
            break;
        }
    }

    std::printf("[producer] done: sent=%llu failed=%llu reconnects=%llu  "
                "write avg=%.1f us max=%.1f us  (%.1f frames/s)\n",
                (unsigned long long)sent, (unsigned long long)failed,
                (unsigned long long)reconnects,
                sent ? sumWriteUs / (double)sent : 0.0, maxWriteUs,
                (double)sent / std::max(1e-6, now_s() - t0));
    client.close();
    return failed == 0 ? 0 : 1;
}
