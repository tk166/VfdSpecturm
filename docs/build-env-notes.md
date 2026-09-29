# 本机构建环境笔记（Windows + VS 2026 BuildTools）

> 这些全是**实际踩过**的坑，脚本里都有对应规避；换机器/换 shell 时先看这一页，
> 能省掉几小时的"为什么找不到 cmake"。

## 工具链位置

| 组件 | 路径 |
| --- | --- |
| VS BuildTools | `F:\Distros\VisualStudio\18\BuildTools`（v18.1.1） |
| vcvars | `…\BuildTools\VC\Auxiliary\Build\vcvars64.bat` |
| MSVC | `…\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x64\cl.exe`（v145，cl 报 19.50） |
| 另一份 MSVC | `14.44.35207`（同时存在，别搞混） |
| CMake | `C:\Program Files\CMake\bin\cmake.exe`（3.26.3，**不认识 VS 2026 生成器**，所以走 Ninja） |
| Ninja | `…\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe` |
| rc / mt | `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\{rc,mt}.exe`（**不在 PATH 上**） |

## 七个坑（按被坑次数排序）

1. **`PATHEXT` 被破坏成 `.CPL`**（本应 `.COM;.EXE;.BAT;.CMD;…`）。
   后果：在 cmd 里 `cmake`、`where`、`ninja` 全部"not recognized"，即使 PATH 完全正确。
   规避：批处理开头 `set "PATHEXT=.COM;.EXE;.BAT;.CMD"`，并且工具一律用绝对路径。
2. **`& exe args` 放进管道会失败**：`Cannot run a document in the middle of a pipeline`。
   规避：用 `Start-Process -RedirectStandardOutput <file>` 或
   `[Diagnostics.ProcessStartInfo]`（可设 `EnvironmentVariables`）来收输出。
3. **cmd 批处理必须 CRLF 换行**。用 LF 写 .bat 会让行被拆错，出现
   `'OM' is not recognized` 这种碎片错误。
3b. **`.ps1` 也必须纯 ASCII（无 BOM 时）**。Windows PowerShell 5.1 对没有 BOM 的文件按 ANSI
   读取，中文注释的字节会解码成乱码，**而乱码序列可能吞掉紧随其后的那一整行**
   （典型症状：某个变量始终是 `$null`，报 `Join-Path : 无法将参数绑定到参数"Path"，因为该参数是空值`）。
   实测踩过两次：一次是批处理，一次是 `tools/cross_session_vst_test.ps1` 里的中文注释把
   `$programData = ...` 整行吃掉 → 后面 `/tr` 拿到空值 → 任务创建失败。
   规则：**仓库里的 .bat / .ps1 一律 ASCII-only**（要中文就写进 .md 文档）。
4. **cmd 的注释只能用 `rem`，不能用 `#`**。`#` 开头的行会被当命令执行；
   行里的 `-`、`>` 还会被解释成重定向，往当前目录里写垃圾文件（我们真写出过
   `callers`、`passed`、`we` 三个文件）。
5. **变量名不能叫 `CL` / `LINK` / `_CL_`**：它们是 MSVC 的默认选项环境变量，
   cl.exe 会把其值当参数拼进每次编译 —— 症状是 `cl.exe(1): error C2143`（cl 去编译它自己）。
6. **`Start-Process -Wait` + 重定向会卡死**：cl.exe 会拉起 `vctip.exe` 遥测子进程并让它
   继承输出句柄，`-Wait` 会一直等到那个子进程退出（实测卡满 600 s 超时）。
   规避：`-PassThru` 拿到进程对象后用 `$p.WaitForExit(ms)` 轮询，并顺手 kill `vctip`。
7. **带空格的 `-D` 参数**：把多个 `-D…` 拼进一个变量时，含空格的路径必须**在变量里**
   就带上引号（`-DCMAKE_MT="C:/Program Files (x86)/…/mt.exe"`），否则展开后被空格拆散。

## 已知无害告警

- `CMake Warning: Manually-specified variables were not used by the project: CMAKE_C_COMPILER`
  —— 纯 C++ 工程没用 C 编译器，忽略即可。
- `git describe --tags` 在浅克隆的 submodule 上会失败（没有 tag），用 `rev-parse HEAD` 核对版本。

## 子模块（锁定版本）

```
third_party/JUCE     @ 29396c22c93392d6738e021b83196283d6e4d850   (JUCE 8.0.12)
third_party/vstsdk2  @ 6c7dc1464946537bddb3eea39eac176d02e72f68   (VST 2.4 SDK)
```

选这两个版本是因为 `tk166/WindowsCoaxVolumeControllerVST` 用的就是同一组
（JUCE 8.0.12 + `volperenato/vstsdk2.4`），属于**已被验证能编出 EAPO 可加载的 VST2** 的组合。
JUCE 需要 `VST2_SDK_PATH` 下有 `pluginterfaces/vst2.x/aeffect.h` —— 这个镜像正好是这个布局。

拉取（走系统代理 7897，清华源不可用）：

```
git -c http.proxy=http://127.0.0.1:7897 submodule add --depth 1 \
    https://github.com/juce-framework/JUCE.git third_party/JUCE
git -C third_party/JUCE fetch --depth 1 origin 29396c22c93392d6738e021b83196283d6e4d850
git -C third_party/JUCE checkout 29396c22c93392d6738e021b83196283d6e4d850
```

## 构建

```
tools\build.bat           # 编译管道传输工具
tools\build.bat plugin    # 另外编译 JUCE VST2 插件（-DVFD_BUILD_PLUGIN=ON）
```

## 传输层自测

```
build\pipe_server.exe 15                 # 消费者（先起）
build\pipe_producer.exe 10 20            # 生产者（20 fps，10 秒）
build\pipe_producer.exe 0 0 --burst 5000 # 吞吐余量测试
powershell -File tools\cross_session_test.ps1    # 跨会话验证（需管理员）
```

## VST2 插件的自测（不需要 REAPER，也不需要碰 EAPO）

```
rem 1) 先起消费者
build\pipe_server.exe 15
rem 2) 再用 EAPO 的契约加载插件 DLL（另开一个窗口；set 是给插件开调试日志）
set VFD_DEBUG=1
build\vst2_host_stub.exe build\plugin\vfd_eapo_artefacts\Release\VST\VfdSpectrumSender.dll 8
```

**命名约定：产物名不留空格。** 最初叫 `VFD Spectrum Sender.dll`，结果 schtasks 的 `/tr`、
EAPO 的 config 行、各种脚本统统要为引号转义（第一次 session-0 测试就是因此创建任务失败）。
改成 `VfdSpectrumSender.dll` 后**整条路径再无空格**，上面那条命令连引号都不需要。
显示名（UI 里）才允许有空格。

**动真实 EAPO 之前的预检**（需管理员）——它用计划任务以 `NT AUTHORITY\LocalService`
在 session 0 里加载插件 DLL，也就是 audiodg 的账户/会话环境：

```
powershell -ExecutionPolicy Bypass -File tools\cross_session_vst_test.ps1
```

宿主桩会打印 `AEffect` 信息、**`effEditGetRect` 在 open 之前返回的矩形**（EAPO ticket 138 路径），
消费者会打印收到的帧数/丢帧/校验和。`VFD_DEBUG=1` 时插件还会把
`blocks/samples/ch/rate/fftSize/hist/connected/frames` 打到 stderr —— 插件在 audiodg 里
没有任何输出，这是把"没数据"定位到具体环节的唯一手段。

⚠️ 两个测试环境自身的坑（都曾伪装成"插件坏了"）：

1. **宿主桩必须按真实播放速率喂音频**（`sleep_until`），否则 8 秒音频十几毫秒就灌完，
   插件的墙钟节流/重连窗口永远等不到 → 表现为"一帧都没发出去"。
2. **管道服务端不能在超时后重复 `ConnectNamedPipe` + `CancelIo`**：那会让实例进入
   不再接受连接的状态（服务端在 listening，客户端却连不上）。用
   `listen()`（只发起一次）+ `pollClient()`（复用同一个重叠操作）替代。
