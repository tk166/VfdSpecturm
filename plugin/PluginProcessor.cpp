#include "PluginProcessor.h"

#include <cstring>

#include "PluginEditor.h"

VfdEapoAudioProcessor::VfdEapoAudioProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    // 这里必须"什么都不做"：EAPO 每次配置加载和每次音频重锁都会重新实例化插件，
    // 而这一切发生在音频服务里。不碰文件、不初始化 COM、不做 FFT 表、不 spawn 线程
    // （Sender 的 worker 线程在成员构造时启动，那是被允许的轻量操作）。
}

void VfdEapoAudioProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/) {
    // 只登记采样率；FFT 表的重建与内存分配由 worker 线程自己做（见 SpectrumSender）。
    sender_.requestSampleRate(sampleRate);
}

void VfdEapoAudioProcessor::releaseResources() {}

void VfdEapoAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                        juce::MidiBuffer& midiMessages) {
    juce::ignoreUnused(midiMessages);
    juce::ScopedNoDenormals noDenormals;

    // 纯分析插件：不改动音频，直通。
    // 唯一做的事就是把样本丢进无锁环形缓冲（无锁、无分配、无系统调用）。
    sender_.pushAudio(buffer.getArrayOfReadPointers(), buffer.getNumChannels(),
                      buffer.getNumSamples());
}

juce::AudioProcessorEditor* VfdEapoAudioProcessor::createEditor() {
    return new VfdEapoAudioProcessorEditor(*this);
}

void VfdEapoAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    // 无参数。写一个短版本标记，方便在 EAPO 的 config.txt 里看出配置是谁写的。
    // （EAPO 会把它 base64 进 `ChunkData "..."`；空块则走参数路径。）
    static const char tag[] = "VFDSPECT1";
    destData.setSize(sizeof(tag) - 1);
    std::memcpy(destData.getData(), tag, sizeof(tag) - 1);
}

void VfdEapoAudioProcessor::setStateInformation(const void* data, int sizeInBytes) {
    juce::ignoreUnused(data, sizeInBytes);   // 忽略即可，但绝不能抛
}

// VST2 入口：EAPO 通过 GetProcAddress(module, "VSTPluginMain") 找它。
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new VfdEapoAudioProcessor();
}
