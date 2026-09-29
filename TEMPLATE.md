# TEMPLATE.md —— 新插件开工模板

> **用途**：把本仓库当脚手架，复制出一个**新的 EAPO VST2 插件**（"音频进程采集 → 用户会话显示"这一类）。
> 目标是把新插件的开工时间从"从零想架构"压到**半小时改名字 + 改内容**。
>
> 配套阅读：
> * **`SKILLS.md`** —— 每条设计背后的坑与范式，**改任何一行之前先扫一眼对应章节**
> * `docs/eapo-vst-feasibility.md` —— EAPO/VST2 的硬约束（哪些能做、哪些绝对不能做）
> * `docs/build-env-notes.md` —— 本机工具链的坑

---

## 0. 这个模板适合什么 / 不适合什么

**适合**

* 从音频流里取数据，重活放到**用户会话**里算、再显示出来（频谱 / 电平表 / 示波器 / 相位表 / 响度计 / 向量示波器 …）
* **只读不改**音频（直通）
* 必须跨 session（`audiodg` 里拿不到窗口，桌面侧拿不到音频）

**也能用、但要自己补**

* 要**改音频**的插件（EQ/限幅）：分工照样成立，但 `processBlock` 里的"直通"要换成你的处理链；注意 **EAPO 没有延迟补偿**，需要 lookahead 的算法自己想办法。

**不适合**

* 需要 **VST3**（EAPO 只吃 VST2.4）
* 需要参数自动化 / 预设 / 多 program（本模板的 state 只放一个短标记）
* 想在 `audiodg` 里**弹窗**（不可能，见 SKILLS §1.1）

---

## 1. 脚手架分层，以及你要动哪些文件

| 文件 | 角色 | 复用度 | 你要改什么 |
| --- | --- | --- | --- |
| `src/vfd_pipe.h` | 帧格式 + 管道 client/server + 名字覆盖 + 会话号 | 改协议就动它 | 帧结构、`kMagic`、版本号 |
| `plugin/SpectrumSender.h` | 音频线程 → worker 线程 → 管道（**只搬运**） | **改名照抄** | 类名、取哪几路、帧里放什么 |
| `plugin/PanelConsumer.h` | UI 侧服务端线程 + 环形缓冲 + 接缝/丢弃计数 | **照抄** | 类名、帧解析 |
| `plugin/PluginProcessor.*` | VST2 处理器（直通 + 无锁入队） | 照抄 | 插件名、state 标记 |
| `plugin/PluginEditor.*` | 面板：定时器 → 取数 → 分析 → 渲染 → 上屏 | 骨架照抄，内容换 | 布局/交互；**但"抽干 / 补零 / 仪表可见"三条必须保留** |
| `plugin/BuildStamp.h` | 编译期 build 指纹 | **照抄** | 不用改 |
| `src/vfd_fft.h` `src/vfd_dsp.h` `src/vfd_render.h` | 零依赖内核（FFT / 分析器 / 点阵渲染） | 按需 | 换掉、扩展，或当参考 |
| `src/pipe_server.cpp` | 独立 viewer（自测 + 出图） | 照抄 | 改协议、改消费内容 |
| `src/pipe_producer.cpp` | 合成信号源（自测） | 照抄 | 改合成内容 |
| `src/vst2_host_stub.cpp` | 用 EAPO 的契约加载 DLL（自测，**不需要 EAPO/REAPER**） | 照抄 | 不用改 |
| `plugin/CMakeLists.txt` | JUCE VST2 目标 + **静态 CRT** | 照抄 | 目标名 / 插件 ID / 产物名 |
| `tools/build.bat`、`tools/reload_audio.ps1` | 构建 / 强制换 audiodg 侧镜像 | 照抄 | 目标名 |

---

## 2. 复制清单（按顺序做）

### Step 0 — 复制并改名

复制整个仓库（或 `src/` + `plugin/` + `tools/` + `third_party/` 的 submodule 引用），
然后把新工程里的**前缀**统一换掉。本仓库用的是 `vfd` / `Vfd` / `VFD_` 三套前缀，`grep` 一遍全换。

**产物名不要带空格**（`MyThingSender.dll` 而不是 `My Thing Sender.dll`）—— 空格会让 schtasks 的 `/tr`、
EAPO 的 config 行、各种脚本统统需要引号转义。**显示名（UI 里）才允许有空格**。

### Step 1 — 改 VST 标识（**必须**）

`plugin/CMakeLists.txt`：

```cmake
juce_add_plugin(mynewthing                 # ← 目标名
    COMPANY_NAME "YourCompany"
    PLUGIN_MANUFACTURER_CODE Your           # ← 4 字符，必须与现有插件不同
    PLUGIN_CODE Mnsp                        # ← 4 字符，必须唯一
    FORMATS VST
    PRODUCT_NAME "MyNewThingSender"         # ← 产物名，别带空格
    ...
```

**为什么必须改**：EAPO 的实例缓存 `VSTPluginLibrary::instanceMap` 是**按 DLL 路径**做键的 —— 路径一样
就复用同一个实例；而宿主（含 EAPO 编辑器）还会用 uniqueId 区分插件。两个同 ID 或同文件名的 DLL
放在一起，你会得到"改了代码行为却不变""加载到另一个插件"这类玄学问题。

### Step 2 — 定协议（还是传音频就跳过）

需要改 `src/vfd_pipe.h` 时，照 §3.3 的清单走，并**把版本号 +1**。

### Step 3 — 采集侧：`SpectrumSender` 改名 + 改"取什么"

本模板取的是**前两声道交错样本**。如果新插件要的是别的（比如 6 声道、或者已经算好的特征），
改 `pushAudio()` 和 worker 里的组帧代码。**保持"音频线程只做无锁入队"这条红线。**

### Step 4 — UI 侧：换内容，别丢三条铁律

`PluginEditor` 里的 `timerCallback()` 是整套东西的心脏。改布局/交互都可以，但这三件事不能省：

1. **抽干**：`readNew` 只少给不丢，调用方循环到取空（SKILLS §3.3）
2. **补零**：没有新数据时按真实 `dt` 补静音，否则画面定格（SKILLS §4.6）
3. **仪表可见**：诊断计数放字符串最前面（SKILLS §3.5）

### Step 5 — 构建

```bat
tools\build.bat           rem 先编工具（管道自测三件套）
tools\build.bat plugin    rem 再编插件 DLL
```

### Step 6 — 自测三连（见 §4）

### Step 7 — 装进 EAPO（见 §5）

---

## 3. 逐文件"改哪里"

### 3.1 `plugin/PluginProcessor.h/.cpp`

只需要改：类名、`getName()`、state 标记字符串。**其余一行都别动**，尤其是这三条铁律：

```cpp
VfdEapoAudioProcessor::VfdEapoAudioProcessor() {
    // 构造函数必须"什么都不做"：EAPO 每次配置加载、每次音频重锁都会重新实例化插件，
    // 而这一切都发生在音频服务里。不碰文件、不初始化 COM、不建 FFT 表、不 spawn 线程
    //（Sender 的 worker 线程在成员构造时启动，是被允许的轻量操作）。
}

void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    // 纯分析插件：不改音频，直通。唯一做的事是无锁、无分配、无系统调用地入队。
    sender_.pushAudio(buffer.getArrayOfReadPointers(), buffer.getNumChannels(),
                      buffer.getNumSamples());
}

// JUCE 会自动处理 effEditGetRect —— 只要你 createEditor() 返回一个 setSize 过的编辑器，
// 就不会踩 EAPO ticket 138（未实现 editor 导致 Open panel 崩配置编辑器）。
juce::AudioProcessorEditor* VfdEapoAudioProcessor::createEditor() {
    return new VfdEapoAudioProcessorEditor(*this);
}
bool hasEditor() const override { return true; }     // 千万别返回 false

void getStateInformation(juce::MemoryBlock& d) {
    // 无参数。写一个短标记：EAPO 会把它 base64 进 config.txt 的 ChunkData "..."
    // —— 这样你在 config 里就能看出这行是谁写的。
    static const char tag[] = "MYTHING01";
    d.setSize(sizeof(tag) - 1);
    std::memcpy(d.getData(), tag, sizeof(tag) - 1);
}
```

### 3.2 `plugin/SpectrumSender.h`（采集侧）

**结构照抄，只换"取什么"。** 关键在于它同时满足四个约束：

```
音频线程：pushAudio()  → 无锁环形缓冲（juce::AbstractFifo）
                                    ↓
worker 线程：攒够一段 → 组帧 → 写管道；连不上就 500 ms 重试一次（不刷日志）
```

三条**从坑里换来的**细节，务必保留：

```cpp
// ① 音频闸门：EAPO 会在配置编辑器进程里再实例化一份插件（那份永远没有音频）。
//    不设闸门，它就会去抢那个单实例管道，把真身挤掉。
if (samplesSeen_.load(std::memory_order_relaxed) == 0) continue;   // 没见过音频就不连接

// ② 没连上就清空暂存：否则一连上会吐出一堆陈旧音频（在频谱上表现为"开头一坨乱"）
if (!client_.connected()) { stage_.clear(); ... continue; }

// ③ 环形缓冲给足 + 任何丢弃都按**帧**记账（静默丢 = 频谱上凭空出现的宽带裙边）
static constexpr int kRingSize = 1 << 18;    // 262144 帧 ≈ 1.37 s @192k
```

还有：`pushAudio()` 里**绝不** `connect/write/malloc/lock`。管道写满时会阻塞（实测最坏 144 µs），
那是音频线程，不能赌。

调试：`VFD_DEBUG=1` 时把 `blocks/samples/ch/rate/connected/frames` 打到 stderr ——
插件在 audiodg 里没有任何输出，"没有数据"时这是唯一能把链条逐段点亮的手段。

### 3.3 `src/vfd_pipe.h`（协议）

改协议时**照这张表逐项确认**（每一项都对应一次翻车）：

| 字段 | 作用 | 不改会怎样 |
| --- | --- | --- |
| `magic` | 分辨"这是不是我们的帧" | 垃圾被当帧解析 |
| `version` | 能**明确报错**地拒绝旧格式 | 旧 DLL 的帧被静默当成噪声（我们吃过：面板一直显示"在等连接"） |
| `n_samples` | 本帧有效样本数 | 别假设恒定！192k 下 EAPO 的块约 1914 帧，不是结构体上限 |
| `samplerate` / `channels` | UI 自适应 | 采样率变了必须**重建分析器** |
| `producer_session` | 一眼看出对面在哪个会话 | 排查"到底谁发的"很费劲 |
| `reserved` | **编译期 build 指纹** | "分不清哪一版在跑"（我们为此白查过一轮） |

管道名走环境变量覆盖，**值必须写全名**：

```cpp
// 只写 "MyPipe" 会 err=123（ERROR_INVALID_NAME）；必须含 \\.\pipe\ 前缀
VFD_PIPE_NAME=\\.\pipe\MyNewThingProbe
```

服务端用 `PIPE_TYPE_MESSAGE`；出缓冲给足（我们给 64 条消息 ≈ 1 MB），别怕占内存。

### 3.4 `plugin/PanelConsumer.h`（消费侧）

**整份照抄**，只改类名与帧解析。它已经踩平了这些：

* 断线自愈（回到 `listening` 并**如实显示**，循环重新 `CreateNamedPipe`）
* `listen()` 只发起一次 + `pollClient()` 轮询（**别重复 `ConnectNamedPipe` + `CancelIo`**，会把实例弄死）
* 环形缓冲 ≥1 s，**读端只少给不丢**，写满时**按帧记账**
* 接缝检测器：门限 = 本块内部正常最大斜率的 **4 倍**（别写死绝对值）

### 3.5 `plugin/PluginEditor.h/.cpp`（面板）

保留 `timerCallback()` 的四段式骨架，换掉里面的内容：

```cpp
void MyEditor::timerCallback() {
    // 0) 先认采样率/格式（否则第一拍取出来的数据会被"还不知道格式"白扔）
    const int sr = consumer_.sampleRate();
    if (sr > 0 && sr != appliedRate_) rebuildAnalyzer(sr);

    // 1) 【抽干】循环到取空，中途不许 break
    int got = 0;
    for (int round = 0; round < 32; ++round) {
        const int n = consumer_.readNew(pull_.data(), pullFrames_);
        if (n <= 0) break;
        ... 喂给分析器/仪表 ...
        got += n;
    }

    // 1b) 【补零】没有新数据也要喂静音，否则画面定格
    if (appliedRate_ > 0 && got == 0) { ... push dt*sr 个 0 ... }

    // 2) 分析（用**真实经过的 dt**，不是"上一个有声 tick 的 dt"）
    analyzer_.process(dt);

    // 3) 画到点阵层 + 磷光衰减
    // 4) 状态行（如果要有）：**两个计数放最前面**
}
```

面板尺寸：`setSize(screen_.width() + 2*margin, screen_.height() + 2*margin [+ 状态行])`。
点阵网格建议 `cell=4 / dot=3`。

### 3.6 `plugin/CMakeLists.txt`

**这一段一个字都别改**（静态 CRT 是硬要求，理由在文件注释里）：

```cmake
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(JUCE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
```

其余就是 Step 1 的标识 + `target_sources` 列表（把你复用/新增的头文件都列进去，
这样 ninja 的依赖跟踪才完整）。

### 3.7 EAPO 侧的 `config.txt`

装好之后，EAPO 的配置文件里就是一行（`ConfigPath` 从注册表读，本机是
`C:\Program Files\EqualizerAPO\config`，实测**非提权可写**）：

```
Device:  扬声器 TOPPING USB DAC {c9c284d4-384e-453a-bb9e-7ebbf5369cbd}
VSTPlugin: Library <插件 DLL 的绝对路径> ChunkData "TXlUaGluZzAx"
```

**EAPO 自己监视这个目录**（`FindFirstChangeNotificationW` + `FILE_NOTIFY_CHANGE_LAST_WRITE`），
所以**写完盘就自动重载，不需要任何 IPC/握手**。用一个 DPG/自研前端改配置时，只要原子写
（temp + rename）就行。

---

## 4. 自测三连（不需要 EAPO，也不需要 REAPER）

```bat
rem ① 先起消费者（独立 viewer）
build\pipe_server.exe 15
rem ② 再用合成源灌数据：验证协议 + 渲染
build\pipe_producer.exe 10 --rate 192000 --tone 440
rem ③ 用 EAPO 的契约直接加载插件 DLL：验证 VST2 契约（含 effEditGetRect）
set VFD_DEBUG=1
build\vst2_host_stub.exe build\plugin\...\VST\MyNewThingSender.dll 8
```

期望：`pipe_server` 打印 `chunks=... gaps=0 bad=0 jumps=0`，并落一张 PNG；
宿主桩打印 `AEffect` 信息、**open 之前 `effEditGetRect` 返回的矩形**、之后能看到帧计数上涨。

再加两个"体检工具"，它们是我方排障的主力：

* `build\dsp_probe.exe` —— 分析器离线体检（不碰管道/设备）：哪个频段没出数、静音掉不掉得下来
* `build\render_bench.exe --theme N --png` —— 渲染对照图 / 性能

⚠️ 两个**测试环境自身**的坑（都曾伪装成"插件坏了"）：
宿主桩必须**按真实播放速率**喂音频（`sleep_until`），否则 8 秒音频十几毫秒灌完，
插件的重连/节流窗口永远等不到；管道服务端**不能**在超时后重复 `ConnectNamedPipe`。

---

## 5. 装进 EAPO

1. 把 DLL 放到一个**路径无空格**的目录（本项目是 `build\plugin\vfd_eapo_artefacts\Release\VST\`）；
2. EAPO Configuration Editor → 添加 `VSTPlugin:` 滤镜 → 选 DLL → `Save`；
3. 播点声音（**没有音频就不会建音频实例、也不会有数据**）；
4. 点 `Open panel`。

**改了代码重新编译之后**（这一步每次都有人踩）：

```
① 旧 DLL 被两个进程映射着 → 链接会报 LNK1104
   → 同卷移动（映射中的文件不能删、但能移动）到 build\stale\，再编
   → 旧镜像别留在插件目录里（EAPO 会扫到同 ID 的副本）
② 面板那一半：**关掉面板窗口再打开**（编辑器实例是打开面板时才 new 的）
③ audiodg 那一半：跟着滤镜链重建换；强制换用 tools\reload_audio.ps1（管理员）
④ 用 build stamp / STALE 标记确认到底换了没有
```

---

## 6. 内核给了什么（如果新插件也要频谱/点阵）

| 文件 | 内容 | 备注 |
| --- | --- | --- |
| `src/vfd_fft.h` | 零依赖 radix-2 实数 FFT | PC 和 MCU 通用，无外部依赖 |
| `src/vfd_dsp.h` | `Analyzer`（窗 + FFT + 对数分频 + 插值 + dB/回落/峰值 + 多分辨率阶梯）+ `MeterBank`（VU/峰值弹道） | 直接 `prepare(rate, Config::fromEnv())` 就能用，全走环境变量调参 |
| `src/vfd_render.h` | 点阵渲染（格域泛光 / 磷光暂留 / 暗角 / 调色板查表）+ 5 套主题 + `computeLayout()` | 换 `Screen(cols, rows, cell, dot)` 就能适配别的屏 |

用法：

```cpp
vfddsp::Analyzer an;
an.prepare(192000.0, vfddsp::Config::fromEnv());
an.push(mono, n);
an.process(dt);
screen.drawBars(an.bands(), an.numBands());
screen.drawPeaks(an.peaks(), an.numBands());
screen.applyPersistence(dt, 0.07f);
screen.render(rgb888);
```

---

## 7. 第一天就会撞上的问题

| 现象 | 去 SKILLS.md 查 |
| --- | --- |
| 频谱有假裙边 / 山包 | §3（绝不丢帧）—— 先看 `drop`/`jump`，别怀疑音源 |
| 某段频率齐齐切断 | §4.4（`divisor == 1` 陷阱） |
| 音频一停画面定格 | §4.6（补零） |
| 面板数字像旧版 / 显示旧格式 | §5.2（两份实例 + 换 DLL 姿势）、§5.4（build 指纹） |
| 一帧都收不到 | §2.2 / §2.3，以及"宿主桩要按真实速率喂" |
| audiodg 崩 | §5.3（静态 CRT、`effectCount = 0` 除零） |
| 打开面板崩配置编辑器 | §5.3（必须实现 editor，ticket 138） |
| 找到问题但改完没效果 | §6.1（对照要在同一条路上做） |
