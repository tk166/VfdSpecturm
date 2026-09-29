# Equalizer APO + C++/JUCE VST plugin + external GUI — feasibility research

Scope: what is *sourced*. Every claim carries a URL. Assumptions are labelled ASSUMED / UNCERTAIN.

Primary source for EAPO internals: the GitHub mirror `mirror/equalizerapo`, default branch `master`, HEAD `53d885f7` (2024-09-27), `version.h` = 1.4.0 — https://github.com/mirror/equalizerapo/tree/53d885f7
Latest official release: **1.4.2 (2025-03-21)** — https://sourceforge.net/projects/equalizerapo/files/
⚠️ The mirror stops at 1.4.0, so any 1.4.1/1.4.2-only change is outside my evidence.

---

## HARD CONSTRAINTS

### Format support
- **VST2 only. No VST3 in the official codebase.** EAPO loads the library by `GetProcAddress(module, "VSTPluginMain")` and answers `audioMasterVersion` with `2400` (VST 2.4) — https://github.com/mirror/equalizerapo/blob/53d885f7/helpers/VSTPluginLibrary.cpp , https://github.com/mirror/equalizerapo/blob/53d885f7/helpers/VSTPluginInstance.cpp
- I enumerated the whole 1.4.0 tree for `vst3|vst3sdk|IComponent|IPluginFactory`: **zero hits** (GitHub trees API on `master`).
- VST3 exists only in **unofficial third-party forks**, not in EAPO: https://github.com/Mixomo/EqAPO64_with_VST3_support and https://github.com/TheFireKahuna/equalizerAPO64 (344 stars, ships its own installers, notably a fork-owner comment "Ideally VST3 support would be the best route" — https://github.com/TheFireKahuna/equalizerAPO64/issues/14). Shipping a commercial plugin against a fork is a support-matrix decision, not an official target.
- EAPO's community voice is explicit: "I think Equalizer APO can't handle VST 3 plugins" — https://sourceforge.net/p/equalizerapo/tickets/302/
- The official Configuration reference does **not document `VSTPlugin` at all** (page last updated 2019-06-10; only 4 wiki pages exist) — https://sourceforge.net/p/equalizerapo/wiki/Configuration%20reference/ , https://sourceforge.net/p/equalizerapo/wiki/browse_pages/
- VST support was added in **version 1.2**: "Added: VST Plugin support. Add and configure plugins via Configuration Editor." — https://audiophilesoft.com/equalizer_apo/changelog.txt

### Bitness (this is the killer constraint)
- The plugin DLL must match the **bitness of the host process**; EAPO's arch check reports "must be %d-bit" derived from `_WIN64` of EAPO itself — https://github.com/mirror/equalizerapo/blob/53d885f7/filters/VSTPluginFilterFactory.cpp , https://github.com/mirror/equalizerapo/blob/53d885f7/Editor/guis/VSTPluginFilterGUI.cpp
- On 64-bit Windows, **audiodg.exe is 64-bit only** and there is no 32-bit variant: "A 32-bit Equalizer APO will not work on a 64-bit system. This is because ... Equalizer APO, just like any other APO, is a DLL that gets loaded into the Windows Audio Engine. On 64-bit Windows, the Windows Audio Engine is a 64-bit process (audiodg.exe). It is not technically possible to load a 32-bit DLL in a 64-bit process — the architecture has to match exactly." (Etienne Dechamps) — https://sourceforge.net/p/equalizerapo/discussion/general/thread/6a6aded57d/
- Independent confirmation: "On 64 bit systems there is only a 64 bit version with no 32 bit version" — https://medium.com/@boutnaru/the-windows-process-journey-audiodg-exe-windows-audio-device-graph-isolation-ea39546e205b
- Consequence: **ship x64.** The 32-bit EAPO installer even refuses to install on 64-bit Windows — same thread above. EAPO's own build produces both 32- and 64-bit binaries — https://sourceforge.net/p/equalizerapo/wiki/Developer%20documentation/
- EAPO also now has an **ARM64** build, whose commit message warns of "limited VST support as native ARM64 VST plugins are rare" — https://github.com/mirror/equalizerapo/commit/53d885f7
- The plugin DLL is loaded into **two** processes at once (audio engine + Configuration Editor), so its bitness must match both hosts — editor evidence below.

### Configuration syntax
Command name is exactly `VSTPlugin` (case-sensitive `command == L"VSTPlugin"`), parameters are space-separated **key/value pairs** parsed by `StringHelper::splitQuoted(parameters, ' ')`:

```
VSTPlugin Library "C:\path\MyPlugin.dll" "Gain" 0.5 "Frequency" 1000.0
VSTPlugin Library "MyPlugin.dll" ChunkData "<base64 state>"
```
- Two state modes, chosen by `effFlagsProgramChunks`: `ChunkData` (base64, via `CryptBinaryToStringW`/`CryptStringToBinaryW`) or free-form `"<param name>" <float>` pairs — https://github.com/mirror/equalizerapo/blob/53d885f7/helpers/VSTPluginInstance.cpp , https://github.com/mirror/equalizerapo/blob/53d885f7/Editor/guis/VSTPluginFilterGUI.cpp
- **Relative paths resolve under `<InstallPath>\VSTPlugins`**, where `InstallPath` comes from `HKLM\SOFTWARE\EqualizerAPO\InstallPath` — https://github.com/mirror/equalizerapo/blob/53d885f7/helpers/VSTPluginLibrary.cpp (`getDefaultPluginPath`). Absolute paths are allowed. The official changelog calls the folder "plugins" while the code and community say `VSTPlugins` — https://audiophilesoft.com/equalizer_apo/changelog.txt , https://sourceforge.net/p/equalizerapo/tickets/302/ (minor doc/code inconsistency).
- The path is only wrapped in quotes **if it contains a space**; parameter names containing space or `"` are quoted with `""` escaping — `VSTPluginFilterGUI::store()`, link above.
- Any key that is not `Library`/`ChunkData` is coerced with `wcstof` into the param map — so a typo silently becomes a parameter. Lines with unknown *commands* are silently ignored — https://sourceforge.net/p/equalizerapo/wiki/Configuration%20reference/
- The DLL (and any files it opens) must be **readable by the audio service**; the Editor checks this against `SECURITY_LOCAL_SERVICE_RID` and warns — https://github.com/mirror/equalizerapo/blob/53d885f7/Editor/guis/VSTPluginFilterGUI.cpp (`updatePermissionWarning`)

### Process model
- APOs "run inside the Windows audio graph process (`audiodg.exe`) which is itself managed by the Windows Audio service (`audiosrv`)" — https://github.com/dechamps/APO/blob/master/README.md
- audiodg.exe is spawned by AudioSrv (`svchost.exe -k LocalServiceNetworkRestricted -p`), runs as `NT AUTHORITY\LOCAL SERVICE`, **inherits AudioSrv's filtered token**, and "runs in a different login session from the logged on user (isolated) in order that content and plug-ins cannot be modified" — https://medium.com/@boutnaru/the-windows-process-journey-audiodg-exe-windows-audio-device-graph-isolation-ea39546e205b , https://medium.com/@S.1.l.k.y/abusing-windows-audio-for-local-privilege-escalation-1d59440116cb
- Corroboration inside EAPO itself: its log file is at `C:\Windows\ServiceProfiles\LocalService\AppData\Local\Temp\EqualizerAPO.log` — https://sourceforge.net/p/equalizerapo/wiki/Documentation/
- Microsoft states plainly: "**The audio engine runs in its own protected process**, which is separate from the process that the application runs in." — https://learn.microsoft.com/en-us/windows/win32/coreaudio/user-mode-audio-components
- Because it is protected, **unsigned APOs do not load by default**. EAPO's own developer documentation requires: "the DWORD value `DisableProtectedAudioDG` has to be set to 1. This disables the signature check for APOs, so that unsigned APOs will be loaded. This also means that applications requiring a secure audio path may change their behaviour or refuse to output audio altogether." — https://sourceforge.net/p/equalizerapo/wiki/Developer%20documentation/
- Under protection, an APO with an **embedded manifest** breaks: "having an embedded manifest triggers certain calls that aren't allowed in the protected Audio Engine" / "your APO will run with DisableProtectedAudioDG=1, but when this test key is removed, your APO will fail to load, even if it is WHQL-signed" — https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/implementing-audio-processing-objects , https://community.osr.com/t/apo-signing-and-installation/55812
- Signing expectation for the *proper* route: APOs "must be signed with a certificate indicating conformance and compliance with the robustness rules of the **Windows Protected Environment**" (Target Feature `Device.Audio.APO`) — https://learn.microsoft.com/en-us/previous-versions/windows/hardware/cert-program/deviceaudio-requirements
- EAPO's whole existence is unsupported: "this approach to setting up sAPOs *is not officially supported by Microsoft*; the fact that it works should be considered an 'happy accident'" — https://github.com/dechamps/APO/blob/master/README.md
- Microsoft's own guidance for a custom APO's UI is a **separate app**: "any configuration UI is packaged as a separate UWP or Desktop Bridge app" / "the recommended approach is to create a Hardware Support App" — https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/implementing-audio-processing-objects , https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/audio-processing-object-architecture
- Also: general-purpose APOs are against the guidelines — "You are not allowed to have 'general purpose' APOs; each APO must be associated with a specific audio device, and installed with the driver package for that device." — https://community.osr.com/t/apo-signing-and-installation/55812

### (a) Create your own Win32 window / GUI — **NO** (from the processing instance)
1. The processing instance lives in audiodg.exe, which has no interactive desktop and is session-isolated — https://medium.com/@boutnaru/... above.
2. EAPO **never** calls `effEditOpen` in the audio process. `VSTPluginInstance::startEditing()` is only reachable from the Editor — https://github.com/mirror/equalizerapo/blob/53d885f7/helpers/VSTPluginInstance.cpp
3. EAPO **never dispatches `effEditIdle`** in the audio process either; `doIdle()` is only called from `VSTPluginFilterGUI::on_idle()`, wired to Qt's `aboutToBlock` signal — https://github.com/mirror/equalizerapo/blob/53d885f7/Editor/guis/VSTPluginFilterGUI.cpp
4. Host callback services are minimal: `audioMasterGetTime` → `0`; `audioMasterIdle` → `0`; `audioMasterBeginEdit`/`EndEdit` → `0`; `audioMasterCanDo` returns 1 **only** for `"startStopProcess"` and `"sizeWindow"`; `audioMasterWantMidi` → `0` — https://github.com/mirror/equalizerapo/blob/53d885f7/helpers/VSTPluginInstance.cpp

### (a') Can the editor window be opened at all? — **YES, but in the Editor's own process, from a second instance**
- `VSTPluginFilterGUI` (Qt, inside the Configuration Editor) constructs its **own** `VSTPluginInstance(library, 1)` and calls `effect->startEditing((HWND)ui->frame->winId(), &w, &h)` for the embedded case, or hosts it in a modal `VSTPluginFilterGUIDialog` for "Open panel" — https://github.com/mirror/equalizerapo/blob/53d885f7/Editor/guis/VSTPluginFilterGUI.cpp
- Community confirmation of the two-instance reality: "It's audio manipulating engine (equalizerapo.dll) runs its own instance of the plugin. **The plugin you interface with isn't the one that is actually doing the audio manipulation.**" — https://sourceforge.net/p/equalizerapo/tickets/302/
- Therefore your spectrum data lives in a different process and a different object than the window the user can open. There is **no host-provided channel** to move data between them. The only state channel EAPO offers is the config file (chunk/params), polled at ~1 Hz by the Editor (`lastReadTimer`, `on_idle`) — https://github.com/mirror/equalizerapo/blob/53d885f7/Editor/guis/VSTPluginFilterGUI.cpp

### (b) Open files — possible, with heavy caveats
- OK: any path the audio service can read. **Not OK:** `C:\Users\<you>` and per-user profile data. "for security and isolation reasons, the service does *not* have access to your user directory" — https://github.com/dechamps/APO/blob/master/README.md
- "most VSTs do not anticipate running into permission issues. When such VSTs are used inside Equalizer APO ... the resulting behaviour can be erratic and hard to troubleshoot." — same source
- Classic real-world failure: license/activation files → plugin loads, UI looks fine, but the audio-side instance silently does nothing — https://sourceforge.net/p/equalizerapo/tickets/302/ and https://sourceforge.net/p/equalizerapo/tickets/221/
- Fix patterns: put files in `Program Files`/`ProgramData`, or grant **`NT SERVICE\Audiosrv`** read on the file; debug with Process Monitor filtered on `audiodg.exe` + `ACCESS DENIED` — https://github.com/dechamps/APO/blob/master/README.md

### (c) Networking — **do not**
- No primary Microsoft statement found. Secondary (security research): audiosrv runs under `LocalServiceNetworkRestricted` and audiodg "inherits a filtered LOCAL SERVICE token ... Service SID restriction ... Write-restricted SIDs limiting object creation and access ... Constrained object namespaces affecting named pipes and RPC endpoints" — https://medium.com/@S.1.l.k.y/abusing-windows-audio-for-local-privilege-escalation-1d59440116cb (secondary source — treat as indicative).

### (d) Spawn threads — allowed
- It's a normal user-mode DLL in a normal-ish process; `CreateThread` etc. work. Constraints are about *where you may block*, not about thread creation: APO real-time methods "must not block, use paged memory, or call any blocking system routines"; "All buffers ... must be nonpageable"; "APOs should not introduce significant latency" — https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/implementing-audio-processing-objects
- EAPO explicitly marks its processing entry point as real-time code (`#pragma AVRT_CODE_BEGIN`, which is `code_seg(push, "RT_CODE")` from the Windows SDK header) — https://github.com/mirror/equalizerapo/blob/53d885f7/EqualizerAPO/EqualizerAPO.cpp , `baseaudioprocessingobject.h` in the Windows SDK
- Caveat: there is no host message pump / `effIdle` in the audio process (see (a)), so anything depending on a pumped message loop (e.g. a JUCE `Timer` driven by `MessageManager`) is unreliable there.

### (e) Shared memory / named pipes to another process — **named pipes are precedented; you must handle "other side absent"**
EAPO itself does exactly this from inside audiodg:
- Server side (Configurator/DeviceSelector process) creates `\\.\pipe\EqualizerAPODeviceTest` and publishes the name to the registry value `HKLM\SOFTWARE\EqualizerAPO\DeviceTestPipeName` — https://github.com/mirror/equalizerapo/blob/53d885f7/DeviceSelector/DeviceTestThread.cpp
- Client side (inside audiodg) reads that registry value in `Initialize()` and writes JSON to the pipe — https://github.com/mirror/equalizerapo/blob/53d885f7/EqualizerAPO/EqualizerAPO.cpp (`EqualizerAPO::sendMessage`, `CreateFileW(L"\\\\.\\pipe\\" + name, GENERIC_WRITE, ...)`, `WaitNamedPipeW(..., 1000)` retry)
- **Crucially: it is only called from `Initialize()`, never from `APOProcess`** (call sites at lines 146 and 217 of that file).
- **It is best-effort by design**: on failure it logs "Could not connect to named pipe" and clears `deviceTestPipeName` so it stops trying — same file. This is precisely the "other side absent" pattern you need.
- Shared memory: I found **no EAPO precedent** → UNCERTAIN (see below).

### Audio processing contract
- **Format: float32 only.** EAPO reinterprets the APO connection buffers as `float*`, and Microsoft states the base class "operates on the required float32 format" — https://github.com/mirror/equalizerapo/blob/53d885f7/EqualizerAPO/EqualizerAPO.cpp , https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/implementing-audio-processing-objects
- **Sample rate / channel count are negotiated at `LockForProcess`**, then `engine.initialize(outFormat.fFramesPerSecond, inFormat.dwSamplesPerFrame, realChannelCount, outFormat.dwSamplesPerFrame, channelMask, maxFrameCount)` — https://github.com/mirror/equalizerapo/blob/53d885f7/EqualizerAPO/EqualizerAPO.cpp
- **Multi-instance, not multi-channel.** EAPO computes `effectChannelCount = max(numInputs, numOutputs)` and `effectCount = ceil(channelCount / effectChannelCount)`, creating **that many separate plugin instances**. A stereo VST on a 7.1 device ⇒ **4 instances**. Missing channels are fed zeroed scratch buffers (`emptyChannels`); if `numOutputs < numInputs`, the surplus outputs are zeroed — https://github.com/mirror/equalizerapo/blob/53d885f7/filters/VSTPluginFilter.cpp
- **Block size is host-determined and variable.** `effSetBlockSize` is called with `maxFrameCount` (the host's `APO_CONNECTION_DESCRIPTOR::u32MaxFrameCount`), but `processReplacing` is called with `ppInputConnections[0]->u32ValidFrameCount`, which can be smaller. So: negotiate a cap, then handle `frameCount <= cap` on every call — https://github.com/mirror/equalizerapo/blob/53d885f7/filters/VSTPluginFilter.cpp , https://github.com/mirror/equalizerapo/blob/53d885f7/EqualizerAPO/EqualizerAPO.cpp
- **Both process paths supported.** `processReplacing` if `effFlagsCanReplacing`, else legacy `process` — and in that case EAPO zeroes the output buffers first (correct accumulate semantics) — https://github.com/mirror/equalizerapo/blob/53d885f7/filters/VSTPluginFilter.cpp
- **Real-time thread.** EAPO constructs the audio-side instance with process level **2**, and feeds that straight back through `audioMasterGetCurrentProcessLevel`. The editor-side instance gets **1** — https://github.com/mirror/equalizerapo/blob/53d885f7/filters/VSTPluginFilter.cpp , https://github.com/mirror/equalizerapo/blob/53d885f7/Editor/guis/VSTPluginFilterGUI.cpp , https://github.com/mirror/equalizerapo/blob/53d885f7/helpers/VSTPluginInstance.cpp . In the VST2 SDK, `2 = kVstProcessLevelRealtime` — https://docs.rs/crate/truce-vst2/latest/source/shim/vst2_types.h
- **Allocation/blocking in `processBlock`: forbidden.** "All methods that are members of real-time interfaces must be implemented as nonblocking members. They must not block, use paged memory, or call any blocking system routines." — https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/implementing-audio-processing-objects
- **Latency: EAPO reports ZERO.** `EqualizerAPO::GetLatency` sets `*pTime = 0` and only forwards the *wrapped child APO's* latency. The VST's `initialDelay` is never read anywhere in 1.4.0 — https://github.com/mirror/equalizerapo/blob/53d885f7/EqualizerAPO/EqualizerAPO.cpp . A lookahead plugin therefore causes uncompensated A/V desync; the fork had to patch `initialDelay` support in by hand — https://github.com/TheFireKahuna/equalizerAPO64/issues/14
- **MIDI: not supported.** `audioMasterWantMidi` → `0`, and `effProcessEvents` is never dispatched — https://github.com/mirror/equalizerapo/blob/53d885f7/helpers/VSTPluginInstance.cpp
- **Instance lifecycle: destroyed and recreated on re-lock.** `VSTPluginFilter::initialize()` calls `cleanup()` (which destroys instances) before constructing new ones. Any state not in the chunk/param map is lost — https://github.com/mirror/equalizerapo/blob/53d885f7/filters/VSTPluginFilter.cpp
- **Crash containment is partial.** Init and process are wrapped in `__try/__except`; on a crash EAPO logs and passes audio through unchanged (`skipProcessing`, or `memcpy` in the handler). SEH catches access violations, **not** heap corruption or C++ exceptions — same file. And an APO crash still takes down audiodg.exe: "If an Audio Processing Object (APO) crashes, only audiodg.exe is affected, while the AudioSrv service continues running and can restart the audiodg.exe automatically." — https://medium.com/@S.1.l.k.y/...

---

## REFERENCE REPO IPC

**Headline: `tk166/WindowsCoaxVolumeControllerVST` has NO IPC, NO second process, NO tray application, and no shared memory, named pipe, WM_COPYDATA or registry channel. The premise of the brief is incorrect.**

Full repository inventory (GitHub trees API, `main`), excluding submodule contents — https://github.com/tk166/WindowsCoaxVolumeControllerVST :

```
.gitignore
.gitmodules
.vscode/settings.json
.vscode/settings.json.ucrtold
CMakeLists.txt
Doc/pic01.png
README.md
Source/PluginEditor.cpp     (2270 B)
Source/PluginEditor.h       ( 953 B)
Source/PluginProcessor.cpp  (5148 B)
Source/PluginProcessor.h    (2798 B)
+ submodules: JUCE, vstsdk2
```
State at fetch time: **2 commits** (last 2025-12-21), **0 releases, 0 issues, 0 forks, 0 stars**. The author (`tk166`) has 9 public repos; I enumerated all of them — **there is no companion GUI/tray/COAX USB application anywhere in the account** — https://api.github.com/users/tk166/repos

### What actually runs where
- **One artifact only** (two files, same code): `build\SysVolFollower_artefacts\Release\VST\Sys Vol Follower.dll` and `...\Release\VST3\Sys Vol Follower.vst3` — https://github.com/tk166/WindowsCoaxVolumeControllerVST/blob/main/README.md
- There is no EXE, no service, no tray icon. The JUCE `AudioProcessorEditor` (`SysVolFollowerAudioProcessorEditor`, `PluginEditor.cpp`) is the whole "GUI" — it draws a volume bar and a dB readout at 200×100 and repaints on a 33 ms `juce::Timer`.
- The **`SysVolFollowerAudioProcessor`** is instantiated twice by EAPO like any other VST2 (audio-side instance in audiodg via `createPluginFilter()`; editor-side instance in the Configuration Editor when the user opens the panel) — for the two-instance model see https://sourceforge.net/p/equalizerapo/tickets/302/ and https://github.com/mirror/equalizerapo/blob/53d885f7/Editor/guis/VSTPluginFilterGUI.cpp

### What it does instead of IPC (the real mechanism: COM, in-process)
The "external communication" is not IPC at all — it is **COM against the Windows MMDevice / EndpointVolume APIs, called from inside the plugin's own process**. Emitted by both processes independently:

- Types: `IMMDeviceEnumerator* deviceEnumerator`, `IAudioEndpointVolume* endpointVolume` (`Source/PluginProcessor.h`)
- `SysVolFollowerAudioProcessor::updateSystemVolumeDirectly()` (`PluginProcessor.cpp`): lazily `CoInitializeEx(NULL, COINIT_MULTITHREADED)` → `CoCreateInstance(__uuidof(MMDeviceEnumerator), ...)` → `GetDefaultAudioEndpoint(eRender, eMultimedia, &defaultDevice)` → `defaultDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, &endpointVolume)` → `GetMasterVolumeLevelScalar(&vol)` + `GetMute(&mute)`
- Result stored in `std::atomic<float> currentGain`; smoothing via `juce::LinearSmoothedValue<float> smoothedGain` (`reset(sampleRate, 0.15)` = 150 ms), applied with `smoothedGain.applyGain(...)`
- State enum `ComStatus { initializing, ok, errorPermission, errorNoDevice }` is declared but `errorPermission`/`errorNoDevice` are **never assigned** — dead code.
- The README's claim to "directly control the volume of COAX devices (such as the CT7601 and PCM2704)" is **not implemented**: the code only *reads* system volume and *writes* gain into the audio buffer. There is no HID/USB/COAX device access anywhere. It is a software volume follower.

### Data structures passed between "the two sides"
**None.** There is no struct, no ring buffer, no mutex, no event, no shared section, no serialization. The only cross-boundary object is `std::atomic<float> currentGain`, and it is shared between **threads of one process**, not between processes.

### Synchronisation
- `SysVolFollowerAudioProcessor` inherits `juce::Timer` and calls `startTimer(100)` in its constructor; `timerCallback()` does the COM init + volume read (100 ms).
- `SysVolFollowerAudioProcessorEditor` calls `startTimer(33)` and `timerCallback()` just calls `repaint()`.
- "Other side absent" handling: none needed (there is no other side). If COM init fails, `currentGain` simply keeps its previous value (initialised to `1.0f`), i.e. it fails open at unity gain. `getComStatus()` exists but is never surfaced in the UI.

### How it "avoids doing GUI work in the audio process"
It doesn't, really — it just relies on EAPO/JUCE giving the editor a separate instance in the Configuration Editor process. The processor's own `createEditor()` returns a new `SysVolFollowerAudioProcessorEditor`, which EAPO will only ever instantiate from the Editor process — https://github.com/mirror/equalizerapo/blob/53d885f7/Editor/guis/VSTPluginFilterGUI.cpp

**Anti-patterns in this repo worth copying *against*:**
1. `processBlock()` **also** calls `updateSystemVolumeDirectly()` every ~50 ms of audio (`samplesSinceLastVolumeCheck >= getSampleRate() * 0.05`) — so the audio thread performs `CoInitializeEx` + `CoCreateInstance` + COM cross-process calls on first use. That directly violates the APO real-time contract (https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/implementing-audio-processing-objects). The source comment even says the *timer* thread is the correct place, contradicting the code two functions above.
2. A `juce::Timer` in an audio processor hosted by audiodg: `effEditIdle` is never dispatched there and there is no host message pump (https://github.com/mirror/equalizerapo/blob/53d885f7/Editor/guis/VSTPluginFilterGUI.cpp), so whether `timerCallback()` ever fires in the audio process is unverified — UNCERTAIN.
3. It relies on `effEditGetRect` being consumed via EAPO's `startEditing()`, which is the exact code path that crashes for plugins without an editor — https://github.com/mirror/equalizerapo/issues/2 (JUCE always provides an editor here, so it dodges that specific bug).

### Build system / JUCE version / format
- **CMake** (`CMakeLists.txt`): `cmake_minimum_required(VERSION 3.15)`, `CXX_STANDARD 11`, Ninja generator, MSVC `/utf-8`, warnings 4100/4828 disabled, **static CRT** (`CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded...`) explicitly "to prevent missing dlls" (the `target_link_options` static block itself is commented out).
- `juce_add_plugin(SysVolFollower ... FORMATS VST VST3 PRODUCT_NAME "Sys Vol Follower" NEEDS_JUCE_HEADER TRUE)` and `target_compile_definitions(... JUCE_VST3_CAN_REPLACE_VST2=0)` — so it **does build a VST3**, which EAPO cannot load (see HARD CONSTRAINTS).
- Vendor/plugin codes: `PLUGIN_MANUFACTURER_CODE "MyHB"`, `PLUGIN_CODE "SysV"`.
- Includes `PluginEditor.cpp` in `target_sources` despite the comment claiming the editor is excluded, and links `ole32 oleaut32 uuid mmdevapi` for the COM volume code.
- Submodules (`.gitmodules`): `JUCE` → `juce-framework/JUCE` (branch `master`) and `vstsdk2` → `volperenato/vstsdk2.4`, with `juce_set_vst2_sdk_path("${CMAKE_CURRENT_SOURCE_DIR}/vstsdk2")`.
- **JUCE pinned commit `29396c22c93392d6738e021b83196283d6e4d850` = "JUCE version 8.0.12", dated 2025-12-16** — https://github.com/juce-framework/JUCE/commit/29396c22c93392d6738e021b83196283d6e4d850
- Built with VS 2026 BuildTools, x64 dev shell (`.vscode/settings.json`).

### Verification status of this repo
No third-party reports exist: 0 issues, 0 forks, 0 stars, no releases, 2 commits, and no README/blog/forum reference found. It has never been independently exercised, and the README's COAX claim is contradicted by its own source. **Treat as an unvalidated sketch, not a working reference.**

---

## PITFALLS

- **Editor crashes on "Open panel" when the plugin has no editor.** `VSTPluginInstance::startEditing()` uses `rect` from `effEditGetRect` without checking it was written. Open, unfixed since 2020 — https://sourceforge.net/p/equalizerapo/tickets/138/ , https://github.com/mirror/equalizerapo/issues/2 . ⇒ if you ship a plugin with a parameter-only "editor", make sure `effEditGetRect` always fills a valid `VstRect`.
- **Plugins that crash the Configuration Editor on load.** soothe2 VST2: editor crashes and the log shows `VSTPluginFilterFactory.cpp:91: File ??? not found` (EAPO logs `%s` for a `wchar_t*` path — a logging bug that also hides the real diagnostics) — https://sourceforge.net/p/equalizerapo/tickets/221/
- **Plugins that visibly work but do nothing audibly (the #1 failure mode).** GoodHertz CanOpener: the analysis panel shows a response change, but there is no audible effect, and "Open panel" kills the Config Editor. Diagnostic from a contributor: "the VST is not actually being applied to the audio signal, it might be because that VST does not expect to be run inside the Windows Audio Service and is misbehaving as a result." — https://sourceforge.net/p/equalizerapo/tickets/263/
- **License/activation and 32-bit traps.** Klevgrand Brusfri: loads, UI opens, never processes. Cause per the forum moderator: it is a 32-bit plugin *and/or* the audio-side instance cannot reach the license file, because it runs as LocalService inside audiodg — https://sourceforge.net/p/equalizerapo/tickets/302/
- **Plugin scanning / caching: there is none.** No scanner, no scan-on-install, no cache file, no plugin database anywhere in the tree; the Editor offers only a file dialog (`on_selectButton_clicked`) and libraries are cached per-process in a path-keyed `instanceMap` (`VSTPluginLibrary::getInstance`) — https://github.com/mirror/equalizerapo/blob/53d885f7/helpers/VSTPluginLibrary.cpp , https://github.com/mirror/equalizerapo/blob/53d885f7/Editor/guis/VSTPluginFilterGUI.cpp . So a plugin with slow `VSTPluginMain`/`effOpen` delays every config load and every audio re-lock, on the audio service.
- **Latency compensation does not exist.** EAPO reports zero latency to the audio engine and ignores `initialDelay`; the fork needed a custom patch to support it, and even then the reporter saw a 10-second delay with a 5-second lookahead buffer and concluded "delay compensation might only be possible with offline audio" — https://github.com/TheFireKahuna/equalizerAPO64/issues/14 , https://github.com/mirror/equalizerapo/blob/53d885f7/EqualizerAPO/EqualizerAPO.cpp
- **Sample-rate / format changes re-instantiate the plugin.** Changing the device format forces a new `LockForProcess` → new `engine.initialize(...)` → filter re-creation → plugin instances destroyed and rebuilt, so only chunk data survives — https://github.com/mirror/equalizerapo/blob/53d885f7/EqualizerAPO/EqualizerAPO.cpp , https://github.com/mirror/equalizerapo/blob/53d885f7/filters/VSTPluginFilter.cpp . EAPO's own config language encourages `If: sampleRate == 48000` branches, i.e. rate changes at runtime are expected — https://sourceforge.net/p/equalizerapo/wiki/Configuration%20reference/
- **Repeated APO failures get the whole APO stack disabled.** "if the failure count ... reaches a system-specified limit, the SFX, MFX and EFX APOs are disabled by setting the `PKEY_Endpoint_Disable_SysFx` registry key to '1'. The system-specified limit is currently a value of 10." Counted on `CoCreateInstance`/`IsInputFormatSupported`/`IsOutputFormatSupported`/`LockForProcess` failures — https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/implementing-audio-processing-objects
- **A plugin crash kills all system audio (temporarily).** Only audiodg.exe dies; AudioSrv restarts it — https://medium.com/@S.1.l.k.y/...
- **Windows updates / audio-driver reinstalls wipe EAPO's APO registration**, so the whole system silently reverts to no-EQ until re-configured — https://github.com/dechamps/APO/blob/master/README.md
- **Startup ordering / intermittent breakage on newer Windows.** A long-running report of `audiodg.exe` / `EqualizerAPO.dll` application errors, glitching and mic dropouts on Windows 10/11, with the observation that "The difference between both version is that 1.2.1 supports VST plugins" and that users reverted to 1.1.2 to fix issues — https://sourceforge.net/p/equalizerapo/discussion/general/thread/b4157fae51/ . Also documented instability when coexisting with the card driver's original APOs — https://sourceforge.net/p/equalizerapo/wiki/Documentation/
- **EAPO's built-in "analysis panel" is NOT a live spectrum tap.** `AnalysisThread::run()` builds its **own** `FilterEngine` in the Editor process, feeds a step/DC signal, and FFTs the impulse response with FFTW to derive the frequency response (`fftwf_plan_dft_r2c_1d`). It never sees audio from audiodg — https://github.com/mirror/equalizerapo/blob/53d885f7/Editor/AnalysisThread.cpp . So it cannot be repurposed to feed your display.
- **No reports exist about this specific repository** — it is effectively unused (see above).

---

## UNCERTAINTIES

1. **Exact protection level of audiodg.exe.** Microsoft says only "protected process" (https://learn.microsoft.com/en-us/windows/win32/coreaudio/user-mode-audio-components). Whether it is a full Protected Process (PP) or Protected Process Light (PPL), and which specific APIs are consequently blocked, I could **not** source. The `DisableProtectedAudioDG` mitigation and the observed CI page-hash failures are consistent with a protected process, but the level is unproven from primary sources.
2. **`DisableProtectedAudioDG` is a prerequisite.** Every source I found (including EAPO's own dev docs and an OSR thread) describes development/usage with this mitigation disabled. I found **no source** describing a signed, officially-installable EAPO VST chain with the mitigation *enabled* — so the shipping story for a commercial plugin on a stock Windows box is unverified and may be impossible without WHQL/Protected-Environment signing of the whole APO.
3. **Networking from inside audiodg.** No primary source. My "do not" recommendation rests on the `LocalServiceNetworkRestricted` + filtered-token claims in a secondary security article (https://medium.com/@S.1.l.k.y/...) and is therefore **inference, not sourced fact**.
4. **Shared-memory (file mapping) from inside audiodg.** No EAPO precedent and no source found. The claim that the token operates with "constrained object namespaces" (which would affect naming/session visibility) is secondary-only.
5. **Named-pipe details.** EAPO's pipe is written *from* audiodg (client) *to* a user-process server, and only at `Initialize()`, best-effort. Whether a *long-lived* duplex pipe created/opened from the audio-side plugin during `processBlock` is viable is untested by any source I found. Also note EAPO's client-side open can simply fail (it logs and gives up).
6. **`u32MaxFrameCount` values.** EAPO passes the host's value through; I found no documentation of typical magnitudes. The engine period is ~10 ms at 48 kHz, but **that is my inference, not sourced**.
7. **Whether a JUCE `MessageManager`/`Timer` actually pumps inside audiodg.** The reference repo assumes it does (it calls `startTimer` on the processor) but no source confirms that a message loop exists on the thread that instantiates audio-side plugin instances. Assume it does not.
8. **EAPO 1.4.1 / 1.4.2 VST changes.** The readable mirror ends at 1.4.0 (2024-09-27); 1.4.2 shipped 2025-03-21 with no changelog I could locate. Any VST-related fix in 1.4.1/1.4.2 is **unresearched**.
9. **`VSTPlugin` is officially undocumented.** The official Configuration reference (last updated 2019-06-10) omits it entirely; the syntax I give is derived from source, not from documentation.
10. **"Open panel" vs "embedded" reliability.** The embedded path (`ui->frame->winId()` + `setFixedSize`) and the dialog path behave differently across plugins per tickets 138/263/221; I could not establish a rule for which plugins work.
11. **The reference repo has zero validation.** Everything in the REFERENCE REPO IPC section describes what the source *says*; nothing in it has been reported working by a third party, and its README contradicts its own implementation.
