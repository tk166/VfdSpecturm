# reload_audio.ps1 —— 强制 audiodg 重新加载 APO / 插件 DLL（需要管理员）
#
# 为什么需要它（2026-09-28 实测教训）：
#   在 EAPO 编辑器里"关掉再打开 VSTPlugin 滤波器"**不会**让 audiodg 卸载我们的 DLL：
#   EAPO 的 VSTPluginLibrary 是个进程内静态缓存（helpers/VSTPluginLibrary.cpp 里的
#   instanceMap，weak_ptr），而 audiodg 侧的滤镜链/模块在设备音频管线重建之前一直驻留。
#   结果就是"换了 DLL 但面板上跑的还是旧的"——我们因此误判过一次"渲染要 4 ms"。
#
#   真正能让 audiodg 换镜像的只有"让音频引擎重建管线"：
#     1) 结束 audiodg.exe（AudioSrv 会自动把它拉起来）← 本脚本的默认做法，最快
#     2) Restart-Service Audiosrv -Force（下面 --service 开关）
#     3) 在设备管理器里禁用/启用音频端点，或者重启系统
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File tools\reload_audio.ps1
#   powershell -ExecutionPolicy Bypass -File tools\reload_audio.ps1 -Service
#
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [switch]$Service,                       # 改用重启音频服务（更彻底，音频中断稍久）
    [string]$Dll = "$PSScriptRoot\..\build\plugin\vfd_eapo_artefacts\Release\VST\VfdSpectrumSender.dll"
)

$ErrorActionPreference = 'Stop'

function Show-State([string]$title) {
    Write-Host "--- $title ---"
    $p = Get-Process -Name audiodg -ErrorAction SilentlyContinue
    if (-not $p) { Write-Host "  audiodg: 未运行"; return }
    Write-Host ("  audiodg: pid={0}  start={1}" -f $p.Id, $p.StartTime)
    try {
        $mods = $p.Modules | Where-Object { $_.ModuleName -like '*Vfd*' }
        if ($mods) {
            foreach ($m in $mods) {
                Write-Host ("  mapped:  {0}  image={1:N0} KB  path={2}" -f `
                    $m.ModuleName, ($m.ModuleMemorySize / 1KB), $m.FileName)
            }
        } else {
            Write-Host "  mapped:  未看到 Vfd* 模块（插件此刻可能没在链上）"
        }
    } catch {
        Write-Host "  mapped:  读不到模块列表（受保护进程，通常需要管理员）"
    }
}

if (Test-Path $Dll) {
    $f = Get-Item $Dll
    Write-Host ("disk DLL: {0}  {1:N0} bytes  {2}" -f $f.Name, $f.Length, $f.LastWriteTime)
} else {
    Write-Host "disk DLL: 找不到 $Dll（先跑 tools\build.bat plugin）"
}

Show-State 'BEFORE'

if ($Service) {
    Write-Host "restarting service Audiosrv ..."
    Restart-Service -Name Audiosrv -Force
} else {
    Write-Host "killing audiodg.exe (AudioSrv will respawn it) ..."
    Stop-Process -Name audiodg -Force
}

# 等新的 audiodg 起来
$deadline = (Get-Date).AddSeconds(15)
do {
    Start-Sleep -Milliseconds 300
    $new = Get-Process -Name audiodg -ErrorAction SilentlyContinue
} until (($new -and $new.StartTime -gt (Get-Date).AddSeconds(-20)) -or (Get-Date) -gt $deadline)

Show-State 'AFTER'
Write-Host ""
Write-Host "下一步（重要）："
Write-Host "  * EAPO 面板要 关掉再打开 —— 编辑器进程里的实例是 打开面板时才 new 出来的，"
Write-Host "    所以面板窗口不关，它就一直用着打开那一刻加载的旧镜像；"
Write-Host "  * 面板状态栏现在带 build 戳（例: v3 build Sep 28 2026 10:18:39），"
Write-Host "    它和上面 disk DLL 的时间对得上，才说明真的换了新镜像。"
