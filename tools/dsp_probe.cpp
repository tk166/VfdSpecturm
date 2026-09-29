// dsp_probe.cpp —— 分析器离线体检（不碰管道、不碰音频设备、不碰 GUI）
//
// 为什么要有它：2026-09-29 那个 bug（多分辨率阶梯第 0 档负责的 495Hz~20kHz
// **一个频段都不更新**，纯音时看不出来、一放音乐就在 500Hz 处一刀切）在
// pipe_server / 面板上都只能"看图猜"，因为图里没有数字。这个工具直接把音频
// 喂给 vfddsp::Analyzer，把每一档负责的频段范围和若干频点的 dB 读数打出来：
// 哪个档没出数、静音时掉不掉得下来，一眼就看见。
//
// 用法:
//   dsp_probe                       默认: 192k, 5kHz 纯音 2s, 再静音 2s
//   dsp_probe --rate 48000
//   dsp_probe --tone 440 --amp 0.5
//   dsp_probe --levels 50,200,440,1000,5000,10000   (想看哪些频点)
//   dsp_probe --window hann|blackman|blackman-harris|rect
//
// 期望（健康）:
//   * 每个频点都落在"某一档负责的范围"里 —— 若某个频点比 20kHz 以下任何一档的
//     上界都高，说明没有档认领它（选档表漏了）。
//   * 纯音那一列：音调所在频点接近 --amp 对应的 dBFS（-6.02 dBFS @ amp 0.5），
//     高频档同样如此（第 0 档死了的话这里会是 -78 = 显示下限）。
//   * 静音那一列：全部回到显示下限附近（回落弹道 55dB/s，2s 足够掉完 78dB）。

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vfd_dsp.h"

int main(int argc, char** argv) {
    double rate = 192000.0;
    double toneHz = 5000.0;
    double amp = 0.5;
    double seconds = 2.0;
    double silenceSeconds = 2.0;
    const char* windowName = nullptr;
    std::vector<double> levels{20, 50, 100, 200, 440, 1000, 2000, 5000, 10000, 18000};

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) rate = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--tone") == 0 && i + 1 < argc) toneHz = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--amp") == 0 && i + 1 < argc) amp = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--silence") == 0 && i + 1 < argc) silenceSeconds = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--window") == 0 && i + 1 < argc) windowName = argv[++i];
        else if (std::strcmp(argv[i], "--levels") == 0 && i + 1 < argc) {
            levels.clear();
            std::string s = argv[++i];
            size_t pos = 0;
            while (pos < s.size()) {
                size_t comma = s.find(',', pos);
                if (comma == std::string::npos) comma = s.size();
                levels.push_back(std::atof(s.substr(pos, comma - pos).c_str()));
                pos = comma + 1;
            }
        }
    }

    vfddsp::Config cfg = vfddsp::Config::fromEnv();
    if (windowName) {
        if (std::strcmp(windowName, "rect") == 0) cfg.window = vfddsp::WindowType::rectangular;
        else if (std::strcmp(windowName, "hann") == 0) cfg.window = vfddsp::WindowType::hann;
        else if (std::strcmp(windowName, "blackman") == 0) cfg.window = vfddsp::WindowType::blackman;
        else cfg.window = vfddsp::WindowType::blackmanHarris;
    }

    vfddsp::Analyzer an;
    an.prepare(rate, cfg);

    std::printf("[probe] rate %.0f Hz, window %s, dbRange %.0f..%.0f dB, bands %d\n",
                rate, vfddsp::windowName(cfg.window), cfg.dbMin, cfg.dbMax, an.numBands());
    std::printf("[probe] tone %.1f Hz amp %.3f (%.2f dBFS), %.1f s, then %.1f s silence\n",
                toneHz, amp, 20.0 * std::log10(std::max(amp, 1e-9)), seconds, silenceSeconds);
    std::printf("[probe] ---- 阶梯档位 ----\n");
    for (int i = 0; i < an.stageCount(); ++i) {
        std::printf("[probe]   stage %d: FFT %7d (%6.1f ms)  %6d bands  %8.1f..%-8.1f Hz  refresh /%d\n",
                    i, an.stageFftSize(i), an.stageWindowMs(i), an.stageBandCount(i),
                    an.stageBandFreq(i, true), an.stageBandFreq(i, false), an.stageDivisor(i));
    }
    // 每个频点由哪个频段管 —— 顺便暴露"选档表漏了某个频点"
    std::printf("[probe] ---- 频点 -> 频段 ----\n");
    std::printf("[probe]   %10s %10s %10s\n", "freq", "band", "bandFreq");
    for (double f : levels) {
        const int b = an.bandForFreq(f);
        std::printf("[probe]   %10.1f %10d %10.1f\n", f, b, an.bandFreq(b));
    }

    const double tick = 1.0 / 12.0;              // 面板就是 12fps
    const int framesPerTick = (int)std::lround(tick * rate);
    std::setvbuf(stdout, nullptr, _IONBF, 0);    // 崩溃时也别把已打印的内容吞掉
    std::vector<float> buf((size_t)std::max(framesPerTick, 1), 0.0f);

    auto runSeconds = [&](double secs, bool silent) {
        const long long total = (long long)std::lround(secs * rate);
        long long done = 0;
        double phase = 0.0;
        while (done < total) {
            const int n = (int)std::min<long long>(framesPerTick, total - done);
            for (int i = 0; i < n; ++i) {
                if (silent) {
                    buf[(size_t)i] = 0.0f;
                } else {
                    buf[(size_t)i] = (float)(amp * std::sin(phase));
                    phase += 2.0 * 3.14159265358979323846 * toneHz / rate;
                    if (phase > 6.283185307179586) phase -= 6.283185307179586;
                }
            }
            an.push(buf.data(), n);
            an.process(tick);
            done += n;
        }
    };

    std::printf("[probe] ---- 读数 (dB) ----\n");
    std::printf("[probe] %10s %12s %12s\n", "freq", "tone", "silence");

    runSeconds(seconds, false);
    std::vector<double> toneDb(levels.size(), 0.0);
    for (size_t i = 0; i < levels.size(); ++i) {
        toneDb[i] = an.bandDb(an.bandForFreq(levels[i]));
    }
    const double tonePeak = an.peakDb();

    runSeconds(silenceSeconds, true);
    std::printf("[probe]   (峰值 %.1f dB -> 静音后 %.1f dB)\n", tonePeak, an.peakDb());
    for (size_t i = 0; i < levels.size(); ++i) {
        std::printf("[probe] %10.1f %12.1f %12.1f\n", levels[i], toneDb[i],
                    an.bandDb(an.bandForFreq(levels[i])));
    }

    // 退出码: 有任何"贴着显示下限"的档位区间就报 2，方便脚本化回归
    int bad = 0;
    for (size_t i = 0; i < levels.size(); ++i) {
        if (levels[i] <= an.stageBandFreq(0, false) * 1.5) continue;   // 只查第 0 档该管的
        if (toneDb[i] < cfg.dbMin + 6.0 && std::fabs(levels[i] - toneHz) < 1.0) ++bad;
    }
    std::printf("[probe] %s\n", bad == 0 ? "OK" : "SUSPECT (某个频点没出数)");
    return bad == 0 ? 0 : 2;
}
