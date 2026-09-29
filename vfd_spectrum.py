#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
VFD Spectrum Analyzer — 点阵荧光频谱分析仪
==========================================

用 DearPyGui 复刻上世纪点阵 VFD(真空荧光显示) 面板风格的实时频谱显示程序。

特性
----
* WASAPI 回环采集: 直接捕获当前 Windows 默认输出设备正在播放的声音
* 真·点阵渲染: 频谱、网格、刻度、文字全部画在逐点可控的点阵网格上
* 荧光衰减: dB 线性释放(余辉) + 峰值保持帽 + 磷光泛光(bloom) + 暗角
* 对数频率轴: 20Hz~20kHz,低清点阵刻度 + 横纵点阵网格
* 右上角显示设备采样率与位深(读 Windows 设备格式,与声音控制面板一致)
* 窗口可缩放,点阵网格随尺寸自动重排

依赖
----
    numpy, dearpygui, soundcard      (见 README.md / run.bat)

用法
----
    python vfd_spectrum.py                  # 启动
    python vfd_spectrum.py --list-devices   # 列出可采集的输出设备
    python vfd_spectrum.py --device TOPPING # 指定输出设备(名称子串)
    python vfd_spectrum.py --preview out.png   # 离线渲染预览,不开窗口

已知环境坑(重要)
----------------
DearPyGui 2.3.1 的 ``get_dearpygui_version()`` 在本机会在 C++ 运行库内触发
访问违例(0xC0000005) 直接崩进程,因此本程序**绝不调用**该函数。
"""

from __future__ import annotations

import argparse
import ctypes
import math
import os
import struct
import sys
import threading
import time
import zlib
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np

# ============================================================================
#  1. 点阵字库 (3x5 低清点阵, "低清刻度" 的灵魂)
# ============================================================================

FONT_3X5: Dict[str, Tuple[str, str, str, str, str]] = {
    "0": ("###", "#.#", "#.#", "#.#", "###"),
    "1": (".#.", "##.", ".#.", ".#.", "###"),
    "2": ("###", "..#", "###", "#..", "###"),
    "3": ("###", "..#", "###", "..#", "###"),
    "4": ("#.#", "#.#", "###", "..#", "..#"),
    "5": ("###", "#..", "###", "..#", "###"),
    "6": ("###", "#..", "###", "#.#", "###"),
    "7": ("###", "..#", "..#", "..#", "..#"),
    "8": ("###", "#.#", "###", "#.#", "###"),
    "9": ("###", "#.#", "###", "..#", "###"),
    "A": ("###", "#.#", "###", "#.#", "#.#"),
    "B": ("##.", "#.#", "##.", "#.#", "##."),
    "C": ("###", "#..", "#..", "#..", "###"),
    "D": ("##.", "#.#", "#.#", "#.#", "##."),
    "E": ("###", "#..", "###", "#..", "###"),
    "F": ("###", "#..", "###", "#..", "#.."),
    "G": ("###", "#..", "#.#", "#.#", "###"),
    "H": ("#.#", "#.#", "###", "#.#", "#.#"),
    "I": ("###", ".#.", ".#.", ".#.", "###"),
    "J": ("..#", "..#", "..#", "#.#", "###"),
    "K": ("#.#", "#.#", "##.", "#.#", "#.#"),
    "L": ("#..", "#..", "#..", "#..", "###"),
    "M": ("#.#", "###", "###", "#.#", "#.#"),
    "N": ("##.", "#.#", "#.#", "#.#", "#.#"),
    "O": ("###", "#.#", "#.#", "#.#", "###"),
    "P": ("###", "#.#", "###", "#..", "#.."),
    "Q": ("###", "#.#", "#.#", "###", "..#"),
    "R": ("###", "#.#", "###", "##.", "#.#"),
    "S": ("###", "#..", "###", "..#", "###"),
    "T": ("###", ".#.", ".#.", ".#.", ".#."),
    "U": ("#.#", "#.#", "#.#", "#.#", "###"),
    "V": ("#.#", "#.#", "#.#", "#.#", ".#."),
    "W": ("#.#", "#.#", "###", "###", "#.#"),
    "X": ("#.#", "#.#", ".#.", "#.#", "#.#"),
    "Y": ("#.#", "#.#", ".#.", ".#.", ".#."),
    "Z": ("###", "..#", ".#.", "#..", "###"),
    "-": ("...", "...", "###", "...", "..."),
    ".": ("...", "...", "...", "...", ".#."),
    ",": ("...", "...", "...", ".#.", "#.."),
    ":": ("...", ".#.", "...", ".#.", "..."),
    "/": ("..#", "..#", ".#.", "#..", "#.."),
    "(": (".#.", "#..", "#..", "#..", ".#."),
    ")": (".#.", "..#", "..#", "..#", ".#."),
    "+": ("...", ".#.", "###", ".#.", "..."),
    "%": ("#.#", "..#", ".#.", "#..", "#.#"),
    "*": ("#.#", ".#.", "###", ".#.", "#.#"),
    "=": ("...", "###", "...", "###", "..."),
    "?": ("###", "..#", ".#.", "...", ".#."),
    "_": ("...", "...", "...", "...", "###"),
    "!": (".#.", ".#.", ".#.", "...", ".#."),
    "#": ("#.#", "###", "#.#", "###", "#.#"),
    " ": ("...", "...", "...", "...", "..."),
}

GLYPH_W, GLYPH_H = 3, 5

# 颜色渐变节点: (强度, RGB) —— 磷光绿 VFD 风格,高亮处过驱动泛白
PHOSPHOR_STOPS: Tuple[Tuple[float, Tuple[float, float, float]], ...] = (
    (0.00, (0.008, 0.028, 0.022)),
    (0.10, (0.030, 0.180, 0.130)),
    (0.30, (0.070, 0.480, 0.320)),
    (0.55, (0.180, 0.820, 0.550)),
    (0.78, (0.400, 1.000, 0.720)),
    (1.00, (0.900, 1.000, 0.940)),
)
LUT_SIZE = 512

# 点阵亮度层级
V_OFF = 0.030          # 未点亮的点(点阵屏的"底纹")
V_GRID = 0.075         # 普通网格点
V_GRID_MAJOR = 0.145   # 主网格点(十倍频程 / 0dB)
V_AXIS = 0.230         # 坐标轴
V_TICK = 0.280         # 刻度线
V_LABEL = 0.520        # 刻度文字
V_TITLE = 0.720        # 标题
V_READOUT = 1.000      # 右上角读数
V_STATUS = 0.400       # 状态栏
V_BEZEL = 0.200        # 外框
V_PEAK = 1.000         # 峰值保持帽
# 声量计/峰值计：峰值条主体压暗("叠加在下面")，条头与频谱 peak 帽同亮
V_METER_BODY = 0.240
V_METER_TIP = V_PEAK


# ============================================================================
#  2. 配置
# ============================================================================

@dataclass
class Config:
    # ---- 窗口 / 点阵 ----
    # 注意: 这里是"客户区"(viewport client)目标尺寸, 不含标题栏/边框 ——
    # 网格是按客户区算的。1532x966 配合 cell=4 / pixel_scale=3 (每格 12px)
    # 正好得到 127x80 格、纹理 508x320 的初始状态(多余的 8x6 像素由图像拉伸吸收)。
    window_w: int = 1532
    window_h: int = 966
    pixel_scale: int = 3      # 原生像素 : 屏幕像素
    cell: int = 4             # 每个点阵单元的原生像素边长
    dot: int = 3              # 单元内点亮的像素边长(其余为间隙)
    fps: int = 60
    max_frames: int = 0       # >0 时渲染这么多帧后自动退出(自检/截图用)

    # ---- 频谱 ----
    f_min: float = 20.0
    f_max: float = 20000.0
    db_min: float = -78.0
    db_max: float = 0.0
    # FFT 窗长(秒) —— **分辨率只由它决定**（bin 宽 = 采样率 / 2^order）。
    # 0.18s: 48kHz -> 8192 点(bin 5.86Hz, 4097 频点), 192kHz -> 32768 点(bin 5.86Hz, 16385 频点)
    fft_seconds: float = 0.18
    fft_order: int = 0              # >0 时直接指定 2 的幂(覆盖 fft_seconds)
    bands: int = 0                  # 频段数; 0 = 自动 = 频点数(bins), 一个 bin 都不丢
    window: str = "bh"              # FFT 窗: rect | hann | blackman | bh(默认)
    tilt_db_per_decade: float = 3.0  # 粉噪倾斜补偿,让高频可见
    gain_db: float = 0.0            # 显示增益偏移(系统音量偏低时可用 +10 之类)
    release_db_s: float = 55.0      # 荧光余辉衰减速度 dB/s
    peak_release_db_s: float = 21.0  # 峰值保持帽衰减速度 dB/s(原先 7.0, 现加快 3 倍)
    phosphor_tau_s: float = 0.070   # 点阵像素磷光暂留时间常数(秒), 0 = 关闭

    # ---- 采集 ----
    blocksize: int = 4096
    device: Optional[str] = None    # 设备名子串,None = 默认输出设备


# ============================================================================
#  3. 工具函数
# ============================================================================

def make_window(kind: str, n: int) -> np.ndarray:
    """FFT 窗函数（与 src/vfd_dsp.h 的 makeWindow 逐点一致，都是对称窗）。

    为什么必须加窗：非整周期截断的正弦会在整个频轴上留下 sinc 泄漏
    （不加窗第一旁瓣 -13.3 dB、之后只掉 6 dB/oct）。加窗后旁瓣被压到
    -31 dB(hann) / -58 dB(blackman) / -92 dB(blackman-harris)。
    显示下限是 -78 dB，所以默认用 blackman-harris —— 旁瓣落在显示下限以下，
    屏幕上就看不到"裙子"了（代价：主瓣 4→8 bin，谱线更胖）。
    """
    i = np.arange(max(1, n), dtype=np.float64)
    x = 2.0 * np.pi * i / max(1, n - 1)
    if kind in ("rect", "rectangular", "none"):
        w = np.ones_like(x)
    elif kind in ("hann", "hanning"):
        w = 0.5 - 0.5 * np.cos(x)
    elif kind in ("blackman", "bm"):
        w = 0.42 - 0.5 * np.cos(x) + 0.08 * np.cos(2.0 * x)
    else:                                            # blackman-harris（默认）
        w = (0.35875 - 0.48829 * np.cos(x) + 0.14128 * np.cos(2.0 * x)
             - 0.01168 * np.cos(3.0 * x))
    return w.astype(np.float32)


def box_blur_cells(src: np.ndarray, r: int) -> np.ndarray:
    """**格域**可分离盒式模糊（边缘 replicate），用于模拟磷光泛光。

    为什么不像以前那样在像素域做：508x302 = 152k 个像素，而 127x75 的格只有 9.5k 个
    —— 差 16 倍；而且点阵屏的辉光本来就该以"点"为单位扩散（早年的 16 位单片机也是
    这么干的）。实测像素域两级 box_blur 要 3.6 ms/帧，挪到格域后整帧 <1 ms。
    """
    if r <= 0:
        return src.copy()
    k = 2 * r + 1
    h, w = src.shape
    pad = np.pad(src, ((r, r), (r, r)), mode="edge")
    acc = np.zeros_like(src)
    for i in range(k):
        acc += pad[i:i + h, r:r + w]
    pad2 = np.pad(acc, ((0, 0), (r, r)), mode="edge")
    out = np.zeros_like(src)
    for j in range(k):
        out += pad2[:, j:j + w]
    out /= float(k * k)
    return out


def build_lut(size: int = LUT_SIZE) -> np.ndarray:
    """构造 强度 -> RGBA 的查找表(磷光渐变色)。"""
    t = np.linspace(0.0, 1.0, size, dtype=np.float32) ** 0.85
    stops = np.array([s[0] for s in PHOSPHOR_STOPS], dtype=np.float32)
    cols = np.array([s[1] for s in PHOSPHOR_STOPS], dtype=np.float32)
    rgb = np.stack([np.interp(t, stops, cols[:, c]) for c in range(3)], axis=1)
    alpha = np.ones((size, 1), dtype=np.float32)
    return np.concatenate([rgb, alpha], axis=1).astype(np.float32)


def write_png(path: str, rgba_u8: np.ndarray) -> None:
    """最小 PNG 编码器(8bit RGBA),避免引入 Pillow 依赖。"""
    h, w = rgba_u8.shape[:2]
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(rgba_u8[y].tobytes())

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    blob = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
            + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(blob)


def ascii_only(text: str, limit: int = 0) -> str:
    """点阵字库只有 ASCII,把中文设备名过滤成可显示的英文片段。"""
    out = "".join(ch if 32 <= ord(ch) < 127 else " " for ch in text)
    out = " ".join(out.split())
    if limit and len(out) > limit:
        out = out[:limit]
    return out or "OUTPUT DEVICE"


def fmt_freq(f: float) -> str:
    if f >= 1000:
        return ("%gK" % (f / 1000.0)).upper()
    return "%g" % f


# ============================================================================
#  4. 设备格式读取 (采样率 / 位深)
# ============================================================================

_PKEY_DEVICE_FORMAT = "{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0"
_REG_RENDER = r"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
_GUID_PCM = bytes.fromhex("0100000000001000800000aa00389b71")
_GUID_FLOAT = bytes.fromhex("0300000000001000800000aa00389b71")


@dataclass
class DeviceFormat:
    """Windows 音频端点格式(等价于声音控制面板里显示的 采样率/位深)。"""
    samplerate: int = 48000
    channels: int = 2
    container_bits: int = 32
    valid_bits: int = 32
    is_float: bool = True
    source: str = "fallback"

    @property
    def bits(self) -> int:
        if 0 < self.valid_bits <= self.container_bits:
            return self.valid_bits
        return self.container_bits

    def rate_label(self) -> str:
        return "%dHZ" % self.samplerate

    def bit_label(self) -> str:
        return "%dBIT%s" % (self.bits, "F" if self.is_float else "")


def read_device_format(device_id: str) -> DeviceFormat:
    """从注册表读 PKEY_AudioEngine_DeviceFormat(WAVEFORMATEXTENSIBLE blob)。

    该值是 PROPVARIANT 序列化: 8 字节头 + WAVEFORMATEXTENSIBLE(40 字节)。
    为避免猜偏移,直接用 KSDATAFORMAT_SUBTYPE GUID 反推结构体起点。
    """
    fmt = DeviceFormat()
    try:
        import winreg
    except ImportError:                                 # pragma: no cover
        return fmt

    guid = device_id.rsplit(".", 1)[-1]
    subkey = r"%s\%s\Properties" % (_REG_RENDER, guid)
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, subkey) as key:
            data, _ = winreg.QueryValueEx(key, _PKEY_DEVICE_FORMAT)
    except OSError:
        return fmt
    if not isinstance(data, (bytes, bytearray)) or len(data) < 26:
        return fmt

    base = None
    for pattern, is_float in ((_GUID_FLOAT, True), (_GUID_PCM, False)):
        idx = bytes(data).find(pattern)
        if idx >= 24:
            base, fmt.is_float = idx - 24, is_float
            break
    if base is None:                     # 老式 WAVEFORMATEX(16bit PCM 等)
        base, fmt.is_float = 8, False
    if len(data) < base + 18:
        return fmt

    _tag, channels, samplerate, _abps, _ba, bits, cb_size = struct.unpack_from(
        "<HHIIHHH", data, base)
    valid = bits
    if cb_size >= 22 and len(data) >= base + 20:
        valid = struct.unpack_from("<H", data, base + 18)[0]

    if not (8000 <= samplerate <= 768000) or not (1 <= channels <= 16) \
            or bits not in (8, 16, 20, 24, 32):
        return fmt
    fmt.samplerate = int(samplerate)
    fmt.channels = int(channels)
    fmt.container_bits = int(bits)
    fmt.valid_bits = int(valid)
    fmt.source = "registry"
    return fmt


# ============================================================================
#  5. 音频采集 (WASAPI 回环 / 后台线程)
# ============================================================================

class RingBuffer:
    """单生产者/单消费者环形缓冲,存放单声道浮点样本。"""

    def __init__(self, capacity: int) -> None:
        self._cap = int(capacity)
        self._buf = np.zeros(self._cap, dtype=np.float32)
        self._w = 0
        self._total = 0
        self._lock = threading.Lock()

    @property
    def total(self) -> int:
        with self._lock:
            return self._total

    def write(self, x: np.ndarray) -> None:
        n = x.size
        if n == 0:
            return
        with self._lock:
            if n >= self._cap:
                self._buf[:] = x[-self._cap:]
                self._w = 0
            else:
                end = self._w + n
                if end <= self._cap:
                    self._buf[self._w:end] = x
                else:
                    k = self._cap - self._w
                    self._buf[self._w:] = x[:k]
                    self._buf[:end - self._cap] = x[k:]
                self._w = end % self._cap
            self._total += n

    def latest(self, n: int) -> np.ndarray:
        with self._lock:
            n = min(int(n), self._cap)
            idx = np.arange(self._w - n, self._w) % self._cap
            return self._buf[idx]


class StereoRing:
    """交错立体声环形缓冲（只给声量计/峰值计用；频谱那条路仍走单声道 RingBuffer）。"""

    def __init__(self, capacity: int) -> None:
        self._cap = int(capacity)
        self._buf = np.zeros((self._cap, 2), dtype=np.float32)
        self._w = 0
        self._r = 0
        self._lock = threading.Lock()

    def write(self, block: np.ndarray) -> None:
        b = np.asarray(block, dtype=np.float32)
        if b.ndim == 1:
            b = np.stack([b, b], axis=1)
        if b.shape[1] < 2:                                # 单声道源：复制成 L=R
            b = np.stack([b[:, 0], b[:, 0]], axis=1)
        b = b[:, :2]
        n = b.shape[0]
        if n >= self._cap:
            with self._lock:
                self._buf[:] = b[-self._cap:]
                self._w = 0
            return
        with self._lock:
            end = self._w + n
            if end <= self._cap:
                self._buf[self._w:end] = b
            else:
                k = self._cap - self._w
                self._buf[self._w:] = b[:k]
                self._buf[:end - self._cap] = b[k:]
            self._w = end % self._cap

    def latest_frames(self, n: int) -> np.ndarray:
        with self._lock:
            n = min(int(n), self._cap)
            idx = np.arange(self._w - n, self._w) % self._cap
            return self._buf[idx].copy()

    def read_new(self, max_frames: int) -> np.ndarray:
        """取走"上次之后新到"的帧（顺序保持），最多 max_frames 帧。

        **只少给，不丢**：曾经这里写成"待取帧数 > max_frames 就丢掉最旧的"，
        在 C++ 面板那边正是谱线上那条假裙边的元凶（丢几毫秒音频 = 分析窗里插入
        一个相位台阶 = 一条 ∝1/f 的宽带裙边，听不出来但在屏上非常明显）。
        所以调用方必须循环取到空为止，别指望一次取完。
        """
        with self._lock:
            n = (self._w - self._r) % self._cap
            if n > max_frames:                               # 只少给，不丢
                n = max_frames
            if n == 0:
                return np.zeros((0, 2), dtype=np.float32)
            idx = (np.arange(self._r, self._r + n)) % self._cap
            out = self._buf[idx].copy()
            self._r = (self._r + n) % self._cap
            return out


class LoopbackCapture(threading.Thread):
    """后台线程: 回环采集默认(或指定)输出设备的声音。

    设备切换/拔出会自动重连;线程内需自行初始化 COM(MTA)。
    """

    def __init__(self, cfg: Config) -> None:
        super().__init__(name="loopback-capture", daemon=True)
        self.cfg = cfg
        self.ring = RingBuffer(max(1 << 16, 8 * cfg.blocksize))
        self.sring = StereoRing(max(1 << 15, 4 * cfg.blocksize))   # 声量计用的 L/R
        self.fmt = DeviceFormat()
        self.device_name = ""
        self.state = "starting"     # starting | running | no-device | error
        self.message = ""
        self.level = 0.0            # 最近一块的峰值电平,用于"有无信号"判断
        self._stop = threading.Event()
        self._device_id = ""

    # -- 生命周期 ---------------------------------------------------------
    def stop(self) -> None:
        self._stop.set()

    def run(self) -> None:                              # noqa: D401
        try:
            import soundcard as sc
        except Exception as exc:                        # noqa: BLE001
            self.state = "error"
            self.message = ascii_only("IMPORT FAIL: %s" % exc, 40)
            return
        # COM 是**按线程**初始化的: soundcard 首次导入时会在导入线程里初始化 COM(MTA),
        # 若模块已被主线程导入过,采集线程必须自己再初始化一次,否则 WASAPI 调用会失败。
        # 已初始化时 CoInitializeEx 返回 S_FALSE(非错误),因此忽略返回值。
        try:
            ctypes.windll.ole32.CoInitializeEx(None, 0x0)
        except Exception:                               # noqa: BLE001
            pass

        while not self._stop.is_set():
            try:
                spk = self._pick_speaker(sc)
                if spk is None:
                    self.state, self.message = "no-device", "NO OUTPUT DEVICE"
                    self._stop.wait(1.5)
                    continue

                self._device_id = str(spk.id)
                self.device_name = ascii_only(str(spk.name))
                self.fmt = read_device_format(self._device_id)
                mic = sc.get_microphone(id=self._device_id, include_loopback=True)
                if mic is None:
                    self.state, self.message = "error", "NO LOOPBACK"
                    self._stop.wait(1.5)
                    continue

                with mic.recorder(samplerate=self.fmt.samplerate,
                                  channels=min(2, max(1, self.fmt.channels)),
                                  blocksize=self.cfg.blocksize) as rec:
                    self.state, self.message = "running", ""
                    last_check = time.monotonic()
                    while not self._stop.is_set():
                        block = rec.record(numframes=self.cfg.blocksize)
                        if block is None or len(block) == 0:
                            continue
                        mono = block.mean(axis=1) if block.ndim > 1 else block
                        mono = np.asarray(mono, dtype=np.float32)
                        self.ring.write(mono)
                        self.sring.write(block)              # L/R 分开留一份给声量计
                        self.level = float(np.abs(mono).max()) if mono.size else 0.0
                        if time.monotonic() - last_check > 2.0:
                            last_check = time.monotonic()
                            if not self._same_device(sc):
                                break        # 输出设备变了 -> 重开流
            except Exception as exc:                      # noqa: BLE001
                self.state = "error"
                self.message = ascii_only("%s: %s" % (type(exc).__name__, exc), 40)
                self._stop.wait(1.5)

    # -- 设备选择 ---------------------------------------------------------
    def _pick_speaker(self, sc):                          # noqa: ANN001
        if self.cfg.device:
            needle = self.cfg.device.lower()
            for spk in sc.all_speakers():
                if needle in str(spk.name).lower():
                    return spk
            return None
        return sc.default_speaker()

    def _same_device(self, sc) -> bool:                   # noqa: ANN001
        try:
            spk = self._pick_speaker(sc)
            return spk is not None and str(spk.id) == self._device_id
        except Exception:                                 # noqa: BLE001
            return True


# ============================================================================
#  6. 频谱引擎 (FFT -> 对数频段 -> 荧光包络)
# ============================================================================

class SpectrumEngine:
    """把时域样本变成 nbands 个对数频段的归一化高度(0~1)。"""

    def __init__(self, cfg: Config, nbands: int, samplerate: int) -> None:
        self.cfg = cfg
        self.samplerate = int(samplerate)
        if getattr(cfg, "fft_order", 0) > 0:
            self.nfft = int(1 << int(cfg.fft_order))
        else:
            self.nfft = int(2 ** round(math.log2(max(1024.0, cfg.fft_seconds * samplerate))))
        self.nfft = int(np.clip(self.nfft, 2048, 65536))
        self.bins = self.nfft // 2 + 1
        # 频段数 = 0 时自动取"频点数"，保证一个 bin 都不丢（频段只是对数轴上的重采样）
        self.nbands = int(nbands) if nbands and nbands > 0 else self.bins
        self.window = make_window(cfg.window, self.nfft)
        self.wgain = float(self.window.sum())
        self.freqs = np.fft.rfftfreq(self.nfft, 1.0 / samplerate)
        self.bars = np.zeros(self.nbands, dtype=np.float32)
        self.peaks = np.zeros(self.nbands, dtype=np.float32)
        self._build_bands()

    # ------------------------------------------------------------------
    def _build_bands(self) -> None:
        nyq = self.samplerate * 0.5
        f_hi = min(self.cfg.f_max, nyq * 0.97)
        edges = np.geomspace(self.cfg.f_min, f_hi, self.nbands + 1)
        lo = np.searchsorted(self.freqs, edges[:-1], side="left")
        hi = np.searchsorted(self.freqs, edges[1:], side="right")
        hi = np.maximum(hi, lo + 1)
        hi = np.minimum(hi, len(self.freqs))
        lo = np.minimum(lo, len(self.freqs) - 1)
        self.band_lo, self.band_hi = lo.astype(np.int64), hi.astype(np.int64)
        mid = np.sqrt(edges[:-1] * edges[1:])
        self.band_mid = mid
        # 频段跨度 < 2 个 bin ⇒ FFT 在这个频点上没有独立分辨率 ⇒ 走插值（见 process）
        self.band_interp = (self.band_hi - self.band_lo) < 2
        self.sub_bin_bands = int(self.band_interp.sum())
        self.tilt = (self.cfg.tilt_db_per_decade
                     * np.log10(np.maximum(mid, 1.0) / 1000.0)).astype(np.float32)

    def retune(self, nbands: int, samplerate: int) -> None:
        """窗口尺寸或设备采样率变化后重建频段映射(保留包络状态)。

        nbands <= 0 表示"自动 = 频点数"，此时只要采样率没变就不用重建——
        否则每帧都会走一遍 __init__（0 永远不等于实际频段数，是个很容易踩的坑）。
        """
        if samplerate == self.samplerate and (nbands <= 0 or int(nbands) == self.nbands):
            return
        old_bars = self.bars
        self.__init__(self.cfg, nbands, samplerate)       # noqa: PLC2801
        if old_bars.size == self.nbands:
            self.bars, self.peaks = old_bars, old_bars.copy()

    # ------------------------------------------------------------------
    def process(self, samples: np.ndarray, dt: float) -> Tuple[np.ndarray, np.ndarray]:
        n = self.nfft
        if samples.size < n:
            x = np.zeros(n, dtype=np.float32)
            x[n - samples.size:] = samples
        else:
            x = samples[-n:]
        spec = np.abs(np.fft.rfft(x * self.window))
        spec *= (2.0 / self.wgain)

        levels = np.empty(self.nbands, dtype=np.float32)
        wide = ~self.band_interp
        if wide.any():
            # 宽频段：取 bin 的 max（保证窄峰不被平均掉）。
            # 用 maximum.reduceat 而不是 Python 循环：reduceat 的第 i 段是
            # [idx[i], idx[i+1])，所以把"最后一个宽频段的上界"补在末尾当哨兵，
            # 再去掉多出来的那一项 —— 语义与逐段 max 完全一致，但没有解释器开销。
            lo_w = self.band_lo[wide]
            idx = np.append(lo_w, self.band_hi[wide][-1])
            levels[wide] = np.maximum.reduceat(spec, idx)[:-1]
        narrow = self.band_interp
        if narrow.any():
            # 比一个 bin 还窄的频段：FFT 在这里没有独立信息，取 max 只会得到阶梯
            # （192kHz/16384 点 → bin 11.7Hz，512 个对数频段的前 55% 都窄于一个 bin）。
            # 改为按频率线性插值，得到连续、单调的谱包络。
            levels[narrow] = np.interp(self.band_mid[narrow], self.freqs, spec)

        span = self.cfg.db_max - self.cfg.db_min
        db = 20.0 * np.log10(np.maximum(levels, 1e-7)) + self.tilt + self.cfg.gain_db
        norm = np.clip((db - self.cfg.db_min) / span, 0.0, 1.0)

        fall = (self.cfg.release_db_s * dt) / span
        pfall = (self.cfg.peak_release_db_s * dt) / span
        self.bars = np.maximum(norm, self.bars - fall)
        # 峰值保持: 严格按 dB/s 线性回落。
        # 注意: 惯量(荧光暂留)不在这里做 —— 那是对"每个点阵像素的亮度"做的时域
        # 一阶滤波, 见 VfdScreen.apply_persistence()。若把 IIR 加在峰值数值上, 每帧
        # 只走 beta 比例, 会把这里配置的 dB/s 拖慢约 1/beta 倍(实测 1.9s -> 9.6s)。
        self.peaks = np.maximum(norm, self.peaks - pfall)
        return self.bars, self.peaks


# ============================================================================
#  7. 点阵屏 (绘制原语 + 磷光后处理)
# ============================================================================

METER_DB_MIN = -60.0          # 声量计/峰值计的计量下限(dBFS),底部横向刻度也用它


class MeterBank:
    """声量计(VU) + 峰值计——两条弹道，与 src/vfd_dsp.h 的 MeterBank 同一套规则：

      * VU = 对**功率**做一阶 IIR 平均(经典 VU 表 300ms 时间常数)，看着"肉"；
      * 峰值 = 本块样本峰的即时值 + dB/s 线性回落(快起慢落)，**没有额外的延时保持段**。

    两者都归一化到 [METER_DB_MIN, 0] dBFS，与屏幕底部那条横向刻度严格对应。
    只在 UI 端算：输入就是回环采集/管道透传过来的交错立体声。
    """

    def __init__(self, db_min: float = METER_DB_MIN, vu_tau: float = 0.30,
                 peak_release_db_s: float = 24.0, channels: int = 2) -> None:
        self.db_min = float(db_min)
        self.vu_tau = float(vu_tau)
        self.peak_release_db_s = float(peak_release_db_s)
        n = max(1, min(4, int(channels)))
        self.n = n
        self._sum_sq = np.zeros(n, dtype=np.float64)
        self._count = 0
        self._block_peak = np.zeros(n, dtype=np.float64)
        self._power = np.zeros(n, dtype=np.float64)
        self._peak_lin = np.zeros(n, dtype=np.float64)
        self.vu = np.zeros(n, dtype=np.float32)      # 归一化 0..1
        self.peak = np.zeros(n, dtype=np.float32)

    def push(self, frames: np.ndarray) -> None:
        """frames: (n, ch) float32（只取前 self.n 个声道）。"""
        if frames.size == 0:
            return
        f = frames if frames.ndim > 1 else frames[:, None]
        ch = f.shape[1]
        for c in range(self.n):
            src = f[:, c if c < ch else ch - 1].astype(np.float64)
            self._sum_sq[c] += float(np.dot(src, src))
            self._block_peak[c] = max(self._block_peak[c], float(np.abs(src).max()))
        self._count += f.shape[0]

    def process(self, dt: float) -> None:
        dt = max(0.0, float(dt))
        rng = 0.0 - self.db_min
        alpha = 1.0 - math.exp(-dt / self.vu_tau) if self.vu_tau > 1e-6 else 1.0
        decay = 10.0 ** (-(self.peak_release_db_s * dt) / 20.0)
        for c in range(self.n):
            ms = (self._sum_sq[c] / self._count) if self._count > 0 else 0.0
            self._power[c] += alpha * (ms - self._power[c])
            self._peak_lin[c] = max(self._block_peak[c], self._peak_lin[c] * decay)
            self.vu[c] = self._norm(math.sqrt(self._power[c]), rng)
            self.peak[c] = self._norm(self._peak_lin[c], rng)
            self._sum_sq[c] = 0.0
            self._block_peak[c] = 0.0
        self._count = 0

    def _norm(self, lin: float, rng: float) -> float:
        db = 20.0 * math.log10(max(lin, 1e-9))
        return float(min(1.0, max(0.0, (db - self.db_min) / rng)))

    def vu_db(self, ch: int = 0) -> float:
        return 20.0 * math.log10(max(math.sqrt(self._power[ch]), 1e-9))

    def peak_db(self, ch: int = 0) -> float:
        return 20.0 * math.log10(max(self._peak_lin[ch], 1e-9))


class VfdScreen:
    """一块 cols x rows 的点阵屏。

    所有绘制都落在"格子"坐标系上;输出时展开成原生像素并做
    泛光(bloom) + 暗角(vignette) + 颜色查找表,得到 VFD 观感。
    """

    def __init__(self, cols: int, rows: int, cfg: Config) -> None:
        self.cfg = cfg
        self.cols, self.rows = int(cols), int(rows)
        self.cell_px, self.dot_px = int(cfg.cell), int(cfg.dot)
        self.native_w = self.cols * self.cell_px
        self.native_h = self.rows * self.cell_px

        self.field = np.zeros((self.rows, self.cols), dtype=np.float32)
        self.static = np.zeros((self.rows, self.cols), dtype=np.float32)
        # 磷光余辉缓冲: 每个点阵像素的"当前实际亮度", 由 apply_persistence() 逐帧
        # 向本帧目标亮度(field)做一阶逼近。输出走这个缓冲, 才有点阵荧光拖尾。
        self.persist = np.zeros((self.rows, self.cols), dtype=np.float32)
        self._lut = build_lut()
        self._glyph_cache: Dict[Tuple[str, int, int], Tuple[Tuple[int, int], ...]] = {}
        self._text_dots: Dict[Tuple[str, int, int], Tuple[Tuple[int, int], ...]] = {}

        # 单元展开掩膜: 每 cell 内前 dot 行为点亮区, 其余为点间隙
        keep = np.zeros((self.cell_px, self.cell_px), dtype=np.float32)
        keep[:self.dot_px, :self.dot_px] = 1.0
        self._keep = np.tile(keep, (self.rows, self.cols))

        # 暗角: 四周略暗,中心略亮
        yy = np.linspace(-1.0, 1.0, self.native_h, dtype=np.float32)[:, None]
        xx = np.linspace(-1.0, 1.0, self.native_w, dtype=np.float32)[None, :]
        self._vignette = (1.06 - 0.24 * (xx * xx * 0.7 + yy * yy * 0.5)).astype(np.float32)

        self._compute_layout()

    # ---------------------------------------------------------------- 布局
    def fit_text(self, s: str, max_w: int, scales: Sequence[int] = (2, 1)) -> Tuple[str, int]:
        """在给定宽度内选最大的字号;实在放不下就逐步截短。"""
        for sc in scales:
            if self.text_width(s, sc, 1) <= max_w:
                return s, sc
        sc = scales[-1]
        pitch = (GLYPH_W + 1) * sc
        n = max(1, (max_w + sc) // pitch)
        return s[:n], sc

    def _compute_layout(self) -> None:
        c, r = self.cols, self.rows
        cfg = self.cfg
        self.margin = 2
        self.header_top = 2

        # ---- 右上角读数(采样率/位深,两行 scale1) + 左上角标题 ----
        self.readout_w = max(self.text_width("888888HZ", 1, 1),
                             self.text_width("88BIT 8CH", 1, 1))
        self.readout_h = GLYPH_H * 2 + 1
        self.title = "SPECTRUM"
        self.title_w = c - self.margin * 2 - self.readout_w - 5
        self.title_scale = 2 if self.text_width(self.title, 2, 1) <= self.title_w else 1
        self.title = (self.title if self.text_width(self.title, self.title_scale, 1)
                      <= self.title_w else "VFD")
        self.header_h = max(GLYPH_H * self.title_scale, self.readout_h)

        # ---- 底部（由下往上）：dBFS 刻度 -> 声量计两条(L/R) -> 频率刻度 -> 频谱轴线 ----
        #   dBFS 刻度标签在最底下一行，风格与频率刻度一致（只有数字，不写 "dBFS" 字样）
        self.dbfs_label_top = r - self.margin - GLYPH_H
        self.dbfs_tick_row = self.dbfs_label_top - 1
        self.meter_bottom = self.dbfs_label_top - 3          # 与刻度之间留 2 格
        self.meter_h = max(3, int(round(15.0 / max(1, self.cell_px))))
        self.meter_gap = 1
        self.meter_top = self.meter_bottom - (2 * self.meter_h + self.meter_gap) + 1

        # 频谱的 X 轴（刻度线 + 频率标签）紧贴绘图区下方
        self.axis_label_top = self.meter_top - 2 - GLYPH_H
        self.axis_tick_row = self.axis_label_top - 2
        self.axis_row = self.axis_tick_row - 1
        self.plot_bottom = max(self.header_top + self.header_h + 8, self.axis_row - 1)
        self.plot_top = self.header_top + self.header_h + 2

        # ---- 绘图区: 左侧不再留 dB 刻度列（省下 14 列给频谱）----
        self.y_label_right = self.margin
        self.plot_left = self.margin + 1
        self.plot_right = c - self.margin - 1
        self.plot_w = max(8, self.plot_right - self.plot_left + 1)

        # ---- dB 网格: 步长按可用行数自适应,避免网格线挤在一起 ----
        span_rows = max(1, self.plot_bottom - self.plot_top)
        db_total = abs(cfg.db_min)
        self.db_step = 78
        for step in (6, 12, 24, 39, 78):
            if step / db_total * span_rows >= 2.4:
                self.db_step = step
                break
        self.db_lines: List[Tuple[int, float]] = []
        for level in range(0, int(db_total) + 1, self.db_step):
            db = -float(level)                                    # 0dB 在顶, 越往下越负
            t = (db - cfg.db_min) / (cfg.db_max - cfg.db_min)     # 1 = 顶部
            row = int(round(self.plot_bottom - t * span_rows))
            self.db_lines.append((row, db))
        # 标签步长/标签行原本是给左侧 dB 刻度用的，刻度列已移除（版面全给频谱），
        # 所以这里只保留网格线本身。

        # ---- 横轴频率刻度 ----
        majors = [20.0, 50.0, 100.0, 500.0, 1000.0, 5000.0, 10000.0, 20000.0]
        minors = [30.0, 70.0, 200.0, 300.0, 700.0, 2000.0, 3000.0, 7000.0]
        self.ticks = sorted([(f, True) for f in majors] + [(f, False) for f in minors])
        self.tick_labels = self._pick_tick_labels(majors)

    def _pick_tick_labels(self, majors: Sequence[float]) -> List[Tuple[float, int, str]]:
        """按优先级挑出不重叠的刻度标签(十进刻度优先),返回 (频率, 列, 文本)。"""
        decade = {100.0, 1000.0, 10000.0}
        cands = sorted(majors, key=lambda f: (f not in decade, f))
        placed: List[Tuple[float, int, str]] = []
        taken: List[Tuple[int, int]] = []
        for f in cands:
            label = fmt_freq(f)
            w = self.text_width(label, 1, 1)
            col = int(round(self.freq_to_col(f))) - w // 2
            col = max(self.plot_left, min(col, self.plot_right - w + 1))
            if any(not (col + w - 1 < a - 1 or col > b + 1) for a, b in taken):
                continue
            taken.append((col, col + w - 1))
            placed.append((f, col, label))
        return sorted(placed)

    # ------------------------------------------------------------ 声量计/峰值计
    def meter_col_for_db(self, db: float) -> int:
        """dBFS → 声量计区的列号（线性 in dB，与 MeterBank 的归一化严格对应）。"""
        t = (db - METER_DB_MIN) / (0.0 - METER_DB_MIN)
        return self.plot_left + int(round(t * (self.plot_w - 1)))

    def draw_meters(self, vu: np.ndarray, peak: np.ndarray) -> None:
        """两条横向条（上=L，下=R），风格与频谱柱体一致；峰值条整体压暗，
        只有它的**条头**用与频谱 peak 帽相同的最高亮度。"""
        w = self.plot_w
        if w <= 2:
            return
        for ch in range(min(2, len(vu))):
            r0 = self.meter_top + ch * (self.meter_h + self.meter_gap)
            r1 = min(self.rows, r0 + self.meter_h)
            if r0 >= self.rows:
                continue
            v_vu = float(min(1.0, max(0.0, vu[ch])))
            v_pk = float(min(1.0, max(0.0, peak[ch])))
            vu_cols = int(round(v_vu * w))
            pk_cols = int(round(v_pk * w))
            for c in range(w):
                level = (c + 1) / float(w)
                v = V_OFF
                if c < pk_cols:
                    v = max(v, V_METER_BODY)                  # 峰值条主体（压暗）
                if c == pk_cols - 1:
                    v = max(v, V_METER_TIP)                   # 峰值条头 = 最亮
                if c < vu_cols:                               # 声量条（深度渐变）
                    depth = min(1.0, level / v_vu) if v_vu > 1e-4 else 1.0
                    # 与频谱柱体同一条竖直渐变曲线(0.12+0.88*d^1.15)，但把底部抬到
                    # 明显高于峰值条主体，否则条子前半段看不出"声量 vs 峰值"两层
                    ramp = 0.30 + 0.70 * (0.12 + 0.88 * depth ** 1.15)
                    v = max(v, min(1.0, ramp * (v_vu ** 0.30 + 0.02)))
                col = self.plot_left + c
                block = self.field[r0:r1, col]
                np.maximum(block, v, out=block)

    def freq_to_col(self, f: float) -> float:
        span = math.log10(self.cfg.f_max / self.cfg.f_min)
        t = math.log10(max(f, 1e-6) / self.cfg.f_min) / span
        return self.plot_left + t * (self.plot_w - 1)

    # ------------------------------------------------------------ 绘制原语
    def clear_dynamic(self) -> None:
        np.copyto(self.field, self.static)

    def dot(self, c: int, r: int, v: float) -> None:
        if 0 <= c < self.cols and 0 <= r < self.rows and v > self.field[r, c]:
            self.field[r, c] = v

    def hline(self, c0: int, c1: int, r: int, v: float, dash: int = 1) -> None:
        if not (0 <= r < self.rows):
            return
        c0, c1 = max(0, min(c0, c1)), min(self.cols - 1, max(c0, c1))
        for c in range(c0, c1 + 1):
            if dash <= 1 or (c % dash) == 0:
                self.dot(c, r, v)

    def vline(self, c: int, r0: int, r1: int, v: float, dash: int = 1) -> None:
        if not (0 <= c < self.cols):
            return
        r0, r1 = max(0, min(r0, r1)), min(self.rows - 1, max(r0, r1))
        for r in range(r0, r1 + 1):
            if dash <= 1 or (r % dash) == 0:
                self.dot(c, r, v)

    def text_width(self, s: str, scale: int = 1, spacing: int = 1) -> int:
        if not s:
            return 0
        return len(s) * (GLYPH_W + spacing) * scale - spacing * scale

    def _glyph(self, ch: str, scale: int, spacing: int) -> Tuple[Tuple[int, int], ...]:
        key = (ch, scale, spacing)
        cached = self._glyph_cache.get(key)
        if cached is not None:
            return cached
        rows = FONT_3X5.get(ch, FONT_3X5["?"])
        dots: List[Tuple[int, int]] = []
        for ry, line in enumerate(rows):
            for rx, bit in enumerate(line):
                if bit != "#":
                    continue
                for sy in range(scale):
                    for sx in range(scale):
                        dots.append((rx * scale + sx, ry * scale + sy))
        out = tuple(dots)
        self._glyph_cache[key] = out
        return out

    def text(self, c: int, r: int, s: str, v: float = 1.0,
             scale: int = 1, spacing: int = 1) -> int:
        """在 (c, r) 处绘制点阵文字,返回结束列(不含)。"""
        cur = c
        for ch in s.upper():
            for dx, dy in self._glyph(ch, scale, spacing):
                self.dot(cur + dx, r + dy, v)
            cur += (GLYPH_W + spacing) * scale
        return cur - spacing * scale

    def cached_text(self, key: Tuple[str, int, int], s: str) -> Tuple[Tuple[int, int], ...]:
        """把一串文字的相对点坐标缓存起来,避免每帧重复解析字库。"""
        ent = self._text_dots.get(key)
        if ent is None or ent[0] != s:                     # type: ignore[index]
            dots: List[Tuple[int, int]] = []
            cur = 0
            _ch, scale, spacing = key
            for ch in s.upper():
                for dx, dy in self._glyph(ch, scale, spacing):
                    dots.append((cur + dx, dy))
                cur += (GLYPH_W + spacing) * scale
            ent = (s, tuple(dots))                         # type: ignore[assignment]
            self._text_dots[key] = ent                     # type: ignore[assignment]
        return ent[1]                                      # type: ignore[index]

    def blit(self, dots: Sequence[Tuple[int, int]], c: int, r: int, v: float) -> None:
        field = self.field
        cols, rows = self.cols, self.rows
        for dx, dy in dots:
            x, y = c + dx, r + dy
            if 0 <= x < cols and 0 <= y < rows and v > field[y, x]:
                field[y, x] = v

    # ------------------------------------------------- 静态层(网格/刻度/标题)
    def build_static(self) -> None:
        saved = self.field
        s = np.full((self.rows, self.cols), V_OFF, dtype=np.float32)
        # 外框(bezel): 最外一圈亮一点
        s[0, :] = s[-1, :] = V_BEZEL
        s[:, 0] = s[:, -1] = V_BEZEL
        s[1, 1:-1] = s[-2, 1:-1] = V_BEZEL * 0.35
        s[1:-1, 1] = s[1:-1, -2] = V_BEZEL * 0.35

        self.field = s
        # 横向网格(dB 线)
        for row, db in self.db_lines:
            major = abs(db) < 1e-6
            self.hline(self.plot_left, self.plot_right, row,
                       V_GRID_MAJOR if major else V_GRID, dash=2)
        # 纵向网格(频率线): 低清点阵下竖线会和信号柱糊在一起, 故不画;
        # 频率信息交给底部刻度线 + 标签表达, 背景只留横线更干净。
        # 坐标轴
        self.hline(self.plot_left - 1, self.plot_right, self.axis_row, V_AXIS)
        self.vline(self.plot_left - 1, self.plot_top, self.axis_row, V_AXIS)
        # 刻度线
        for f, major in self.ticks:
            col = int(round(self.freq_to_col(f)))
            self.vline(col, self.axis_tick_row,
                       self.axis_tick_row + (1 if major else 0), V_TICK)
        # 纵轴 dB 标签与底部状态行都已移除：版面全给频谱（信息由窗口标题承担）。
        # 横轴频率标签(自适应挑出不重叠的)
        for _f, col, label in self.tick_labels:
            self.text(col, self.axis_label_top, label, V_LABEL, 1, 1)
        # 声量计区的 dBFS 网格(faint 竖点线，与频谱的横网格同一种风格)
        db = METER_DB_MIN
        while db <= -0.01:
            col = self.meter_col_for_db(db)
            major = abs(abs(db) % 12.0) < 0.01
            self.vline(col, self.meter_top, self.meter_bottom,
                       V_GRID_MAJOR if major else V_GRID, 2)
            db += 6.0
        # 底部横向 dBFS 刻度(风格与频率刻度一致：短刻度线 + 数字，不写单位)
        db = METER_DB_MIN
        while db <= 0.01:
            col = self.meter_col_for_db(db)
            self.vline(col, self.dbfs_tick_row, self.dbfs_tick_row, V_TICK)
            if abs(abs(db) % 12.0) < 0.01:                    # 每 12 dB 一个数字
                label = "0" if db > -0.01 else "%d" % int(db)
                w_lab = self.text_width(label, 1, 1)
                lx = max(self.margin, min(col - w_lab // 2, self.cols - self.margin - w_lab))
                self.text(lx, self.dbfs_label_top, label, V_LABEL, 1, 1)
            db += 6.0
        # 标题(按可用宽度自动选字号)
        title_top = self.header_top + max(0, (self.header_h - GLYPH_H * self.title_scale) // 2)
        self.text(self.margin, title_top, self.title, V_TITLE, self.title_scale, 1)

        self.static = s
        self.field = saved
        np.copyto(self.field, self.static)
        np.copyto(self.persist, self.static)         # 起手就是网格本身, 不要黑屏渐亮

    # ------------------------------------------------------------ 磷光暂留
    def apply_persistence(self, dt: float) -> None:
        """把本帧目标亮度(field)混入余辉缓冲(persist), 模拟荧光粉的辉光暂留:

            p_t = beta * p_target + (1 - beta) * p_{t-1},  beta = 1 - exp(-dt / tau)

        这是逐"点阵像素"的时域一阶滤波——亮过的点不会立刻熄灭, 而是按指数拖尾,
        具体每一格的余辉强度由 beta 调制。beta 用 dt 换算, 因此与帧率无关。
        """
        tau = self.cfg.phosphor_tau_s
        if tau <= 1e-6:                                  # 关闭: 直接跟随目标
            np.copyto(self.persist, self.field)
            return
        beta = 1.0 - math.exp(-max(0.0, dt) / tau)
        self.persist *= (1.0 - beta)
        self.persist += beta * self.field
        # 声量计那一段**跳过暂留**：它自己已经带弹道（VU 300ms 积分 + 峰值 dB/s 回落），
        # 再叠一层磷光余辉会把读数整体抬高、也变得迟钝。渲染时的泛光仍然覆盖它。
        self.persist[self.meter_top:self.meter_bottom + 1, :] = \
            self.field[self.meter_top:self.meter_bottom + 1, :]

    # ------------------------------------------------------ 动态层(频谱/读数)
    def draw_spectrum(self, bars: np.ndarray, peaks: np.ndarray) -> None:
        n = self.plot_w
        if bars.size != n and bars.size > 0:
            # 频段数远多于显示列（4097 频段 -> 122 列）时，**每列取覆盖范围内的最大值**。
            # 不能用 np.interp：线性插值会把落在两根谱线之间的窄峰整根抹掉（实测很显眼）。
            src_n = bars.size
            if src_n >= n:
                lo = (np.arange(n) * src_n) // n
                bars = np.maximum.reduceat(bars, lo)
                peaks = np.maximum.reduceat(peaks, lo)
            else:                                          # 频段比列还少：最近邻放大
                idx = np.minimum((np.arange(n) * src_n) // n, src_n - 1)
                bars = bars[idx]
                peaks = peaks[idx]

        r0, r1 = self.plot_top, self.plot_bottom
        rows = np.arange(r0, r1 + 1, dtype=np.float32)[:, None]      # (R,1)
        height = np.clip(bars, 0.0, 1.0)[None, :] * (r1 - r0)        # (1,C) 高度(格)
        up = (r1 - rows)                                             # 距基线的格数(向上为正)
        lit = np.clip(height - up, 0.0, 1.0)                         # 柱顶软边
        depth = up / np.maximum(height, 1e-6)                        # 0=柱底 1=柱顶
        # 竖直方向磷光梯度: 柱底幽暗 -> 柱顶炽亮
        bright = (0.12 + 0.88 * np.clip(depth, 0.0, 1.0) ** 1.15) * lit
        bright *= np.clip(bars, 0.0, 1.0)[None, :] ** 0.30 + 0.02    # 弱信号更暗
        block = self.field[r0:r1 + 1, self.plot_left:self.plot_right + 1]
        np.maximum(block, np.clip(bright, 0.0, 1.0), out=block)

        # 峰值保持帽: 每一列都画(不再做可见性裁剪), 每列只点亮一个点(单像素,
        # 一格宽一格高)。不加左右/上下拖尾——在 row+1 与 col±1 补 0.45 亮度会把帽
        # 糊成一条又粗又脏的带子; 帽与柱顶的区分靠亮度承担(V_PEAK=1.0 vs 柱顶 <=0.6)。
        # 注意: 行号向下增大(r1=基线, r0=顶部), 与柱体用的 up = r1 - rows 保持一致,
        # 否则峰值帽会上下颠倒(衰减时反而往上飞)。
        peak_rows = np.clip(r1 - np.rint(np.clip(peaks, 0, 1) * (r1 - r0)),
                            r0, r1).astype(np.int64)
        cols = np.arange(self.plot_left, self.plot_right + 1)
        rows = peak_rows[:cols.size]
        # 注意: 花式索引 field[rows, cols] 返回的是副本, 不能拿它当 out= 写回
        cur = self.field[rows, cols]
        self.field[rows, cols] = np.maximum(cur, V_PEAK)

    def draw_readout(self, lines: Sequence[str]) -> None:
        """右上角读数(采样率/位深),右对齐。"""
        right = self.cols - self.margin
        top = self.header_top + max(0, (self.header_h - self.readout_h) // 2)
        rows = (top, top + GLYPH_H + 1)
        for line, row in zip(lines, rows):
            if not line:
                continue
            dots = self.cached_text((line, 1, 1), line)
            self.blit(dots, right - self.text_width(line, 1, 1), row, V_READOUT)

    # 注意: 原来还有一个 draw_status()(左下设备名 / 右下 VFD 字样)。这个状态行已经
    # 整行还给频谱了 —— 设备名与帧率改由窗口标题承担（插件端同理，放在 JUCE 状态栏）。

    # ------------------------------------------------------------ 输出纹理
    def to_rgba(self) -> np.ndarray:
        # 输出的是磷光余辉缓冲(而非本帧目标), 这样点阵才有荧光拖尾。
        # 泛光在**格域**做（与 src/vfd_render.h 的 Screen::render 同一套做法）：
        # 原来在 508x302 像素域做两级 box_blur，实测 3.6 ms/帧；挪到 127x75 格域后
        # 整帧 <1 ms —— 点阵屏的辉光本来就该以"点"为单位扩散。
        base = self.persist
        glow = box_blur_cells(base, 1) * 0.30 + box_blur_cells(base, 2) * 0.12
        cell = self.cell_px
        # 点亮区 = 自身磷光 + 泛光; 间隙只吃 0.55 倍泛光(留出点阵颗粒感)
        lit_dot = np.repeat(np.repeat(base + glow, cell, axis=0), cell, axis=1)
        lit_gap = np.repeat(np.repeat(glow * 0.55, cell, axis=0), cell, axis=1)
        lit = np.where(self._keep > 0.0, lit_dot, lit_gap)
        lit *= self._vignette
        np.clip(lit, 0.0, 1.0, out=lit)
        idx = (lit * (LUT_SIZE - 1)).astype(np.int32)
        return self._lut[idx]


# ============================================================================
#  8. 应用程序 (DearPyGui)
# ============================================================================

WINDOW_TITLE = "VFD Spectrum Analyzer"


class SpectrumApp:
    def __init__(self, cfg: Config) -> None:
        self.cfg = cfg
        self.capture = LoopbackCapture(cfg)
        self.screen: Optional[VfdScreen] = None
        self.engine: Optional[SpectrumEngine] = None
        self.meters = MeterBank()                        # 声量计/峰值计（L/R）
        self.tex_tag, self.img_tag, self.reg_tag = "vfd_tex", "vfd_img", "vfd_reg"
        self.fps_ema = 0.0
        self._pending_size: Optional[Tuple[int, int]] = None
        self._last_total = -1
        self._stale_since = time.monotonic()
        self._readout: Tuple[str, ...] = ("", "")
        self._status_left, self._status_right = "", ""

    # ------------------------------------------------------------------ UI
    def build(self) -> None:
        import dearpygui.dearpygui as dpg
        dpg.create_context()
        try:                                             # 尽量去掉窗口内边距
            with dpg.theme() as theme:
                with dpg.theme_component(dpg.mvAll):
                    dpg.add_theme_color(dpg.mvThemeCol_WindowBg, (3, 6, 5, 255))
                    dpg.add_theme_style(dpg.mvStyleVar_WindowPadding, 0, 0)
                    dpg.add_theme_style(dpg.mvStyleVar_WindowBorderSize, 0, 0)
            dpg.bind_theme(theme)
        except Exception:                                # noqa: BLE001
            pass
        with dpg.texture_registry(tag=self.reg_tag, show=False):
            dpg.add_dynamic_texture(4, 4, [0.0] * 64, tag=self.tex_tag)
        with dpg.window(tag="main", no_title_bar=True, no_resize=True,
                        no_move=True, no_scrollbar=True, no_collapse=True):
            dpg.add_image(self.tex_tag, tag=self.img_tag)
        dpg.set_primary_window("main", True)
        dpg.create_viewport(title=WINDOW_TITLE, width=self.cfg.window_w,
                            height=self.cfg.window_h, resizable=True, vsync=True)
        dpg.setup_dearpygui()
        dpg.set_viewport_resize_callback(self._on_resize)
        dpg.show_viewport()
        w = dpg.get_viewport_client_width()
        h = dpg.get_viewport_client_height()
        # 客户区尺寸此刻已经是有效值(实测), 先按它建一屏, 再把边框补偿掉。
        # 若已经正好等于目标就无需补偿, 直接按这一屏报告。
        dx = self.cfg.window_w - w
        dy = self.cfg.window_h - h
        self._rebuild(max(320, w), max(240, h), report=not (dx or dy))
        if dx or dy:
            self._align_client_size(dx, dy)

    def _align_client_size(self, dx: int, dy: int) -> None:
        """把客户区尺寸补偿到 cfg.window_w x cfg.window_h(改窗户尺寸, 不是网格)。

        dearpygui 的 viewport 尺寸是"含标题栏/边框"的窗口尺寸(ST_CreateViewport 实测:
        请求 1532x966 只得到 1516x927 的客户区), 而网格是按客户区算的
        (客户区 // (cell * scale))。按实测边框厚度补偿后, 初始状态才正好是
        127x80 格 / 纹理 508x320 / 客户区 1532x966。
        补偿不在这里等尺寸生效(主循环里 split_frame 会卡), 而是让窗口尺寸变化触发的
        _on_resize -> _frame -> _rebuild 走正常重排流程, 由那一步报告最终网格。
        """
        import dearpygui.dearpygui as dpg
        dpg.set_viewport_width(self.cfg.window_w + dx)
        dpg.set_viewport_height(self.cfg.window_h + dy)

    def _on_resize(self) -> None:
        import dearpygui.dearpygui as dpg
        try:
            self._pending_size = (dpg.get_viewport_client_width(),
                                  dpg.get_viewport_client_height())
        except Exception:                                # noqa: BLE001
            pass

    def _rebuild(self, client_w: int, client_h: int, report: bool = True) -> None:
        import dearpygui.dearpygui as dpg
        cfg = self.cfg
        cols = max(80, int(client_w) // (cfg.cell * cfg.pixel_scale))
        rows = max(48, int(client_h) // (cfg.cell * cfg.pixel_scale))
        self.screen = VfdScreen(cols, rows, cfg)
        self.screen.build_static()
        if self.engine is None:
            self.engine = SpectrumEngine(cfg, cfg.bands,
                                         self.capture.fmt.samplerate or 48000)
        else:
            self.engine.retune(cfg.bands, self.capture.fmt.samplerate or 48000)
        rgba = self.screen.to_rgba()
        if dpg.does_item_exist(self.img_tag):
            dpg.delete_item(self.img_tag)
        if dpg.does_item_exist(self.tex_tag):
            dpg.delete_item(self.tex_tag)
        dpg.add_dynamic_texture(rgba.shape[1], rgba.shape[0], rgba.ravel(),
                                parent=self.reg_tag, tag=self.tex_tag)
        dpg.add_image(self.tex_tag, parent="main", tag=self.img_tag,
                      width=client_w, height=client_h)
        if report:
            print("[vfd] 点阵网格 %dx%d 格, 纹理 %dx%d, 窗口 %dx%d"
                  % (cols, rows, rgba.shape[1], rgba.shape[0], client_w, client_h),
                  flush=True)

    # -------------------------------------------------------------- 每帧更新
    def _frame(self, dt: float) -> None:
        import dearpygui.dearpygui as dpg
        if self._pending_size:
            w, h = self._pending_size
            self._pending_size = None
            if (w // (self.cfg.cell * self.cfg.pixel_scale),
                    h // (self.cfg.cell * self.cfg.pixel_scale)) != \
                    (self.screen.cols, self.screen.rows):
                self._rebuild(w, h)

        cap, screen, engine = self.capture, self.screen, self.engine
        assert screen is not None and engine is not None

        total = cap.ring.total
        if total != self._last_total:
            self._last_total = total
            self._stale_since = time.monotonic()
        signal_lost = (time.monotonic() - self._stale_since) > 2.0

        engine.retune(self.cfg.bands, cap.fmt.samplerate or 48000)
        samples = cap.ring.latest(engine.nfft)
        bars, peaks = engine.process(samples, dt)

        screen.clear_dynamic()
        screen.draw_spectrum(bars, peaks)
        screen.draw_readout(self._readout)
        if self.meters is not None:
            # 抽干（read_new 只少给不丢），否则声量计会越落越远
            for _ in range(8):
                chunk = cap.sring.read_new(
                    max(1, int(dt * (cap.fmt.samplerate or 48000) * 1.5)))
                if chunk.shape[0] == 0:
                    break
                self.meters.push(chunk)
            self.meters.process(dt)
            screen.draw_meters(self.meters.vu, self.meters.peak)
        screen.apply_persistence(dt)                     # 磷光暂留(逐点亮度)
        dpg.set_value(self.tex_tag, screen.to_rgba().ravel())

        # 状态行已经还给频谱了：设备名/帧率/分辨率/丢信号提示改放窗口标题
        dpg.set_viewport_title("%s | %s | %.0fFPS | FFT%d/%.2fHz/%.0f bands%s"
                               % (WINDOW_TITLE, self._status_left or "-", self.fps_ema,
                                  engine.nfft, engine.samplerate / engine.nfft / 1.0,
                                  engine.nbands,
                                  "  !! NO SIGNAL" if signal_lost else ""))

    def _update_text(self) -> None:
        cap = self.capture
        fmt = cap.fmt
        self._readout = (fmt.rate_label(), "%s %dCH" % (fmt.bit_label(), fmt.channels))
        if cap.state == "running":
            self._status_left = cap.device_name
        else:
            self._status_left = cap.message or "WAITING FOR AUDIO"

    # ---------------------------------------------------------------- 主循环
    def run(self) -> None:
        import dearpygui.dearpygui as dpg
        self.build()
        self.capture.start()
        t_prev = time.perf_counter()
        period = 1.0 / self.cfg.fps if self.cfg.fps else 0.0
        frames = 0
        try:
            while dpg.is_dearpygui_running():
                t0 = time.perf_counter()
                dt = max(1e-4, t0 - t_prev)
                t_prev = t0
                self.fps_ema = 0.9 * self.fps_ema + 0.1 * (1.0 / dt)
                self._update_text()
                self._frame(dt)
                dpg.render_dearpygui_frame()
                frames += 1
                if self.cfg.max_frames and frames >= self.cfg.max_frames:
                    print("[vfd] 自检结束: 已渲染 %d 帧" % frames, flush=True)
                    break
                if period:
                    rest = period - (time.perf_counter() - t0)
                    if rest > 0:
                        time.sleep(rest)
        except KeyboardInterrupt:
            pass
        finally:
            self.capture.stop()
            dpg.destroy_context()


# ============================================================================
#  9. 离线预览 (不开窗口,直接出 PNG,方便调视觉)
# ============================================================================

def _synth_signal(n: int, sr: int, t0: float, rng: np.random.Generator) -> np.ndarray:
    """合成一段"像音乐"的信号: 若干对数分布的乐音 + 宽带底噪。"""
    t = t0 + np.arange(n, dtype=np.float32) / sr
    out = np.zeros(n, dtype=np.float32)
    tones = getattr(_synth_signal, "_tones", None)
    if tones is None:
        freqs = np.geomspace(45.0, 12000.0, 14)
        amps = 10 ** (rng.uniform(-1.5, -0.15, freqs.size))
        phases = rng.uniform(0, 2 * np.pi, freqs.size)
        rates = rng.uniform(0.3, 2.0, freqs.size)
        tones = (freqs, amps, phases, rates)
        _synth_signal._tones = tones                        # type: ignore[attr-defined]
    freqs, amps, phases, rates = tones
    for i in range(freqs.size):
        env = 0.55 + 0.45 * np.sin(2 * np.pi * rates[i] * t + phases[i])
        out += (amps[i] * env * np.sin(2 * np.pi * freqs[i] * t)).astype(np.float32)
    noise = rng.normal(0, 0.00035, n).astype(np.float32)
    out += noise
    out += 0.02 * np.sin(2 * np.pi * 55.0 * t).astype(np.float32)
    return np.tanh(out * 1.1).astype(np.float32)


def preview(out_path: str, cfg: Config, frames: int = 150,
            size: Optional[Tuple[int, int]] = None) -> None:
    """用合成信号跑一遍完整 DSP+渲染管线,输出 PNG。

    size = (宽, 高) 原生像素；不给就按窗口尺寸推。做"黄金帧"时直接给 508x302，
    并配 --scale 1，出图就与 C++ 的 render_bench --png 尺寸完全一致。
    """
    rng = np.random.default_rng(7)
    sr = 48000
    win_w, win_h = size if size else (cfg.window_w, cfg.window_h)
    cols = max(80, win_w // (cfg.cell * cfg.pixel_scale))
    rows = max(48, win_h // (cfg.cell * cfg.pixel_scale))
    screen = VfdScreen(cols, rows, cfg)
    screen.build_static()
    engine = SpectrumEngine(cfg, cfg.bands, sr)
    print("[preview] 网格 %dx%d, FFT %d (%.1f ms, 窗 %s), bin %.2f Hz, 频段 %d (自动=%s, "
          "其中插值 %d)"
          % (cols, rows, engine.nfft, 1000.0 * engine.nfft / sr, cfg.window,
             sr / engine.nfft, engine.nbands, cfg.bands <= 0, engine.sub_bin_bands),
          flush=True)

    n = engine.nfft
    chunk = max(1, n // 3)
    buf = np.zeros(4 * n, dtype=np.float32)
    t = 0.0
    dt = 1.0 / 60.0
    meters = MeterBank()
    for i in range(frames):
        piece = _synth_signal(chunk, sr, t, rng)
        t += chunk / sr
        buf = np.roll(buf, -chunk)
        buf[-chunk:] = piece
        bars, peaks = engine.process(buf, dt)
        # 每帧都要画 + 暂留, 余辉才会累积(否则最后一帧只有单帧余辉);
        # 只有 to_rgba() 这种重活留到最后一次, 免得预览慢几十倍。
        screen.clear_dynamic()
        screen.draw_spectrum(bars, peaks)
        screen.draw_readout(["%dHZ" % sr, "%s %dCH" % ("24BIT", 2)])
        # 声量计：右声道取一半电平，肉眼能看出两条不一样
        meters.push(np.stack([piece, piece * 0.5], axis=1))
        meters.process(dt)
        screen.draw_meters(meters.vu, meters.peak)
        screen.apply_persistence(dt)                     # 磷光暂留(逐点亮度)
    rgba = screen.to_rgba()
    # 按 pixel_scale 放大到"屏幕上的样子"(最近邻,保持点阵硬边)
    if cfg.pixel_scale > 1:
        rgba = np.repeat(np.repeat(rgba, cfg.pixel_scale, axis=0),
                         cfg.pixel_scale, axis=1)
    u8 = (np.clip(rgba, 0, 1) * 255).astype(np.uint8)
    write_png(out_path, u8)
    print("[preview] 已写出 %s (%dx%d, 放大 %dx)"
          % (out_path, u8.shape[1], u8.shape[0], cfg.pixel_scale), flush=True)


# ============================================================================
#  10. 命令行
# ============================================================================

def list_devices() -> None:
    import soundcard as sc
    default = sc.default_speaker()
    print("默认输出设备: %s" % default.name)
    print("\n可回环采集的输出设备:")
    for spk in sc.all_speakers():
        fmt = read_device_format(str(spk.id))
        mark = "*" if str(spk.id) == str(default.id) else " "
        print("  %s %-45s %6dHz %2dbit %dch  (%s)"
              % (mark, spk.name, fmt.samplerate, fmt.bits, fmt.channels, fmt.source))


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="VFD 点阵荧光频谱分析仪",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    p.add_argument("--device", help="输出设备名称子串(默认用系统默认输出)")
    p.add_argument("--list-devices", action="store_true", help="列出可采集设备后退出")
    p.add_argument("--scale", type=int, default=3, help="点阵放大倍数(原生像素:屏幕像素)")
    p.add_argument("--cell", type=int, default=4, help="点阵单元边长(原生像素), 2 = 更细密")
    p.add_argument("--dot", type=int, default=3, help="单元内点亮的边长(其余为间隙)")
    p.add_argument("--fps", type=int, default=60, help="目标帧率")
    p.add_argument("--release", type=float, default=55.0, help="荧光衰减速度 dB/s")
    p.add_argument("--peak-release", type=float, default=21.0,
                   help="峰值帽掉落速度 dB/s(默认是荧光衰减的约 3 倍, 掉得更干脆)")
    p.add_argument("--phosphor", type=float, default=0.070,
                   help="点阵像素磷光暂留时间常数, 秒; 0 = 关闭(越小越干脆)")
    p.add_argument("--frames", type=int, default=0,
                   help="渲染 N 帧后自动退出(0 = 一直运行), 用于自检")
    p.add_argument("--tilt", type=float, default=3.0, help="粉噪倾斜补偿 dB/decade")
    p.add_argument("--gain-db", type=float, default=0.0, help="显示增益偏移 dB")
    p.add_argument("--db-min", type=float, default=-78.0, help="显示下限 dB")
    p.add_argument("--fft-seconds", type=float, default=0.18,
                   help="FFT 窗长(秒) —— 分辨率只由它决定: bin 宽 = 采样率/2^order")
    p.add_argument("--fft-order", type=int, default=0,
                   help="直接指定 FFT 点数的 2 的幂(如 15 = 32768), 0 = 按窗长自动")
    p.add_argument("--bands", type=int, default=0,
                   help="频段数, 0 = 自动 = 频点数(bins), 一个 bin 都不丢")
    p.add_argument("--window", default="bh",
                   help="FFT 窗: rect | hann | blackman | bh(默认, 旁瓣 -92dB 看不到裙边)")
    p.add_argument("--preview", metavar="PNG", help="离线渲染一张预览图后退出")
    p.add_argument("--preview-size", metavar="WxH",
                   help="预览图的原生像素尺寸(如 508x302), 配合 --scale 1 可做黄金帧")
    p.add_argument("--preview-frames", type=int, default=150, help="预览跑的帧数")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    if args.list_devices:
        list_devices()
        return 0

    cfg = Config(device=args.device, pixel_scale=max(1, args.scale), fps=args.fps,
                 cell=max(1, args.cell), dot=max(1, args.dot),
                 release_db_s=args.release, tilt_db_per_decade=args.tilt,
                 peak_release_db_s=args.peak_release,
                 phosphor_tau_s=max(0.0, args.phosphor),
                 max_frames=max(0, args.frames),
                 gain_db=args.gain_db, db_min=args.db_min,
                 fft_seconds=max(0.01, args.fft_seconds), fft_order=max(0, args.fft_order),
                 bands=max(0, args.bands), window=args.window)
    if args.preview:
        size = None
        if args.preview_size:
            w, _, h = args.preview_size.lower().partition("x")
            size = (int(w), int(h))
        preview(args.preview, cfg, frames=max(1, args.preview_frames), size=size)
        return 0

    SpectrumApp(cfg).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
