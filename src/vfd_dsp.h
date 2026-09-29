// vfd_dsp.h —— 平台无关的频谱分析核心（音频 → 对数频段），零依赖
//
// 三端共用：插件面板、独立 viewer(pipe_server.exe)、ESP32 参考实现。
//
// =============================== 关于分辨率 ===============================
// 分辨率**不是**频段数，而是 **FFT 窗长**：绝对分辨率 ≈ 1/窗长(Hz)，主瓣宽度 = K 个 bin。
//   * 0.17s 窗(5.9Hz bin, BH 主瓣 47Hz)在 10kHz 处只占一列的 0.2% —— 很尖；
//   * 同样的窗在 100Hz 处占 ±23Hz，而对数轴上 100Hz 的一"列"只有 5.8Hz
//     → 一根纯音会横跨 ~10 列，看起来就像"泄漏"（其实那是主瓣，不是旁瓣）。
//
// 所以 Log 频谱用**多分辨率（constant-Q 式）阶梯**：每档是 2 倍长度的 FFT，每个频段
// 用"主瓣刚好塞得进显示一列"的那一档 —— 高频用短窗保时间响应，低频用长窗拿绝对分辨率。
//
// ⚠️ 硬切换会在档位分界处留下**断层**（同一段噪声底，细档的 bin 更窄 → 读数更低，
//    切换处出现一截台阶；实测用户一眼就看出 200Hz / 1kHz 两处）。所以相邻两档在
//    分界附近要做**功率域渐变**（blendOctaves 个倍频程内平滑过渡）—— 断层就没了。
//
// 旁瓣那一半靠窗型解决：默认 Blackman-Harris（旁瓣 -92dBc，落在 -78dB 显示下限以下）。
// 旁瓣体检工具：tools/fft_window_check.cpp。

#pragma once

#include "vfd_fft.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vfddsp {

// ---------------------------------------------------------------- 窗函数
//
// 为什么必须加窗：非整周期截断的正弦会在整个频轴上留下 sinc 泄漏
// （不加窗第一旁瓣 -13.3 dB、之后每倍频程只掉 6 dB）—— 教科书上那道题。
// 加窗后旁瓣被压到 -31 dB(hann) / -58 dB(blackman) / -92 dB(blackman-harris)。
// 用对称窗（分母 n-1），与 numpy.hanning 等价 —— 保证 Python 与 C++ 逐点一致。
enum class WindowType {
    rectangular = 0,     // 不加窗（只用于对照/诊断，正常不要用）
    hann = 1,            // 4 bin 主瓣, 旁瓣 -31 dB
    blackman = 2,        // 6 bin 主瓣, 旁瓣 -58 dB
    blackmanHarris = 3,  // 8 bin 主瓣, 旁瓣 -92 dB  —— 默认
};

inline const char* windowName(WindowType t) {
    switch (t) {
        case WindowType::rectangular:    return "rectangular";
        case WindowType::hann:           return "hann";
        case WindowType::blackman:       return "blackman";
        case WindowType::blackmanHarris: return "blackman-harris";
    }
    return "blackman-harris";
}

// 主瓣(零点到零点)宽度，单位是 bin —— 多分辨率阶梯用它选档
inline int mainLobeBins(WindowType t) {
    switch (t) {
        case WindowType::rectangular:    return 2;
        case WindowType::hann:           return 4;
        case WindowType::blackman:       return 6;
        case WindowType::blackmanHarris: return 8;
    }
    return 8;
}

inline WindowType windowFromName(const char* s) {
    if (s == nullptr) return WindowType::blackmanHarris;
    if (std::strcmp(s, "rect") == 0 || std::strcmp(s, "rectangular") == 0
        || std::strcmp(s, "none") == 0) {
        return WindowType::rectangular;
    }
    if (std::strcmp(s, "hann") == 0 || std::strcmp(s, "hanning") == 0) {
        return WindowType::hann;
    }
    if (std::strcmp(s, "blackman") == 0 || std::strcmp(s, "bm") == 0) {
        return WindowType::blackman;
    }
    return WindowType::blackmanHarris;
}

inline std::vector<float> makeWindow(WindowType type, int n) {
    std::vector<float> w((size_t)std::max(1, n), 1.0f);
    const double twoPi = 6.283185307179586476925286766559;
    const double denom = (double)std::max(1, n - 1);
    for (int i = 0; i < n; ++i) {
        const double x = twoPi * (double)i / denom;
        double v = 1.0;
        switch (type) {
            case WindowType::rectangular: v = 1.0; break;
            case WindowType::hann: v = 0.5 - 0.5 * std::cos(x); break;
            case WindowType::blackman:
                v = 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2.0 * x);
                break;
            case WindowType::blackmanHarris:
            default:
                v = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2.0 * x)
                    - 0.01168 * std::cos(3.0 * x);
                break;
        }
        w[(size_t)i] = (float)v;
    }
    return w;
}

struct Config {
    double fMin = 20.0;
    double fMax = 20000.0;
    double dbMin = -78.0;
    double dbMax = 0.0;
    double tiltDbPerDecade = 3.0;       // 粉噪倾斜补偿
    double gainDb = 0.0;
    double releaseDbPerSec = 55.0;      // 柱体荧光余辉
    double peakReleaseDbPerSec = 21.0;  // 峰值帽（约 3 倍快）

    // ---- 分辨率：多分辨率阶梯的两端（秒）----
    double windowSeconds = 0.18;        // 最短窗 → 高频的时间响应（48k/8192、192k/32768）
    double maxWindowSeconds = 1.4;      // 最长窗 → 低频的绝对分辨率（192k/262144, bin 0.73Hz）
    bool multiResolution = true;        // false = 只用 windowSeconds 单档（无档位断层）
    double blendOctaves = 0.7;          // 档位分界两侧的渐变宽度（倍频程）→ 消掉"断层"
    // 各档是否错开刷新（1/2/4/8 帧一次）。**默认关**：错开会让低频档每 0.33/0.67s
    // 才动一次，屏上就是"每隔半秒跳一下"的呆板感（用户实测一眼看出 500Hz 以下很呆）。
    // 全档每帧都算 = 最灵动，代价是每帧 ~4-9ms（12fps 下占一个核的 5~11%），
    // 在 PC 上无所谓；将来跑 MCU 再打开或加大 minFftOrder。
    bool stageStagger = false;
    int displayColumns = 122;           // 显示列数：用来算"一列多宽"，据此给每个频段选档
    int minFftOrder = 11;               // 2048
    int maxFftOrder = 19;               // 524288（阶梯上限）
    int fftOrder = 0;                   // >0 = 直接指定基础档，覆盖 windowSeconds
    int bands = 0;                      // 0 = 自动 = 基础档的频点数
    int maxBands = 32768;
    WindowType window = WindowType::blackmanHarris;

    // 环境变量开关（不用重编译就能试）：
    //   VFD_WINDOW_S=0.18 / VFD_MAX_WINDOW_S=1.4 / VFD_MULTIRES=0|1 / VFD_BLEND=0.7
    //   VFD_COLS=122 / VFD_WINDOW=rect|hann|blackman|bh
    //   VFD_FFT_ORDER=15 / VFD_BANDS=16384 / VFD_DB_MIN=-78
    static Config fromEnv() {
        Config c;
        if (const char* s = std::getenv("VFD_WINDOW_S")) c.windowSeconds = std::atof(s);
        if (const char* s = std::getenv("VFD_MAX_WINDOW_S")) c.maxWindowSeconds = std::atof(s);
        if (const char* s = std::getenv("VFD_MULTIRES")) c.multiResolution = (std::atoi(s) != 0);
        if (const char* s = std::getenv("VFD_BLEND")) c.blendOctaves = std::atof(s);
        if (const char* s = std::getenv("VFD_COLS")) c.displayColumns = std::atoi(s);
        if (const char* s = std::getenv("VFD_FFT_ORDER")) c.fftOrder = std::atoi(s);
        if (const char* s = std::getenv("VFD_BANDS")) c.bands = std::atoi(s);
        if (const char* s = std::getenv("VFD_WINDOW")) c.window = windowFromName(s);
        if (const char* s = std::getenv("VFD_DB_MIN")) c.dbMin = std::atof(s);
        if (const char* s = std::getenv("VFD_STAGGER")) c.stageStagger = (std::atoi(s) != 0);
        return c;
    }
};

class Analyzer {
public:
    void prepare(double sampleRate, const Config& cfg) {
        cfg_ = cfg;
        sampleRate_ = sampleRate > 1.0 ? sampleRate : 48000.0;

        // ---- 1) 建窗阶阶梯（每档长度翻倍）------------------------------------
        const int baseOrder = cfg_.fftOrder > 0 ? clampOrder(cfg_.fftOrder)
                                                : autoOrder(cfg_.windowSeconds);
        int topOrder = baseOrder;
        if (cfg_.multiResolution) {
            while (topOrder < cfg_.maxFftOrder
                   && (double)(1 << (topOrder + 1)) / sampleRate_ <= cfg_.maxWindowSeconds) {
                ++topOrder;
            }
        }
        st_.clear();
        for (int o = baseOrder; o <= topOrder; ++o) {
            Stage s;
            s.order = o;
            s.n = 1 << o;
            s.bins = s.n / 2 + 1;
            s.binHz = sampleRate_ / (double)s.n;
            s.fft.prepare(o);
            s.win = makeWindow(cfg_.window, s.n);
            double sum = 0.0;
            for (float v : s.win) sum += (double)v;
            s.wgain = sum > 0.0 ? sum : 1.0;
            s.frame.assign((size_t)s.n, 0.0f);
            s.mags.assign((size_t)s.bins, 0.0f);
            // 错开刷新只对"算不动"的场合有意义（见 Config::stageStagger 的注释）
            s.divisor = cfg_.stageStagger ? (1 << (int)st_.size()) : 1;
            st_.push_back(std::move(s));
        }

        // ---- 2) 对数频段网格（数量 = 基础档的频点数）--------------------------
        nBands_ = cfg_.bands > 0 ? cfg_.bands : st_[0].bins;
        nBands_ = std::max(8, std::min(cfg_.maxBands, nBands_));
        const double fHi = std::min(cfg_.fMax, sampleRate_ * 0.5 * 0.97);
        edges_.assign((size_t)nBands_ + 1, 0.0);
        const double logLo = std::log(cfg_.fMin);
        const double logHi = std::log(fHi);
        for (int i = 0; i <= nBands_; ++i) {
            edges_[(size_t)i] = std::exp(logLo + (logHi - logLo) * (double)i / (double)nBands_);
        }

        // ---- 3) 每个频段选档 + 在分界附近做功率域渐变 ------------------------
        //   want(f)  = 显示一列的宽度(Hz)
        //   lobe(s)  = 第 s 档的主瓣宽度(Hz) = mainLobeBins * fs / N_s
        //   选档规则：取"主瓣 ≤ want"的最小档 sHi（= 时间响应最好的一档）；
        //   sLo = sHi+1（更长的一档）。权重按 log2(lobe(sHi)/want) 在 ±blendOctaves
        //   之间线性过渡：深处用 sLo，越过分界用 sHi，中间按功率混合 → 没有断层。
        const double cols = std::max(8, cfg_.displayColumns);
        const double colRatio = std::pow(cfg_.fMax / cfg_.fMin, 1.0 / cols) - 1.0;
        const int lobe = mainLobeBins(cfg_.window);
        const int nst = (int)st_.size();
        wHi_.assign((size_t)nBands_, 1.0f);
        stageHi_.assign((size_t)nBands_, 0);
        stageLo_.assign((size_t)nBands_, -1);
        for (int b = 0; b < nBands_; ++b) {
            const double fMid = std::sqrt(edges_[(size_t)b] * edges_[(size_t)b + 1]);
            const double want = std::max(fMid * colRatio, 1e-9);
            // sMain = **够用的最短窗**（时间响应最好的一档）：最小的、主瓣 ≤ want 的档。
            int sMain = nst - 1;
            for (int si = 0; si < nst; ++si) {
                const double thisLobe = (double)lobe * sampleRate_ / (double)st_[si].n;
                if (thisLobe <= want) { sMain = si; break; }
            }
            // partner = 比它再短一档：主瓣略宽于 want（"差一点就够用"）。只在分界附近
            // 混一点进来，把硬切换的台阶抹平；深处完全用 sMain。
            const int partner = (sMain > 0) ? sMain - 1 : -1;
            double wFast = 0.0;
            if (partner >= 0) {
                const double rPart =
                    ((double)lobe * sampleRate_ / (double)st_[(size_t)partner].n) / want;
                wFast = std::max(0.0, std::min(1.0, 0.5 - std::log2(std::max(rPart, 1e-9))
                                                           / (2.0 * std::max(0.05, cfg_.blendOctaves))));
            }
            stageHi_[(size_t)b] = sMain;
            stageLo_[(size_t)b] = partner;
            wHi_[(size_t)b] = (float)(1.0 - wFast);
        }
        // 每一档负责哪些频段（可能两档共同负责同一频段 —— 渐变区）
        for (Stage& s : st_) s.own.clear();
        for (int b = 0; b < nBands_; ++b) {
            if (wHi_[(size_t)b] > 1e-4f) {
                st_[(size_t)stageHi_[(size_t)b]].own.push_back({b, true});      // 记到 hi 槽
            }
            const int sl = stageLo_[(size_t)b];
            if (sl >= 0 && wHi_[(size_t)b] < 1.0f - 1e-4f) {
                st_[(size_t)sl].own.push_back({b, false});                     // 记到 lo 槽
            }
        }

        // ---- 4) 每一档建自己的"频段 → bin"表（只建它负责的频段）--------------
        subBinBands_ = 0;
        for (Stage& s : st_) {
            s.lo.clear();
            s.hi.clear();
            s.interp.clear();
            s.mid.clear();
            s.lo.reserve(s.own.size());
            s.hi.reserve(s.own.size());
            s.interp.reserve(s.own.size());
            s.mid.reserve(s.own.size());
            for (const BandOwner& o : s.own) {
                const int b = o.band;
                int lo = (int)std::floor(edges_[(size_t)b] / s.binHz);
                int hi = (int)std::ceil(edges_[(size_t)(b + 1)] / s.binHz);
                lo = std::max(0, std::min(s.bins - 1, lo));
                hi = std::max(lo + 1, std::min(s.bins, hi));
                s.lo.push_back(lo);
                s.hi.push_back(hi);
                s.interp.push_back((hi - lo < 2) ? 1 : 0);
                s.mid.push_back(std::sqrt(edges_[(size_t)b] * edges_[(size_t)(b + 1)]));
                if (hi - lo < 2) ++subBinBands_;
            }
        }

        // ---- 5) 倾斜补偿、状态缓冲、环形缓冲 ---------------------------------
        tilt_.assign((size_t)nBands_, 0.0f);
        for (int b = 0; b < nBands_; ++b) {
            const double mid = std::sqrt(edges_[(size_t)b] * edges_[(size_t)(b + 1)]);
            tilt_[(size_t)b] =
                (float)(cfg_.tiltDbPerDecade * std::log10(std::max(mid, 1.0) / 1000.0));
        }
        magHi_.assign((size_t)nBands_, 0.0f);
        magLo_.assign((size_t)nBands_, 0.0f);
        bandMag_.assign((size_t)nBands_, 0.0f);
        dtAccum_.assign((size_t)nBands_, 0.0);
        bands_.assign((size_t)nBands_, 0.0f);
        peaks_.assign((size_t)nBands_, 0.0f);
        const int cap = std::max(1024, 2 * st_.back().n);
        ring_.assign((size_t)cap, 0.0f);
        ringPos_ = 0;
        ringFill_ = 0;
    }

    // ---- 诊断信息 ----
    int stageCount() const { return (int)st_.size(); }
    int stageFftSize(int i) const { return (i >= 0 && i < (int)st_.size()) ? st_[(size_t)i].n : 0; }
    double stageWindowMs(int i) const {
        return (i >= 0 && i < (int)st_.size())
                   ? 1000.0 * (double)st_[(size_t)i].n / sampleRate_ : 0.0;
    }
    int stageDivisor(int i) const {
        return (i >= 0 && i < (int)st_.size()) ? st_[(size_t)i].divisor : 1;
    }
    // 该档负责的频段数与频率范围（用于打印诊断：档位分界落在哪些 Hz）
    int stageBandCount(int i) const {
        return (i >= 0 && i < (int)st_.size()) ? (int)st_[(size_t)i].own.size() : 0;
    }
    double stageBandFreq(int i, bool first) const {
        if (i < 0 || i >= (int)st_.size() || st_[(size_t)i].own.empty()) return 0.0;
        const int b = first ? st_[(size_t)i].own.front().band : st_[(size_t)i].own.back().band;
        return std::sqrt(edges_[(size_t)b] * edges_[(size_t)(b + 1)]);
    }
    // 基础档（= 频段数/显示分辨率基准）的信息，保持旧接口不变
    int fftSize() const { return st_.empty() ? 0 : st_[0].n; }
    int fftOrder() const { return st_.empty() ? 0 : st_[0].order; }
    int bins() const { return st_.empty() ? 0 : st_[0].bins; }
    int numBands() const { return nBands_; }
    double binHz() const { return st_.empty() ? 1.0 : st_[0].binHz; }
    double windowMs() const { return stageWindowMs(0); }
    double maxWindowMs() const { return stageWindowMs((int)st_.size() - 1); }
    int subBinBands() const { return subBinBands_; }
    // 频段 b 的几何中心频率（诊断/工具用；屏上的"频标"就是按它算列号的）
    double bandFreq(int b) const {
        if (b < 0 || b >= nBands_) return 0.0;
        return std::sqrt(edges_[(size_t)b] * edges_[(size_t)(b + 1)]);
    }
    // 哪个频段管这个频率（edges_ 单调，二分）
    int bandForFreq(double f) const {
        if (edges_.empty()) return 0;
        const auto it = std::upper_bound(edges_.begin(), edges_.end(), f);
        int b = (int)(it - edges_.begin()) - 1;
        if (b < 0) b = 0;
        if (b > nBands_ - 1) b = nBands_ - 1;
        return b;
    }
    // 频段 b 当前的显示电平（dB，已含倾斜/增益/回落）
    double bandDb(int b) const {
        if (b < 0 || b >= nBands_) return cfg_.dbMin;
        return cfg_.dbMin + (double)bands_[(size_t)b] * (cfg_.dbMax - cfg_.dbMin);
    }
    // 所有频段里的最大值（dB）—— 用来一眼判断"整块屏是不是全黑"
    double peakDb() const {
        float m = 0.0f;
        for (float v : bands_) m = std::max(m, v);
        return cfg_.dbMin + (double)m * (cfg_.dbMax - cfg_.dbMin);
    }
    double colRatio() const {
        const double cols = std::max(8, cfg_.displayColumns);
        return std::pow(cfg_.fMax / cfg_.fMin, 1.0 / cols) - 1.0;
    }

    // ---- 音频输入 ----
    void push(const float* mono, int n) {
        if (n <= 0 || ring_.empty()) return;
        const int cap = (int)ring_.size();
        if (n >= cap) {
            std::copy(mono + (n - cap), mono + n, ring_.begin());
            ringPos_ = 0;
            ringFill_ = cap;
            return;
        }
        const int first = std::min(n, cap - ringPos_);
        std::copy(mono, mono + first, ring_.begin() + ringPos_);
        if (n > first) std::copy(mono + first, mono + n, ring_.begin());
        ringPos_ = (ringPos_ + n) % cap;
        ringFill_ = std::min(cap, ringFill_ + n);
    }

    int buffered() const { return ringFill_; }

    // ---- 每帧推进 -----------------------------------------------------------
    // 各档按自己的节奏刷新它负责的频段（长窗天然变化慢，没必要每帧重算）；
    // 然后统一算幅度（渐变区按功率混合）→ dB → 回落/峰值。
    int process(double dtSeconds) {
        const double dt = std::max(0.0, dtSeconds);
        bool anyRefresh = false;
        for (int b = 0; b < nBands_; ++b) dtAccum_[(size_t)b] += dt;

        for (Stage& s : st_) {
            const bool refresh = (s.tick++ % s.divisor) == 0;
            if (!refresh || s.own.empty() || ringFill_ <= 0) continue;
            refreshMeasure(s);
            anyRefresh = true;
            // ⚠️ 这里**不能**清零 dtAccum_：清零必须发生在 applyBands **用完之后**。
            // 曾经的写法是"量完立刻清零"，而 applyBands 里又有 `if (acc <= 0) continue`，
            // 于是"刚量过的那一档"永远被跳过 —— divisor==1 的第 0 档（495Hz~20kHz）
            // 首当其冲，一个频段都不更新，屏上 500Hz 以上全黑（放音乐一刀切）。
            // 其它档 divisor 是 2/4/8，隔帧才有 acc>0，靠"用上一帧旧值"歪打正着地活着。
        }
        applyBands(anyRefresh);
        return nBands_;
    }

    const float* bands() const { return bands_.data(); }
    const float* peaks() const { return peaks_.data(); }

private:
    struct BandOwner {
        int band = 0;
        bool isHi = true;      // true → 写 magHi_，false → 写 magLo_
    };
    struct Stage {
        int order = 0, n = 0, bins = 0;
        double binHz = 0.0, wgain = 1.0;
        vfdfft::RealFft fft;
        std::vector<float> win, frame, mags;
        std::vector<BandOwner> own;         // 负责的频段（可能与其他档共享）
        std::vector<int> lo, hi;
        std::vector<uint8_t> interp;
        std::vector<double> mid;
        int divisor = 1, tick = 0;
    };

    int clampOrder(int o) const {
        return std::max(cfg_.minFftOrder, std::min(cfg_.maxFftOrder, o));
    }

    int autoOrder(double seconds) const {
        int order = cfg_.minFftOrder;
        while (order < cfg_.maxFftOrder) {
            const double size = (double)(1 << (order + 1));
            if (size / sampleRate_ > seconds) break;
            ++order;
        }
        return order;
    }

    void refreshMeasure(Stage& s) {
        const int n = s.n;
        const int have = std::min(ringFill_, n);
        if (have < n) std::fill(s.frame.begin(), s.frame.begin() + (n - have), 0.0f);
        const int cap = (int)ring_.size();
        int start = (ringPos_ - have) % cap;
        if (start < 0) start += cap;
        for (int i = 0; i < have; ++i) {
            s.frame[(size_t)(n - have + i)] = ring_[(size_t)((start + i) % cap)];
        }
        for (int i = 0; i < n; ++i) s.frame[(size_t)i] *= s.win[(size_t)i];
        s.fft.magnitude(s.frame.data(), s.mags.data());
        const float scale = (float)(2.0 / s.wgain);
        for (int i = 0; i < s.bins; ++i) s.mags[(size_t)i] *= scale;

        for (size_t k = 0; k < s.own.size(); ++k) {
            float level;
            if (s.interp[k] != 0) {
                level = interpMagStage(s, s.mid[k]);
            } else {
                level = 0.0f;
                for (int i = s.lo[k]; i < s.hi[k]; ++i) level = std::max(level, s.mags[(size_t)i]);
            }
            if (s.own[k].isHi) magHi_[(size_t)s.own[k].band] = level;
            else               magLo_[(size_t)s.own[k].band] = level;
        }
    }

    float interpMagStage(const Stage& s, double f) const {
        if (s.mags.empty()) return 0.0f;
        const double x = f / s.binHz;
        if (x <= 0.0) return s.mags[0];
        const int k = (int)x;
        if (k >= s.bins - 1) return s.mags[(size_t)(s.bins - 1)];
        const float fr = (float)(x - (double)k);
        return s.mags[(size_t)k] * (1.0f - fr) + s.mags[(size_t)(k + 1)] * fr;
    }

    // 只在"该频段这一帧有新测量"时动回落，回落量用距上次刷新的累计时间：
    // 否则长窗档的频段会在两次刷新之间一路塌下去（12fps 下 8 帧 = 0.67s，55dB/s 掉 37dB）。
    // 判据 = dtAccum_ > 0（= 距上次刷新过了多久）；**用掉之后才清零**（见 process() 里的注释）。
    void applyBands(bool anyRefresh) {
        if (!anyRefresh) return;
        const double span = cfg_.dbMax - cfg_.dbMin;
        for (int b = 0; b < nBands_; ++b) {
            const double acc = dtAccum_[(size_t)b];
            if (acc <= 0.0) continue;                 // 这一帧没有新测量 → 保持不动
            const double w = (double)wHi_[(size_t)b];
            const double hi = (double)magHi_[(size_t)b];
            const double lo = (stageLo_[(size_t)b] >= 0) ? (double)magLo_[(size_t)b] : 0.0;
            const double p = w * hi * hi + (1.0 - w) * lo * lo;      // 功率域混合
            const double mag = std::sqrt(std::max(p, 0.0));
            const double db = 20.0 * std::log10(std::max(mag, 1e-7))
                              + (double)tilt_[(size_t)b] + cfg_.gainDb;
            const float norm = (float)std::max(0.0, std::min(1.0, (db - cfg_.dbMin) / span));
            const float fall = (float)(cfg_.releaseDbPerSec * acc / span);
            const float pfall = (float)(cfg_.peakReleaseDbPerSec * acc / span);
            bands_[(size_t)b] = std::max(norm, bands_[(size_t)b] - fall);
            peaks_[(size_t)b] = std::max(norm, peaks_[(size_t)b] - pfall);
            dtAccum_[(size_t)b] = 0.0;                // 用完了才清零
        }
    }

    Config cfg_{};
    double sampleRate_ = 48000.0;
    int nBands_ = 0;
    int subBinBands_ = 0;
    std::vector<Stage> st_;
    std::vector<double> edges_;
    std::vector<int> stageHi_, stageLo_;
    std::vector<float> wHi_;
    std::vector<float> magHi_, magLo_, bandMag_;
    std::vector<double> dtAccum_;
    std::vector<float> tilt_, bands_, peaks_, ring_;
    int ringPos_ = 0, ringFill_ = 0;
};

// ---------------------------------------------------------------- 声量计 / 峰值计
//
// foobar2000 那套"声量计(VU) + 峰值计"的两条弹道：
//   * VU = 对功率做一阶 IIR 平均（经典 VU 表 300 ms 时间常数），→ 看着"肉"，像指针表；
//   * 峰值 = 本块样本峰的即时值 + dB/s 线性回落（快起慢落），无额外延时保持段。
// 两者都归一化到 [dbMin, 0] dBFS，与底部那条横向 dBFS 刻度严格对应（线性 dB 映射）。
//
// 只在 UI 端算：输入就是管道透传过来的交错音频（协议 v4），音频进程不参与。
struct MeterConfig {
    double dbMin = -60.0;               // 计量下限 dBFS（= 底部刻度的起点）
    double vuTauSeconds = 0.30;         // VU 时间常数
    double peakReleaseDbPerSec = 24.0;  // 峰值回落速度
    int channels = 2;                   // 只做前两个（L/R）
};

class MeterBank {
public:
    void prepare(double sampleRate, const MeterConfig& cfg) {
        cfg_ = cfg;
        sampleRate_ = sampleRate > 1.0 ? sampleRate : 48000.0;
        nCh_ = std::max(1, std::min(4, cfg.channels));
        sumSq_.assign((size_t)nCh_, 0.0);
        count_.assign((size_t)nCh_, 0);
        blockPeak_.assign((size_t)nCh_, 0.0f);
        power_.assign((size_t)nCh_, 0.0);
        peakLin_.assign((size_t)nCh_, 0.0);
        vuNorm_.assign((size_t)nCh_, 0.0f);
        peakNorm_.assign((size_t)nCh_, 0.0f);
        prepared_ = true;
    }

    bool ready() const { return prepared_; }
    int channels() const { return nCh_; }

    // 交错输入：x[i*channels + c]
    void pushInterleaved(const float* x, int frames, int channels) {
        if (!prepared_ || x == nullptr || frames <= 0) return;
        const int ch = std::max(1, channels);
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < nCh_; ++c) {
                const int src = c < ch ? c : ch - 1;      // 单声道源就两边都一样
                const float v = x[(size_t)i * (size_t)ch + (size_t)src];
                sumSq_[(size_t)c] += (double)v * (double)v;
                const float a = std::fabs(v);
                if (a > blockPeak_[(size_t)c]) blockPeak_[(size_t)c] = a;
            }
        }
        for (int c = 0; c < nCh_; ++c) count_[(size_t)c] += frames;
    }

    // 推进弹道（UI 端每帧调一次）
    void process(double dtSeconds) {
        if (!prepared_) return;
        const double dt = std::max(0.0, dtSeconds);
        const double range = 0.0 - cfg_.dbMin;            // dbMin(-60) → 0 dBFS，range = +60
        const double alpha = (cfg_.vuTauSeconds > 1e-6)
                                 ? (1.0 - std::exp(-dt / cfg_.vuTauSeconds))
                                 : 1.0;
        const double decayLin = std::pow(10.0, -(cfg_.peakReleaseDbPerSec * dt) / 20.0);
        for (int c = 0; c < nCh_; ++c) {
            const double n = (double)count_[(size_t)c];
            const double ms = (n > 0.0) ? sumSq_[(size_t)c] / n : 0.0;   // 没数据当静音处理
            power_[(size_t)c] += alpha * (ms - power_[(size_t)c]);
            peakLin_[(size_t)c] = std::max((double)blockPeak_[(size_t)c],
                                           peakLin_[(size_t)c] * decayLin);
            vuNorm_[(size_t)c] = normFromLin(std::sqrt(power_[(size_t)c]), range);
            peakNorm_[(size_t)c] = normFromLin(peakLin_[(size_t)c], range);
            sumSq_[(size_t)c] = 0.0;
            count_[(size_t)c] = 0;
            blockPeak_[(size_t)c] = 0.0f;
        }
    }

    const float* vuNorm() const { return vuNorm_.data(); }
    const float* peakNorm() const { return peakNorm_.data(); }
    double vuDb(int c) const { return linToDb(std::sqrt(power_[(size_t)c])); }
    double peakDb(int c) const { return linToDb(peakLin_[(size_t)c]); }

private:
    static double linToDb(double v) { return 20.0 * std::log10(std::max(v, 1e-9)); }
    // range = 0 - dbMin（正数）：norm = (dB - dbMin) / range ∈ [0,1]
    float normFromLin(double v, double range) const {
        if (range <= 1e-9) return 0.0f;
        const double t = (linToDb(v) - cfg_.dbMin) / range;
        return (float)std::max(0.0, std::min(1.0, t));
    }

    MeterConfig cfg_{};
    double sampleRate_ = 48000.0;
    int nCh_ = 2;
    bool prepared_ = false;
    std::vector<double> sumSq_;
    std::vector<int> count_;
    std::vector<float> blockPeak_;
    std::vector<double> power_, peakLin_;
    std::vector<float> vuNorm_, peakNorm_;
};

}  // namespace vfddsp
