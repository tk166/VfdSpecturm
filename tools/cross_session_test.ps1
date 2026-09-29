# cross_session_test.ps1 -- THE decisive IPC test for the EAPO plugin route.
#
# WHY: the failure mode we care about (audiodg lives in session 0, the viewer lives in
# the interactive session 1) CANNOT be reproduced by two console apps in the same
# session, nor by hosting the plugin in REAPER. Both of those pass while the real
# deployment fails. This script reproduces the session split without EAPO:
#
#   * pipe SERVER (consumer) runs here, in the interactive session 1
#   * pipe CLIENT (producer, = what the plugin will do) runs in SESSION 0 as a service
#     account, via a temporary scheduled task
#
# MUST BE RUN FROM AN ELEVATED POWERSHELL (creating a service-account task needs admin).
#
# PASS = the server log reports frames > 0 (session 0 -> session 1 pipe write worked).
# FAIL = "no client connected before deadline" (cross-session pipe is blocked).
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\cross_session_test.ps1 [-Seconds 8]

param([int]$Seconds = 8)

$ErrorActionPreference = 'Continue'
$root   = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$server = Join-Path $root 'build\pipe_server.exe'
$client = Join-Path $root 'build\pipe_producer.exe'
$taskName = 'VfdPipeCrossSessionTest'
$logDir = Join-Path $env:TEMP 'vfd_cross_session'
$serverLog = Join-Path $logDir 'server.log'
$taskLog   = Join-Path $logDir 'client.log'

New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$isAdmin = (New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "[FAIL] This script must run elevated (creating the session-0 task needs admin)." -ForegroundColor Red
    exit 1
}
foreach ($exe in @($server, $client)) {
    if (-not (Test-Path $exe)) { Write-Host "[FAIL] missing $exe -- run tools\build.bat first" -ForegroundColor Red; exit 1 }
}

# make sure nothing is left over from a previous run
Get-Process -Name pipe_server, pipe_producer -ErrorAction SilentlyContinue | Stop-Process -Force
schtasks /delete /tn $taskName /f 2>$null | Out-Null

# The task runs the PRODUCER (client) as a service account => session 0.
# The service account must be able to READ the exe first; otherwise the test would fail
# for an ACL reason instead of a pipe reason. This mirrors EAPO's own check
# ("The library is not readable by the audio service.").
# S-1-5-19 = LOCAL SERVICE, S-1-5-18 = SYSTEM (SIDs avoid localization issues).
$buildDir = Join-Path $root 'build'
Write-Host "[0/3] granting read+execute on $buildDir to LOCAL SERVICE / SYSTEM ..."
icacls $buildDir /grant '*S-1-5-19:(OI)(CI)RX' /T /Q 2>&1 | Out-Null
icacls $buildDir /grant '*S-1-5-18:(OI)(CI)RX' /T /Q 2>&1 | Out-Null

$clientCmd = '"{0}" {1} 20' -f $client, $Seconds
$created = $false
foreach ($account in @('NT AUTHORITY\LocalService', 'NT AUTHORITY\SYSTEM')) {
    schtasks /create /tn $taskName /tr $clientCmd /sc once /st 23:59 /ru $account /f 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { Write-Host "[ok] temp task created as $account"; $created = $true; break }
    Write-Host "[warn] could not create task as $account"
}
if (-not $created) { Write-Host "[FAIL] could not create the scheduled task" -ForegroundColor Red; exit 1 }

Write-Host "[1/3] starting pipe server in this (session 1) shell for $($Seconds + 6)s ..."
Remove-Item $serverLog -ErrorAction SilentlyContinue
$srv = Start-Process -FilePath $server -ArgumentList @($Seconds + 6) -NoNewWindow -PassThru `
                     -RedirectStandardOutput $serverLog
Start-Sleep -Milliseconds 1200

Write-Host "[2/3] launching producer inside session 0 via the task ..."
schtasks /run /tn $taskName | Out-Null

Write-Host "[3/3] waiting ..."
Start-Sleep -Seconds ($Seconds + 3)
$srv.WaitForExit(20000) | Out-Null
if (-not $srv.HasExited) { $srv.Kill() }

schtasks /delete /tn $taskName /f 2>$null | Out-Null
Get-Process -Name pipe_server, pipe_producer -ErrorAction SilentlyContinue | Stop-Process -Force
icacls $buildDir /remove:g '*S-1-5-19' '*S-1-5-18' /T /Q 2>&1 | Out-Null   # undo the ACL grant

Write-Host ""
Write-Host "===== server (session 1, consumer) ====="
$serverText = Get-Content $serverLog -ErrorAction SilentlyContinue
$serverText
Write-Host ""
Write-Host "===== verdict ====="

# NOTE: $serverText is an ARRAY. `-match` on an array filters elements and does NOT
# populate $Matches (that was a bug in the first version of this script) -> join first.
$text = ($serverText -join "`n")
$frames = -1
if ($text -match 'TOTAL frames=(\d+)') { $frames = [int]$Matches[1] }

if ($text -match 'VERDICT: server_session=(\d+) producer_session=(\d+) -> (.+)') {
    Write-Host ("       server_session={0}  producer_session={1}  -> {2}" -f `
                $Matches[1], $Matches[2], $Matches[3].Trim())
}

if ($frames -gt 0 -and $text -match 'CROSS-SESSION OK') {
    Write-Host "[PASS] session 0 -> session 1 named pipe works ($frames frames, 0 gaps expected)." -ForegroundColor Green
    Write-Host "       => the audiodg(plugin) -> viewer transport is VIABLE."
    exit 0
}
if ($frames -gt 0) {
    Write-Host "[INCONCLUSIVE] frames arrived but both sides report the same session." -ForegroundColor Yellow
    Write-Host "       (The scheduled task did not actually switch sessions on this machine.)"
    exit 3
}
Write-Host "[FAIL] no frames arrived: cross-session named pipe did NOT work." -ForegroundColor Red
Write-Host "       => fall back to: let the plugin CREATE the pipe and publish its name in"
Write-Host "          HKLM (EAPO's DeviceTest publishes the name from the other side),"
Write-Host "          or test a Global\\ shared-memory object created by the plugin side."
exit 2
