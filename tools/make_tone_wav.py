# -*- coding: utf-8 -*-
"""make_tone_wav.py —— 生成一根"绝对干净"的参考正弦 WAV（诊断用）

用法（audio 环境或任何有标准库的 Python 3）:
    python tools\\make_tone_wav.py build\\clean_440.wav --freq 440 --amp 0.5 --secs 30

为什么要它: 当频谱上出现"裙边/抖动的旁瓣"时，需要一根**数学上干净**的参照音把
"音源问题"和"系统/分析链问题"切开。用网页发生器（WebAudio）时，音源常常自带
几十 dB 的宽带边带（抖动/重触发/低质重采样），而这是任何加窗、任何显示都治不了的。
把这根 WAV 用 foobar/REAPER/系统播放器放出来，对着同一个频谱看：
  * 干净 → 问题在你的发生器；
  * 也脏 → 问题在播放/采集链路（此时换 48kHz 设备模式、或换 ASIO/独占模式再试）。
"""
from __future__ import annotations

import argparse
import math
import os
import struct
import wave

import numpy as np


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--freq", type=float, default=440.0)
    ap.add_argument("--amp", type=float, default=0.5, help="0.5 = -6 dBFS")
    ap.add_argument("--rate", type=int, default=192000)
    ap.add_argument("--secs", type=float, default=30.0)
    ap.add_argument("--bits", type=int, default=24)
    ap.add_argument("--channels", type=int, default=2)
    args = ap.parse_args()

    n = int(args.rate * args.secs)
    t = np.arange(n, dtype=np.float64) / args.rate
    x = args.amp * np.sin(2.0 * math.pi * args.freq * t)
    # 头尾各 20ms 淡入淡出，避免播放起停的爆音（不引入任何带内失真）
    fade = int(0.02 * args.rate)
    if fade > 0:
        r = np.linspace(0.0, 1.0, fade)
        x[:fade] *= r
        x[-fade:] *= r[::-1]

    if args.bits == 24:
        q = np.clip(np.round(x * (2 ** 23 - 1)), -(2 ** 23), 2 ** 23 - 1).astype(np.int32)
        raw = q.astype("<i4").tobytes()
        frames = b"".join(raw[i * 4:i * 4 + 3] for i in range(q.size))
    elif args.bits == 16:
        q = np.clip(np.round(x * 32767.0), -32768, 32767).astype("<i2")
        frames = q.tobytes()
    else:
        raise SystemExit("只支持 16/24 bit")

    if args.channels == 2:
        inter = bytearray()
        step = len(frames) // n
        for i in range(n):
            s = frames[i * step:(i + 1) * step]
            inter += s + s
        frames = bytes(inter)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    with wave.open(args.out, "wb") as w:
        w.setnchannels(args.channels)
        w.setsampwidth(args.bits // 8)
        w.setframerate(args.rate)
        w.writeframes(frames)
    print("写出 %s: %.0f Hz, %.2f dBFS, %d Hz, %d bit, %dch, %.1f s"
          % (args.out, args.freq, 20 * math.log10(args.amp), args.rate, args.bits,
             args.channels, args.secs))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
