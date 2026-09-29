"""bench_render.py —— Python 参考实现的阶段耗时基准（与 tools/render_bench.cpp 对照）

用法:
    C:\\Users\\TK11\\miniforge3\\envs\\audio\\python.exe tools\\bench_render.py [frames] [--cell 4]

为什么要有它: docs/508x302-feasibility.md §5 那张表是一次性测出来的，之后没人复测过。
C++ 侧已有 tools/render_bench.cpp，Python 侧需要同口径的对手方，否则"谁慢"永远是传说。

它只测渲染/分析阶段（不含 DearPyGui 的 GPU 上传与显示），每阶段取 N 帧均值。
"""

from __future__ import annotations

import argparse
import math
import os
import sys
import time
from typing import List

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import vfd_spectrum as vs  # noqa: E402


class Stat:
    def __init__(self) -> None:
        self.vals: List[float] = []

    def add(self, ms: float) -> None:
        self.vals.append(ms)

    def report(self, name: str) -> None:
        if not self.vals:
            return
        a = np.asarray(self.vals)
        print("  %-24s avg %8.3f ms  min %8.3f  max %8.3f" % (name, a.mean(), a.min(), a.max()))


def synth_bands(frame: int, n: int) -> np.ndarray:
    """与 tools/render_bench.cpp 的 synthBands 同一条曲线（黄金帧可比）。"""
    t = np.linspace(0.0, 1.0, n, dtype=np.float64)
    v = 0.02 + 0.01 * np.sin(3.1 * np.arange(n) + 0.05 * frame)
    for k in range(4):
        c = 0.14 + 0.23 * k + 0.05 * math.sin(0.031 * frame + 1.7 * k)
        w = 0.010 + 0.009 * k
        v += (0.88 - 0.13 * k) * np.exp(-((t - c) ** 2) / (2.0 * w * w))
    return np.clip(v, 0.0, 1.0).astype(np.float32)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("frames", nargs="?", type=int, default=120)
    ap.add_argument("--cell", type=int, default=4)
    ap.add_argument("--samplerate", type=int, default=48000)
    ap.add_argument("--bands", type=int, default=0, help="0 = 跟随 plot_w（现状）")
    args = ap.parse_args()

    cell = args.cell
    cols, rows = 508 // cell, 302 // cell
    cfg = vs.Config(cell=cell, dot=max(1, cell - 1))

    screen = vs.VfdScreen(cols, rows, cfg)
    screen.build_static()

    bands_n = args.bands if args.bands > 0 else cfg.bands
    engine = vs.SpectrumEngine(cfg, bands_n, args.samplerate)

    print("python bench: %dx%d px, grid %dx%d, cell=%d, plot_w=%d, bands=%d (auto=%s), "
          "nfft=%d (bin %.2f Hz), samplerate=%d, sub-bin/interp=%d"
          % (screen.native_w, screen.native_h, cols, rows, cell,
             screen.plot_w, engine.nbands, bands_n <= 0, engine.nfft,
             engine.samplerate / engine.nfft, args.samplerate, engine.sub_bin_bands))

    rng = np.random.default_rng(1)
    audio = rng.standard_normal(engine.nfft).astype(np.float32) * 0.1

    s_fft, s_bars, s_persist, s_rgba, s_total = Stat(), Stat(), Stat(), Stat(), Stat()
    for f in range(args.frames):
        t0 = time.perf_counter()
        t = time.perf_counter()
        bars, peaks = engine.process(audio, 1.0 / 30.0)
        t1 = time.perf_counter()
        screen.clear_dynamic()
        screen.draw_spectrum(synth_bands(f, engine.nbands), synth_bands(f, engine.nbands))
        t2 = time.perf_counter()
        screen.apply_persistence(1.0 / 30.0)
        t3 = time.perf_counter()
        rgba = screen.to_rgba()
        t4 = time.perf_counter()
        s_fft.add((t1 - t) * 1e3)
        s_bars.add((t2 - t1) * 1e3)
        s_persist.add((t3 - t2) * 1e3)
        s_rgba.add((t4 - t3) * 1e3)
        s_total.add((t4 - t0) * 1e3)
        if f == 0:
            print("  first frame rgba shape=%s" % (rgba.shape,))

    print("--- per frame (%d frames) ---" % args.frames)
    s_fft.report("SpectrumEngine.process")
    s_bars.report("draw_spectrum")
    s_persist.report("apply_persistence")
    s_rgba.report("to_rgba")
    s_total.report("TOTAL")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
