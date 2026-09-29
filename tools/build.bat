rem Build the VFD spectrum tools / plugin with the VS 2026 BuildTools toolchain.
rem Usage:  tools\build.bat          -> build the pipe transport tools
rem         tools\build.bat plugin   -> also build the JUCE VST2 plugin
rem
rem Why this script looks like this (every item below bit us on this machine):
rem  1. PATHEXT is broken in this environment (".CPL" only), so cmake/where are not
rem     resolvable by name -> set PATHEXT explicitly and use ABSOLUTE tool paths.
rem  2. cmd.exe requires CRLF line endings; this file is written with CRLF on purpose.
rem  3. NEVER name a variable CL / LINK / _CL_: those are MSVC default-option env vars
rem     and cl.exe will try to compile itself (symptom: "cl.exe(1): error C2143").
rem  4. Start-Process -Wait plus redirected output hangs while cl.exe telemetry child
rem     (vctip.exe) keeps the handle open -> poll for exit instead of using -Wait.
rem  5. rc.exe / mt.exe are not on PATH here -> passed to CMake explicitly.
rem  6. cmd comments must use rem, not # (a # line gets executed).
rem
@echo off
setlocal
set "PATHEXT=.COM;.EXE;.BAT;.CMD"

set "VCVARS=F:\Distros\VisualStudio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "MSVC_DIR=F:/Distros/VisualStudio/18/BuildTools/VC/Tools/MSVC/14.50.35717/bin/Hostx64/x64"
set "SDK_BIN=C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
set "CMAKE_EXE=C:\Program Files\CMake\bin\cmake.exe"
set "NINJA_EXE=F:/Distros/VisualStudio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"

if not exist "%VCVARS%" ( echo [ERROR] vcvars64.bat not found: %VCVARS% & exit /b 1 )
call "%VCVARS%" >nul 2>&1

cd /d "%~dp0.."

set "BUILD_FLAGS=-S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" -DCMAKE_BUILD_TYPE=Release"
set "BUILD_FLAGS=%BUILD_FLAGS% -DCMAKE_CXX_COMPILER="%MSVC_DIR%/cl.exe" -DCMAKE_LINKER="%MSVC_DIR%/link.exe""
set "BUILD_FLAGS=%BUILD_FLAGS% -DCMAKE_RC_COMPILER="%SDK_BIN%/rc.exe" -DCMAKE_MT="%SDK_BIN%/mt.exe""

if "%1"=="plugin" set "BUILD_FLAGS=%BUILD_FLAGS% -DVFD_BUILD_PLUGIN=ON"

echo === configure ===
"%CMAKE_EXE%" %BUILD_FLAGS%
if errorlevel 1 ( echo CONFIGURE_FAILED & exit /b 1 )

rem 第二个参数 = 只构建某个目标 (例如 render_bench)。用在"插件 DLL 正被 audiodg /
rem EAPO 编辑器占用、不想让整条构建失败"的场合。
set "BUILD_TARGET="
if not "%2"=="" set "BUILD_TARGET=--target %2"

echo === build ===
"%CMAKE_EXE%" --build build %BUILD_TARGET%
if errorlevel 1 ( echo BUILD_FAILED & exit /b 1 )

echo === artifacts ===
if exist build\pipe_server.exe   echo   build\pipe_server.exe
if exist build\pipe_producer.exe echo   build\pipe_producer.exe
if exist build\plugin\vfd_eapo_artefacts\Release\VST\*.dll (
  echo   VST2 plugin:
  dir /b build\plugin\vfd_eapo_artefacts\Release\VST\*.dll
)
echo BUILD_OK
endlocal
