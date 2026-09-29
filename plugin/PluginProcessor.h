// PluginProcessor.h —— EAPO 用的 VST2 插件（频谱数据生产者）
//
// 分工（每一条都有出处，见 docs/508x302-feasibility.md）：
//   * 本插件跑在 audiodg.exe 里（Session 0、LocalService、无交互桌面）→ 不能建窗口、
//     收不到 effEditOpen/effEditIdle。真正的显示在另一个进程（viewer）。
//   * processBlock 只做"混单 + 写无锁环形缓冲"，其余全在 SpectrumSender 的 worker 线程。
//   * 构造函数与 VSTPluginMain 必须极快：EAPO 没有插件扫描缓存，且 APO 连续失败
//     10 次会被系统整体禁用（PKEY_Endpoint_Disable_SysFx）。

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "SpectrumSender.h"

class VfdEapoAudioProcessor : public juce::AudioProcessor {
public:
    VfdEapoAudioProcessor();
    ~VfdEapoAudioProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "VFD Spectrum Sender"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    vfdspec::SpectrumSender& sender() noexcept { return sender_; }

private:
    vfdspec::SpectrumSender sender_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VfdEapoAudioProcessor)
};
