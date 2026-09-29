#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
测试音源 —— 通过默认输出设备播放一段"频谱内容很丰富"的信号。

因为频谱仪采集的是 **系统输出回环**,播放这个信号就能直接在屏幕上看到效果
(不用真的去放音乐)。

用法
----
    python tools/play_test_tone.py                     # 循环播放(扫频/和弦/粉噪)
    python tools/play_test_tone.py 30                  # 播 30 秒
    python tools/play_test_tone.py 30 0.3              # 30 秒,音量 0.3
    python tools/play_test_tone.py 12 --tone 1000      # 稳态 1kHz 正弦(标定用)
    python tools/play_test_tone.py 12 --tone 1000 --amp 0.5
"""

from __future__ import annotations

import ctypes
import sys
import time

import numpy as np


def build_pattern(sr: int) -> np.ndarray:
    """三段式: 对数扫频 -> 多音和弦 -> 粉噪,拼成约 12 秒的循环。"""
    rng = np.random.default_rng(20260927)
    parts = []

    # 1) 20Hz -> 20kHz 对数扫频 (6s)
    dur = 6.0
    n = int(sr * dur)
    t = np.arange(n) / sr
    k = np.log(20000.0 / 20.0) / dur
    phase = 2 * np.pi * 20.0 * (np.exp(k * t) - 1.0) / k
    sweep = 0.35 * np.sin(phase) * np.hanning(n)
    parts.append(sweep.astype(np.float32))

    # 2) 多音和弦 + 轻微颤音 (4s)
    dur = 4.0
    n = int(sr * dur)
    t = np.arange(n) / sr
    chord = np.zeros(n, dtype=np.float32)
    for f, a in ((55, 0.30), (110, 0.24), (220, 0.20), (440, 0.16),
                 (880, 0.12), (1760, 0.09), (3520, 0.06), (7040, 0.04)):
        vib = 1.0 + 0.004 * np.sin(2 * np.pi * 5.0 * t)
        chord += (a * np.sin(2 * np.pi * f * vib * t)).astype(np.float32)
    parts.append(chord)

    # 3) 粉噪(近似): 白噪 + 一阶低通迭代 (2s)
    dur = 2.0
    n = int(sr * dur)
    white = rng.normal(0, 1.0, n).astype(np.float32)
    pink = np.cumsum(white, dtype=np.float32)
    pink -= np.linspace(pink[0], pink[-1], n, dtype=np.float32)
    pink /= (np.abs(pink).max() + 1e-9)
    parts.append((0.30 * pink).astype(np.float32))

    sig = np.concatenate(parts)
    peak = float(np.abs(sig).max()) or 1.0
    return (sig / peak * 0.85).astype(np.float32)


def build_steady(sr: int, freq: float, amp: float, seconds: float) -> np.ndarray:
    """稳态正弦: 用来标定/检查显示(例如 1kHz 满量程)。"""
    n = max(1, int(sr * seconds))
    t = np.arange(n) / sr
    edge = min(n // 20, int(sr * 0.02))               # 20ms 淡入淡出,避免爆音
    wave = (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)
    if edge > 0:
        wave[:edge] *= np.linspace(0.0, 1.0, edge, dtype=np.float32)
        wave[-edge:] *= np.linspace(1.0, 0.0, edge, dtype=np.float32)
    return wave


def main() -> int:
    argv = sys.argv[1:]
    steady_freq = None
    if "--tone" in argv:
        i = argv.index("--tone")
        steady_freq = float(argv[i + 1])
        del argv[i:i + 2]
    amp = 0.85
    if "--amp" in argv:
        i = argv.index("--amp")
        amp = float(argv[i + 1])
        del argv[i:i + 2]
    seconds = float(argv[0]) if argv else 0.0
    volume = float(argv[1]) if len(argv) > 1 else 1.0

    import soundcard as sc
    ctypes.windll.ole32.CoInitializeEx(None, 0x0)   # 必须在 import 之后(见 README 说明)
    spk = sc.default_speaker()
    sr = 48000
    try:                                   # 尽量用设备原生采样率
        import winreg
        import struct
        guid = str(spk.id).rsplit(".", 1)[-1]
        key = (r"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
               r"\%s\Properties" % guid)
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key) as k:
            blob, _ = winreg.QueryValueEx(k, "{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0")
        off = bytes(blob).find(bytes.fromhex("0300000000001000800000aa00389b71"))
        if off < 24:
            off = bytes(blob).find(bytes.fromhex("0100000000001000800000aa00389b71"))
        base = off - 24
        rate = struct.unpack_from("<I", blob, base + 4)[0]
        if 8000 <= rate <= 768000:
            sr = int(rate)
    except Exception:                       # noqa: BLE001
        pass

    signal = None
    if steady_freq is not None:
        signal = build_steady(sr, steady_freq, amp, seconds if seconds > 0 else 10.0)
        print("模式     : 稳态正弦 %.1f Hz, 幅度 %.2f (%.1f dBFS)"
              % (steady_freq, amp, 20 * np.log10(max(amp, 1e-9))))
    else:
        pattern = build_pattern(sr) * max(0.0, min(1.0, volume))
        print("模式     : 循环 扫频 / 和弦 / 粉噪")
        print("循环长度 : %.1f 秒" % (len(pattern) / sr))
    print("输出设备 : %s" % spk.name)
    print("采样率   : %d Hz" % sr)
    print("按 Ctrl+C 停止。\n")

    t_end = time.monotonic() + seconds if seconds > 0 else None
    with spk.player(samplerate=sr, channels=2, blocksize=8192) as player:
        while t_end is None or time.monotonic() < t_end:
            if signal is not None:
                player.play(signal)
                break
            player.play(pattern)
    print("播放结束。")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n已停止。")
