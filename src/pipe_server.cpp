// pipe_server.cpp —— 消费者：接收音频帧，自己做分析并渲染（独立 viewer / 黄金帧入口）
//
// 用法: pipe_server [运行秒数=15] [--png <文件>] [--cell 4] [--dump <原始帧文件>]
//   * 逐帧统计（消息率/丢帧/到达间隔抖动/写管道耗时）
//   * **自己算频谱**：协议 v3 只透传音频，窗/FFT/对数分频/回落/峰值都在这里做
//     （与插件面板用同一份 src/vfd_dsp.h，所以两端结果可直接对照）
//   * 用 src/vfd_render.h 渲染（与 Python 参考实现同一套视觉逻辑），结束时可导出 PNG
//
// 设计要点：
//   * 重叠 I/O + 有界等待：避免阻塞式 ConnectNamedPipe 把服务端永久卡住。
//   * 用宽松 SDDL 建管道，让 Session 0 的客户端（audiodg 里的插件）也能连上。
//   * 分辨率随时可调：VFD_WINDOW_S / VFD_FFT_ORDER / VFD_BANDS（见 vfd_dsp.h）。

#include "vfd_dsp.h"
#include "vfd_pipe.h"
#include "vfd_render.h"
#include "png_writer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace vfdpipe;

static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    double runSeconds = 15.0;
    const char* dumpPath = nullptr;
    const char* pngPath = nullptr;
    int cell = 4;
    float phosphorTau = 0.07f;
    int theme = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dumpPath = argv[++i];
        } else if (std::strcmp(argv[i], "--png") == 0 && i + 1 < argc) {
            pngPath = argv[++i];
        } else if (std::strcmp(argv[i], "--cell") == 0 && i + 1 < argc) {
            cell = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--tau") == 0 && i + 1 < argc) {
            phosphorTau = (float)std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
            theme = std::atoi(argv[++i]);          // 0..4 = 黄绿/深绿/蓝/粉/橘
        } else {
            runSeconds = std::atof(argv[i]);
        }
    }
    if (cell < 2) cell = 2;

    // 目标显示尺寸 508x360（= 面板把整块显示区加高 20%、多出来的高度全给频谱之后的
    // 真实分辨率），按 cell 换算成点阵网格
    const int cols = 508 / cell;
    const int rows = 360 / cell;
    vfdrender::Screen screen(cols, rows, cell, std::max(1, cell - 1));
    screen.setTheme((vfdrender::Theme)(((theme % vfdrender::Screen::themeCount())
                                        + vfdrender::Screen::themeCount())
                                       % vfdrender::Screen::themeCount()));
    std::printf("[server] pipe   = %ls\n", pipeName());
    std::printf("[server] audio frame = %zu bytes (%u samples/chunk)\n",
                sizeof(VfdAudioFrame), (unsigned)kAudioChunk);
    std::printf("[server] my session id = %u (>=1 = interactive user session)\n",
                (unsigned)currentSessionId());
    std::printf("[server] render = %dx%d px, grid %dx%d, cell=%d, tau=%.3fs\n",
                screen.width(), screen.height(), cols, rows, cell, phosphorTau);
    if (pngPath) std::printf("[server] png    -> %s\n", pngPath);
    std::printf("[server] run    = %.1f s\n", runSeconds);
    std::printf("[server] dsp    = resolution is FFT-window driven: "
                "VFD_WINDOW_S/VFD_FFT_ORDER/VFD_BANDS\n");
    std::fflush(stdout);

    std::FILE* dump = nullptr;
    if (dumpPath) dump = std::fopen(dumpPath, "wb");

    const double tEnd = now_s() + runSeconds;
    unsigned long long totalFrames = 0, totalGaps = 0, badFrames = 0;
    int connection = 0;
    uint16_t producerSessionSeen = 0xFFFFu;
    double drawMsSum = 0.0, analyzeMsSum = 0.0;
    unsigned long long renderedFrames = 0;
    double lastDraw = 0.0;
    uint64_t jumps = 0;
    float lastJumpAmp = 0.0f;
    bool havePrev = false;
    float prevLast[kAudioChannels] = {};
    float blockMaxSlope[kAudioChannels] = {};

    vfddsp::Config dspCfg = vfddsp::Config::fromEnv();
    vfddsp::Analyzer analyzer;
    vfddsp::MeterBank meters;
    bool prepared = false;
    int sampleRate = 0;
    unsigned long long overrunDrops = 0;

    std::vector<uint8_t> rgb;
    std::vector<uint8_t> msg(kMaxMessageBytes);
    std::vector<float> mono;

    while (now_s() < tEnd) {
        PipeServer server;
        if (!server.begin()) {
            const DWORD e = server.lastError();
            std::printf("[server] CreateNamedPipe failed: err=%lu (%s)\n", e,
                        PipeServer::describe(e));
            std::fflush(stdout);
            return 2;
        }
        std::printf("[server] listening (connection #%d) ...\n", ++connection);
        std::fflush(stdout);

        bool connected = false;
        if (!server.listen()) {
            std::printf("[server] ConnectNamedPipe failed: err=%lu (%s)\n",
                        server.lastError(), PipeServer::describe(server.lastError()));
            server.reset();
            continue;
        }
        while (now_s() < tEnd) {
            const int r = server.pollClient(200);
            if (r == 1) { connected = true; break; }
            if (r < 0) {
                std::printf("[server] connect failed: err=%lu (%s)\n",
                            server.lastError(), PipeServer::describe(server.lastError()));
                break;
            }
        }
        if (!connected) {
            std::printf("[server] no client connected before deadline\n");
            break;
        }
        std::printf("[server] producer connected\n");
        std::fflush(stdout);

        unsigned long long frames = 0, gaps = 0;
        uint64_t expectIndex = 0;
        bool first = true;
        double tFirst = now_s(), tLast = tFirst, tPrev = tFirst;
        double sumInterval = 0.0, maxInterval = 0.0;
        unsigned intervalCount = 0;

        while (now_s() < tEnd) {
            const size_t got = server.readMessage(msg.data(), msg.size(), 2000);
            if (got == 0) {
                const DWORD e = server.lastError();
                if (e == ERROR_TIMEOUT) {
                    std::printf("[server] idle 2s, recycling connection\n");
                } else {
                    std::printf("[server] client gone: err=%lu (%s) after %llu messages\n",
                                e, PipeServer::describe(e), frames);
                }
                std::fflush(stdout);
                break;
            }
            const double t = now_s();
            const uint32_t magic = *(const uint32_t*)msg.data();
            const uint32_t ver = *(const uint32_t*)(msg.data() + 4);
            if (magic != kMagic || ver != kVersionAudio
                || got != sizeof(VfdAudioFrame)) {
                ++badFrames;
                std::printf("[server] BAD MESSAGE magic=%08X version=%u bytes=%zu\n", magic,
                            ver, got);
                continue;
            }
            const auto& f = *reinterpret_cast<const VfdAudioFrame*>(msg.data());

            if (first) {
                first = false;
                sampleRate = (int)f.samplerate;
                analyzer.prepare((double)sampleRate, dspCfg);
                vfddsp::MeterConfig mcfg;
                mcfg.dbMin = (double)vfdrender::kMeterDbMin;
                meters.prepare((double)sampleRate, mcfg);
                mono.assign((size_t)kAudioChunk, 0.0f);
                prepared = true;
                char r1[32], r2[32];
                std::snprintf(r1, sizeof(r1), "%uHZ", (unsigned)f.samplerate);
                std::snprintf(r2, sizeof(r2), "32F %uCH", (unsigned)f.channels);
                screen.setHeader("SPECTRUM", r1, r2);
                std::printf("[server] header: %s  %s\n", r1, r2);
                std::printf("[server] dsp: base FFT %d (%.1f ms window @%.0fHz, %s), bin %.2f Hz, "
                            "bands %d (auto=%s), dbMin %.1f, sub-bin/interpolated %d\n",
                            analyzer.fftSize(), analyzer.windowMs(), (double)sampleRate,
                            vfddsp::windowName(dspCfg.window), analyzer.binHz(),
                            analyzer.numBands(), dspCfg.bands > 0 ? "no" : "yes",
                            dspCfg.dbMin, analyzer.subBinBands());
                // 多分辨率阶梯：每档的窗长/负责的频段与频率范围（诊断档位分界落在哪）
                for (int si = 0; si < analyzer.stageCount(); ++si) {
                    if (analyzer.stageBandCount(si) == 0) continue;
                    std::printf("[server]   stage %d: FFT %7d (%6.0f ms)  %6d bands  "
                                "%8.0f..%-8.0f Hz  refresh /%d\n",
                                si, analyzer.stageFftSize(si), analyzer.stageWindowMs(si),
                                analyzer.stageBandCount(si),
                                analyzer.stageBandFreq(si, true),
                                analyzer.stageBandFreq(si, false), analyzer.stageDivisor(si));
                }
                std::printf("[server] layout: plot rows %d..%d, meters rows %d..%d (2 x %d)\n",
                            screen.plotTop(), screen.plotBottom(), screen.meterTopRow(),
                            screen.meterTopRow() + 2 * screen.meterHeight(),
                            screen.meterHeight());
                expectIndex = f.frame_index;
            } else {
                if (f.frame_index != expectIndex) {
                    const uint64_t miss = f.frame_index > expectIndex
                                              ? f.frame_index - expectIndex : 0;
                    gaps += miss;
                    std::printf("[server] gap: expected idx %llu, got %llu (missing %llu)\n",
                                (unsigned long long)expectIndex,
                                (unsigned long long)f.frame_index,
                                (unsigned long long)miss);
                }
                const double dt = t - tPrev;
                sumInterval += dt;
                if (dt > maxInterval) maxInterval = dt;
                ++intervalCount;
            }
            expectIndex = f.frame_index + 1;
            tPrev = t;
            tLast = t;
            ++frames;
            producerSessionSeen = f.producer_session;

            if (dump && std::fwrite(&f, sizeof(f), 1, dump) != 1) {
                std::printf("[server] dump write failed\n");
            }

            // ---- 接缝检测（与面板一致）：块与块之间若样本跳变，就是丢了/重了样本。
            //      门限自适应：用本块内部正常最大斜率的 4 倍做基准（高音在低采样率下
            //      每样本本来就差得多，写死阈值会把正常音频判成跳变）。
            {
                if (havePrev) {
                    for (uint32_t c = 0; c < kAudioChannels; ++c) {
                        const float d = std::fabs(f.samples[c] - prevLast[c]);
                        const float thr = std::max(4.0f * blockMaxSlope[c], 1e-4f);
                        if (d > thr) {
                            ++jumps;
                            lastJumpAmp = d;
                        }
                    }
                }
                for (uint32_t c = 0; c < kAudioChannels; ++c) {
                    float mx = 0.0f;
                    for (uint32_t i = 1; i < f.n_samples; ++i) {
                        const float d = std::fabs(
                            f.samples[(size_t)i * kAudioChannels + c]
                            - f.samples[(size_t)(i - 1) * kAudioChannels + c]);
                        if (d > mx) mx = d;
                    }
                    blockMaxSlope[c] = mx;
                    prevLast[c] = f.samples[(size_t)(f.n_samples - 1) * kAudioChannels + c];
                }
                havePrev = true;
            }

            // ---- 分析 + 渲染（与插件面板同一份 vfd_dsp.h）----
            // 限速到 ~24 Hz：192kHz 下每秒到 94 条消息，每条都跑一遍 32768 点 FFT
            // 纯属浪费（面板是 12 fps，磷光余辉会把中间帧补出来）。
            const int nFrames = (int)f.n_samples;
            for (int i = 0; i < nFrames; ++i) {
                mono[(size_t)i] = 0.5f * (f.samples[(size_t)i * 2]
                                          + f.samples[(size_t)i * 2 + 1]);
            }
            analyzer.push(mono.data(), nFrames);
            meters.pushInterleaved(f.samples, nFrames, (int)kAudioChannels);
            const double nowS = now_s();
            if (nowS - lastDraw >= (1.0 / 24.0)) {
                const double dtDraw = (lastDraw > 0.0) ? (nowS - lastDraw) : (1.0 / 24.0);
                lastDraw = nowS;
                const double a0 = now_s();
                analyzer.process(dtDraw);
                meters.process(dtDraw);
                analyzeMsSum += (now_s() - a0) * 1000.0;

                const double r0 = now_s();
                screen.clearDynamic();
                screen.drawBars(analyzer.bands(), analyzer.numBands());
                screen.drawPeaks(analyzer.peaks(), analyzer.numBands());
                screen.drawMeters(meters.vuNorm(), meters.peakNorm(), meters.channels());
                screen.applyPersistence((float)dtDraw, phosphorTau);
                drawMsSum += (now_s() - r0) * 1000.0;
                ++renderedFrames;
            }

            if (frames % 200 == 0) {
                const double elapsed = t - tFirst;
                float bandMax = 0.0f;
                const float* bb = analyzer.bands();
                for (int i = 0; i < analyzer.numBands(); ++i) bandMax = std::max(bandMax, bb[i]);
                std::printf("[server] chunks=%llu  fps=%.1f  avg_dt=%.2fms  max_dt=%.2fms  "
                            "band_max=%.3f  producer_session=%u  sr=%u ch=%u  "
                            "analyze=%.2fms draw=%.2fms  meter_L=%.1f/%.1fdB "
                            "R=%.1f/%.1fdB (vu/peak)\n",
                            frames, frames / (elapsed > 0 ? elapsed : 1e-9),
                            intervalCount ? sumInterval / intervalCount * 1e3 : 0.0,
                            maxInterval * 1e3, bandMax, (unsigned)f.producer_session,
                            (unsigned)f.samplerate, (unsigned)f.channels,
                            analyzeMsSum / (double)std::max(1ull, renderedFrames),
                            drawMsSum / (double)std::max(1ull, renderedFrames),
                            meters.vuDb(0), meters.peakDb(0),
                            meters.vuDb(1), meters.peakDb(1));
                std::fflush(stdout);
            }
        }

        if (frames) {
            const double elapsed = tLast - tFirst;
            std::printf("[server] connection #%d summary: chunks=%llu elapsed=%.2fs "
                        "fps=%.1f gaps=%llu avg_dt=%.2fms max_dt=%.2fms\n",
                        connection, frames, elapsed, frames / (elapsed > 0 ? elapsed : 1e-9),
                        gaps, intervalCount ? sumInterval / intervalCount * 1e3 : 0.0,
                        maxInterval * 1e3);
            totalFrames += frames;
            totalGaps += gaps;
        }
        server.reset();
    }

    if (dump) std::fclose(dump);

    if (pngPath && renderedFrames > 0 && prepared) {
        const double t0 = now_s();
        screen.render(rgb);
        const bool ok = pngw::writePng(pngPath, rgb.data(), screen.width(), screen.height());
        std::printf("[server] wrote %s (%dx%d) in %.1f ms  [%s]\n", pngPath, screen.width(),
                    screen.height(), (now_s() - t0) * 1000.0, ok ? "ok" : "FAILED");
    } else if (pngPath) {
        std::printf("[server] no audio received -> no png written\n");
    }

    std::printf("[server] TOTAL chunks=%llu gaps=%llu bad=%llu connections=%d "
                "analyze_avg=%.2fms draw_avg=%.2fms jumps=%llu (last %.4f)\n",
                totalFrames, totalGaps, badFrames, connection,
                analyzeMsSum / (double)std::max(1ull, renderedFrames),
                drawMsSum / (double)std::max(1ull, renderedFrames),
                (unsigned long long)jumps, (double)lastJumpAmp);
    (void)overrunDrops;

    if (producerSessionSeen != 0xFFFFu) {
        const unsigned mine = (unsigned)currentSessionId();
        const bool cross = (mine != (unsigned)producerSessionSeen);
        std::printf("[server] VERDICT: server_session=%u producer_session=%u -> %s\n",
                    mine, (unsigned)producerSessionSeen,
                    cross ? "CROSS-SESSION OK"
                          : "SAME SESSION (this run proves nothing about cross-session)");
    } else {
        std::printf("[server] VERDICT: no frame received -> cannot judge cross-session\n");
    }
    return badFrames == 0 ? 0 : 1;
}
