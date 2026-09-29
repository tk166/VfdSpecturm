# -*- coding: utf-8 -*-
"""峰值帽与磷光暂留的回归约束。

1) 纵轴方向: 点阵屏行号向下增大(plot_bottom 是基线, plot_top 是顶部), 峰值帽必须
   与柱体用同一约定, 否则峰值衰减时帽会往上飞。
2) 始终绘制 + 单像素: 每一列都要有一个帽, 且每列只能有 1 个点亮格。
3) 掉落速度: 峰值严格按 peak_release_db_s 线性回落(不被别的滤波拖慢)。
4) 磷光暂留: 是对"每个点阵像素的亮度"做 p_t = beta*p_target + (1-beta)*p_{t-1},
   beta = 1-exp(-dt/tau), 与帧率无关; 且不得作用在峰值数值上(那会把 dB/s 拖慢
   约 1/beta 倍, 实测 1.9s -> 9.6s)。

用法: <audio env>\\python.exe tools\\check_peak_axis.py
"""
from __future__ import annotations

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np  # noqa: E402

from vfd_spectrum import Config, SpectrumEngine, VfdScreen  # noqa: E402

CAP_THRESHOLD = 0.90          # >= 此亮度即认为是峰值帽(0.15 高的柱顶最高约 0.60)
HALO_THRESHOLD = 0.45         # 旧版拖尾亮度, 低于它才算"没有拖尾"
DT60 = 1.0 / 60.0


def cap_positions(screen: VfdScreen, bars: np.ndarray, peaks: np.ndarray) -> np.ndarray:
    """返回每列峰值帽所在的行号(-1 表示该列没有帽)。"""
    screen.clear_dynamic()
    screen.draw_spectrum(bars, peaks)
    r0, r1 = screen.plot_top, screen.plot_bottom
    rows = []
    for c in range(screen.plot_left, screen.plot_right + 1):
        lit = np.nonzero(screen.field[r0:r1 + 1, c] >= CAP_THRESHOLD)[0]
        rows.append(r0 + int(lit[0]) if lit.size else -1)
    return np.array(rows)


def check_direction(screen: VfdScreen) -> bool:
    """峰值从高到低衰减时, 帽必须往下掉(行号变大)。"""
    r0, r1 = screen.plot_top, screen.plot_bottom
    span = float(r1 - r0)
    n = screen.plot_w
    bars = np.full(n, 0.15, dtype=np.float32)

    ok = True
    print("[方向] plot_top=%d plot_bottom=%d (行号向下增大, 基线在 r1)" % (r0, r1))
    print("[方向] %-8s %-12s %-12s %s" % ("peaks", "期望行号", "实测行号", "结论"))
    prev_row = None
    for pk in (0.95, 0.75, 0.50, 0.25):
        caps = cap_positions(screen, bars, np.full(n, pk, dtype=np.float32))
        assert (caps >= 0).all(), "每一列都应该画出峰值帽"
        expect = r1 - pk * span
        got = float(caps.mean())
        good = abs(got - expect) <= 1.0
        ok &= good
        print("[方向] %-8.2f %-12.2f %-12.2f %s" % (pk, expect, got, "OK" if good else "FAIL"))
        if prev_row is not None:
            fall = got > prev_row
            ok &= fall
            print("[方向]          ^ 相比上一档 %s (行号 %+.1f, 向%s移动)"
                  % ("OK" if fall else "FAIL", got - prev_row, "下" if fall else "上"))
        prev_row = got

    caps = cap_positions(screen, bars, np.full(n, 0.60, dtype=np.float32))
    top_row = r1 - 0.15 * span
    above = bool((caps < top_row).all())
    ok &= above
    print("[方向] 峰值帽位于柱顶之上: %s (帽 %d < 柱顶 %.1f)"
          % ("OK" if above else "FAIL", int(caps.max()), top_row))
    return ok


def check_always_single_pixel(screen: VfdScreen) -> bool:
    """每列都必须有帽, 且只能有 1 个点亮格, 上下左右都不得有 0.45 拖尾。

    峰值取隔列高低交替(0.90 / 0.30): 相邻列的帽落在不同行, 才能把"横向拖尾"
    暴露出来——若各列帽都在同一行, 邻列的邻格本来就是帽, 测不出污染。
    """
    r0, r1 = screen.plot_top, screen.plot_bottom
    span = float(r1 - r0)
    n = screen.plot_w
    bars = np.full(n, 0.15, dtype=np.float32)
    peaks = bars.copy()
    even = np.arange(n) % 2 == 0
    peaks[even] = 0.90
    peaks[~even] = 0.30

    screen.clear_dynamic()
    screen.draw_spectrum(bars, peaks)
    plot = screen.field[r0:r1 + 1, screen.plot_left:screen.plot_right + 1]
    r_hi = r1 - int(round(0.90 * span))
    r_lo = r1 - int(round(0.30 * span))

    ok = True
    counts = (plot >= CAP_THRESHOLD).sum(axis=0)

    every_col = bool((counts == 1).all())
    ok &= every_col
    print("[单像素] 每列恰好 1 个亮点: %s (min=%d max=%d, 列数=%d)"
          % ("OK" if every_col else "FAIL", int(counts.min()), int(counts.max()), counts.size))

    rows = cap_positions(screen, bars, peaks)
    map_ok = bool((rows[even] == r_hi).all() and (rows[~even] == r_lo).all())
    ok &= map_ok
    print("[单像素] 行号映射正确: %s (0.90 -> 第%d行, 0.30 -> 第%d行)"
          % ("OK" if map_ok else "FAIL", r_hi, r_lo))

    up = max(plot[r_hi - 1 - r0, even].max() if r_hi - 1 >= r0 else 0.0,
             plot[r_lo - 1 - r0, ~even].max() if r_lo - 1 >= r0 else 0.0)
    down = max(plot[r_hi + 1 - r0, even].max() if r_hi + 1 <= r1 else 0.0,
               plot[r_lo + 1 - r0, ~even].max() if r_lo + 1 <= r1 else 0.0)
    vert_clean = bool(max(up, down) < HALO_THRESHOLD)
    ok &= vert_clean
    print("[单像素] 帽上下无拖尾: %s (row-1=%.3f row+1=%.3f, 阈值 %.2f)"
          % ("OK" if vert_clean else "FAIL", up, down, HALO_THRESHOLD))

    side_hi = float(plot[r_hi - r0, ~even].max())
    side_lo = float(plot[r_lo - r0, even].max())
    side_clean = bool(max(side_hi, side_lo) < HALO_THRESHOLD)
    ok &= side_clean
    print("[单像素] 帽左右无拖尾: %s (高帽行邻列=%.3f 低帽行邻列=%.3f, 阈值 %.2f)"
          % ("OK" if side_clean else "FAIL", side_hi, side_lo, HALO_THRESHOLD))
    return ok


def check_persistence() -> bool:
    """磷光暂留: 逐点阵像素的一阶时域滤波(不是滤峰值数值)。"""
    cfg = Config(phosphor_tau_s=0.07)
    dt = DT60
    ok = True

    scr = VfdScreen(126, 70, cfg)
    scr.build_static()
    n = scr.plot_w
    bars = np.full(n, 0.60, dtype=np.float32)
    peaks = np.full(n, 0.90, dtype=np.float32)

    # a) 单帧必须严格等于 p_t = beta*p_target + (1-beta)*p_{t-1}
    scr.clear_dynamic()
    scr.draw_spectrum(bars, peaks)
    prev = scr.persist.copy()
    target = scr.field.copy()
    scr.apply_persistence(dt)
    beta = 1.0 - math.exp(-dt / cfg.phosphor_tau_s)
    expect = beta * target + (1.0 - beta) * prev
    err = float(np.abs(scr.persist - expect).max())
    single_ok = err < 1e-6
    ok &= single_ok
    print("[暂留] 单帧=p_t=beta*p_target+(1-beta)*p_t-1: %s (beta=%.4f, 最大误差 %.2e)"
          % ("OK" if single_ok else "FAIL", beta, err))

    # b) 帧率无关: 同样 0.1s, 60fps 与 30fps 的余辉应一致
    def run(fps: int) -> np.ndarray:
        s = VfdScreen(126, 70, cfg)
        s.build_static()
        for _ in range(max(1, int(round(0.1 * fps)))):
            s.clear_dynamic()
            s.draw_spectrum(bars, peaks)
            s.apply_persistence(1.0 / fps)
        return s.persist

    d = float(np.abs(run(60) - run(30)).max())
    rate_ok = d < 0.01
    ok &= rate_ok
    print("[暂留] 帧率无关(0.1s @60fps vs @30fps): %s (最大差 %.5f)"
          % ("OK" if rate_ok else "FAIL", d))

    # c) 余辉拖尾: 撤掉信号后按 exp(-t/tau) 衰减
    scr.clear_dynamic()
    scr.draw_spectrum(bars, peaks)
    for _ in range(240):                                  # 先收敛到目标亮度
        scr.apply_persistence(dt)
    hot = float((scr.field - scr.static).max())
    scr.clear_dynamic()                                   # 目标变回静态网格
    for _ in range(int(round(0.2 / dt))):
        scr.apply_persistence(dt)
    rest = float((scr.persist - scr.static).max()) / max(1e-9, hot)
    theory = math.exp(-0.2 / cfg.phosphor_tau_s)
    tail_ok = abs(rest - theory) < 0.03
    ok &= tail_ok
    print("[暂留] 撤信号 0.2s 后残留: %s (实测 %.1f%% vs 理论 %.1f%%)"
          % ("OK" if tail_ok else "FAIL", 100 * rest, 100 * theory))

    # d) tau=0 时必须完全等价于不滤波
    s2 = VfdScreen(126, 70, Config(phosphor_tau_s=0.0))
    s2.build_static()
    s2.clear_dynamic()
    s2.draw_spectrum(bars, peaks)
    s2.apply_persistence(dt)
    off_ok = bool(np.array_equal(s2.persist, s2.field))
    ok &= off_ok
    print("[暂留] --phosphor 0 关闭滤波: %s" % ("OK" if off_ok else "FAIL"))
    return ok


def check_fall_rate() -> bool:
    """峰值掉落速度: 严格按 peak_release_db_s 的 dB/s 线性回落。

    这一项同时是"暂留别放错层"的守卫: 若把 IIR 加到峰值数值上, 每帧只走 beta
    比例, 实测耗时会比标称慢约 1/beta 倍(约 9.6s, 而不是 1.7s)。
    """
    cfg = Config()
    eng = SpectrumEngine(cfg, 16, 48000)
    n = eng.nfft
    tone = (0.8 * np.sin(2 * np.pi * 1000.0 * np.arange(n) / 48000.0)).astype(np.float32)
    silence = np.zeros(n, dtype=np.float32)

    for _ in range(240):
        eng.process(tone, DT60)
    top = float(eng.peaks.max())

    frames = 0
    while frames < 100000 and float(eng.peaks.max()) > 0.5 * top:
        eng.process(silence, DT60)
        frames += 1
    t_meas = frames * DT60
    span = cfg.db_max - cfg.db_min
    t_nominal = (0.5 * top * span) / cfg.peak_release_db_s      # 掉一半的标称耗时
    t_old = (0.5 * top * span) / 7.0                            # 原先进度对照

    ok = 0.8 * t_nominal <= t_meas <= 1.25 * t_nominal
    print("[掉落] 满刻度->半格: 实测 %.2fs, 标称 %.2fs (原 7dB/s 需 %.2fs) -> %s"
          % (t_meas, t_nominal, t_old, "OK" if ok else "FAIL"))
    return ok


def main() -> int:
    cfg = Config()
    screen = VfdScreen(126, 70, cfg)
    screen.build_static()

    ok = check_direction(screen)
    print()
    ok &= check_always_single_pixel(screen)
    print()
    ok &= check_fall_rate()
    print()
    ok &= check_persistence()

    print("\n结果:", "全部通过" if ok else "存在失败项")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
