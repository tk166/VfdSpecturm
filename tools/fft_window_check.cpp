// fft_window_check.cpp —— 量化"加窗到底压掉了多少泄漏"
//
// 用法: fft_window_check [--rate 48000] [--fft 8192] [--freq 1000] [--amp 0.5]
//
// 做法：喂一个**非整周期**的正弦（故意取不在 bin 中心上的频率，这样不管理论上还是
// 实际上都会暴露泄漏），用 vfdfft::RealFft + vfddsp::makeWindow 算出幅度谱，
// 打印主瓣与旁瓣的相对电平（dB，相对峰值）。
//
// 判据（教科书值，可直接对照输出）：
//   rectangular : 第一旁瓣 -13.3 dB，之后 ~6 dB/oct 衰减（几乎不衰减）
//   hann        : 第一旁瓣 -31.5 dB，之后 ~18 dB/oct
//   blackman-harris: 旁瓣 -92 dB 量级
// 若输出里 hann 的旁瓣跟 rectangular 一样高，说明窗**没有真的加上**（就是代码 bug）。

#include "vfd_dsp.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv) {
    double rate = 48000.0;
    int n = 8192;
    double freq = 1000.0;      // 1000 Hz 在 48k/8192 上 = 170.67 bin，刻意不在 bin 中心
    double amp = 0.5;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) rate = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--fft") == 0 && i + 1 < argc) n = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--freq") == 0 && i + 1 < argc) freq = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--amp") == 0 && i + 1 < argc) amp = std::atof(argv[++i]);
    }

    const double binHz = rate / (double)n;
    std::printf("fft_window_check: rate=%.0f  N=%d  bin=%.4f Hz  tone=%.4f Hz (= %.3f bin)  "
                "amp=%.3f\n", rate, n, binHz, freq, freq / binHz, amp);
    std::printf("理论幅度归一化后峰值应为 %.1f dB (amp=%.3f)\n\n", 20.0 * std::log10(amp), amp);

    std::vector<float> x((size_t)n, 0.0f);
    for (int i = 0; i < n; ++i) {
        x[(size_t)i] = (float)(amp * std::sin(2.0 * 3.14159265358979 * freq * (double)i / rate));
    }
    std::vector<float> mag((size_t)(n / 2 + 1), 0.0f);
    std::vector<float> win;

    vfdfft::RealFft fft;
    fft.prepare((int)std::lround(std::log2((double)n)));

    const vfddsp::WindowType types[] = {vfddsp::WindowType::rectangular,
                                        vfddsp::WindowType::hann,
                                        vfddsp::WindowType::blackman,
                                        vfddsp::WindowType::blackmanHarris};
    const int offsets[] = {0, 1, 2, 3, 5, 10, 20, 50, 100, 500};

    for (vfddsp::WindowType t : types) {
        win = vfddsp::makeWindow(t, n);
        double wsum = 0.0;
        std::vector<float> xw((size_t)n);
        for (int i = 0; i < n; ++i) {
            xw[(size_t)i] = x[(size_t)i] * win[(size_t)i];
            wsum += win[(size_t)i];
        }
        fft.magnitude(xw.data(), mag.data());
        const float scale = (float)(2.0 / (wsum > 0 ? wsum : 1.0));
        for (int i = 0; i <= n / 2; ++i) mag[(size_t)i] *= scale;

        int peakBin = 0;
        for (int i = 1; i <= n / 2; ++i) if (mag[(size_t)i] > mag[(size_t)peakBin]) peakBin = i;
        const double peakDb = 20.0 * std::log10(std::max((double)mag[(size_t)peakBin], 1e-12));

        std::printf("--- %-16s  peak bin=%d (%.2f Hz)  peak=%.2f dB ---\n",
                    vfddsp::windowName(t), peakBin, (double)peakBin * binHz, peakDb);
        std::printf("   offset(bin):");
        for (int o : offsets) std::printf("%9d", o);
        std::printf("\n   level(dBc) :");
        for (int o : offsets) {
            const int k = peakBin + o;
            const double v = (k >= 0 && k <= n / 2) ? (double)mag[(size_t)k] : 0.0;
            std::printf("%9.1f", 20.0 * std::log10(std::max(v, 1e-12)) - peakDb);
        }
        std::printf("\n");

        // -6 dB 主瓣宽度：从峰值往两侧走到 -6 dB 的 bin 数
        int half = 0;
        const double halfAmp = (double)mag[(size_t)peakBin] * 0.5011872336;   // -6 dB
        while (peakBin + half + 1 <= n / 2 && (double)mag[(size_t)(peakBin + half + 1)] > halfAmp) {
            ++half;
        }
        const int mainHalf = half;
        // 最大旁瓣：只统计**主瓣之外**（离峰值 > 主瓣半宽 + 2 bin）的最大值，
        // 否则宽主瓣的窗（bh）会把主瓣肩部算成"旁瓣"，数字看着比 hann 还差。
        double worst = -200.0;
        for (int k = 0; k <= n / 2; ++k) {
            if (std::abs(k - peakBin) <= mainHalf + 2) continue;
            worst = std::max(worst, 20.0 * std::log10(std::max((double)mag[(size_t)k], 1e-12))
                                        - peakDb);
        }
        std::printf("   -6dB 主瓣宽 ≈ %d bin (%.1f Hz)   主瓣外最大旁瓣 %.1f dBc\n\n",
                    2 * half + 1, (double)(2 * half + 1) * binHz, worst);
    }
    return 0;
}
