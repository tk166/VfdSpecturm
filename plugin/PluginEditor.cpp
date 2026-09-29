#include "PluginEditor.h"

#include <cstdlib>

#include "BuildStamp.h"

namespace {
const juce::Colour kBg{0xff04070a};
const juce::Colour kStatus{0xff35ff9e};
const juce::Colour kWarn{0xffffc247};
constexpr int kMargin = 8;
constexpr int kStatusH = 18;
constexpr float kPhosphorTau = 0.07f;   // 与 Python 参考实现一致

// 主题选择在**进程内**记住：关掉面板再打开还是上次那个颜色（重启进程才回默认）。
// 不做持久化是有意的 —— 这是"看着舒服"的旋钮，不值得往磁盘上写东西。
int g_themeIndex = 0;

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}
}  // namespace

VfdEapoAudioProcessorEditor::VfdEapoAudioProcessorEditor(VfdEapoAudioProcessor& p)
    : juce::AudioProcessorEditor(&p), proc_(p) {
    showStatus_ = envFlag("VFD_SHOW_STATUS");   // 默认不画底部调试行（把空间还给频谱）
    // 网格高度可用环境变量覆盖（重开面板即可试，不用重新编译）
    if (const char* r = std::getenv("VFD_ROWS")) {
        const int rows = juce::jlimit(40, 200, std::atoi(r));
        if (rows != kGridRows) screen_ = vfdrender::Screen(127, rows, 4, 3);
    }
    if (const char* t = std::getenv("VFD_THEME")) g_themeIndex = std::atoi(t);  // 固定起始主题
    screen_.setTheme((vfdrender::Theme)(g_themeIndex % vfdrender::Screen::themeCount()));
    setSize(screen_.width() + kMargin * 2,
            screen_.height() + kMargin * 2 + (showStatus_ ? kStatusH : 0));
    setResizable(false, false);
    image_ = juce::Image(juce::Image::RGB, screen_.width(), screen_.height(), true);
    pullFrames_ = 32768;                              // 12fps@192k 每拍 ~16.0k 帧，留 2 倍余量
    pull_.assign((size_t)pullFrames_ * vfdpipe::kAudioChannels, 0.0f);
    mono_.assign((size_t)pullFrames_, 0.0f);
    dspCfg_ = vfddsp::Config::fromEnv();
    status_ = "starting consumer ...";
    // 面板是"编辑器和 audiodg 各有一个实例"里的编辑器那一半。把 build 戳写进日志，
    // 换 DLL 之后能立刻确认编辑器有没有真的加载新文件（audiodg 那半看 VFD_DEBUG 日志）。
    juce::Logger::writeToLog(juce::String("vfd panel: build ") + VFD_BUILD_STAMP
                             + ", protocol v3, pipe " + juce::String(vfdpipe::pipeName()));
    startTimerHz(12);      // 面板 12 fps 足够：点阵 + 磷光看起来是连续的
}

VfdEapoAudioProcessorEditor::~VfdEapoAudioProcessorEditor() {
    stopTimer();
}

void VfdEapoAudioProcessorEditor::rebuildAnalyzer(int sampleRate) {
    analyzer_.prepare((double)sampleRate, dspCfg_);
    appliedRate_ = sampleRate;
    vfddsp::MeterConfig mcfg;
    mcfg.dbMin = (double)vfdrender::kMeterDbMin;
    meters_.prepare((double)sampleRate, mcfg);
    // 头一次拿到采样率时打一行日志（面板在配置编辑器里，看不到 stderr 也没关系）
    juce::Logger::writeToLog(juce::String("vfd panel: ") + juce::String(sampleRate)
                             + " Hz -> FFT " + juce::String(analyzer_.fftSize())
                             + " (" + juce::String(analyzer_.windowMs(), 1) + " ms), bin "
                             + juce::String(analyzer_.binHz(), 2) + " Hz, bands "
                             + juce::String(analyzer_.numBands())
                             + " (sub-bin/interp " + juce::String(analyzer_.subBinBands())
                             + "), window "
                             + juce::String(vfddsp::windowName(dspCfg_.window)));
}

void VfdEapoAudioProcessorEditor::timerCallback() {
    // 先认采样率（消费端收到第一块就知道），否则下面抽出来的音频会因为
    // "还不知道采样率"被白白扔掉 —— 那同样是一段静默的数据丢失。
    const int sr = consumer_.sampleRate();
    if (sr > 0 && sr != appliedRate_) rebuildAnalyzer(sr);

    // ---- 1) 把 audiodg 透传过来的音频收进分析器 ---------------------------------
    // 【必须抽干】readNew 只"少给不丢"（见 PanelConsumer.h 里那段血案注释）：
    // 12fps @192k 每拍到货 ~16.0k 帧，只要每次取的量比到货量小哪怕一点点，
    // 剩下的就会被丢掉 → 分析窗里出现相位台阶 → 屏上一条假裙边。
    // 所以这里循环到取空为止（环形缓冲 2.7s，最多 16 轮就抽干），中途不许 break。
    int got = 0;
    for (int round = 0; round < 32; ++round) {
        const int n = consumer_.readNew(pull_.data(), pullFrames_);
        if (n <= 0) break;
        // 频谱用单声道混合；声量计/峰值计要按 L/R 分开，所以两者都吃这份交错数据
        for (int i = 0; i < n; ++i) {
            mono_[i] = 0.5f * (pull_[(size_t)i * 2] + pull_[(size_t)i * 2 + 1]);
        }
        analyzer_.push(mono_.data(), n);
        meters_.pushInterleaved(pull_.data(), n, (int)vfdpipe::kAudioChannels);
        got += n;
    }

    const double now = juce::Time::getMillisecondCounterHiRes();
    const double dt = (lastTickTime_ > 0.0) ? juce::jlimit(0.0, 0.25, (now - lastTickTime_) / 1000.0)
                                            : (1.0 / 12.0);
    lastTickTime_ = now;

    // ---- 1b) 没音频就**补零** ----------------------------------------------------
    // 音频一停（播放器暂停 / 设备被切走 / 插件被旁路），audiodg 那边就不再发帧，
    // 管道会断开、状态栏回到 "listening"。这时候如果什么都不喂，分析器会拿
    // 环形缓冲里那段**陈旧数据**一遍遍重算 —— 窗里内容永远不变 ⇒ 谱线定格在最后一帧，
    // 整块屏看起来就是"卡死"（实测：暂停后画面完全不动）。
    // 所以按 dt 补等量静音：窗被静音灌满 → 柱体和峰值按自己的弹道正常掉到底，
    // 声量计也一样。注意补的是**真实经过的时间**，不是每次固定一大坨。
    if (appliedRate_ > 0 && got == 0) {
        const int pad = juce::jlimit(1, pullFrames_,
                                     (int)std::lround(dt * (double)appliedRate_));
        std::fill_n(mono_.data(), pad, 0.0f);
        analyzer_.push(mono_.data(), pad);
        std::fill_n(pull_.data(), (size_t)pad * vfdpipe::kAudioChannels, 0.0f);
        meters_.pushInterleaved(pull_.data(), pad, (int)vfdpipe::kAudioChannels);
    }

    // ---- 2) 分析（窗 + FFT + 对数分频 + 回落/峰值）------------------------------
    if (appliedRate_ > 0) {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        analyzer_.process(dt);
        meters_.process(dt);
        analyzeMs_ = juce::Time::getMillisecondCounterHiRes() - t0;
        lastBands_ = analyzer_.numBands();
    }

    // ---- 3) 画到点阵层 ---------------------------------------------------------
    if (appliedRate_ > 0 && analyzer_.numBands() > 0) {
        ensureHeader();
        screen_.clearDynamic();
        screen_.drawBars(analyzer_.bands(), analyzer_.numBands());
        screen_.drawPeaks(analyzer_.peaks(), analyzer_.numBands());
        screen_.drawMeters(meters_.vuNorm(), meters_.peakNorm(), meters_.channels());
    }
    // 没有新帧时也继续走磷光衰减，避免画面"冻住"
    screen_.applyPersistence((float)juce::jmin(dt, 0.25), kPhosphorTau);

    // 有新音频就渲染；空转时最多 4 Hz 渲染一次
    if (got > 0 || (now - lastRenderWall_) > 250.0) {
        lastRenderWall_ = now;
        renderIntoImage();
    }

    // ---- 4) 状态行（在图像下方，不占点阵面积）---------------------------------
    if (got > 0) {
        // 面板与 audiodg 是两个进程、各自持有一份加载时刻的镜像 —— 指纹不一致
        // 就说明之一还是旧的（2026-09-28 的坑：面板显示 254 BANDS / render 4.0 ms，
        // 其实源码早改了，只是 audiodg 还跑着旧 DLL）。
        // ⚠️ 警告只能用**极短**的标记：状态栏只有 524px，长警告会把后面的
        // jump/drop 顶出可视范围 —— 仪表装上了却看不见 = 没装（已经栽过一次）。
        // 详细的 build 指纹去日志里看（juce::Logger）和后面的 build stamp。
        const uint32_t mine = vfdspec::kVfdBuildId;
        const uint32_t theirs = consumer_.producerBuild();
        if (theirs != 0 && theirs != mine) {
            staleBuild_ = true;
            juce::Logger::writeToLog(
                juce::String("vfd panel: STALE producer build ")
                + juce::String::toHexString((int)theirs) + " (panel "
                + juce::String::toHexString((int)mine) + ") - run tools\\reload_audio.ps1");
        } else {
            staleBuild_ = false;
        }
        // 两个计数放**最前面**：状态栏宽 524px，长了会被截断 —— 仪表必须看得见。
        // jump  = 收到的流里块接缝处的不连续次数（管道线程看的，>0 ⇒ audiodg 那侧在丢）
        // drop  = 本面板**真丢掉的音频帧数**（读端保序丢弃；>0 ⇒ 屏上必有假裙边）
        //         健康状态两个都必须是 0；只要 drop 不是 0，谱线就是假的，不用怀疑音源。
        status_ = (staleBuild_ ? juce::String("STALE ") : juce::String())
                  + "jump " + juce::String((juce::int64)consumer_.jumps())
                  + "  drop " + juce::String((juce::int64)consumer_.droppedFrames())
                  + "  |  FFT " + juce::String(analyzer_.fftSize())
                  + "->" + juce::String(lastBands_)
                  + "  " + juce::String(consumer_.fps(), 1) + "c/s"
                  + "  a" + juce::String(analyzeMs_, 1)
                  + " r" + juce::String(renderMs_, 1)
                  + "  s" + juce::String(consumer_.producerSession())
                  + "  " + juce::String(VFD_BUILD_STAMP);
    } else {
        updateWaitingStatus();
    }
    repaint();
}

void VfdEapoAudioProcessorEditor::ensureHeader() {
    const int rate = consumer_.sampleRate();
    const int ch = consumer_.channels();
    if (headerSet_ && rate == lastRate_ && ch == lastCh_) return;
    headerSet_ = true;
    lastRate_ = rate;
    lastCh_ = ch;

    char r1[32], r2[32];
    std::snprintf(r1, sizeof(r1), "%dHZ", rate);
    // 链路上是 float32；源位深 EAPO 没说，这里如实写 32F
    std::snprintf(r2, sizeof(r2), "32F %dCH", juce::jmax(1, ch));
    screen_.setHeader("SPECTRUM", r1, r2);
}

void VfdEapoAudioProcessorEditor::renderIntoImage() {
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    std::vector<uint8_t> rgb;
    screen_.render(rgb);
    renderMs_ = juce::Time::getMillisecondCounterHiRes() - t0;

    const juce::Image::BitmapData bd(image_, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < screen_.height(); ++y) {
        uint8_t* dst = bd.getLinePointer(y);
        const uint8_t* src = rgb.data() + (size_t)y * (size_t)screen_.width() * 3;
        std::memcpy(dst, src, (size_t)screen_.width() * 3);
    }
}

void VfdEapoAudioProcessorEditor::updateWaitingStatus() {
    using State = vfdspec::PanelConsumer::State;
    switch (consumer_.state()) {
        case State::pipeTaken:
            status_ = "pipe busy: another consumer owns it (close build\\pipe_server.exe)";
            break;
        case State::wrongVersion:
            status_ = "plugin is protocol v2 (254-band mode) - close EAPO, rebuild the DLL, "
                      "then re-enable the filter";
            break;
        case State::listening:
            status_ = "listening on " + juce::String(vfdpipe::pipeName())
                      + " - waiting for the audiodg instance (audio must be playing)";
            break;
        case State::receiving:
            status_ = "connected, " + juce::String((juce::int64)consumer_.samplesReceived())
                      + " samples buffered, waiting for a full FFT window ...";
            break;
        default:
            status_ = "starting consumer ...";
            break;
    }
}

void VfdEapoAudioProcessorEditor::paint(juce::Graphics& g) {
    g.fillAll(kBg);
    g.drawImageAt(image_, kMargin, kMargin);
    if (!showStatus_) return;               // 默认把整行还给频谱（见 PluginEditor.h）

    g.setColour(status_.startsWith("pipe busy") || status_.startsWith("plugin is protocol")
                    || staleBuild_
                    ? kWarn : kStatus);
    g.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(),
                                           12.0f, juce::Font::plain)));
    const juce::Rectangle<int> row(kMargin, kMargin + screen_.height() + 2,
                                   getWidth() - kMargin * 2, kStatusH - 2);
    g.drawFittedText(status_, row, juce::Justification::centredLeft, 1, 0.7f);
}

// 单击点阵屏 → 换主题色（顺序：黄绿 → 深绿 → 蓝 → 粉 → 橘）
void VfdEapoAudioProcessorEditor::mouseUp(const juce::MouseEvent&) {
    cycleTheme();
}

void VfdEapoAudioProcessorEditor::cycleTheme() {
    screen_.nextTheme();
    g_themeIndex = (int)screen_.theme();
    juce::Logger::writeToLog(juce::String("vfd panel: theme -> ") + screen_.themeLabel());
    renderIntoImage();          // 立刻重画，不等下一个 timer
    repaint();
}
