@echo off
rem ===========================================================================
rem  VFD Spectrum Analyzer 启动脚本
rem  使用 conda audio 环境运行(含 numpy / soundcard / dearpygui)
rem ===========================================================================
setlocal

set "PY=%USERPROFILE%\miniforge3\envs\audio\python.exe"
if not exist "%PY%" set "PY=%CONDA_PREFIX%\python.exe"

if not exist "%PY%" (
    echo [错误] 找不到 Python 解释器。
    echo        期望路径: %USERPROFILE%\miniforge3\envs\audio\python.exe
    echo        请先创建环境:
    echo            conda create -n audio python=3.12 -y
    echo            conda run -n audio pip install -r requirements.txt -i https://pypi.org/simple
    pause
    exit /b 1
)

cd /d "%~dp0"
"%PY%" vfd_spectrum.py %*
if errorlevel 1 pause
endlocal
