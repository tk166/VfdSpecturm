# cross_session_vst_test.ps1 -- load the VST2 plugin DLL from SESSION 0 as a service account.
#
# WHY: the plugin runs inside audiodg.exe = LocalService + session 0, where the DLL search
# path, the already-loaded CRT, the account and the desktop all differ from an interactive
# shell. Our first /MD build picked up EAPO's older MSVCP140 there, faulted during init, and
# EAPO then divided by zero -> audiodg died. This script reproduces the account/session half
# of that environment BEFORE touching the real audio chain.
#
# MUST BE RUN FROM AN ELEVATED POWERSHELL.
#
# PASS = the stub log shows "after effOpen: numInputs=..." AND the pipe server got frames.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\cross_session_vst_test.ps1 [-Seconds 5]
#
# NOTE: this file is deliberately ASCII-ONLY. Windows PowerShell 5.1 reads BOM-less files as
# ANSI: non-ASCII comment bytes then decode into garbage that can swallow the NEXT line
# (symptom: variables silently stay $null). Same rule as our .bat files.

param([int]$Seconds = 5)

$ErrorActionPreference = 'Continue'
$root   = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$server = Join-Path $root 'build\pipe_server.exe'
$stub   = Join-Path $root 'build\vst2_host_stub.exe'
$dll    = Join-Path $root 'build\plugin\vfd_eapo_artefacts\Release\VST\VfdSpectrumSender.dll'
$taskName = 'VfdVstCrossSessionTest'

# Some shells here have these environment variables empty, so fall back explicitly.
$programData = $env:ProgramData
if ([string]::IsNullOrEmpty($programData)) { $programData = 'C:\ProgramData' }
$systemRoot = $env:SystemRoot
if ([string]::IsNullOrEmpty($systemRoot)) { $systemRoot = 'C:\Windows' }
$tempDir = $env:TEMP
if ([string]::IsNullOrEmpty($tempDir)) { $tempDir = 'C:\Windows\Temp' }

$work      = Join-Path $programData 'vfd_xsession'   # service accounts can write here
$serverLog = Join-Path $tempDir 'vfd_xsession_server.log'
$stubLog   = Join-Path $work 'stub.log'

# Always call system tools by absolute path: name resolution is unreliable on this machine.
$schtasks = Join-Path $systemRoot 'System32\schtasks.exe'
$icacls   = Join-Path $systemRoot 'System32\icacls.exe'
foreach ($t in @($schtasks, $icacls)) {
    if (-not (Test-Path -LiteralPath $t)) {
        Write-Host "[FAIL] required tool missing: $t" -ForegroundColor Red
        exit 1
    }
}

$isAdmin = (New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "[FAIL] This script must run elevated (creating the session-0 task needs admin)." -ForegroundColor Red
    exit 1
}
foreach ($f in @($server, $stub, $dll)) {
    if (-not (Test-Path -LiteralPath $f)) {
        Write-Host "[FAIL] missing $f -- run tools\build.bat plugin first" -ForegroundColor Red
        exit 1
    }
}

Get-Process -Name pipe_server, pipe_producer, vst2_host_stub -ErrorAction SilentlyContinue | Stop-Process -Force
& $schtasks /delete /tn $taskName /f 2>$null | Out-Null
New-Item -ItemType Directory -Force -Path $work | Out-Null
Remove-Item $stubLog -ErrorAction SilentlyContinue

# The service account must be able to read the exe/dll and write its log.
Write-Host "[0/3] granting access to LOCAL SERVICE / SYSTEM ..."
& $icacls (Join-Path $root 'build') /grant '*S-1-5-19:(OI)(CI)RX' /T /Q 2>&1 | Out-Null
& $icacls (Join-Path $root 'build') /grant '*S-1-5-18:(OI)(CI)RX' /T /Q 2>&1 | Out-Null
& $icacls $work /grant '*S-1-5-19:(OI)(CI)M' /T /Q 2>&1 | Out-Null
& $icacls $work /grant '*S-1-5-18:(OI)(CI)M' /T /Q 2>&1 | Out-Null

# session-0 side: the stub loads the plugin DLL exactly like EAPO does and writes a log.
# We register a small wrapper .cmd instead of putting the command into /tr: schtasks is very
# picky about nested quotes (that is what broke the first attempt). The wrapper lives at a
# space-free path on purpose.
$wrapper = Join-Path $work 'run.cmd'
$wrapperBody = @(
    '@echo off',
    ('"{0}" "{1}" {2} --log "{3}"' -f $stub, $dll, $Seconds, $stubLog)
) -join "`r`n"
[System.IO.File]::WriteAllText($wrapper, $wrapperBody + "`r`n",
                               (New-Object System.Text.ASCIIEncoding))
Write-Host "[0b/3] wrapper: $wrapper"

$created = $false
foreach ($account in @('NT AUTHORITY\LocalService', 'NT AUTHORITY\SYSTEM')) {
    $out = & $schtasks /create /tn $taskName /tr $wrapper /sc once /st 23:59 /ru $account /f 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "[ok] temp task created as $account"
        $created = $true
        break
    }
    Write-Host "[warn] schtasks failed for $account (exit $LASTEXITCODE):"
    $out | Select-Object -Last 3 | ForEach-Object { Write-Host "       $_" }
}
if (-not $created) { Write-Host "[FAIL] could not create the scheduled task" -ForegroundColor Red; exit 1 }

Write-Host "[1/3] starting pipe server here (session 1) for $($Seconds + 6)s ..."
Remove-Item $serverLog -ErrorAction SilentlyContinue
$srv = Start-Process -FilePath $server -ArgumentList @($Seconds + 6) -NoNewWindow -PassThru `
                     -RedirectStandardOutput $serverLog
Start-Sleep -Milliseconds 1200

Write-Host "[2/3] loading the plugin DLL from session 0 ..."
& $schtasks /run /tn $taskName | Out-Null
Start-Sleep -Seconds ($Seconds + 3)

Write-Host "[3/3] collecting ..."
$srv.WaitForExit(20000) | Out-Null
if (-not $srv.HasExited) { $srv.Kill() }
& $schtasks /delete /tn $taskName /f 2>$null | Out-Null
Get-Process -Name pipe_server, vst2_host_stub -ErrorAction SilentlyContinue | Stop-Process -Force

Write-Host ""
Write-Host "===== plugin host log (session 0) ====="
$stubText = Get-Content $stubLog -ErrorAction SilentlyContinue
$stubText
Write-Host ""
Write-Host "===== pipe server (session 1) ====="
$srvText = Get-Content $serverLog -ErrorAction SilentlyContinue
$srvText
Write-Host ""
Write-Host "===== verdict ====="
$sText = ($stubText -join "`n")
$vText = ($srvText -join "`n")
$loaded = $sText -match 'after effOpen: numInputs=(\d+) numOutputs=(\d+)'
$frames = -1
if ($vText -match 'TOTAL frames=(\d+)') { $frames = [int]$Matches[1] }

if ($loaded -and $frames -gt 0) {
    Write-Host "[PASS] the plugin initialised under a service account in session 0 and streamed $frames frames." -ForegroundColor Green
    Write-Host "       => the /MT build survives the audiodg-like environment. Safe to try in EAPO."
    $rc = 0
} elseif ($loaded) {
    Write-Host "[PARTIAL] the plugin initialised in session 0 but no frames arrived." -ForegroundColor Yellow
    $rc = 3
} else {
    Write-Host "[FAIL] the plugin did NOT initialise in session 0 -- do NOT put it into EAPO yet." -ForegroundColor Red
    Write-Host "       (see the session-0 log above; this is the failure mode that killed audiodg)"
    $rc = 2
}

& $icacls (Join-Path $root 'build') /remove:g '*S-1-5-19' '*S-1-5-18' /T /Q 2>&1 | Out-Null
& $icacls $work /remove:g '*S-1-5-19' '*S-1-5-18' /T /Q 2>&1 | Out-Null
exit $rc
