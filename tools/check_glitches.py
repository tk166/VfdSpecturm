# -*- coding: utf-8 -*-
"""check_glitches.py —— 检查"落盘的音频流"里有没有台阶/丢样本（旁瓣抖动的真凶）

用法:
    python tools\\check_glitches.py <dump.bin>            # pipe_server --dump 出来的文件
    python tools\\check_glitches.py <dump.bin> --fft 262144

为什么: 一个稳定的单音，若频谱里出现"幅度大幅抖动的旁瓣"，那不是窗的问题 ——
是样本流里出现了**不连续**（丢块/重复块/重连清空）。一次跳变在一窗内就是一个
sinc 型宽带裙边（1/f 衰减），它随跳变进出窗而忽大忽小，看起来就是"抖动的旁瓣"。
这个脚本直接把台阶找出来：统计 |x[n]-x[n-1]| 的分布、列出最大的几次跳变位置，
再对跳变前后各取一窗算频谱，对比"干净段/含跳变段"的旁瓣电平。
"""
from __future__ import annotations

import argparse
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from vfd_spectrum import make_window  # noqa: E402

HDR = struct.Struct("<IIIIQd f HH I")   # magic,ver,n_samples,rate,index,ts,gain,ch,sess,reserved
HDR_BYTES = HDR.size                     # 44
CHUNK = 2048
CHANS = 2


def load_dump(path: str) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """返回 (左声道, 右声道, 每块的 frame_index)。"""
    raw = open(path, "rb").read()
    rec = HDR_BYTES + CHUNK * CHANS * 4
    n = len(raw) // rec
    if n == 0:
        raise SystemExit("文件里没有完整记录（%d 字节）" % len(raw))
    idx = np.empty(n, dtype=np.int64)
    left = np.empty(n * CHUNK, dtype=np.float32)
    right = np.empty(n * CHUNK, dtype=np.float32)
    for i in range(n):
        off = i * rec
        magic, ver, nsamp, rate, findex, _ts, _g, ch, _sess, _res = HDR.unpack_from(raw, off)
        if magic != 0x46444656:
            raise SystemExit("第 %d 条记录 magic 不对 (%08X)" % (i, magic))
        idx[i] = findex
        s = np.frombuffer(raw, dtype=np.float32, count=nsamp * CHANS,
                          offset=off + HDR_BYTES).reshape(nsamp, CHANS)
        left[i * CHUNK:i * CHUNK + nsamp] = s[:, 0]
        right[i * CHUNK:i * CHUNK + nsamp] = s[:, 1]
    print("[dump] %d 条记录, ver=%d, rate=%d, %d 帧 (%.2f s)"
          % (n, ver, rate, n * CHUNK, n * CHUNK / rate))
    gaps = np.diff(idx)
    bad = np.nonzero(gaps != 1)[0]
    print("[dump] frame_index 连续: %s（异常 %d 处）"
          % ("是" if bad.size == 0 else "否", bad.size))
    for b in bad[:10]:
        print("   idx 从 %d 跳到 %d (+%d)" % (idx[b], idx[b + 1], gaps[b]))
    return left, right, idx


def side_lobes(x: np.ndarray, rate: float, n: int, window: str = "bh") -> None:
    """打印一窗内主瓣之外的旁瓣电平（相对峰值）。"""
    if len(x) < n:
        print("   样本不足一窗")
        return
    for tag, seg in (("首窗", x[:n]), ("末窗", x[-n:])):
        w = make_window(window, n).astype(np.float64)
        spec = np.abs(np.fft.rfft(seg * w)) * (2.0 / w.sum())
        db = 20 * np.log10(np.maximum(spec, 1e-12))
        pk = int(np.argmax(db))
        far = np.concatenate([db[max(0, pk - 4000):max(0, pk - 8)],
                              db[pk + 9:pk + 4000]]) if pk > 8 else db[pk + 9:]
        print("   %s: 峰值 %.1f dBFS @ %.1f Hz   主瓣外最大 %.1f dBc   中位 %.1f dBc"
              % (tag, db[pk], pk * rate / n, float(far.max()) if far.size else 0.0,
                 float(np.median(far)) if far.size else 0.0))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--fft", type=int, default=262144)
    ap.add_argument("--window", default="bh")
    args = ap.parse_args()

    left, right, _idx = load_dump(args.dump)
    rate = 192000.0
    for name, x in (("L", left), ("R", right)):
        if x.size < 64:
            continue
        d = np.diff(x.astype(np.float64))
        ad = np.abs(d)
        rms = float(np.sqrt(np.mean(x.astype(np.float64) ** 2)))
        pk = float(np.max(np.abs(x)))
        # 一个正常正弦的相邻样本差上限 ≈ 2*pi*f/fs*amp；超过 8 倍中位数就当"台阶"
        med = float(np.median(ad))
        thr = max(20.0 * med, 0.02 * max(pk, 1e-9))
        spikes = np.nonzero(ad > thr)[0]
        print("[%s] 峰值=%.4f RMS=%.4f  相邻差中位=%.2e 最大=%.2e（%.0f 倍中位）"
              % (name, pk, rms, med, float(ad.max()), float(ad.max() / max(med, 1e-12))))
        print("[%s] 疑似台阶(> max(20x中位, 2%%满度)): %d 处 / %d 样本"
              % (name, spikes.size, x.size))
        for s in spikes[:8]:
            print("   样本 %8d: x[-1]=%+.5f -> x[0]=%+.5f  (跳 %+.5f)  t=%.4f s"
                  % (s, x[s], x[s + 1], d[s], s / rate))
        side_lobes(x.astype(np.float64), rate, args.fft, args.window)
        # 若有台阶，再单独看"含台阶的那一窗"
        if spikes.size:
            s = int(spikes[0])
            a = max(0, min(s - args.fft // 2, len(x) - args.fft))
            print("   含台阶窗(中心在第一个台阶):")
            side_lobes(x[a:a + args.fft].astype(np.float64), rate, args.fft, args.window)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
