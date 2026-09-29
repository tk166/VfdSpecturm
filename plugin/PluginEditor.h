// PluginEditor.h —— 面板：把 VFD 频谱画在 EAPO 的配置编辑器里
//
// 这是本插件存在的意义所在：音频实例在 audiodg(session 0、无桌面、不能建窗口)，
// 而面板实例在配置编辑器进程(用户会话) —— 两者只能靠命名管道连通。
//
// 协议 v3 起分工翻转：audiodg 侧只**透传音频**，本面板做**全部重活**
// （窗 + FFT + 对数分频 + 回落/峰值 + 渲染）。这样做的好处：
//   * audiodg 里没有 DSP，少一个把系统音频搞崩的机会；
//   * 分辨率不再被"管道里传多少个频段"限制，本进程可以直接开 32768/65536 点 FFT；
//   * 想看更多细节就调 VFD_WINDOW_S / VFD_BANDS（见 src/vfd_dsp.h 的 Config::fromEnv）。
//
// 另外它也承担"必须存在"的义务：EAPO 的 "Open panel" 对没有编辑器的插件会踩空
// effEditGetRect（ticket 138）。

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <vector>

#include "PanelConsumer.h"
#include "PluginProcessor.h"
#include "vfd_dsp.h"
#include "vfd_render.h"

class VfdEapoAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    private juce::Timer {
public:
    explicit VfdEapoAudioProcessorEditor(VfdEapoAudioProcessor&);
    ~VfdEapoAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override {}
    // 单击点阵屏 = 循环切换主题配色（顺序见 vfdrender::Theme）
    void mouseUp(const juce::MouseEvent&) override;
    void cycleTheme();

private:
    void timerCallback() override;
    void ensureHeader();
    void renderIntoImage();
    void updateWaitingStatus();
    void rebuildAnalyzer(int sampleRate);

    VfdEapoAudioProcessor& proc_;
    vfdspec::PanelConsumer consumer_;

    // 重活全在这里：vfddsp::Analyzer 自带环形缓冲，喂进去的是原始样本
    vfddsp::Config dspCfg_;
    vfddsp::Analyzer analyzer_;
    vfddsp::MeterBank meters_;         // 声量计/峰值计（L/R）
    std::vector<float> pull_;          // readNew 的落地缓冲（交错立体声）
    std::vector<float> mono_;          // 混单后喂给频谱分析器的那一份
    int pullFrames_ = 32768;
    int appliedRate_ = 0;

    // 点阵网格：宽度 127 列（508px）不变；高度按"整块显示区 +20%"给到 **90 行**
    // （360px = 原来的 300px × 1.2）。多出来的 60px 全部进频谱绘图区（30 行 → 47 行），
    // 声量计条仍是 4 行、底部 dBFS 刻度也原样 —— 也就是"只有频谱变大"。
    // 想临时试别的高度：设 VFD_ROWS=<行数> 重开面板（不必重新编译）。
    static constexpr int kGridRows = 90;
    vfdrender::Screen screen_{127, kGridRows, 4, 3};
    juce::Image image_;

    bool headerSet_ = false;
    int lastRate_ = 0;
    int lastCh_ = 0;
    int lastBands_ = 0;

    double lastTickTime_ = 0.0;     // 上一次 timer 的墙钟（算 dt：磷光暂留 / 回落 / 补零）
    double lastRenderWall_ = 0.0;   // 上一次真正渲染的墙钟(空转时限速)
    double analyzeMs_ = 0.0;
    double renderMs_ = 0.0;
    bool staleBuild_ = false;       // 面板与 audiodg 跑的不是同一版 DLL
    // 底部那行调试信息（jump/drop/FFT/耗时/build 戳）默认**不画**：屏幕空间还给频谱。
    // 排查问题时设 VFD_SHOW_STATUS=1 就回来（它占窗口高度，不占点阵）。
    bool showStatus_ = false;
    juce::String status_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VfdEapoAudioProcessorEditor)
};
