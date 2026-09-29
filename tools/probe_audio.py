# -*- coding: utf-8 -*-
"""probe_audio.py —— 抓一段回环音频（或自己放一根干净正弦再抓），直接算频谱

用法（audio 环境）:
    python tools\\probe_audio.py 0.7                      # 抓当前正在播放的声音
    python tools\\probe_audio.py 0.7 --play 102           # 自己放 102Hz 正弦，同时抓回环
    python tools\\probe_audio.py 0.7 --play 102 --amp 0.5 --rate 192000

为什么要它: 屏幕上的频谱 = 信号 × 我们的分析链 × 渲染。怀疑"泄漏"时必须先把**原始信号**
的频谱单独打出来，否则永远在猜是窗的问题、还是信号里本来就有谐波/噪声/削波/胶水音。

`--play` 是最狠的对照：自己生成一根数学上绝对干净的正弦，从默认设备播出去（会经过
EAPO 与我们的插件），同时抓回环 —— 于是浏览器/网页音源被排除在外。若这样抓回来的
频谱都是脏的，那就是 OS/APO/回环这一段；若是干净的，问题在源或者在我们自己。
"""
from __future__ import annotations

import argparse
import os
import sys
import threading
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from vfd_spectrum import make_window, read_device_format  # noqa: E402

PROBE_HZ = [20, 30, 50, 70, 102, 150, 200, 300, 500, 1000, 2000, 5000, 10000, 15000]


def spectrum_report(tag: str, mono: np.ndarray, rate: float, n: int, window: str) -> None:
    """按我们关心的几个口径打印一段单声道的频谱。"""
    if len(mono) < 64:
        print("[%s] 数据太少" % tag)
        return
    while n > len(mono):
        n >>= 1
    seg = mono[len(mono) - n:]
    w = make_window(window, n).astype(np.float64)
    spec = np.abs(np.fft.rfft(seg * w)) * (2.0 / w.sum())
    freqs = np.fft.rfftfreq(n, 1.0 / rate)
    db = 20.0 * np.log10(np.maximum(spec, 1e-12))
    peak = float(np.abs(seg).max())
    rms = float(np.sqrt(np.mean(seg ** 2)))
    print("[%s] N=%d bin=%.3f Hz  峰值=%.4f(%.2f dBFS) RMS=%.4f(%.1f dBFS) 波峰因数=%.1f dB "
          "削波样本=%d"
          % (tag, n, rate / n, peak, 20 * np.log10(max(peak, 1e-12)), rms,
             20 * np.log10(max(rms, 1e-12)), 20 * np.log10(max(peak, 1e-12) / max(rms, 1e-12)),
             int((np.abs(seg) >= 0.9995).sum())))
    pts = []
    for f in PROBE_HZ:
        if f >= rate * 0.45:
            continue
        k = int(round(f / (rate / n)))
        pts.append("%.0f:%.1f" % (f, float(db[max(0, k - 2):k + 3].max())))
    print("  指定点(dBFS): " + "  ".join(pts))
    top = np.argsort(db)[::-1][:12]
    print("  最强 12 根 : " + "  ".join("%.1fHz:%.1f" % (freqs[i], db[i]) for i in sorted(top)))
    edges = np.geomspace(20.0, min(20000.0, rate * 0.45), 11)
    bands = []
    for a, b in zip(edges[:-1], edges[1:]):
        m = (freqs >= a) & (freqs < b)
        if m.any():
            bands.append("%.0f-%.0f:%.1f"
                         % (a, b, 10 * np.log10(max(float(np.mean(spec[m] ** 2)), 1e-24))))
    print("  对数带平均 : " + "  ".join(bands))


def steps_report(mono: np.ndarray, rate: float, n: int) -> None:
    """找"不连续点"：把实测最大相邻差与**该正弦的理论最大斜率**比。

    一个 f0 的正弦，相邻样本差上限 = 2*pi*f0/fs*amp。若实测远大于它，就说明流里有跳变
    （丢样本/重复样本/缓冲区接缝）。跳变幅度换算成相对音量的 dB 之后，可以直接判断
    "该不该听得见"：-40dB 以上会炒豆，-50dB 以下通常被强音掩蔽 —— 但 FFT 积分后照样
    变成整屏可见的裙边，而且随窗滑动忽大忽小。
    """
    if len(mono) < n:
        return
    w = make_window("bh", n).astype(np.float64)
    spec = np.abs(np.fft.rfft(mono[-n:] * w)) * (2.0 / w.sum())
    k = int(np.argmax(spec))
    f0 = k * rate / n
    amp = float(np.abs(mono).max())
    d = np.diff(mono)
    ad = np.abs(d)
    expect = 2.0 * np.pi * f0 / rate * amp          # 该正弦的理论最大斜率
    hits = np.nonzero(ad > 2.0 * max(expect, 1e-9))[0]
    print("[steps] 主频 %.1f Hz, 幅度 %.3f；理论最大相邻差 %.2e，实测最大 %.2e (%.1f 倍)"
          % (f0, amp, expect, float(ad.max()), float(ad.max() / max(expect, 1e-12))))
    print("[steps] 超过 2 倍理论斜率的位置: %d 处 / %d 样本" % (hits.size, len(d)))
    if hits.size == 0:
        print("[steps] 没找到跳变 —— 这个流是干净的")
        return
    db_of = 20.0 * np.log10(max(amp, 1e-12))
    times = hits / rate
    print("[steps] 前 12 处（幅度换算成相对满刻度 dB）:")
    for s in hits[:12]:
        print("   t=%7.3f s  跳变 %+.5f  = %.1f dBFS   间隔 %s"
              % (times[s], d[s], 20.0 * np.log10(max(abs(float(d[s])), 1e-12)),
                 ("%.3f s" % (times[s] - times[hits[max(0, np.nonzero(hits == s)[0][0] - 1)]]
                              if s != hits[0] else 0.0))))
    iv = np.diff(times)
    if iv.size:
        print("[steps] 间隔: 中位 %.3f s, 最小 %.3f, 最大 %.3f（若等间隔 ≈ 周期性 glitch）"
              % (float(np.median(iv)), float(iv.min()), float(iv.max())))
    big = 20.0 * np.log10(max(float(ad[hits].max()), 1e-12))
    print("[steps] 最大跳变 %.1f dBFS（-40dB 以上会炒豆；以下多半被强音掩蔽，但 FFT 看得见）"
          % big)


def residual_report(mono: np.ndarray, rate: float, n: int) -> None:
    """拟合一根理想正弦，看残差与相位跳变 —— 找"小幅但周期性"的不连续。

    比斜率阈值灵敏得多：24bit/float 流的底噪在 -140dB 量级，一个 -60dB 的跳变
    在残差里是 10^4 倍于底噪的存在，绝不会漏。相位跳变同理（一步 0.003 rad 也看得见）。
    """
    if len(mono) < 4096:
        print("[resid] 数据太少")
        return
    N = min(len(mono), n)
    seg = mono[-N:]
    w = make_window("bh", N).astype(np.float64)
    spec = np.fft.rfft(seg * w)
    mag = np.abs(spec)
    k = int(np.argmax(mag))
    if 0 < k < len(mag) - 1:                        # 抛物线插值细化主频
        a, b, c = mag[k - 1], mag[k], mag[k + 1]
        den = a - 2 * b + c
        d = 0.5 * (a - c) / den if abs(den) > 1e-30 else 0.0
    else:
        d = 0.0
    f0 = (k + d) * rate / N

    t = np.arange(len(mono)) / rate
    M = np.stack([np.sin(2 * np.pi * f0 * t), np.cos(2 * np.pi * f0 * t),
                  np.ones_like(t)], axis=1)
    coef, *_ = np.linalg.lstsq(M, mono, rcond=None)
    fit = M @ coef
    res = mono - fit
    amp = float(np.hypot(coef[0], coef[1]))
    rms = float(np.sqrt(np.mean(res ** 2)))
    print("[resid] 主频 %.4f Hz, 幅度 %.4f (%.2f dBFS)" % (f0, amp,
                                                         20 * np.log10(max(amp, 1e-12))))
    print("[resid] 残差 RMS %.3e (%.1f dBFS), 峰值 %.3e (%.1f dBFS), 峰均比 %.1f"
          % (rms, 20 * np.log10(max(rms, 1e-15)), float(np.abs(res).max()),
             20 * np.log10(max(float(np.abs(res).max()), 1e-15)),
             float(np.abs(res).max()) / max(rms, 1e-15)))
    thr = max(8.0 * rms, 1e-9)
    hits = np.nonzero(np.abs(res) > thr)[0]
    print("[resid] |残差| > 8*RMS 的点: %d 个 / %d 样本（稳健流应该几乎没有）"
          % (hits.size, len(res)))
    if hits.size:
        grp = []
        cur = [hits[0]]
        for h in hits[1:]:
            (cur.append(h) if h - cur[-1] <= 4 else (grp.append(cur), cur := [h]))
        grp.append(cur)
        print("[resid] 成簇后 %d 处事件（簇内样本数 / 时间 / 峰值 dBFS）:" % len(grp))
        for g in grp[:20]:
            s = g[int(np.argmax(np.abs(res[g])))]
            print("    t=%8.4f s  宽度 %2d 样本  峰值 %7.2f dBFS"
                  % (s / rate, len(g), 20 * np.log10(max(abs(float(res[s])), 1e-15))))
        if len(grp) > 3:
            tt = np.array([g[0] for g in grp]) / rate
            iv = np.diff(tt)
            print("    事件间隔: 中位 %.4f s（最小 %.4f / 最大 %.4f）"
                  % (float(np.median(iv)), float(iv.min()), float(iv.max())))

    # 相位跳变（用解析信号）
    X = np.fft.fft(mono)
    h = np.zeros(len(mono))
    h[0] = 1.0
    if len(mono) % 2 == 0:
        h[len(mono) // 2] = 1.0
    h[1:(len(mono) + 1) // 2] = 2.0
    z = np.fft.ifft(X * h)
    ph = np.unwrap(np.angle(z))
    dph = np.diff(ph)
    med = float(np.median(dph))
    dev = np.abs(dph - med)
    pthr = max(10.0 * float(np.median(dev)) if np.median(dev) > 0 else 1e-6, 1e-6)
    ph_hits = np.nonzero(dev > pthr)[0]
    print("[phase] 相位步进中位 %.6f rad/样本, 最大偏离 %.6f rad（阈值 %.2e）→ %d 处"
          % (med, float(dev.max()), pthr, ph_hits.size))
    for s in ph_hits[:10]:
        print("    t=%8.4f s  相位跳 %+.5f rad  (= %+.1f° )"
              % (s / rate, dph[s] - med, np.degrees(dph[s] - med)))


def watch_report(mono: np.ndarray, rate: float, n: int, window: str, freqs: list) -> None:
    """逐窗跟踪：把指定频率的电平随时间打出来 —— 抖动的旁瓣会在这里现原形。

    每个窗做一次 FFT（窗之间不重叠），逐帧列出这些频率的电平（dBFS），
    并给出每列的"最大-最小"摆幅。若某频率的摆幅有十几 dB，那就不是窗的问题。
    """
    hop = n // 2
    frames = max(1, (len(mono) - n) // hop + 1)
    w = make_window(window, n).astype(np.float64)
    wsum = w.sum()
    kk = [int(round(f / (rate / n))) for f in freqs]
    rows = []
    for i in range(frames):
        seg = mono[i * hop:i * hop + n]
        spec = np.abs(np.fft.rfft(seg * w)) * (2.0 / wsum)
        rows.append([20 * np.log10(max(float(spec[max(0, k - 1):k + 2].max()), 1e-12))
                     for k in kk])
    arr = np.asarray(rows)                      # (frames, nfreq)
    print("[watch] %d 窗（跳 %d 样本 = %.1f ms/窗，FFT %d = %.1f ms）"
          % (frames, hop, 1000.0 * hop / rate, n, 1000.0 * n / rate))
    print("  频率(Hz)  " + "".join("%9.1f" % f for f in freqs))
    for i in range(min(frames, 24)):
        print("  第%2d窗    " % i + "".join("%9.1f" % v for v in arr[i]))
    print("  ---- 每列摆幅(max-min, dB) ----")
    print("            " + "".join("%9.1f" % (arr[:, j].max() - arr[:, j].min())
                                  for j in range(arr.shape[1])))
    print("  ---- 每列中位(dBFS) ----")
    print("            " + "".join("%9.1f" % float(np.median(arr[:, j]))
                                  for j in range(arr.shape[1])))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("seconds", nargs="?", type=float, default=0.7)
    ap.add_argument("--rate", type=float, default=0.0, help="0 = 用设备默认采样率")
    ap.add_argument("--fft", type=int, default=32768)
    ap.add_argument("--window", default="bh")
    ap.add_argument("--device", default=None, help="输出设备名子串")
    ap.add_argument("--play", type=float, default=0.0, help=">0: 自己放这个频率的正弦再抓")
    ap.add_argument("--amp", type=float, default=0.5, help="--play 的幅度")
    ap.add_argument("--watch", default="", help="逐窗跟踪这些频率, 逗号分隔(如 92,96,99,102,105)")
    ap.add_argument("--watch-fft", type=int, default=65536, help="逐窗跟踪用的 FFT 点数")
    ap.add_argument("--resid", action="store_true",
                    help="拟合理想正弦看残差/相位跳变（找小幅周期性不连续）")
    args = ap.parse_args()

    import soundcard as sc

    spk = None
    if args.device:
        for s in sc.all_speakers():
            if args.device.lower() in str(s.name).lower():
                spk = s
                break
    if spk is None:
        spk = sc.default_speaker()
    rate = int(args.rate) if args.rate > 0 else (read_device_format(str(spk.id)).samplerate
                                                 or 48000)
    print("[probe] device: %s   rate: %d Hz   window=%s  FFT=%d"
          % (spk.name, rate, args.window, args.fft))

    if args.play > 0:
        t = np.arange(int(rate * 3.0)) / rate
        tone = (args.amp * np.sin(2.0 * np.pi * args.play * t)).astype(np.float32)
        stereo = np.stack([tone, tone], axis=1)
        spectrum_report("生成的信号(地面真值)", tone, rate, args.fft, args.window)

        def _play() -> None:
            with spk.player(samplerate=rate, channels=2, blocksize=4096) as pl:
                pl.play(stereo)

        threading.Thread(target=_play, daemon=True).start()
        time.sleep(0.4)                     # 等播放真的开始

    mic = sc.get_microphone(id=str(spk.id), include_loopback=True)
    need = max(args.fft, int(rate * args.seconds))
    with mic.recorder(samplerate=rate, channels=2, blocksize=4096) as rec:
        chunks = []
        got = 0
        t0 = time.monotonic()
        while got < need and time.monotonic() - t0 < 6.0:
            b = rec.record(numframes=4096)
            if b is None or len(b) == 0:
                continue
            chunks.append(np.asarray(b, dtype=np.float32))
            got += len(b)
    data = np.concatenate(chunks, axis=0) if chunks else np.zeros((0, 2), np.float32)
    if data.size == 0:
        print("[probe] 没抓到数据")
        return 2
    print("[probe] captured %d frames (%.2f s)" % (len(data), len(data) / rate))
    spectrum_report("抓到的回环(左)", data[:, 0].astype(np.float64), rate, args.fft, args.window)
    if data.shape[1] > 1:
        spectrum_report("抓到的回环(右)", data[:, 1].astype(np.float64), rate, args.fft,
                        args.window)
    if args.watch:
        freqs = [float(v) for v in args.watch.split(",") if v.strip()]
        watch_report(data[:, 0].astype(np.float64), rate, args.watch_fft, args.window, freqs)
    if args.resid:
        residual_report(data[:, 0].astype(np.float64), rate, args.fft)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
