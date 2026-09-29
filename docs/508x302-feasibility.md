# 508×302 VFD 频谱显示：双用途可行性分析

> 目标：把现在这个 DearPyGui 原型（`vfd_spectrum.py`）沉淀成 **508×302** 的真实渲染分辨率，
> 供两个用途复用：
> **A. Equalizer APO 用的 JUCE VST 插件**（PC 侧显示）
> **B. ESP32-P4 + I2S 输入的独立屏幕设备**
>
> 本文档记录结论、依据和待验证项。工程目录保持 `D:\Vibe\260927-fft`。
>
> **另见（根目录两份可迁移文档）**：
> * **`SKILLS.md`** —— EAPO VST2 插件开发的踩坑清单与工程范式（给"下一个插件"看）
> * **`TEMPLATE.md`** —— 新插件开工模板：复制清单 + 逐文件改哪里 + 最小骨架
>
> 本文档则专注**这个项目本身**的决策、数据与排障记录。

---

## 0. 结论速览

| 用途 | 可行性 | 主要成本/风险 | 建议 |
| --- | --- | --- | --- |
| A. EAPO 的 JUCE VST | **改动后的方案可行**（"插件自己弹窗"不可行） | 音频实例在 `audiodg.exe`（**实测在 Session 0**、LocalService + 受限令牌）→ **不能建窗口**，且**会话内命名对象（`Local\`）跨不过去**；编辑器实例是**另一个进程里的另一个对象**、拿不到音频；必须 **VST2 + x64**；EAPO 要靠关掉一项 Windows 安全缓解（`DisableProtectedAudioDG`）才能加载未签名插件；插件崩了会带走整个系统音频 | 传输选**命名管道**（EAPO 自身先例、全局命名空间天然跨会话）；共享内存（`CppUtils`）需改 `Global\` + SDDL + seqlock 才可用；先做 spike，并先回答"为什么不用回环" |
| B. ESP32-P4 + I2S | **可行且余量充足**（按 400 MHz 双核 + PSRAM 估算） | 真正的瓶颈不是 CPU 而是**面板接口带宽**和**bloom 放在哪一级**；`508×302` 与 `cell=4` 不整除（300 vs 302） | 面板选 MIPI-DSI 或并行 RGB；bloom 放"格域"做 |

---

## 1. 用途 A：Equalizer APO 的 JUCE VST 插件

### 1.1 先确认 EAPO 侧的硬事实（都有源码依据）

1. **EAPO 的 VST 是 VST2.4，不是 VST3。** 上游 `mirror/equalizerapo` 的加载器是
   `GetProcAddress(module, "VSTPluginMain")`，向插件声明 `audioMasterVersion = 2400`；
   官方 1.4.2 树里**没有 VST3 加载器**（VST3 只有第三方 fork：
   `Mixomo/EqAPO64_with_VST3_support`，基于 `TheFireKahuna/equalizerAPO64`）。
   配置语法（`filters/VSTPluginFilterFactory.cpp`）：
   `VSTPlugin: Library "<path>" [ChunkData "<base64>"]` 或 `VSTPlugin: Library "<path>" "<参数名>" <float> ...`；
   **相对路径解析到 `<InstallPath>\VSTPlugins`**，绝对路径随意。
   → **我们的插件按 VST2 出**（JUCE 可以同时产出 VST/VST3，但 EAPO 只吃 VST2 那个）。
   → 本机实测：已安装 **EAPO 1.3.0.0（x64）**，`C:\Program Files\EqualizerAPO\config\config.txt`
   目前是 **0 字节**（还没配任何滤波器），所以插件路线需要先往配置里加 `VSTPlugin: Library ...`，
   并且会**改到用户的实际音频链**——这一点在决定路线时要意识到。
2. **音频侧的插件实例跑在音频服务上下文里（LocalService / audiodg.exe）。**
   证据：EAPO 的编辑器会检查插件 DLL 对 **`SECURITY_LOCAL_SERVICE_RID`** 的可读性，读不到就警告
   "The library is not readable by the audio service."
   （`Editor/guis/VSTPluginFilterGUI.cpp` 的 `updatePermissionWarning()`）。
   参考仓库 `WindowsCoaxVolumeControllerVST` 的代码注释也直说"在 audiodg.exe 进程内也需要初始化一次 COM"。
3. **GUI 是另一个进程里的事。** EAPO 的 `VSTPluginFilterGUI::initPlugin()` 会在
   **Configuration Editor（Qt 进程）里另外 `new VSTPluginInstance(library, 1)`**，
   只用来 `startEditing(hwnd)` / `readFromEffect()`（参数与 chunk 同步），
   `on_idle()` 挂在 Qt 的 `aboutToBlock` 上泵消息。**这个实例从头到尾不喂音频。**
4. 因此有两种 GUI 呈现方式（独立 dialog / 嵌入编辑器），**但都拿不到实时音频**。
5. **没有沙箱**：EAPO 会用 `__try/__except` 兜住插件崩溃并提示
   "Plugin crashed during initialization" / "Plugin crashed when opening panel"，
   说明插件是**在同一进程内直接跑**的 —— 插件把 audiodg 搞崩，整个系统音频一起没
   （audiodg 挂掉 AudioSrv 会自动重启它，但那一刻所有声音中断）。
   注意 `__try/__except` 只抓访问违规，**抓不住堆损坏和 C++ 异常**。
6. **必须 x64**：64 位 Windows 上 `audiodg.exe` 只有 64 位版本，
   32 位 EAPO 在 64 位系统上根本装不上（32 位 DLL 也无法加载进 64 位进程）。
   我们的 DLL 要同时被**音频引擎进程**和**配置编辑器进程**加载，两者位宽都要对。
7. **进程隔离比想象的更彻底**：audiodg 由 AudioSrv 以 `NT AUTHORITY\LOCAL SERVICE`
   启动、持有**受限令牌**，而且**与登录用户处于不同的登录会话**（这是刻意的隔离设计，
   防止音频内容与插件被篡改）；微软称音频引擎运行在"它自己的受保护进程"里。
   → 它**拿不到用户目录**（`C:\Users\<你>`），只能碰 `Program Files`/`ProgramData`
   或显式给 `NT SERVICE\Audiosrv` 授权。
8. **未签名 APO 需要先关掉一项 Windows 安全缓解**：EAPO 的开发文档要求
   `DisableProtectedAudioDG=1` 才能让自编译 APO/插件加载；EAPO 官方也自述这套做法
   "不被微软支持，属于一个 happy accident"。**这是插件路线最大的一条现实性风险**——
   目前**没有**找到任何"在开启该缓解的情况下仍能正式安装工作"的先例。
9. **音频实例收不到任何 GUI 事件**：EAPO **从不**在音频进程里调 `effEditOpen`，
   也**从不**派发 `effEditIdle`；宿主服务被裁剪到极小
   （`audioMasterGetTime`→0、`audioMasterCanDo` 只对 `startStopProcess`/`sizeWindow` 返回真、
   `audioMasterWantMidi`→0）。→ 在音频实例里做 UI 或依赖 JUCE `Timer` 都不可靠。
10. **EAPO 报告零延迟**：`EqualizerAPO::GetLatency` 恒返回 0，插件的 `initialDelay`
    在 1.4.0 里**从不被读取**。→ 任何带前瞻的插件都会造成未补偿的声画不同步（对纯分析插件无影响）。
11. **音频契约**：仅 **float32**；采样率/声道数在 `LockForProcess` 协商；
    **是"多实例"而不是"多声道"** —— 立体声 VST 挂在 7.1 端点会被实例化 **4 次**；
    block size 由宿主决定（`effSetBlockSize` 拿到的是最大值 `u32MaxFrameCount`，
    而 `processReplacing` 收到的是可能更小的 `u32ValidFrameCount`）；音频侧进程级别为 **2（实时）**。
12. **格式变化会销毁并重建插件实例**：只有 chunk/参数能存活。所以 producer 必须能
    "被销毁再重建后自动重新衔接"（重新打开共享对象/管道、重置 FFT 状态）。
13. **EAPO 自带的"分析面板"不是实时抽头**：它在编辑器进程里**自建**一套 `FilterEngine`，
    喂阶跃/DC 信号、用 FFTW 算冲激响应，**看不到 audiodg 里的真实音频**。
    → 不要指望复用它来驱动显示。

### 1.2 参考仓库的真实机制（**重要更正：它没有任何 IPC**）

我把 `WindowsCoaxVolumeControllerVST` 的完整文件树和全部源码都过了一遍：

- 仓库只有 `Source/{PluginProcessor,PluginEditor}.{h,cpp}` + `CMakeLists.txt` + `Doc/pic01.png`
  （加两个 submodule：JUCE、vstsdk2），**没有第二个进程、没有托盘程序、没有伴生应用**；
- 它做的事：JUCE `AudioProcessor` 用 **Core Audio COM** 读**默认渲染端点**的系统音量
  （`CoCreateInstance(MMDeviceEnumerator)` → `GetDefaultAudioEndpoint(eRender, eMultimedia)`
  → `Activate(IAudioEndpointVolume)` → `GetMasterVolumeLevelScalar()` / `GetMute()`），
  把它当平滑增益乘到音频上，外加一个 200×100 的编辑器；
- 所谓的"多进程通信"，**其实就是 MMDevAPI 这层 COM 本身**（插件在 audiodg 里，
  音量在音频服务进程里，跨进程由 COM 代理完成）——**没有共享内存、没有命名管道、没有 WM_COPYDATA**。

**所以它不是一个可用的 IPC 参考，但它是有价值的正反两面教材：**

- ✅ **正面**：证明了"在 audiodg 进程内做 COM"是可行的，并给出了初始化写法
  （`CoInitializeEx(NULL, COINIT_MULTITHREADED)`）。
  我们可以用同一招**顺便查端点格式**（`IAudioClient::GetMixFormat`）来填右上角的"采样率/位深"读数。
  打包方式（CMake + JUCE、`/MT` 静态运行时、一次产出 VST2+VST3、`juce_set_vst2_sdk_path`）也可直接抄。
- ❌ **反面（很重要）**：它是**把 COM 调用直接写在音频回调里**的——`processBlock` 里每约 50 ms
  就调一次 `updateSystemVolumeDirectly()` 去做跨进程 COM 查询。这正是 APO 宿主环境下的反模式：
  跨进程调用可能阻塞，而这条线程是系统音频引擎的实时线程。**我们要反着做**：
  `processBlock` 只做"往无锁环形缓冲 memcpy + 每 N 帧置一次事件"，所有 COM/FFT/IPC 都挪到非实时线程。

**真正可参考的 IPC 先例其实在 EAPO 自己身上**：EAPO 的设备测试功能就是从 audiodg 里
`CreateFileW` 打开一个**命名管道** `\\.\pipe\EqualizerAPODeviceTest`，
管道名通过注册表 `HKLM\SOFTWARE\EqualizerAPO\DeviceTestPipeName` 发布给客户端
（只在设备测试期间用，不在 `APOProcess` 里）。
→ 这直接说明：**"音频进程 → 命名对象 → 外部进程"这条路在 audiodg 里是通的**，
而且"把名字写在注册表里让消费者去发现"是个现成的模式，我们照抄即可。
（本机交叉验证：`HKLM\SOFTWARE\EqualizerAPO` 键存在，含 `InstallPath` / `ConfigPath` / `EnableTrace`；
`DeviceTestPipeName` 此刻不存在——与"只在设备测试时创建"一致。）

### 1.3 推荐架构：生产者 / 消费者

```
┌──────────── audiodg.exe · Session 0 · LocalService (受保护, 无桌面) ─────────┐
│  EAPO APO 链 … → [vfd_eapo.dll  (VST2)]  → …                                │
│      · 环形缓冲收样本 (无锁)                                                 │
│      · worker 线程: **只做"取前两声道 + 攒成 2048 帧一段 → 写 VfdAudioFrame"**  │
│        (交错立体声 16.4 KB/段; 48k → 23 段/s, 192k → 94 段/s)               │
│      · processBlock 只做 memcpy + 每 N 帧置一次标志, 绝不阻塞/握手           │
└─────────────────────────────────────────────────────────────────────────────┘
                          │  \\.\pipe\VfdSpectrum  (音频帧, 48k≈190KB/s, 192k≈770KB/s)
                          │  [管道命名空间是全局的 → 跨会话无需特权]
                          ▼
┌──────────── 用户会话 · Session 1 (交互桌面) ────────────┐   ┌── EAPO Config Editor ──┐
│ vfd_viewer (现有 DPG 程序 / JUCE standalone)            │   │ 插件面板也接同一管道    │
│ · 当管道服务端(CreateNamedPipe) → 收音频 → **重活全在这** │   │ (面板=编辑器进程里的   │
│ · 窗 + FFT + 对数分频 + 回落/峰值 + 508×302 渲染          │   │  另一个实例, 手里有     │
│ · 磷光暂留/峰值保持/泛光全在这边做                        │   │  音频, 所以能算)        │
└─────────────────────────────────────────────────────────┘   └─────────────────────────┘
```

> **v3/v4 起方向翻转（2026-09-28）**：以前是"插件里算好频段、管道只传 254~512 个 float"，
> 现在是**插件只透传音频、重活全在 UI 端**。原因见 §1.13：
> 分辨率只由 **FFT 窗长** 决定，而 audiodg 里不该跑重活；UI 端算才能开到 32768/65536 点。

**为什么只推"数据"而不是整帧 508×302 位图？**

| 方案 | 每帧数据 | 30 fps 带宽 | 说明 |
| --- | --- | --- | --- |
| **推音频（v4 现状，推荐）** | 2048 帧 × 2ch × 4 B = 16.4 KB/段 | 48k: 380 KB/s；192k: 1.54 MB/s | 插件里**不做 DSP**；分辨率由 UI 端自由决定（可到 65536 点）。实测管道突发 140 MB/s，占不到 1% |
| 推频段值（v2，旧） | 512×4×2 B ≈ 4 KB | 30 KB/s | 分辨率被管道里的频段数锁死 —— 254/512 个频段配 8192 点 FFT 只是把谱线重新分组。**已弃用** |
| 推整帧 RGB565 | 306 KB | 9.2 MB/s | 音频进程要白白多做一遍渲染；只有"显示端算力极弱"时才划算 |
| 推整帧 RGBA | 613 KB | 18.4 MB/s | 更没必要 |

### 1.4 跨进程通信：参考实现 `tk166/CppUtils` 的评估

参考库 `tk166/CppUtils`（你自己的工具库，无 README，共 4 个文件：
`SharedStruct.h` / `Peer.cpp` / `CMakeLists.txt` / `.gitignore`）的思路与我们的需求高度吻合：

**它是什么**：一个 `SharedMemory` 类（Windows `CreateFileMapping(INVALID_HANDLE_VALUE,…)` 页文件支撑 +
`MapViewOfFile`；POSIX `shm_open` + `ftruncate` + `mmap`），把一块共享内存映射成一个
`#pragma pack(1)` 的协议结构体 `BiDirectionalBridge`，双方直接用**无锁原子量**读写；
用一个 `initState`（0 空 / 1 初始化中 / 2 就绪）做握手，创建方 placement-new 后置 2，
加入方每 5 ms 轮询、5 s 超时抛"另一端可能已崩溃"。
`static_assert(ATOMIC_INT_LOCK_FREE == 2)` 这句写得很对：确保原子量本身不带隐藏锁。

**值得直接继承的优点**：单头文件、无依赖、跨平台；"创建或打开"语义（任一方可先启动）；
无锁、零拷贝；有超时与明确的诊断信息。作为一块**协议骨架**是合适的。

**但在 EAPO 这个场景下，有 6 处必须改（前两条是硬伤）：**

1. **`Local\` 前缀会直接失效（本机已实测）。**
   CppUtils 用的是 `"Local\\" + name`。而**实测：`audiodg.exe` 运行在 Session 0，
   交互用户会话是 Session 1**。会话内命名空间（`\Sessions\0\BaseNamedObjects` 与
   `\Sessions\1\BaseNamedObjects`）是互相看不见的 → 插件与 viewer **必然连不上**。
   必须改成 `Global\` 前缀（或改用命名管道，见下）。
2. **`Global\` 方向还有个特权陷阱（本机已实测）**：
   | 操作（从交互会话 Session 1 发出） | 结果 |
   | --- | --- |
   | `CreateFileMapping Local\...` | ✅ 成功 |
   | `CreateFileMapping Global\...` | ❌ **err=5 Access denied** |
   | `CreateEvent Global\...` | ✅ 成功 |
   | `OpenFileMapping Global\...` | 不需要特权（不存在时返回 err=2） |
   → 也就是说：**若走共享内存，必须由插件侧（Session 0，服务令牌）去创建 `Global\` 映射，
   viewer 侧只负责"打开"**；反过来（viewer 创建）在当前机器的用户会话下**做不到**。
   这条必须在真实 EAPO 环境里验证——**不要用两个控制台程序互相测**（它们同会话，
   `Local\` 也能通，会把问题掩盖到部署时才爆）。
3. **没有安全描述符**：`CreateFileMappingA(..., NULL, ...)` 会继承创建者的默认 DACL。
   LocalService 令牌创建的对象的默认 DACL 很可能不允许交互用户打开 →
   需要显式 `SECURITY_ATTRIBUTES` + SDDL（例如给 `LS`/`AU` 相应权限）。
4. **握手会阻塞最长 5 秒**：`waitForInitialization()` 5 ms 轮询、5 s 超时。
   这**绝不能出现在 `processBlock` 里**（违反 APO 实时契约，见 §1.5）；
   而且创建方若在置 `initState=2` 前死掉，之后每个加入方都会白等 5 秒。
5. **抛异常**：库用 `throw std::runtime_error` 报错。在 audiodg 里让异常逃逸是大忌
   （`__try/__except` 抓不住 C++ 异常）→ 改成返回状态码，失败即安静降级。
6. **没有版本/魔数/长度校验**：作者自己在示例里靠改名（`SafeBridgeV3`）规避"残留内存块
   大小不一致"——这正是缺校验的后果。应把 **magic + 协议版本 + sizeof 断言**放进结构体，
   并把版本号写进对象名；另外**单个结构体没有防撕裂机制**，
   `countA/countB` 只是计数器、不是一致性协议 → 多 KB 的频谱帧需要 **seqlock**
   （`seq` 奇数=写入中/偶数=稳定，读者重试）或双缓冲+序号。

**建议的帧格式**（与 `src/vfd_pipe.h` 一致，改动前先看那里的 `static_assert`）：

```cpp
// ---- 协议 v4：立体声音频帧（现状）----
#pragma pack(push,1)
struct VfdAudioFrame {
    uint32_t magic;         // 'VFDF'
    uint32_t version;       // 4
    uint32_t n_samples;     // = 2048（固定，便于消费者按固定大小收帧）
    uint32_t samplerate;    // 原始采样率（可能 192000）
    uint64_t frame_index;   // 生产者自增；消费者用它检测丢帧
    double   timestamp_s;
    float    gain_db;
    uint16_t channels;          // 源声道数（已混单，这里只记录）
    uint16_t producer_session;  // ProcessIdToSessionId，自证跨会话
    uint32_t reserved;
    float    samples[2048*2];   // 交错 L/R float32
};
#pragma pack(pop)
static_assert(sizeof(VfdAudioFrame) == 16428, "协议布局变了要一起升版本号");

// ---- 协议 v2：频段帧（旧，保留给老 DLL / 只想要柱状数据的消费者）----
struct VfdFrame { uint32_t magic, version, n_bands, samplerate; ... float bands[512], peaks[512]; };
```

> **为什么音频帧不需要 seqlock**：管道是**消息模式**（`PIPE_TYPE_MESSAGE`），
> 一次 `WriteFile` 就是一整条消息，读端要么拿到完整那条、要么失败 —— 天然不会撕裂。
> 当年为共享内存准备的 seqlock 方案因此一直没用上（见 §1.4，那条路已经不需要走了）。

### 1.5 传输选型结论（把上面两条实测合起来）

| 方案 | 结论 |
| --- | --- |
| **命名管道（首选）** | **EAPO 自己就是这么干的**：设备测试时由**用户会话的 DeviceSelector 进程**建 `\\.\pipe\EqualizerAPODeviceTest`（名字写进 `HKLM\SOFTWARE\EqualizerAPO\DeviceTestPipeName`），**audiodg 侧作为客户端** `CreateFileW` + `WaitNamedPipeW(...,1000)` 重试连接，失败就记日志放弃重试。管道命名空间是**全局**的，天然跨会话，**不需要 `Global\` 特权**。数据量也够：1 KB/帧 × 30 Hz = **30 KB/s** |
| 共享内存（CppUtils 路线，第二阶段） | 每帧零拷贝、无锁，长期最优；但要按上面 6 条改造（`Global\` + 显式 SDDL + seqlock + 不抛异常 + 不在音频线程握手），并先验证 Session 0 侧能否创建 `Global\` 映射 |
| `WM_COPYDATA` | 需要在 Session 0 里找到 Session 1 的 HWND，不可靠 |
| 本地 socket | 无先例；`LocalServiceNetworkRestricted` 令牌下有额外不确定性 |
| 落盘文件轮询 | audiodg **碰不到用户目录**，只能写 `ProgramData`；IO 抖动 + SSD 磨损，仅作兜底 |

> **推荐路径**：spike 先做**命名管道**（有 EAPO 自身的成功先例、跨会话无特权门槛），
> 跑通后再用 CppUtils 那套共享内存把它替换成零拷贝版本 —— 帧格式（上面的 `VfdFrame`）
> 在两种传输下完全一致，替换成本很低。

> ⚠️ **关于"怎么验证"的一个警告**：这类 IPC 的失败模式**在普通宿主里复现不出来**。
> 同会话跑两个控制台程序，`Local\` 共享内存也能通；用 REAPER 当宿主，插件的账户、
> 会话、令牌、`effEditOpen` 行为全都不是 EAPO 的那一套（这正是你提到"没办法用 REAPER
> 调试"的同一类问题）。**所以 IPC 只能在真实 EAPO + audiodg 链路里验收**——
> 开发期可以用 REAPER 验证纯 DSP，但 IPC 一上线就要回到 EAPO。

> ⚠️ **关于"怎么验证"的一个警告**：这类 IPC 的失败模式**在普通宿主里复现不出来**。
> 同会话跑两个控制台程序，`Local\` 共享内存也能通；用 REAPER 当宿主，插件的账户、
> 会话、令牌、`effEditOpen` 行为全都不是 EAPO 的那一套（这正是你提到"没办法用 REAPER
> 调试"的同一类问题）。**所以 IPC 只能在真实 EAPO + audiodg 链路里验收**——
> 开发期可以用 REAPER 验证纯 DSP，但 IPC 一上线就要回到 EAPO。

### 1.6 必须先验证的三件事（建议按顺序做 spike）

1. **跨会话/跨账户的 IPC（第一优先，成败在此）**
   按 §1.5 的结论：**先做命名管道** —— viewer（用户会话）当服务端建管道，
   插件（Session 0）当客户端连；连接失败只记一次日志、安静降级（照抄 EAPO 的处理方式），
   绝不进重试循环（见 §1.7 的"10 次失败禁用整个 APO 栈"）。
   管道跑通后再评估是否用 CppUtils 的共享内存做零拷贝替换（要按 §1.4 的 6 条改造）。
   *这个 spike 是一小时量级的：最小 VST2（worker 线程里连管道 + 写 `VfdFrame`）+ 30 行消费者。*
   验收**必须在真实 EAPO 里**做，不能只看"两个控制台程序能通"。
2. **EAPO 能正常加载我们的 DLL**：DLL 对 `LocalService` 可读（否则编辑器直接告警
   "The library is not readable by the audio service."）、静态运行时不缺 DLL、
   VST2 `uniqueID` 不与他人相撞、架构必须是 **x64**（EAPO 1.3.0.0 是 x64 安装）。
3. **多实例语义**：EAPO 会在编辑器进程**再实例化一个插件实例**（`effEditOpen` 传的是编辑器
   自己的 HWND，两实例之间除了配置文件没有任何共享状态）→ 我们的 producer 必须容忍
   "第二个实例没有音频、也不该抢共享内存"（谁先创建谁当 producer，后到者只当 consumer）。

> 开发期不需要动 EAPO：本机已装 **REAPER (x64)**，可以直接把 DLL 丢进
> `C:\Program Files\VSTPlugins` 用 REAPER 验证 DSP/线程/IPC，最后再挂到 EAPO 上做真实链路测试。

### 1.7 其它风险（都是查证过的坑，不是猜测）

- **没有插件扫描、也没有缓存**：EAPO 不扫描、不建库，每次配置加载和**每次音频重锁**都会
  重新走一遍 `VSTPluginMain`/`effOpen`，而这一切发生在**音频服务里**。
  → **我们的 `VSTPluginMain` 和构造函数必须极快**：不做文件 IO、不做 COM 初始化、
  不 spawn 线程（把重活推迟到非实时的 `resume()`/首次 `processBlock` 之后的 worker 里）。
- **APO 连续失败 10 次会被系统整体禁用**：Windows 在 `CoCreateInstance`/`IsInputFormatSupported`/
  `LockForProcess` 等环节计数，达到上限（当前为 **10**）就把 APO 栈关掉
  （`PKEY_Endpoint_Disable_SysFx = 1`）。→ **任何初始化失败都不能反复重试**，
  必须"失败一次就安静降级"（这也正好是 EAPO 自己处理管道失败的方式）。
- **没有编辑器时要填有效的 `VstRect`**：EAPO 的 `startEditing()` 直接用 `effEditGetRect`
  的返回值且不检查是否写入，插件不给会**让配置编辑器崩溃**（2020 年至今未修）。
  → 即使我们不需要 UI，也要提供合法编辑器（或至少让 `effGetEditRect` 返回有效矩形）。
- **VST2 SDK 授权**：VST2 SDK 早已停止分发，参考仓库用的是第三方镜像
  (`volperenato/vstsdk2.4`) 并显式关掉相关警告。自用没问题，**若要分发需要慎重**。
- **分发可能撞签名墙**：受保护环境下的 APO 需要 WHQL / Windows Protected Environment 签名。
  调研**没有找到**任何"保持 `DisableProtectedAudioDG` 开启且能正式安装"的先例 ——
  自用可行，**做成给别人装的产品可能是真阻碍**。
- **稳定性**：插件与 audiodg 同生共死。`processBlock` 里除了"写环形缓冲"什么都不做；
  FFT/渲染/IPC 全在 worker 线程；不做分配、不加锁、不调用可能阻塞的系统例程。
- **EAPO 版本耦合**：上游只支持 VST2；本机是 1.3.0.0，官方最新 1.4.2
  （可读的镜像只到 1.4.0，1.4.1/1.4.2 的 VST 相关改动未能核实）。
- **采样率切换 / 格式变化**：实例会被销毁重建（见 §1.1 第 12 条），必须能自动重新衔接。
- **Windows 更新或声卡驱动重装可能抹掉 EAPO 的 APO 注册**，系统会静默回到"无 EQ"状态
  —— 排查"为什么频谱没数据"时要先想到这一条。
- **刷新率**：音频线程按 ~50 ms 节拍投递即可，不需要 60 Hz —— 磷光余辉会把中间帧补出来。

### 1.8 一个必须先回答的问题：**为什么不用现在的 WASAPI 回环？**

现在这个程序已经能"捕获当前 Windows 输出"，而 EAPO 是 APO，处理发生在**音频引擎内**，
所以**回环抓到的其实已经是 EQ 之后的信号**。那么 VST 到底多买到什么？

| 维度 | WASAPI 回环（现状） | EAPO VST |
| --- | --- | --- |
| 拿到 EQ 后信号 | ✅（引擎输出即 EQ 后） | ✅ |
| 需要改用户音频链 | ❌ 不用 | ✅ 要在 EAPO 配置里加插件 |
| 崩溃影响面 | 只死 viewer | **整个系统音频**（audiodg 内） |
| 采样率/位深读数 | 只能读端点 mix format | 能拿到 EAPO 实际处理格式 + 端点格式 |
| 独占模式 / ASIO 输出 | ❌ 抓不到 | ❌ 也管不到（ASIO 绕过 APO） |
| 多声道 / 分声道预处理 | 只能拿引擎混音后的 | ✅ 可以在链上任意位置、按声道 |
| 延迟 | 回环有额外缓冲 | 最低（就在链上） |
| 工程复杂度 | 已跑通 | 全新一套（VST + IPC + 部署 + 双击权限） |

**结论：如果目标只是"屏幕上显示频谱"，回环方案已经够用且风险低一个数量级。**
上 VST 的正当理由应当是其中之一：
(a) 需要链上任意位置/分声道的信号；(b) 需要极低延迟显示；
(c) 需要在没有可回环端点（独占、特殊驱动）时仍能工作；
(d) 想把"显示"和"采集"合成一个可分发的东西。
**如果这些理由不成立，建议把 VST 排后，先把 508×302 渲染器做好。**

---

### 1.9 管道传输实测结果（本机跑通，2026-09-27）

已按本节方案实现并验证最小闭环：

- `src/vfd_pipe.h` —— 协议（`VfdFrame` 2092 B，magic/version/size 校验）+ 服务端/客户端
- `src/pipe_server.cpp` —— 消费者（模拟 viewer 侧，**服务端**）
- `src/pipe_producer.cpp` —— 生产者（模拟插件 worker 线程，**客户端**）
- `tools/build.bat` —— VS 2026 BuildTools + Ninja 构建
- `tools/cross_session_test.ps1` —— 跨会话决定性验证（需管理员，见下）

| 场景 | 实测结果 |
| --- | --- |
| 稳态 20 fps / 254 频段 | 140 帧 / 7.0 s，**0 丢帧**；`WriteFile` 平均 **16.3 µs**、max 51.6 µs |
| 到达间隔 | 平均 50.1 ms、max 64 ms（抖动来自 `sleep_for` 定时器粒度，与管道无关） |
| 突发吞吐 | **66,700 帧/秒（≈140 MB/s）**，5000 帧 0 丢帧 0 坏帧，写平均 3.1 µs |
| 服务端重启 → 生产者重连 | server#1 到点干净退出 → 生产者收到 `err=109`(BROKEN_PIPE) → **自动重连 server#2** 并续传，0 丢帧 |
| 帧完整性 | 消息模式管道：一次 `WriteFile` = 一整帧，全程 0 坏帧 |

**结论：传输不是瓶颈。** 30 fps 的需求是 ~60 KB/s，实测吞吐有约 **2000 倍**余量，
所以"要不要上零拷贝共享内存"现在完全不需要回答。

顺带修掉两个对插件侧同样致命的问题：

1. **阻塞式 `ConnectNamedPipe`/`ReadFile` 会让服务端无法按时退出**，残留实例还会让下一个
   服务端拿到 `ERROR_PIPE_BUSY(231)` → 必须用重叠 I/O + 有界等待。
2. 一次 `WriteFile` 最坏观测到 **144 µs**（管道缓冲满时短暂阻塞）→ 这就是
   "**写管道只能在 worker 线程，绝不能进 `processBlock`**"的实测依据。

**跨会话（Session 0 ↔ Session 1）已验证通过**（2026-09-27，在本机以管理员运行
`tools/cross_session_test.ps1`）：脚本以 `NT AUTHORITY\LocalService` 建临时计划任务，
让生产者在 **session 0** 里运行、连到 **session 1** 的服务端，结果：

```
[server] producer connected
[server] frames=40  fps=20.4  avg_dt=50.31ms  max_dt=65.23ms  ...
[server] TOTAL frames=160 gaps=0 bad=0 connections=2
```

**160 帧、0 丢帧**地从 session 0 送进了 session 1 的进程 → **命名管道路线成立**，
本方案最后一个未知数消除。

为了让这条结论**自证**而不是靠脚本推断，帧格式里加了 `producer_session` 字段
（`ProcessIdToSessionId`），服务端会在结尾打印：

```
[server] my session id = 1 (>=1 = interactive user session)
[server] VERDICT: server_session=1 producer_session=0 -> CROSS-SESSION OK
```

同会话运行（比如两个控制台程序互测）时它会诚实地说
`SAME SESSION (this run proves nothing about cross-session)` —— 这类 IPC 最容易
"通过测试却在上线时失败"，所以让工具自己声明证据强度是值得的。

---

> 数据来源与实测校正见 §5；以下为按目标帧率拆解的工作量账。

### 1.10 插件已实现并端到端验证（VST2 DLL → 管道）

**产物**：`build/plugin/vfd_eapo_artefacts/Release/VST/VfdSpectrumSender.dll`
（构建：`tools\build.bat plugin`）

**结构**（`plugin/`）：

| 文件 | 职责 |
| --- | --- |
| `BandEngine.h` | 窗（手写对称 Hann，与 numpy.hanning 等价）+ FFT + 对数分频 + dB/回落/峰值。参数与 Python 参考实现逐项对齐，便于日后"黄金帧"对照 |
| `SpectrumSender.h` | 无锁环形缓冲 + worker 线程 + 管道客户端。音频线程只做"混单 + 写环形缓冲"；FFT 与管道全在 worker |
| `PluginProcessor.{h,cpp}` | 直通分析插件。构造函数不做任何重活（EAPO 无扫描缓存）；采样率只登记，FFT 表由 worker 建 |
| `PluginEditor.{h,cpp}` | 极简编辑器**但必须存在**：如实说明"本窗口属于配置编辑器进程里的另一个实例、收不到音频"，同时显示本实例的统计 |

**验证工具**：`src/vst2_host_stub.cpp` —— 严格按 EAPO 的 VST2 契约加载并驱动插件：
`GetProcAddress("VSTPluginMain")`、`audioMasterVersion=2400`、`GetCurrentProcessLevel=2`(realtime)、
`CanDo` 只认 `startStopProcess`/`sizeWindow`、`GetTime`/`Idle`/`WantMidi`→0、
**先 `effEditGetRect` 再 `effEditOpen`**（EAPO 崩溃路径），并按真实播放速率喂音频。

**实测结果**：

| 检查项 | 结果 |
| --- | --- |
| 按 EAPO 契约加载 | `VSTPluginMain found`；`AEffect uniqueID=56667370`（'Vfsp'）、`numParams=0` |
| **`effEditGetRect`（open 之前）** | **返回 `0,0 520x168`** → EAPO ticket 138 的崩溃路径安全 |
| `prepareToPlay` → FFT | `rate applied: 48000 Hz, fftSize=4096, bands=254` |
| `processBlock` 收到的声道数 | **2**（JUCE 按 bus 布局适配；即使 `AEffect` 向宿主声明 64 也不会让平均被稀释） |
| 投递速率 | ~24 fps（`kSendHz=25`） |
| 消费者收帧 | **117~165 帧、0 丢帧、0 坏帧**，`band_max=0.915`（真的测到 1 kHz 测试音），校验和逐帧变化 |
| 生命周期 | `effMainsChanged(0)` + `effClose` 干净退出，无崩溃 |

**调试设施**：设 `VFD_DEBUG=1` 时 worker 会把链路状态打到 stderr
（`blocks/samples/ch/rate/fftSize/hist/connected/frames`）。插件在 audiodg 里看不到任何输出，
这个开关配合宿主桩能把"没数据"精确定位到是 processBlock、FFT 还是管道。

**过程中修掉的两个坑（都长得像"插件坏了"）**：

1. **宿主桩没按实时速率灌音频** → 8 秒音频十几毫秒灌完，worker 只跑了 3 轮就随 `suspend/close`
   结束，插件那个"首次重连等 500 ms"的窗口永远等不到。桩改为 `sleep_until` 实时节流；
   插件侧也顺手把首次连接改为**立即可尝试**。
2. **服务端监听循环每 200 ms 重复 `ConnectNamedPipe` 后 `CancelIo`** → 管道实例进入
   "再也不接受连接"的状态（服务端明明在 listening，客户端却怎么都连不上）。
   改为 `listen()` 每个实例只发起一次 + `pollClient()` 复用同一个重叠操作。

> 注：同一次运行里服务端会如实打印
> `VERDICT: ... -> SAME SESSION (this run proves nothing about cross-session)`——
> 因为宿主桩与消费者在同一会话。**跨会话那条结论由 §1.9 的计划任务测试独立给出。**

---

### 1.11 首次装进真实 EAPO：崩溃与修复（2026-09-27）

第一次把 DLL 挂进 EAPO，结果是**系统音频崩了**（audiodg 反复重启）。事件日志：

```
Faulting application name: AUDIODG.EXE
Faulting module name: EqualizerAPO.dll (1.3.0.0)     Exception code: 0xc0000094   ← 整数除零
（更早一条）Faulting module name: MSVCP140.dll       Exception code: 0xc0000005   ← C++ 运行时访问冲突
```

**根因链**（EAPO 的 `filters/VSTPluginFilter.cpp` 原文）：

```cpp
if (!firstEffect->initialize()) { LogF(L"The VST plugin %s crashed during initialization."); skipProcessing = true; }
effectChannelCount = max(firstEffect->numInputs(), firstEffect->numOutputs());
effectCount = (channelCount + (effectChannelCount - 1)) / effectChannelCount;   // ← 没有除零保护
```

`initialize()` **只在 SEH 异常时**返回 false（此时 `effect` 还是空的）→
`numInputs/numOutputs` 都是 0 → `effectChannelCount = 0` → **除零 → audiodg 被打死**。
也就是说：**触发条件是我们的插件初始化失败，而 EAPO 缺一个 `if (effectChannelCount == 0)` 把它放大成了系统级崩溃**
（编辑器里那句 "Plugin crashed during initialization." 是同一个原因的另一半表现）。

**为什么初始化会失败**：事件日志里那条 `MSVCP140.dll / 0xc0000005` 是决定性线索——
EAPO 安装目录里自带一份 **MSVCP140.DLL 14.29**，而 audiodg/Editor 加载插件时，
DLL 搜索路径会先命中那个目录；我们的插件当时是用 **/MD 动态运行时 + MSVC 14.50** 编译的
→ CRT 混用 → 初始化期间崩在 C++ 运行时里。
这正是参考仓库 `WindowsCoaxVolumeControllerVST` 里那句注释的来历：
`# 强制使用静态链接运行时 (/MT 而不是 /MD)` ——"静态链接，防止缺 dll"。

**修复**：插件改为**静态 CRT（/MT）**（`plugin/CMakeLists.txt`，在 `add_subdirectory(JUCE)`
之前设置 `CMAKE_MSVC_RUNTIME_LIBRARY` / `JUCE_MSVC_RUNTIME_LIBRARY`）。
实测 `dumpbin /dependents`，新 DLL **不再导入 MSVCP140/VCRUNTIME140**，只剩系统 DLL：
`KERNEL32 USER32 GDI32 SHELL32 ole32 OLEAUT32 WINMM d2d1 dxgi d3d11 dcomp IMM32 COMCTL32 dwmapi`。

**顺手排除掉的几个假设**（都有实测依据，不是猜的）：

| 假设 | 结论 |
| --- | --- |
| 镜像 VST2 SDK 的 opcode 枚举被宏"平移"，插件与 EAPO 说不同方言 | **不成立**：`tools/vst2_opcode_probe.cpp` 逐项对照 VST 2.4 规范值，**0 处不匹配** |
| JUCE 调了 EAPO 处理不当的宿主 opcode（如往 ptr 里 strcpy） | **不成立**：JUCE wrapper 只调 `Version/CurrentId?/CanDo/GetTime/GetCurrentProcessLevel/SizeWindow/UpdateDisplay/Automate/BeginEdit/EndEdit/IOChanged/ProcessEvents/VendorSpecific`，EAPO 全都安全应答；且实测 JUCE 不调 `GetProductString/GetDirectory/PinConnected` |
| 宿主回调里 `effect->user` 尚未赋值导致解引用垃圾指针 | **不成立**：桩已逐行复刻 EAPO 回调（含 `effect->user` 语义），插件表现正常 |

**新增的预防性验证**：`tools/cross_session_vst_test.ps1`（需管理员）——
用计划任务以 `NT AUTHORITY\LocalService` 在 **session 0** 里、用同一个宿主桩加载插件 DLL。
这正是 audiodg 的账户/会话环境（DLL 搜索路径、已加载的 CRT、无用户配置都不一样），
**下次动 EAPO 之前先跑它**，就不会再拿真实音频链去试错。

> ⚠️ 诚实说明：我无法在本机完整复现 audiodg（受保护进程 + EAPO 的确切模块集），
> 所以 /MT 是**证据支持最强**的修复，而不是被证明的修复。因此才有了上面那个 session-0 预检。

---

### 1.12 面板显示已实现（频谱画在 EAPO 配置编辑器里）

**目标澄清**：插件的意义就是**把 VFD 频谱显示在 EAPO 的那个面板里**；而音频实例在 audiodg、
面板实例在配置编辑器进程，所以必须靠跨进程通信把它们连起来。

**拓扑（关键翻转）**：**面板所在进程当管道服务端，audiodg 侧实例当客户端连过来**。
配置编辑器进程在**用户会话**里，所以它建管道、audiodg(session 0) 去连 ——
这正是 EAPO 自身设备测试的拓扑，已实测跨会话可通。

**实现**：

| 文件 | 职责 |
| --- | --- |
| `plugin/PanelConsumer.h` | 后台线程里当管道服务端：收**音频帧**（协议 v4 立体声）进环形缓冲，`readNew()` 交给消息线程；抢不到管道时如实报 `pipeTaken`，收到旧协议帧报 `wrongVersion` |
| `src/vfd_dsp.h` | **平台无关的分析核心**（窗 + FFT + 对数分频 + 亚 bin 插值 + dB/回落/峰值），零依赖，ESP32 端将来直接复用 |
| `src/vfd_render.h` | **VFD 渲染器的 C++ 移植**（点阵单元/点阵字体/磷光调色板/亮度层级/磷光暂留/格域两级泛光/暗角），零依赖 |
| `plugin/PluginEditor.{h,cpp}` | 12 fps 定时器：收音频 → 分析 → 画柱体与峰值帽 → 磷光暂留 → 渲染进 `juce::Image`；状态信息画在图像**下方**（不占点阵面积） |

面板尺寸 **524×334**（508×300 显示区 + 边距 + 状态行）。

**性能实测**（508×300，MSVC /O2 单线程，2026-09-28 重测）：

| 阶段 | 耗时 |
| --- | --- |
| 点阵域（清屏 + 柱体 + 峰值帽 + 磷光暂留） | **0.014 ms/帧** |
| 出图（**格域**两级 box_blur + 展开 + 暗角 + 查表） | **0.287 ms/次** |
| 分析（8192 点 FFT + 4097 频段 @48kHz） | **0.10~0.14 ms/次** |

→ 面板 12 fps 时单核占用 <1%。**注意上面这张表在本节写下时还是"像素域泛光 6.6~8.8 ms"，
后来按"泛光搬到格域（VFD 格）"改掉了 —— 完整账见 §1.13。**

**注意事项**：
- 管道是**单实例**的，所以"面板"和独立 viewer(`build\pipe_server.exe`) 不能同时收；
  面板抢不到时会显示 `pipe busy: another consumer owns it`。
  调试时可以用环境变量 `VFD_PIPE_NAME` 另开一条通路，不动正在跑的面板。
- 插件只在**有音频**时才尝试连接（见 §1.9 的闸门设计），所以静音时面板会显示
  "waiting for the audiodg instance"，播放后 1 秒内就连上。
- 换新 DLL 后要让 audiodg 与面板都重新加载：**面板窗口必须关掉再打开**（编辑器实例是
  "打开面板时才 new" 出来的）；audiodg 侧若没跟上，用 `tools\reload_audio.ps1` 强制重建管线。
  详见 §1.13(6)。管道是**单实例**的，用环境变量 `VFD_PIPE_NAME` 可另开一条通路做测试。

---

### 1.13 两个遗留问题：渲染 4 ms 与"频段精度不够"（2026-09-28）

用户看面板时指出两件事：状态行显示 **"EAPO 254 BANDS"** 和 **"render 4.0 ms"**，
而早年在一颗 16 位单片机上做同类显示只要 10 ms。两件事的根因与结论如下。

#### (1) 4 ms 是**旧 DLL**，源码里的渲染器早就是"格域"了

面板上那行 `render N ms` 量的是 `vfdrender::Screen::render()`。用 `tools/render_bench.cpp`
在同一台机器上量源码（MSVC /O2，508×300，cell=4）：

| 阶段 | 耗时（avg / min / max，240 帧） |
| --- | --- |
| `clearDynamic`（静态层 memcpy） | 0.001 ms |
| `drawBars`（含"每列取 max"重采样） | 0.008 ms |
| `drawPeaks` | 0.001 ms |
| `applyPersistence`（**逐点阵格** 的一阶 IIR） | 0.004 ms |
| **`render()`（展开 + 两级泛光 + 暗角 + 查表）** | **0.287 / 0.280 / 0.375 ms** |
| 合计 | 0.30 ms |

关键就是**两级 box_blur 从像素域搬到格域**（508×300 = 152,400 像素 → 127×75 = 9,525 格，
少 16 倍），展开时"点亮区 = 自身磷光 + 泛光、间隙只吃 0.55 倍泛光"。这是用户提出的那条
思路的落实。**代码里早就是这样，但当时那份 DLL（9:24 构建）比源码（9:37 的格域改动）旧**
—— 面板显示 4.0 ms 是"拿旧二进制当结论"。

> **重建时 DLL 被锁怎么办**（本机实测可行）：DLL 同时被 `audiodg.exe` 与 EAPO 的
> 配置编辑器映射，链接会报 `LNK1104 无法打开文件`。Windows 允许**重命名**已映射的镜像：
> `Move-Item VfdSpectrumSender.dll VfdSpectrumSender.prev.dll` 之后重新链接即可写出新
> DLL，正在运行的实例仍持有旧镜像（下次 EAPO 重新加载滤波器时才换新的）。

#### (2) "254 频段"不是分辨率 —— 分辨率只由 FFT 窗长决定

- 254 / 512 个**频段**配 8192 点 FFT，只是把 4097 根谱线重新分组；把频段数加到 512
  并不增加任何信息（这正是"256 不够、512 也卧龙凤雏"的原因）。
- 真正的分辨率是 **bin 宽 = 采样率 / 2^order**，只由**窗长**决定：

| 采样率 | 窗长 0.18 s → FFT | bin 宽 | 频点数(bins) | 默认频段数 |
| --- | --- | --- | --- | --- |
| 48 kHz | 8192 点 | 5.86 Hz | 4,097 | 4,097（=bins） |
| 96 kHz | 16384 点 | 5.86 Hz | 8,193 | 8,193 |
| **192 kHz** | **32768 点** | **5.86 Hz** | **16,385** | **16,385（五位数）** |

- 默认策略改为 **`bands = 0`（自动）= 频点数**：**一个 bin 都不丢**。显示端的
  122~249 列由渲染器做 **max 池化**（`max over the column's range`），所以窄峰不会被
  平均掉，也不需要"先降采样成 254"这种丢信息的中间步骤。
- **比一个 bin 还窄的频段必须插值**：192 kHz / 16384 点（bin 11.7 Hz）时，8193 个对数
  频段里有 **6577 个**窄于一个 bin —— 对它们取 `max over bins` 只会画出阶梯。
  现在这类频段改为**按频率线性插值**（`vfddsp::Analyzer::interpMag`），宽频段仍取 max。
  实测 48 kHz / 8192 点下，4097 个频段里有 3289 个走插值。
- 想更细：`VFD_WINDOW_S=0.35` → 192 kHz 上 65536 点（bin 2.93 Hz）。代价是画面更"黏"
  （时频不确定性），这是物理约束，没有别的开关。

#### (3) 架构翻转：插件只透传音频，重活全在 UI 端（协议 v3 → v4 加立体声）

| | 旧（v2） | 新（v4） |
| --- | --- | --- |
| audiodg 侧 | 环形缓冲 + worker 里跑 窗/FFT/分频/峰值 + 写频段 | **只混单 + 攒 2048 样本 + 写音频帧** |
| 管道载荷 | 512×4×2 B ≈ 4 KB/帧 @24 fps = 96 KB/s | 16.4 KB/段（48k 23 段/s = 380 KB/s；192k 94 段/s = 1.54 MB/s） |
| UI 侧 | 只管画 | **全部分析 + 渲染**（`src/vfd_dsp.h` + `src/vfd_fft.h`，零依赖共享） |
| 分辨率上限 | 被频段数锁死 | 由 UI 端的 FFT 窗长决定（可到 65536 点） |
| audiodg 风险面 | 有 DSP，崩了带走系统音频 | 只 memcpy，风险面最小 |

**实测（192 kHz，`pipe_producer` 灌 14 音合成信号）**：1030 段、93.7 段/s、**0 丢帧 0 坏帧**，
写管道平均 12.0 µs / max 72 µs；UI 端 `analyze = 0.29 ms`（32768 点 FFT + 8193 频段/次）、
`draw = 0.02 ms`。**实测（真实插件 DLL + 宿主桩，48 kHz）**：257 段、0 丢帧，
`band_max=0.915`（1 kHz 测试音），`analyze = 0.14 ms`。

#### (4) 版面：把 dB 刻度与状态行还给频谱，并在下方加"声量计 + 峰值计 + dBFS 刻度"

按用户要求去掉左侧 dB 刻度列与底部 `EAPO … BANDS / VFD` 状态行（信息移到 JUCE 状态栏 /
窗口标题），并在绘图区下方加了 foobar2000 那种**声量计(VU) + 峰值计**（只画前两个声道
L/R）与最底部一条**横向 dBFS 刻度**。127×75 网格下的最终版式：

| 区域 | 行 | 格数 |
| --- | --- | --- |
| 标题 + 右上读数 | 2..15 | 14 |
| **频谱绘图区** | 17..46 | **122 × 30** |
| 频谱 X 轴线 / 刻度线 / 频率标签 | 47 / 49..50 / 52..56 | 7 |
| **声量计 L / R**（2 条） | 58..61 / 63..66 | 122 × 4 每条 |
| **dBFS 刻度**（刻度线 + 数字） | 67 / 68..72 | 5 |

（绘图区从"去掉 dB 刻度列+状态行"后的 122×47 变成 122×30 —— 声量计与刻度占了 17 行，
这是"要么频谱高、要么有表"的取舍；小屏时 `meterH_ = round(15/cell)` 会自动收窄。）

**画法（与频谱同一套视觉语言）**：

- 声量条：水平方向的"深度渐变"，条头炽亮（与频谱柱体的竖直渐变同一条曲线 `0.12+0.88d^1.15`），
  底部亮度抬到 0.30 以免与峰值条主体分不开；
- 峰值条：整条按更低亮度（`V_METER_BODY=0.24`）**垫在下面**，只有**条头**用与频谱 peak 帽
  相同的最高亮度（`V_METER_TIP = V_PEAK = 1.0`）；
- **没有额外的延时保持段**：峰值就一条弹道（本块样本峰 + 24 dB/s 回落）；
- 计量坐标是 **linear in dB**，`[-60, 0] dBFS` ↔ 0..1，与底部刻度严格一一对应；
- 声量计那一段**跳过磷光暂留**（它自己已经带弹道，再叠余辉会把读数抬高、变迟钝），
  但渲染时的格域泛光仍然覆盖它，所以看上去还是同一块屏。

**弹道与实测验证**（`vfddsp::MeterBank`，与 Python `MeterBank` 同一套）：

| 输入（L 声道） | 实测 VU | 实测 peak | 理论值 |
| --- | --- | --- | --- |
| 正弦 amp=0.5 (-6.02 dBFS) | -9.0 dBFS | **-6.0 dBFS** | VU = 0.5/√2 → -9.03；peak → -6.02 |
| 正弦 amp=0.25 (-12.04) | -15.1 dBFS | **-12.0 dBFS** | VU → -15.05；peak → -12.04 |

（`tools/render_bench` 之外，用 `pipe_producer --tone 1000` 就能复现这张表。）

#### (5) 加窗与"泄漏"：窗一直是有的，默认换成 Blackman-Harris（2026-09-28 追加）

用户反馈"FFT 没加窗、频谱在泄漏"。先量再改 —— 新建 `tools/fft_window_check.cpp`，
喂一个**故意不落在 bin 中心**的 1 kHz 正弦（48 kHz / 8192 点），打印主瓣与旁瓣：

| 窗 | ±1 bin | ±2 | ±3 | ±5 | ±10 | 主瓣外最大旁瓣 |
| --- | --- | --- | --- | --- | --- | --- |
| 不加窗 | -12.1 | -16.9 | -20.0 | -24.2 | -30.0 | **-18.0 dBc** |
| Hann（当时默认） | -10.9 | -30.9 | -41.1 | -53.9 | -71.4 | **-34.8 dBc** |
| Blackman | -7.7 | -29.8 | -60.7 | -63.5 | -79.3 | **-45.2 dBc** |
| **Blackman-Harris（现默认）** | -5.7 | -19.4 | -47.0 | -110.5 | -99.7 | **-63.7 dBc** |

结论：

1. **窗一直是加着的**（Hann 旁瓣 -34.8 dBc 而不是不加窗的 -18.0 dBc，教科书值）。
2. 但用户的**观感是对的**：显示下限是 **-78 dB**，Hann 的旁瓣只到 -31 dBc ——
   一根 0 dBFS 的音，裙边会爬到屏幕上 **60% 的高度**，看着就是那道经典泄漏题。
   换成 **Blackman-Harris（旁瓣 -92 dBc，落在显示下限以下）** 后裙边直接消失。
   代价是主瓣 4→8 bin（谱线更胖），这就是"分辨率换动态范围"。
3. **低频那一坨不是泄漏**：0.17 s 窗的绝对分辨率是 5.9 Hz，而 45/60/80/106 Hz 这几根
   强音本身只隔十几 Hz —— 主瓣互相重叠糊成一片。换窗只会让它更糊（主瓣更宽），
   唯一的解法是**加长窗**（`VFD_WINDOW_S=0.35` → 192kHz/65536 点，bin 2.93 Hz）。
   单音对照图见 `docs/preview_window_check.png`：BH 下 1 kHz 是一根**光板直立的线**，
   两侧干干净净 —— 这就是"没有泄漏"该有的样子。
4. 开关：`VFD_WINDOW=rect|hann|blackman|bh`（Python 端 `--window`），
   `makeWindow()` 在 `src/vfd_dsp.h` 与 `vfd_spectrum.py` 里逐点一致（都是对称窗，
   分母 n-1，与 numpy.hanning 同口径）。

#### (6) 假泄漏的两个真凶：管道丢块 + 多分辨率档位断层（2026-09-29 凌晨，实测）

用户报"换了窗、换了音源、降了 3dB，屏上旁瓣照样抖着出现"。折腾了几轮，最后用两个
**可复现的实验**定死（教训：对照必须在**同一条路**上做 —— 我先前拿"回环（声音那条路）干净"
去推"面板（管道那条路）也应该干净"，方向就错了）：

1. **管道丢块 → 宽带假裙边**（这才是主因）。
   `pipe_producer --glitch 40`（每 40 段故意少发 1 段）能**精确复现用户截图**：440 Hz 山包 +
   一路拖到 10 kHz 的尾巴；不丢块时是两根光板竖线（对比图：`docs/preview_cpp_panel.png`
   与 `tools/` 下的 `_glitch.png`/`_clean.png`）。
   机理：丢一段 = 分析窗里一个不连续 = sinc 宽带裙边，**而声音走的是另一条路，完全听不见**
   （所以用户的"跳变会炒豆"反驳不成立：跳变不在音频里，只在**显示用的那份拷贝**里）。
   根因是我当初给的缓冲太抠 + 一段"防积压静默丢最旧数据"的代码。已修：
   插件环形缓冲 0.34 s → **1.37 s**（@192k）、worker 暂存 8 段 → **64 段且只记账不静默丢**、
   管道出缓冲 2 条消息 → **64 条（1 MB）**、面板读环 0.68 s → **2.7 s**。
   并且状态栏**最前面**常驻 `jump N drop M`（`jump` = 收到的流里块接缝处的不连续次数，
   门限自适应：本块内部正常最大斜率的 4 倍；实测干净流 0、每 40 段丢 1 段 = 26）。

2. **多分辨率阶梯的档位断层**（用户一眼看出来的第二件事："1kHz 和 200Hz 旁边有非常明显的断层"）。
   阶梯的档位分界（192kHz 下 ~100/201/402/804 Hz；每档"主瓣刚好塞进显示一列"）处，两档的
   分辨率不同 → 同一段噪声底读数差一截 → 硬切换就是一截台阶。
   修法：分界两侧各 `blendOctaves`(默认 0.7) 倍频程内**两档都算、按功率混合**；
   并把选档规则修正为"**够用的最短窗**"（原来权重算反了，把高频也推到长窗上，
   白牺牲时间响应）。现在 192k 下：高频用 32768 点(171 ms)、低频用 262144 点(1.37 s)，
   档位区间重叠过渡，整帧分析 **2.4 ms**（比修正前 4.3 ms 更便宜）。

3. **真正的元凶：读端"保序丢弃"**（2026-09-29 02:00，第二次截图之后才定位）。
   修完上面两条，用户再看：`drop` 一直涨、`jump` **恒为 0**、谱线还是"只剩半边 +
   低频一坨"。`jump 0` 就是最关键的那条线索 —— 接缝检测跑在**管道线程**里，
   它看不见**读端**的丢弃。
   而 `PanelConsumer::readNew()` 里那句"待取帧数 > maxFrames 就丢掉最旧的、只给最新那
   maxFrames 帧"正是读端丢弃：面板 12 fps @192k 每拍到货 **~16.0k 帧**，而 maxFrames
   恰好写成 **16384** —— 就差这一点点，约 20% 的拍子触发丢弃，每次扔掉几毫秒音频。
   丢几毫秒 ⇒ 分析窗里插入一个**相位台阶** ⇒ 频谱上一条 ∝1/f 的宽带裙边（低频爬得很高、
   到几千赫兹就沉到 -78 dB 显示下限以下），所以看着只有"半边"。听不出来，因为声音走的是
   另一条路（EAPO 音频链路），只有我们这条**显示链路**在丢。
   复现（比 `--glitch 40` 更贴近现场）：
   `pipe_producer 4 --rate 192000 --tone 440 --amp 0.5 --glitch 8`（每 80 ms 挖掉 10 ms，
   即 12.5 Hz，与面板 12 fps 同量级）+ `pipe_server --png` → 与用户截图**形状完全一致**；
   不挖洞则是一根细线（`build\_c8.png` / `build\_g8.png`）。
   修法：
   - `readNew()` 改成**只少给、不丢**；面板侧循环抽干为止（`pullFrames_` 16384 → 32768，
     环形 2.7 s 最多 16 轮抽干）。
   - 环形写满时**按帧记账**（原来是 `readPos_ + 1`：每来一帧把时间轴往前抠一格，
     丢了多少根本数不清）。
   - 状态栏的 `drop` 从"丢弃次数"改成 **真丢掉的帧数**（`overruns()` → `droppedFrames()`）。
   - 采样率识别挪到 `timerCallback()` 开头 —— 否则第一拍抽出来的音频会因为
     "还不知道采样率"被 `break` 白扔，那同样是静默丢数据。
   - 顺带把状态栏压短（去掉 `3488ch`/`sess`/`ms`），并把 stale-build 警告压成 4 个字符
     `STALE `：长警告会把 `jump/drop` 顶出 524 px 可视区 —— **仪表装上了看不见 = 没装**。
   Python 参考实现里的 `StereoRing.read_new()` 是同一个写法，一并改成"抽干"。

4. **修好之后应该看到什么**（免得再被当成"还没修好"）：干净的 24-bit 纯音在屏上就是
   **一根细线 + 全黑**。BH 旁瓣 -92 dBc、24-bit 量化底噪 ≈ -150 dBFS，都在 -78 dB 显示
   下限之下 —— 右半边（甚至左半边）本来就**什么都没有**。之前那坨"山包"里没有一丁点
   是真信号，全是丢帧造出来的。想要"能看见底噪"的观感，得把显示下限从 -78 dB 抬到
   ~-60 dB（另说，不是 bug）。

5. **第 0 档死区：495 Hz 以上一个频段都不更新**（2026-09-29 03:00，放音乐时"一刀切"）。
   丢帧修好之后，放音乐会看到**谱线在 500 Hz 处齐齐切断**（切点正好落在频标 "500" 上），
   上面连底噪都没有；纯音测试看不出来，因为 440 Hz 恰好落在第 1 档里。
   根因是 `Analyzer::process()` / `applyBands()` 这对函数**逻辑倒置**：

   ```cpp
   refreshMeasure(s);                                        // 量完
   for (auto& o : s.own) dtAccum_[o.band] = 0.0;             // ← 立刻清零
   ...
   if (acc <= 0.0) continue;                                 // applyBands 又据此跳过
   ```

   刚量完就把"距上次刷新的累计时间"清零，`applyBands` 于是把这一档**整档跳过**。
   其它档 `divisor` 是 2/4/8，隔帧才有 `acc > 0`，靠"用上一帧旧值"歪打正着地活着；
   **只有第 0 档 `divisor == 1`（每帧都量、每帧被清零、每帧被跳过）→ 它负责的
   495 Hz~20 kHz 共 8772 个频段永远停在 0（= 显示下限）**。
   修法：清零挪到 `applyBands()` **用完之后**。
   验证：新增 `tools/dsp_probe.cpp`（见下表）——修前 5 kHz 读 -78 dB，修后 -4.5 dB。

6. **静音时画面定格**（同一轮）。音频一停（暂停 / 设备切走 / 插件被旁路），audiodg 侧
   就不再发帧、管道断开、状态栏回到 `listening ...`；此时若什么都不喂，分析器会拿
   环形缓冲里那段**陈旧数据**一遍遍重算 —— 窗里内容永远不变 ⇒ 谱线定格在最后一帧，
   整块屏看起来就是"卡死"。修法（用户提的"GUI 端长期补零"）：`got == 0` 时按 **dt 的
   真实经过时间**补等量静音（`analyzer_.push` + `meters_.pushInterleaved`），窗被静音
   灌满后柱体/峰值/声量计按各自弹道正常掉到底。同时把 `dt` 的来源从"上一个**有声**
   的 tick"改成"上一个 tick"（原来音频一停 dt 会一路涨到 0.25 s 的钳位值）。
   `tools/dsp_probe.cpp` 里"2 s 纯音 → 2 s 静音"那段就是这条的回归测试（静音后全部
   回到 -78.0 dB）。

#### (7) 新增/更新的工具
| 工具 | 用途 |
| --- | --- |
| `tools/render_bench.cpp` | 渲染器分阶段计时 + 黄金帧导出（`--png` / `--raw`） |
| `tools/bench_render.py` | Python 参考实现的同口径基准 |
| `tools/dsp_probe.cpp` | **分析器离线体检**：打印各档负责的频段范围 + 若干频点的 dB 读数（纯音那一列 / 静音那一列）。不碰管道、不碰音频设备 —— 上面那个"第 0 档死区"就是它一眼看出来的 |
| `tools/fft_window_check.cpp` | 加窗/泄漏体检：四种窗的主瓣宽度与旁瓣电平（dBc） |
| `tools/reload_audio.ps1` | （需管理员）强制音频引擎重建管线，让 audiodg 换掉插件镜像 |
| `src/vfd_fft.h` | 零依赖 radix-2 实数 FFT（插件面板 / viewer / ESP 参考实现共用） |
| `src/vfd_dsp.h` | 平台无关分析核心（窗 + FFT + 对数分频 + 插值 + dB/回落/峰值）**+ MeterBank（VU/峰值弹道）** |
| `VFD_PIPE_NAME` | 覆盖管道名：面板占着默认名字时，测试可以另开一条通路（实测 `err=231`）。**值要写全**，含 `\\.\pipe\` 前缀（只写短名会 `err=123`） |
| `VFD_WINDOW` | FFT 窗：`rect` / `hann` / `blackman` / `bh`（默认） |
| `VFD_WINDOW_S` / `VFD_FFT_ORDER` / `VFD_BANDS` / `VFD_DB_MIN` | 不改代码就调分辨率与显示范围 |
| `plugin/BuildStamp.h` + 面板状态栏 | 面板自报 build 戳，杜绝"分不清跑的是哪一版" |
| `pipe_producer --tone F` | 发单音（做加窗对照、验证声量计读数：正弦 amp=0.5 → peak -6.0 dBFS / VU -9.0 dBFS）。⚠️ **R 声道是 2F 倍频**（故意让左右表读数不同）—— 混单声道后的谱上会出现 F 和 2F **两根**线，别当成 bug |
| `pipe_producer --glitch N` | 每 N 段少发 1 段：复现"丢块造出的假裙边"，同时验证消费端 `jump` 检测器有效 |

#### (8) 换 DLL 之后怎么让它在**两条进程**里都生效（有实测教训）

EAPO 里其实有**两份**插件实例，各自持有"从磁盘加载那一刻"的镜像：

| 实例 | 何时换镜像 |
| --- | --- |
| audiodg 里的音频实例 | 跟着"滤镜链重建"走（改配置保存 / 重选设备 / 重锁音频管线 / 重启音频引擎） |
| 编辑器进程里的**面板**实例 | **打开面板时才 new 出来** → 面板窗口不关，它就一直用旧镜像 |

所以"在 EAPO 编辑器里把 VSTPlugin 关掉再打开"**只刷新了前者**；
**面板窗口必须关掉再打开**。这正是用户 2026-09-28 那次误判的来源 ——
面板上同时显示着 `EAPO 254 BANDS` 与 `render 4.0 ms`，两个数都来自旧镜像，
而源码里的渲染器实测只要 0.287 ms（见本节 (1)）。

**证据与机制**（EAPO 源码 `mirror/equalizerapo`）：

- `helpers/VSTPluginLibrary.cpp`：`instanceMap` 是**进程内静态**的
  `unordered_map<path, weak_ptr<VSTPluginLibrary>>` —— 弱引用全部析构后模块才会
  `FreeLibrary`；谁还持有 `shared_ptr`，谁就继续跑旧镜像。
- `filters/VSTPluginFilterFactory.cpp`：`createFilter(configPath != L"")`
  （编辑器那条路径）只调一次 `library->initialize()` 探活，不建立长期引用。
- 本机实测：DLL 还在被 `audiodg`/编辑器映射时，链接会报 **`LNK1104 无法打开文件`**
  —— **这条报错本身就是"旧镜像还活着"的判据**；此时**重命名**已映射的 DLL
  （`Move-Item X.dll X.prev.dll`）就能写出新文件，正在跑的实例继续持有旧镜像。
- 本机实测：`audiodg` 的 PID/启动时间在多次重建 DLL 期间**不变**，而新 DLL 的文件
  仍被占用 —— 说明 audiodg 侧确实是靠"滤镜链重建"换镜像的，不需要重启进程。

**工具与自证**：

- `tools\reload_audio.ps1`（**需管理员**）：结束 `audiodg.exe`（AudioSrv 会自动拉起）
  或 `Restart-Service Audiosrv -Force`，然后打印新的 audiodg PID 与它映射的 `Vfd*` 模块、
  以及磁盘上 DLL 的大小/时间，方便对照。（禁用/启用音频端点或重启系统同样有效。）
- **面板状态栏现在带 build 戳**：`v3 build Sep 28 2026 22:52:17 … FFT 32768 -> 16385 bands
  … analyze 0.6 ms render 0.3 ms`。换 DLL 之后只要这行的时间跟磁盘上 DLL 的时间对得上，
  就说明面板确实换了新镜像 —— 这类"分不清哪一版在跑"的坑到此为止
  （`plugin/BuildStamp.h`，同时写进 `VFD_DEBUG` 日志的前几行）。
- **两端指纹对照（自动报警）**：`VfdAudioFrame.reserved` 里带着 audiodg 侧镜像的
  build 指纹（编译期 FNV-1a）。面板把它和自己的指纹一比，不一致就在状态栏打出
  `!! audiodg runs build xxxxxxxx (panel yyyyyyyy) - run tools\reload_audio.ps1` 并变黄 ——
  不用再靠"数字看着不对"去猜是谁旧了。

#### (9) 第三轮打磨：灵动性 / 版面 / 主题色（2026-09-29 03:30）

前端三个 bug 排掉之后，用户提了三个体验项：

1. **"500Hz 以下很呆，隔半秒才动一下"** —— 不是 bug，是我当初给多分辨率阶梯加的
   **错峰刷新**（divisor 1/2/4/8）：低频档 0.33/0.67 s 才算一次，两次刷新之间柱体是
   **冻结**的（回落逻辑也只在刷新帧跑），所以看着一跳一跳。
   改法：默认 `stageStagger = false` → **所有档每帧都算**（`refresh /1`）。
   代价实测（192k 阶梯，pipe_server 口径）：**2.4 ms → 9.0 ms**（= c0 0.60 / c1 1.20 /
   c2 2.40 / c3 4.80 ms，正好 1:2:4:8）。12 fps 下是一个核的 ~11%，PC 上无所谓；
   将来上 MCU 用 `VFD_STAGGER=1` 打开错峰、或调大 `VFD_FFT_ORDER`/改小
   `VFD_MAX_WINDOW_S` 换档。
   （想再省一半的话，下一步是给最长那档做**抽取**：低通 + 32 倍降采样后只要
   8192 点 FFT —— 4.8 ms → ~0.3 ms；代价是要设计抗混叠滤波器，得先用
   `dsp_probe` 验证不会引入新的假谱线。）

2. **版面：把加高的空间尽可能给频谱**。口径（第一版我理解错了，这里记正确的那版）：
   **"整块显示区加高 20%，加出来的高度全给频谱；声量计条高度不变"**。
   点阵网格 127×75 → **127×90**（原生 300px → **508×360**，正好 ×1.2）：
   - 底部那行调试信息**整行去掉**（它画在窗口里、不占点阵，所以窗口同时矮了 18px）。
     需要时 `VFD_SHOW_STATUS=1` 召回来。
   - 标题块下方的 padding 减 2 行（`headerH_` 去掉那个 +2 的余量）。
   - 声量计条**保持 15px 标称高**（cell=4 下 4 行），底部 dBFS 刻度也不动。
   净效果：**频谱绘图区 30 行 → 47 行（120px → 188px，+57%）**；显示区里频谱的占比
   从 40% 升到 52%。窗口 334px → 376px。
   想临时试别的高度：`VFD_ROWS=<行数>`（40..200，重开面板即可，不用重编译）。

2b. **去掉频谱区左侧那条竖轴线**。它画在 `plotLeft_-1` 列，原本是"左边曾有 dB 刻度"的
   遗迹；刻度搬到底部横向 dBFS 条之后它只剩副作用 —— 它比柱形区（`plotLeft_` 列）
   **和声量计条（同样是 `plotLeft_` 列）都靠左一格**，于是看着"频谱柱形区左沿和声量计
   左沿没对齐"。去掉它之后，柱形区 / 横向轴线 / 频率刻度 / 声量计条 / dBFS 刻度
   全部从同一列（第 3 列）起；频率标签的最左侧也夹在绘图区内不再往左探。
   （实测口径：渲染图上第 2 列的亮度从 208 降到 70 —— 也就是只剩泛光，线确实没了。）

3. **单击点阵屏循环换主题色**：黄绿（= Python 参考实现的 `PHOSPHOR_STOPS`，逐项相同）
   → 深绿 → 蓝 → 粉 → 橘。实现是 `vfdrender::Theme` + `Screen::setTheme()`（重建那张
   512 项查表，代价可忽略）+ 面板的 `mouseUp()`。选择在**进程内**记住（关掉面板再打开
   还是上次那个色），不做磁盘持久化。`VFD_THEME=0..4` 可以固定起始主题；
   `render_bench --theme N --png` 用来出对照图。
   ⚠️ 关于"python 版本的深绿"：两边调色板在代码里**是同一张表**，肉眼差异来自画面内容
   分布而不是配色。所以这里的"深绿"按**传统 VFD/荧光管**的做法做：整条保持饱和绿，
   只有最亮的点芯才略微发白（`themeName()` = `deep-green`）。要别的绿就改那张表。

---

## 2. 用途 B：ESP32-P4 + I2S 输入的资源预估

### 2.1 目标参数

- 输出：508×302 @ 30 fps（60 fps 作为上限目标）
- 音频：I2S 输入，48 kHz / 32 bit / 2ch
- 算法：同 PC 端（窗 + FFT + 对数分频 + dB 回落 + 峰值保持 + 逐格磷光暂留）

**点阵密度已经实测过两档**（见 `docs/preview_508_cell4.png` / `preview_508_cell2.png`，
用现有渲染器直接按 508×302 出图）：

| 方案 | 网格 | 格数 | 原生 | 观感 | 每帧工作量 |
| --- | --- | --- | --- | --- | --- |
| **cell=4（推荐默认）** | 127×75 | 9,525 | 508×300 | 经典粗点阵，最像上世纪 VFD | 基准 1× |
| cell=2 | 254×151 | 38,354 | 508×302 | 更细密、更"LED 屏"，谱线更平滑 | 约 4× |

- `508 = 4×127 = 2×254`（两种都能整除宽度）；
- `302` 能被 2 整除、**不能被 4 整除** → `cell=4` 时原生高 300 px，
  上下各留 1 px 或拉伸 2 px（`302/300 = 1.0067`，肉眼无感）；
- **ESP 端建议 `cell=4`**：格数少 4 倍 → 暂留/展开/bloom 全部省 4 倍，而且更像点阵 VFD；
- **PC 插件端可以 `cell=2`**：算力富余，换来更平滑的谱线与 302 的精确贴合；
  两端只要共用同一套 `vfd_core`，密度就是个参数。

### 2.2 每帧工作量（预算：30 fps = 33.3 ms，60 fps = 16.7 ms）

> 硬件数据与出处见 `docs/esp32-p4-hardware-facts.md`（含 URL）。标 **[实测]** 的是官方 benchmark
> 的周期数换算，标 *[ESTIMATE]* 的是按工作量推算。

**FFT —— esp-dsp 官方 benchmark 的 P4 周期数**

| 内核 | P4 周期 | @400 MHz | @360 MHz（量产板实际主频） |
| --- | --- | --- | --- |
| 1024 点 f32 radix-2 | 126,053 | 315 µs | 350 µs |
| 1024 点 f32 radix-4 | 89,820 | 225 µs | 250 µs |
| **1024 点 s16(Q15) radix-2** | **17,523** | **44 µs** | **49 µs** |

- **s16 内核比 f32 快约 7 倍** —— 这是本题最划算的一个开关。Q15 输入的信噪比 ≈90 dB，
  对"显示频谱"绰绰有余（`db_min = -78 dB` 都还没用满），dB/对数分频仍可在 float 里算。
- 官方表里**没有 2048/4096 点、也没有实数 FFT 的行**；按 radix-2 的 `N·log₂N` 外推，
  2048 点 s16 ≈ 3.9 万周期 ≈ **100 µs @400 MHz**。
- 结论：**FFT 不是瓶颈**（1024 点 s16 只占 16.7 ms 的 0.3%，占 33 ms 的 0.15%）。
  注意一个反直觉事实：同样这些内核，P4 的**周期数比 S3 还多**（S3 1024 f32 = 97,847），
  P4 靠 400 MHz（S3 240 MHz）才在墙钟时间上赢 —— 所以**别指望 P4 的 SIMD 会自动快很多**，
  能用定点就用定点。

**其余阶段**（`cell=4`，即 127×75 = 9,525 格 / 153,416 像素）

| 阶段 | 工作量 | 估计 | 备注 |
| --- | --- | --- | --- |
| I2S 收样本 | 384 KB/s（≈11.5 KB/帧 @30fps） | ~0 | GDMA 搬运，可忽略 |
| 窗 + 1024 点 s16 FFT | 17,523 周期 | 49 µs [实测换算] | 见上表 |
| 254 频段取 max + dB + 回落 | 254 × 十余次运算 | <30 µs *[ESTIMATE]* | 定点取 max，dB 用查表/近似 |
| **逐格磷光暂留 IIR** | 9,525 格 × 2 MAC | **<10 µs** *[ESTIMATE]* | 有 128-bit XespV 向量指令可用 |
| bloom（**格域**） | 9,525 格 × 2 遍 × 3 tap | ~50 µs *[ESTIMATE]* | 见下方警告 |
| **格 → 像素展开 + 写 FB** | 153,416 px（查表 + 写） | **1.5~2.5 ms** *[ESTIMATE]* | 单核粗略估算；交 PPA 可几乎归零 CPU |
| 面板搬运 | 306,832 B/帧 | 30fps 9.2 MB/s | 见 §2.3 |

**两条来自实测的强烈建议：**

1. **bloom 必须在"格域"做，不要照抄像素域实现。** Python 原型里把 `box_blur` 从 508×302
   像素域挪到 127×75 格域，成本降到 **1/34**（254×151 格域为 1/4.2）。这一步是当前实现里
   唯一的性能大头（占 `to_rgba` 的 85%+）。
2. **不要做 CPU 往返 PSRAM 的全帧读-改-写。** 有公开案例：同样是 P4，
   640×512 的 YUV422→RGB 放在 DSI 驱动里做（硬件）能稳定 **60 fps**，
   同样的转换放在 CPU 上做只有 **~30 fps**，作者的原话是"CPU 大量时间在等 PSRAM 数据回来再写回去"。
   我们的架构**天然避开了这个坑**：磷光暂留发生在**格域**（9,525 个值，躺在 SRAM/缓存里），
   帧缓冲只需**顺序写**，不需要读-改-写 —— 这把 PSRAM 压力直接砍半（见 §2.3）。

**PPA 的未知数**：Espressif 没有发布 PPA 的吞吐指标，只有 LVGL 的相对收益
（填充/贴图平均省 ~30% 渲染时间，纯填充在整数倍缩放时最多 9×；旋转比软件实现省 ~40%；
而"图像混合"因为受 2D-DMA 显存带宽限制**几乎没有收益**）。所以"把展开交给 PPA"
是可行方向，但收益要实测，不能照搬宣传数字。

### 2.3 内存与带宽账

**帧缓冲**

| 格式 | 单帧 | 双缓冲 |
| --- | --- | --- |
| RGB565 | 306,832 B ≈ **300 KiB** | 599 KiB |
| RGB888 | 460 KiB | 899 KiB |
| ARGB8888 | 599 KiB | 1.2 MiB |

**内部 SRAM 的现实**（有实测支撑）：P4 有 768 KB L2MEM，但应用实际可用的只有
**≈569~592 KiB**（开 PSRAM 后 L2 会被占用一部分）；另有 8 KB @400 MHz 的 SPM/TCM。
→ RGB565 **单缓冲 300 KiB 塞得进内部 SRAM，双缓冲 600 KiB 塞不进**。

**PSRAM 的现实**（这点推翻常见直觉）：

| 口径 | 数值 |
| --- | --- |
| 理论（16 线 × DDR × 250 MHz） | ≈1000 MB/s |
| **社区实测**（64 KB 缓冲、带缓存、逐字节校验） | SRAM→PSRAM **104 MB/s**、PSRAM→SRAM **99 MB/s**、PSRAM→PSRAM **61 MB/s**、SRAM→SRAM 365 MB/s（SIMD 优化 758 MB/s） |

→ **实测 PSRAM 带宽只有理论值的约 1/10（~100 MB/s 量级）**，这才是真正的天花板。

**带宽账（508×302 RGB565）**

| 项目 | 30 fps | 60 fps |
| --- | --- | --- |
| 帧缓冲**顺序写**（我们的架构） | 9.2 MB/s | 18.4 MB/s |
| 帧缓冲**读-改-写**（若把暂留做到像素域） | 18.4 MB/s | 36.8 MB/s |
| 加上 DSI 扫描输出（FB 在 PSRAM） | +9.2 | +18.4 |
| 最坏组合（像素域 RMW + 扫描输出）@60fps | — | **≈55 MB/s ≈ 实测 PSRAM 的 46~55%** |
| 串行 SPI 面板 @80 MHz（≈10 MB/s 可用） | 9.2 MB/s → 已顶到 92% | ❌ 不可能（上限≈32 fps） |

**面板接口是唯一可能"一票否决"的变量**，而且有个反直觉的坑：

- P4 的 **MIPI-DSI 只有 video mode，没有 command mode**，官方建议单 lane
  速率 **≥480 Mbps**；而 508×302@60fps 算下来（含 blanking、8b/10b 与协议开销）
  只要 **~219 Mbps/lane** —— **低于官方推荐下限**，小面板未必是 DSI 的舒适区。
  （≥480 这个数字是官方文档；219 是推算，需按实际面板的接受范围核实。）
- 因此 **508×302 这类小面板，优先考虑"并行 RGB"**（P4 支持 8/16/24-bit RGB via LCD_CAM + GDMA），
  或者确认所选面板支持低 lane rate 的 DSI。SPI 屏直接排除（见上表）。

**I2S 输入**

- 带宽上基本免费：48 kHz × 2ch × 32 bit = **384 KB/s ≈ 3.07 Mbit/s**，GDMA 搬运即可。
- P4 有 **3 个 HP I2S + 1 个 LP I2S**，主/从、全/半双工、TDM 最多 16 声道、8/16/24/32-bit，
  DMA 走 GDMA-AHB（DSI 走 VDMA，是两个不同的引擎）。
- **但官方文档没有明确"I2S 收 + 屏幕输出同时工作"**，也没给 PCM 最高采样率
  （IDF 例子用 144 kHz；DMA 单缓冲上限 4092 B）。→ 这是**必须早期验证**的组合项。
- 时钟域仍需决定：P4 作 I2S 从（与 DAC 同源，最理想）还是作主（与 PC 播放时钟不同源，
  会缓慢漂移，对"看频谱"无所谓）。

**无线**：P4 **完全没有 Wi-Fi/BT 射频**。要 OTA/联网就得配 ESP32-C6/C5 作伴生芯片
（ESP-Hosted over SDIO/SPI）。这不影响显示，但会影响 BOM 与固件复杂度。

### 2.4 结论

- **CPU 余量充足**：30 fps 下把"暂留 + bloom"留在格域、只用 Q15 FFT，
  单核占用落在**个位数到 ~15%** 量级（最大头是"格→像素展开"，可再交给 PPA）；
  另一核可空出来做 UI/伴生芯片通信。60 fps 也现实，只要展开走硬件或双核分担。
- **内存是硬约束**：RGB565 双缓冲 599 KiB **放不进**约 570 KiB 的内部 SRAM →
  **FB 要么单缓冲放 SRAM（推荐），要么放 PSRAM 并接受 ~50% 带宽占用**。
  格域缓冲很小：`cell=4` 时 int16 ×2 ≈ 38 KiB，`cell=2` 时 ≈150 KiB。
- **真正的风险排序**：① 面板接口选型（DSI lane rate 下限 / 并行 RGB / SPI 带宽）
  ＞ ② I2S 收 + 显示同时工作的验证 ＞ ③ 展开与 bloom 的实现归属（CPU vs PPA）
  ＞ ④ PSRAM 带宽（实测仅 ~100 MB/s）。
- **一句话**：508×302@30fps 对 P4 是"舒适区"，@60fps 是"可行但要精打细算"；
  决定成败的不是算力，而是**面板接口**和**别让每帧去回读 PSRAM**。

---

## 3. 共用：508×302 渲染器怎么"一份逻辑、三处运行"

三处实现（Python 原型 / JUCE 插件 / ESP 固件）最大的风险是**视觉漂移**。建议：

1. 抽出平台无关的渲染核心 `vfd_core`（C99，无依赖）：
   - 状态：格网格尺寸、调色板 LUT、`phosphor_tau`、频段映射表、回落/峰值参数
   - 接口：`vfd_push_bands(float*, n, dt)` → `vfd_render(uint8_t* fb, w, h, fmt)`
   - JUCE 端用 `juce::Image` 包装它；ESP 端直接输出 RGB565。
2. **把现有 Python 原型定为"参考实现"**，并生成**黄金帧**：
   - 固定随机种子合成信号，跑 N 帧 → 输出 PNG 序列（`tools/` 下已有 `--preview` 雏形）；
   - C/C++ 移植按同样的输入回放，输出逐像素对比（允许 ±1 LSB 量化误差）；
   - 现有 `tools/check_peak_axis.py` 那套"公式级断言"（方向 / 每列单点 / 暂留衰减率）
     直接翻译成 C 端单测即可复用。
3. 参数只保留一份：采样率、`db_min`、`tilt`、`release`、`peak_release`、`phosphor_tau`
   做成一个可序列化 struct，PC 与 ESP 共用同一组默认值。

---

## 4. 落地顺序建议

1. **把渲染器参数化到 508×302** —— *已完成一半*：已按 508×302 出图验证过 `cell=4`
   与 `cell=2` 两档排版都成立（`docs/preview_508_cell*.png`），剩下的是把
   "窗口尺寸 → 网格尺寸"的换算从 CLI 参数固化成配置项（PC 端 `cell=2`、ESP 端 `cell=4`）。
2. **做 §1.4 的三个 spike**，其中 IPC 权限最关键 —— 决定插件路线是否成立。
3. 并行：ESP32-P4 侧先跑通"DSI 面板 + 静态画面 + 帧率计数"，再逐步接
   I2S → FFT → 渲染，每步都对着一帧黄金图验收。
4. 两端都稳定后再谈"插件 + 屏幕同源显示"这类联动。

---

## 5. 附录：实测数据

### 5.1 C++（产品路径，MSVC /O2，本机）

`tools/render_bench.cpp`，508×300，cell=4，512 频段输入，240 帧取均值：

| 阶段 | avg | min | max |
| --- | --- | --- | --- |
| clearDynamic | 0.001 ms | 0.000 | 0.001 |
| drawBars | 0.006 ms | 0.005 | 0.009 |
| drawPeaks | 0.001 ms | 0.001 | 0.001 |
| applyPersistence（含跳过声量计区） | 0.006 ms | 0.005 | 0.009 |
| **render()** | **0.289 ms** | 0.282 | 0.420 |
| 合计 | **0.302 ms** | 0.294 | 0.432 |

声量计/峰值计的绘制成本（2 条 × 122 列 × 4 行 ≈ 1000 格）在整链路实测里可见：
`pipe_server` 的 `draw` 从 **0.02 ms → 0.03 ms**（@192kHz 93.7 段/s 链路）。

cell=2（254×151 格，原生 508×302）合计 0.516 ms（其中 render 0.462 ms）——
像素数翻倍，成本基本线性。

分析侧（`src/vfd_dsp.h` + `src/vfd_fft.h`，单线程标量，每次 analyze 的耗时）：

| 采样率 / 窗长 | FFT 点数 | bin 宽 | 频段数（=bins） | 其中走插值 | analyze |
| --- | --- | --- | --- | --- | --- |
| 48 kHz / 0.18 s | 8192 | 5.86 Hz | 4,097 | 2,469 | **0.13 ms** |
| 192 kHz / 0.18 s | **32768** | 5.86 Hz | **16,385** | 13,153 | **0.63 ms** |
| 192 kHz / 0.35 s | 65536 | 2.93 Hz | 32,768 | 26,304 | **1.37 ms** |

按面板 12 fps 算，三种设置分别占单核 **0.16% / 0.76% / 1.6%**；按 viewer 的 24 Hz 算也只要
0.3% / 1.5% / 3.3%。**分辨率可以随便加，代价不在渲染上。**

传输侧（192 kHz，843 段）：写管道 avg **12.0 µs** / max **72 µs**，**0 丢帧 0 坏帧**，
770 KB/s（是实测突发能力 140 MB/s 的 0.55%）。

### 5.2 Python 参考实现（conda env `audio`，numpy 2.5.3）

`tools/bench_render.py`，120 帧取均值。**注意频段数已从"=显示列数"改为"=频点数"**
（4097），阶段耗时随之变化：

| 阶段 | cell=4（127×75 格，508×300） | cell=2（254×151 格，508×302） |
| --- | --- | --- |
| `SpectrumEngine.process`（FFT 8192 + BH 窗 + 4097 频段） | 0.214 ms | 0.27 ms |
| 绘制柱体+峰值帽（含 max 池化到列） | 0.333 ms | 0.62 ms |
| 磷光暂留 IIR（逐格） | 0.007 ms | 0.028 ms |
| `to_rgba`（**格域** bloom + 展开 + 暗角 + LUT） | **2.919 ms** | 3.20 ms |
| 合计 | 3.47 ms | 4.12 ms |

对照项：

- 改造前 `to_rgba` 是 **3.61 ms**（像素域两级 `box_blur`），改造后 **3.08 ms** ——
  只降了 15%，因为 Python 这条路径的瓶颈**不是 bloom 而是内存带宽**：
  输出是 `508×300×4` 的 **float32**（2.4 MB/帧，DearPyGui 要的格式）再加上
  展开/where/查表几个同样大小的中间数组，numpy 全程在做 5 MB/帧量级的分配与搬运。
  → **这条结论对 C++/MCU 不适用**：C++ 输出的是 457 KB 的 RGB888 且原地写，
  同一个渲染器只要 0.287 ms（见 5.1）。
- 频段数从 110 提到 4097 后，`process` 只从 0.25 ms 涨到 0.27 ms —— 因为归约改成了
  `np.maximum.reduceat` / `np.interp` 两条向量化路径，**没有 4097 次 Python 循环**
  （若按老写法逐频段循环，光这一段就要 ~4 ms）。
- 全分辨率（508×302）逐像素做暂留 IIR：0.026 ms/帧 —— 但"逐格"更便宜且语义正确。

**移植结论（已落实）**：`to_rgba` 的泛光搬到**格域**做（127×75 比 508×302 少 16 倍），
这与 C++ 侧 `vfdrender::Screen::render()` 现在是同一套做法。

---

## 6. 待验证 / 未决项

**已由本轮调研回答的**（结论见正文，出处见附录）：

- [x] EAPO 支持 VST2 还是 VST3 → **只支持 VST2.4**（VST3 仅第三方 fork）
- [x] 插件运行在哪个进程 → **音频实例在 audiodg**（LocalService + 受限令牌 + 与用户不同登录会话）；
      编辑器实例在配置编辑器进程，**两者无任何共享状态**
- [x] 插件能否自己建窗口 → **不能**；`effEditOpen`/`effEditIdle` 在音频进程里从不出现
- [x] 有没有先例的 IPC 传输 → **命名管道**（EAPO 设备测试：注册表发布管道名，best-effort，失败即放弃）
- [x] 参考仓库的"多进程通信" → **不存在**，纯 COM，且是音频线程反模式
- [x] 跨会话命名空间问题 → **实测 `audiodg` 在 Session 0、交互用户在 Session 1**；
      `Local\` 命名对象跨不过去；且**交互会话创建 `Global\` 文件映射被拒（err=5）**，
      而 `Global\` 事件可创建 → 若走共享内存，只能由插件侧创建、viewer 只负责"打开"
- [x] **命名管道能否跨会话** → **能**：生产者以 `LocalService` 在 session 0 运行、
      连 session 1 的服务端，160 帧 0 丢帧（`tools/cross_session_test.ps1`，需管理员）
- [x] 管道传输的容量与稳定性 → 20 fps 写管道平均 16.3 µs；突发 **66,700 帧/秒**；
      服务端重启后生产者自动重连续传（详见 §1.9）
- [x] 面板渲染为什么 4 ms → **拿的是旧 DLL**；源码里的格域泛光版本实测 **0.287 ms**
      （`tools/render_bench.cpp`，详见 §1.13）
- [x] "254 频段精度不够" → 频段数不是分辨率；改为 **窗长驱动**（192kHz → 32768 点 /
      **16385 频点**），频段数默认 = 频点数，比 bin 更窄的频段改走插值（§1.13）
- [x] audiodg 里要不要做 DSP → **不做**：插件只透传音频（协议 v4 = 交错立体声，16.4 KB/段，
      192kHz 下 1.54 MB/s），窗/FFT/分频/声量计/渲染全在 UI 进程（§1.13）
- [x] "频谱没加窗、在泄漏" → **窗一直加着**（Hann 旁瓣 -34.8 dBc vs 不加窗 -18.0 dBc）；
      但默认已换成 **Blackman-Harris**（实测 -63.7 dBc / 理论 -92 dBc），
      因为显示下限是 -78 dB，Hann 的裙边在屏上能爬到 60% 高度（§1.13(5)）

**仍需验证 / 未决**：

- [ ] **EAPO 是否真的按预期加载我们的 DLL**（对 LocalService 可读、x64、`VSTPluginMain` 够快、
      不出现在"连续失败 10 次禁用整个 APO 栈"的名单里）—— 这是下一阶段的头号任务
- [ ] 协议 v4（立体声音频透传）在**真实 audiodg** 里的表现：16428 B/段 × 94 段/s 的持续写入
      是否会影响音频线程（worker 线程里实测 12 µs/次，但要回到 EAPO 链路验收）
- [ ] CppUtils 改造后的共享内存版本，其显式 SDDL 能否让交互用户打开 LocalService 创建的对象
      （仅在"管道不够用"时才需要，目前看没必要；v4 的 1.54 MB/s 仍远低于管道能力）

- [ ] **能否接受开启 `DisableProtectedAudioDG`（关掉一项 Windows 安全缓解）**
      —— 这是插件路线最大的现实性前提，也是"能不能做成产品"的分水岭
- [ ] audiodg 的确切保护级别（PP vs PPL）、"受限对象命名空间"是否妨碍命名对象跨会话访问
      （官方无明文，只有二手安全研究）
- [ ] 命名管道在 `Initialize()` 之外长期使用是否稳定（EAPO 只在设备测试里用过一次）
- [ ] EAPO 实际投递的 block size 量级、静音时是否仍调用 `processBlock`（官方无文档）
- [ ] 508×302 的**面板型号与接口**：DSI lane rate 是否 ≥480 Mbps、还是走并行 RGB
      → 直接决定目标帧率上限，是当前最大的未知数
- [ ] P4 上 **I2S 收 + 屏幕输出同时工作** 是否稳定（官方未文档化）
- [ ] P4 上 PSRAM 实测带宽的手工复测（现有 ~100 MB/s 数据来自一个自称
      "主要是 AI 生成"的仓库，需自己跑一遍 memcpy 基准）
- [ ] esp-dsp 在 P4 上的 **2048/4096 点** FFT 周期数（官方表只有 ≤1024 点）
- [ ] I2S 信号从哪里来（与 DAC 同源还是独立 ADC）
- [ ] 黄金帧对照流程要不要进 CI（决定三处实现的视觉一致性成本）

---
 
## 附录 A：调研原始材料

- `docs/eapo-vst-feasibility.md` —— EAPO + VST 完整调研（硬约束 / IPC 先例 / 音频契约 / 坑 / 未证实清单）
- `docs/esp32-p4-hardware-facts.md` —— ESP32-P4 硬件事实与出处（含 11 项"未能证实"清单）
- 跨进程通信参考实现：<https://github.com/tk166/CppUtils>（`SharedStruct.h` / `Peer.cpp`，评估见 §1.4）
- `docs/preview_508_cell4.png` / `docs/preview_508_cell2.png` —— Python 参考实现在
  508×300 / 508×302 真机分辨率下的两种点阵密度实拍（2026-09-28 重出：新版面 + 声量计）
- `docs/preview_cpp_panel.png` —— **C++ 渲染器**对 192 kHz 14 音合成信号出图
  （同一份 `src/vfd_render.h`，即面板上画的那张图）
- `docs/preview_window_check.png` —— 同一渲染器、单根 1 kHz 正弦（BH 窗）：
  两侧干干净净，用来证明"没有泄漏"的样子
- `tools/render_bench.cpp` / `tools/bench_render.py` / `tools/fft_window_check.cpp`
  —— 渲染基准（分阶段计时 + 黄金帧）与加窗/泄漏体检
