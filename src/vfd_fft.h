// vfd_fft.h —— 零依赖的实数 FFT（radix-2，就地，预计算旋转因子）
//
// 为什么自己写一个而不是用 juce::dsp::FFT / numpy：
//   1) 同一份实现要同时给 **插件面板**、**独立 viewer(pipe_server)** 和将来的
//      **ESP32 参考实现** 用 —— 后者只能用 C99，用不了 JUCE；
//   2) FFT 是"分辨率"这件事的唯一真源头，把它放在共享头里，三端就不可能出现
//      "python 用 8192 点、C++ 用 4096 点"这种对不上的情况（先前就是这样）。
//
// 口径与 numpy.fft.rfft 对齐：输入实数 x[0..N-1]，输出 mag[k] = |X[k]|，k=0..N/2。
// 调用方负责窗与增益缩放（与 vfd_spectrum.py 的 2/wgain 一致）。
//
// 复杂度 O(N log N) 标量；32768 点一次约 1~2 ms（含开方），12~24 fps 下可忽略。

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace vfdfft {

class RealFft {
public:
    // order = log2(N)，N 必须 >= 2。重复调用同一 order 不会重建表。
    void prepare(int order) {
        if (order < 1) order = 1;
        if (order == order_ && n_ == (1 << order) && !rev_.empty()) return;
        order_ = order;
        n_ = 1 << order;
        re_.assign((size_t)n_, 0.0f);
        im_.assign((size_t)n_, 0.0f);
        rev_.assign((size_t)n_, 0);
        halfCos_.assign((size_t)n_ / 2, 1.0f);
        halfSin_.assign((size_t)n_ / 2, 0.0f);

        // 位反转表
        for (int i = 0; i < n_; ++i) {
            int r = 0;
            for (int b = 0; b < order_; ++b) {
                r = (r << 1) | ((i >> b) & 1);
            }
            rev_[(size_t)i] = r;
        }
        // 旋转因子 e^{-i 2pi k / N}，k = 0..N/2-1
        const double twoPi = 6.283185307179586476925286766559;
        for (int k = 0; k < n_ / 2; ++k) {
            const double a = -twoPi * (double)k / (double)n_;
            halfCos_[(size_t)k] = (float)std::cos(a);
            halfSin_[(size_t)k] = (float)std::sin(a);
        }
    }

    int size() const { return n_; }
    int order() const { return order_; }
    int magSize() const { return n_ / 2 + 1; }

    // 就地变换：x 为 n 个实数样本；结果幅度写进 mag[0..n/2]。
    void magnitude(const float* x, float* mag) {
        if (n_ <= 0) return;
        for (int i = 0; i < n_; ++i) {
            re_[(size_t)i] = x[(size_t)rev_[(size_t)i]];
            im_[(size_t)i] = 0.0f;
        }
        for (int len = 2; len <= n_; len <<= 1) {
            const int half = len >> 1;
            const int step = n_ / len;                  // 旋转因子步长
            for (int i = 0; i < n_; i += len) {
                for (int j = 0; j < half; ++j) {
                    const float wr = halfCos_[(size_t)(j * step)];
                    const float wi = halfSin_[(size_t)(j * step)];
                    const int a = i + j;
                    const int b = a + half;
                    const float xr = re_[(size_t)b] * wr - im_[(size_t)b] * wi;
                    const float xi = re_[(size_t)b] * wi + im_[(size_t)b] * wr;
                    re_[(size_t)b] = re_[(size_t)a] - xr;
                    im_[(size_t)b] = im_[(size_t)a] - xi;
                    re_[(size_t)a] += xr;
                    im_[(size_t)a] += xi;
                }
            }
        }
        for (int k = 0; k <= n_ / 2; ++k) {
            const float r = re_[(size_t)k], im = im_[(size_t)k];
            mag[(size_t)k] = std::sqrt(r * r + im * im);
        }
    }

private:
    int order_ = 0;
    int n_ = 0;
    std::vector<float> re_, im_, halfCos_, halfSin_;
    std::vector<int> rev_;
};

}  // namespace vfdfft
