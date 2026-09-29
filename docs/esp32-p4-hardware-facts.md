# ESP32-P4 hardware facts — feasibility input for a 508×302 @60 fps dot-matrix spectrum renderer

Scope: sourced hardware numbers only. Everything unverified is listed under **UNCERTAINTIES**.
Primary source abbreviations used in the table:
- **DS** = ESP32-P4 Series Datasheet v0.7 (preliminary) — https://documentation.espressif.com/esp32-p4_datasheet_en.html
- **PROD** = Espressif product page — https://www.espressif.com/en/products/socs/esp32-p4
- **IDF** = ESP-IDF P4 docs (v6.1) — paths given per row
- **DSP** = ESP-DSP benchmark table — https://docs.espressif.com/projects/esp-dsp/en/latest/esp32/esp-dsp-benchmarks.html

---

## KEY NUMBERS

| Quantity | Value | Source URL |
|---|---|---|
| HP cores / arch / max clock | 2× 32-bit RISC-V, RV32IMAFC, 5-stage pipeline, up to **400 MHz** | https://documentation.espressif.com/esp32-p4_datasheet_en.html |
| CoreMark (dual-core) | **6.92 CoreMark/MHz** | same (DS Features) |
| LP core | 1× RISC-V single-core, RV32IMAC, 2-stage pipeline, up to **40 MHz** | same |
| ISA extensions | Zc (Zcb/Zcmp/Zcmt), Zb, custom **XespV** AI/DSP ext., **XespLoop** hw loop | same (DS §4.1.1.1) |
| Vector/SIMD width | **128-bit** GP registers + 128-bit vector ops (complex mul/add/sub/mul/shift/compare), aligned & unaligned 128-bit load/store | same (DS §4.1.1.3) |
| FPU | single-precision FPU (RV32**F**), stated on product page | https://www.espressif.com/en/products/socs/esp32-p4 |
| HP L2MEM (main SRAM) | **768 KB @ 200 MHz** | https://documentation.espressif.com/esp32-p4_datasheet_en.html |
| HP ROM | 128 KB @ 200 MHz | same |
| SPM / TCM | **8 KB @ 400 MHz** scratchpad; 1-cycle latency (4 cycles if parity enabled) | DS §4.1.3.1 + https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-guides/memory-types.html |
| LP SRAM / LP ROM | 32 KB @ 40 MHz / 16 KB @ 40 MHz | https://documentation.espressif.com/esp32-p4_datasheet_en.html |
| Internal RAM seen by app at boot (P4 dev board, IDF 5.4.1) | heap regions 166 KiB + 18 KiB + 384 KiB + 31 KiB RTCRAM + 7 KiB TCM; **min free heap 606 120 B (≈592 KiB)** | https://www.cnx-software.com/2025/05/18/m5stack-tab5-review-getting-started-esp32-p4-esp-idf-framework-arduino-ide/ |
| Internal SRAM reported free with 32 MB PSRAM active | **569 KB "available SRAM"** (benchmark config) | https://github.com/ctag-fh-kiel/-esp32p4_memcpy_pie_benchmark |
| Cache | L1I 16 KB (4-way), L1D 64 KB (2-way, write-through/back), **L2 128/256/512 KB** (8-way) | https://documentation.espressif.com/esp32-p4_datasheet_en.html |
| In-package PSRAM | **16 MB or 32 MB**, 16 data lines ("sixteen-line"), 1.8 V, OPI/HPI, DDR | same (DS §1.1–1.2, §2.7) |
| PSRAM max clock | **250 MHz** (DS §4.1.3.1). DS Table 5-9 also prints "max clock 80 MHz" — contradictory, see UNCERTAINTIES | same |
| PSRAM max size supported | up to **64 MB** storage, 64 MB map window (64 KB MMU pages) | same |
| PSRAM theoretical bandwidth | `16 lines × 2 edges × 250 MHz` = **8 Gbit/s (≈1000 MB/s)** | same |
| PSRAM measured copy bandwidth (64 KB blocks, cached) | PSRAM→SRAM 99.4–116.3 MB/s; SRAM→PSRAM 104.4–118.0 MB/s; PSRAM→PSRAM 61.5–65.5 MB/s; SRAM→SRAM 365–758 MB/s | https://github.com/ctag-fh-kiel/-esp32p4_memcpy_pie_benchmark |
| MIPI-DSI | **2 lanes × 1.5 Gbps** max (DPHY v1.1); **video mode only, no command mode** | https://documentation.espressif.com/esp32-p4_datasheet_en.html + https://docs.espressif.com/projects/esp-techpedia/en/latest/esp-friends/advanced-development/lcd-application-note/mipi-dsi-summary.html |
| DSI lane rate limits | max **≤1.5 Gbps**, Espressif recommends **≥480 Mbps** per lane | https://docs.espressif.com/projects/esp-techpedia/en/latest/esp-friends/advanced-development/lcd-application-note/mipi-dsi-summary.html |
| MIPI-CSI | 2 lanes × 1.5 Gbps, DPHY v1.1 | https://documentation.espressif.com/esp32-p4_datasheet_en.html |
| Parallel LCD (LCD_CAM) | 8/16/24-bit parallel output; RGB / MOTO6800 / I8080; via GDMA; simultaneous with camera | same (DS §4.2.1.4) |
| PPA | SRM (rotate 0/90/180/270, mirror, scale w/ 4-bit int + 8-bit frac, **bilinear**) + BLEND (alpha, colour-key) + FILL | same (DS §4.2.1.3) + https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/ppa.html |
| PPA measured speed-up (LVGL) | avg **~30 %** rendering-time saving for image/rect fill; up to **9×** for pure fills at integer multiples of display size; rotation saves up to **40 %** | https://lvgl.io/docs/open/9.4/details/integration/chip_vendors/espressif/hardware_accelerator_ppa.html |
| 2D-DMA | 1 AXI master; 4 mem→periph + 3 periph→mem channels; macroblock reorder; **colour-space conversion**; serves PPA + JPEG | https://documentation.espressif.com/esp32-p4_datasheet_en.html |
| JPEG codec | still up to 4K; dynamic **1080p@40 fps / 720p@70 fps** encode **and** decode | same |
| H.264 encoder | **1080p@30 fps** (YUV420), baseline | same |
| I2S controllers | **3× HP I2S + 1× LP I2S**; 8/16/24/32-bit; TDM up to 16 slots; PDM; **GDMA-AHB**; APLL ≤125 MHz | same (DS §4.2.2.6–4.2.2.7) |
| ESP-DSP FFT f32 radix-2, P4, asm `-O2` (cycles) | 64 pt **4 989**; 128 **11 404**; 256 **25 684**; 512 **57 181**; 1024 **126 053** | https://docs.espressif.com/projects/esp-dsp/en/latest/esp32/esp-dsp-benchmarks.html |
| ESP-DSP FFT f32 radix-4, P4, `-O2` (cycles) | 64 pt **3 483**; 256 **18 052**; 1024 **89 820** | same |
| ESP-DSP FFT s16 radix-2, P4, `-O2` (cycles) | 64 pt **897**; 1024 **17 523** | same |
| ESP-DSP FFT f32 radix-2, **ESP32-S3** (different silicon) | 1024 pt **97 847** cycles | same |
| Wireless | **P4 has no Wi-Fi/BT radio.** Companion ESP32-C/S via SPI/SDIO/UART using ESP-Hosted or ESP-AT | https://www.espressif.com/en/products/socs/esp32-p4 |

*Cycle→time conversions (P4 µs) are my arithmetic at 400 MHz, not published — see DERIVED section.*

---

## 1. CPU

- **Two HP cores, both 32-bit RISC-V (RV32IMAFC + Zb/Zc + XespV + XespLoop), 5-stage pipeline, up to 400 MHz.** Documented as a symmetric "dual-core processor"; the only asymmetry the datasheet states is reset behaviour: *HP CPU0 is auto-released after power-up; HP CPU1 is held in reset by default and must be released manually* ([DS §4.1.1.1, §4.1.4.2](https://documentation.espressif.com/esp32-p4_datasheet_en.html)).
- **LP core:** separate 32-bit RISC-V single core, RV32IMAC, 2-stage pipeline, **up to 40 MHz**; stays powered in deep sleep; can access HP memory and the whole peripheral address space ([DS §4.1.1.4](https://documentation.espressif.com/esp32-p4_datasheet_en.html)).
- **FPU/SIMD:** single-precision FPU per product page; the custom **XespV** extension adds **eight 128-bit general-purpose registers** with 128-bit vector multiply/add/sub/shift/compare including complex multiplication, plus 128-bit aligned/unaligned load-store, configurable rounding and saturation ([DS §4.1.1.3](https://documentation.espressif.com/esp32-p4_datasheet_en.html)). A third-party SIMD memcpy benchmark confirms the 128-bit vector path in practice (16 bytes/instruction, 2.08× faster than libc memcpy for SRAM→SRAM) ([repo](https://github.com/ctag-fh-kiel/-esp32p4_memcpy_pie_benchmark)).
- **Clock tree** ([DS §4.1.4.3](https://documentation.espressif.com/esp32-p4_datasheet_en.html)): XTAL 40 MHz reference; **CPLL 400 MHz**, **MPLL 500 MHz**, **SPLL 480 MHz**; slow domain: XTAL32K, RC_SLOW (~150 kHz), RC_FAST (20 MHz default), PLL_LP (8 MHz). Named peripheral clocks in the datasheet: `PLL_F80M_CLK`, `PLL_F160M_CLK`, APB_CLK, XTAL_CLK.
- Verifiable peripheral ceilings that matter here: GP-SPI master **≤80 MHz**; LP-SPI **≤40 MHz**; PARLIO IO clock **≤40 MHz**; I2S APLL **≤125 MHz** ([DS §4.2.2.2, §4.2.2.6, §4.2.2.18](https://documentation.espressif.com/esp32-p4_datasheet_en.html)).
- Default CPU frequency observed on a real board boot log (IDF 5.4.1, M5Stack Tab5): **360 MHz**, not 400 MHz ([CNX log](https://www.cnx-software.com/2025/05/18/m5stack-tab5-review-getting-started-esp32-p4-esp-idf-framework-arduino-ide/)). Same for the Arduino ESP32-P4 board define (`-DF_CPU=360000000L`).

## 2. Internal SRAM

- **768 KB HP L2MEM @ 200 MHz** is the headline SRAM figure, plus **8 KB SPM/TCM @ 400 MHz** (zero-wait, 1-cycle latency without parity) and 128 KB HP ROM ([DS §4.1.3.1](https://documentation.espressif.com/esp32-p4_datasheet_en.html), [product page](https://www.espressif.com/en/products/socs/esp32-p4)).
- There is **no documented 768 KB "split"** other than L2MEM vs the 8 KB SPM; the other memories are LP SRAM (32 KB) and LP ROM (16 KB).
- **How much reaches the application:** with no PSRAM configured, a real P4 boot log reports heap regions of 166 KiB + 18 KiB + 384 KiB (+31 KiB RTCRAM, +7 KiB TCM) and *"Minimum free heap size: 606120 bytes"* ≈ **592 KiB** ([CNX log](https://www.cnx-software.com/2025/05/18/m5stack-tab5-review-getting-started-esp32-p4-esp-idf-framework-arduino-ide/)).
- With **32 MB PSRAM enabled**, a benchmark project reports **"Available SRAM: 569 KB"** on the same chip ([repo](https://github.com/ctag-fh-kiel/-esp32p4_memcpy_pie_benchmark)).
- Reason for the difference: part of L2MEM becomes **L2 cache** (configurable 128/256/512 KB, 8-way) when external memory is used, and cache is shared between flash and PSRAM ([DS §4.1.3.3](https://documentation.espressif.com/esp32-p4_datasheet_en.html)).

## 3. PSRAM

- **Sizes:** in-package 16 MB (ESP32-P4NRW16/16X) or 32 MB (ESP32-P4NRW32/32X), OPI/HPI, 1.8 V; datasheet says PSRAM is *not pinned out*. PSRAM can also be connected off-package via SPI/Dual/Quad/QPI, with external memory max clock **120 MHz** ([DS §1.1–1.2, §2.7, §4.1.3.1](https://documentation.espressif.com/esp32-p4_datasheet_en.html)).
- **Interface width / pins:** 16 data lines ("sixteen-line"), **DDR** (data on both clock edges), max clock **250 MHz** ([DS §4.1.3.1](https://documentation.espressif.com/esp32-p4_datasheet_en.html)).
- **Espressif's published bandwidth formula and figure:**
  `Max theoretical bandwidth = line_num × edge_mode × PSRAM_max_freq` → `16 × 2 × 250 MHz = 8 Gbit/s` (**≈1000 MB/s**) ([DS §4.1.3.1](https://documentation.espressif.com/esp32-p4_datasheet_en.html)).
- **Measured throughput (community benchmark, 64 KB buffers, cached, byte-verified):**

  | Direction | Standard memcpy | Best SIMD impl. |
  |---|---|---|
  | PSRAM → SRAM | 99.4 MB/s | **116.3 MB/s** |
  | SRAM → PSRAM | 104.4 MB/s | **118.0 MB/s** |
  | PSRAM → PSRAM | 61.5 MB/s | **65.5 MB/s** |
  | SRAM → SRAM | 365.3 MB/s | **758.5 MB/s** |

  The repo itself labels PSRAM "theoretical max ~200 MB/s" and says PSRAM transfers are interface-limited ([repo](https://github.com/ctag-fh-kiel/-esp32p4_memcpy_pie_benchmark)). *Note the gap: Espressif's 8 Gbit/s theoretical vs ~100–120 MB/s measured — see UNCERTAINTIES.*
- **Is PSRAM cached?** Yes. P4 has a **two-level cache**; PSRAM is mapped 64 KB blocks into a 64 MB instruction/data space with 8/16/32/**128-bit** read-write, and supports cacheable and non-cacheable access, preload and lock. ESP-IDF states that external RAM shares the cache region with flash, so *"frequently accessed variables in external RAM can be read and modified almost as quickly as in internal RAM"*, but **for accesses >32 KB the cache is insufficient and speed falls back to raw external-RAM speed** ([DS §4.1.3.3](https://documentation.espressif.com/esp32-p4_datasheet_en.html), [IDF external RAM](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-guides/external-ram.html)).
- **Practical config caveat:** PSRAM **200 MHz** is not offered by default in `menuconfig`; an Espressif collaborator confirmed you must first enable *experimental features* to expose the 200 MHz option ([esp-idf issue #15831](https://github.com/espressif/esp-idf/issues/15831)).

## 4. Display pipeline

- **MIPI-DSI:** compliant with MIPI DSI + DPHY v1.1, **2 lanes × 1.5 Gbps**, video mode output, input formats RGB888/666/565, YUV422/420, GRAY; output RGB888/RGB666/RGB565. Dedicated pins 34–40 (needs a 4.02 kΩ `DSI_REXT`) ([DS §4.2.1.7, §2.4](https://documentation.espressif.com/esp32-p4_datasheet_en.html)).
- **Pixel-throughput / lane-rate ceiling:** Espressif's own LCD application note states *"on the ESP32-P4 platform, the maximum `lane_bit_rate_mbps` **≤ 1.5 Gbps**, and the minimum is recommended to be **≥ 480 Mbps**"*, and gives the design formula `pixel_clock = (H + HBP + HFP + HSPW) × (V + VBP + VFP + VSPW) × refresh`, with `×1.25` for 8b/10b, `×1.15` for DSI overhead, divided by lanes, `×1.2` margin ([ESP-Techpedia MIPI-DSI summary](https://docs.espressif.com/projects/esp-techpedia/en/latest/esp-friends/advanced-development/lcd-application-note/mipi-dsi-summary.html)). It also states P4 supports **up to 2 lanes** while most market panels are 4-lane but usually reconfigurable to 2-lane, and that **only video mode is supported (no command/DSI-command mode)**.
- **Other panel interfaces:** the LCD/Camera controller (**LCD_CAM**) drives **8/16/24-bit parallel** panels in **RGB, MOTO6800 and I8080** formats; it can be connected to an external LCD and camera simultaneously; frame data comes from internal or external memory via GDMA. RGB signals are routed through the GPIO matrix on P4. Frame-buffer modes offered by the driver: single FB in internal RAM, single/double FB in PSRAM, user FB, bounce-buffer + PSRAM FB, and bounce-buffer-only (procedurally generated frames) ([DS §4.2.1.4](https://documentation.espressif.com/esp32-p4_datasheet_en.html), [IDF RGB LCD](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/lcd/rgb_lcd.html)).
  - Important documented PSRAM interaction for RGB panels: when the FB is in PSRAM, *"when both the CPU and EDMA need access to PSRAM, the bandwidth is shared between them, meaning EDMA and the CPU each get half… a high pixel clock might cause LCD peripheral starvation, leading to display corruption."* The bounce-buffer mode allows a **higher pixel clock** but consumes CPU and cannot run while the external-memory cache is disabled ([IDF RGB LCD](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/lcd/rgb_lcd.html)).
  - SPI panels: GP-SPI2 master up to **80 MHz** ([DS §4.2.2.2](https://documentation.espressif.com/esp32-p4_datasheet_en.html)).
- **Accelerators:**
  - **PPA** = SRM (0/90/180/270 CCW rotation, H/V mirror, H/V scale with **4-bit integer + 8-bit fractional** factor, internally **bilinear**) + **BLEND** (two same-size layers, alpha, colour-key ranges, A4/A8/ARGB8888/RGB888/RGB565/L4/L8 inputs) + **FILL**. Input/output formats RGB888/666/565, YUV422/420, GRAY ([DS §4.2.1.3](https://documentation.espressif.com/esp32-p4_datasheet_en.html), [PPA API](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/ppa.html)).
  - **No pixel/clock throughput spec is published for the PPA.** Espressif documents that PPA time is proportional to block data and *"highly relies on the PSRAM bandwidth"*, degrading when other peripherals hit PSRAM concurrently; burst length is configurable (8/16/32/64/128 bytes, default max 128) and a smaller burst lowers PPA performance ([PPA API](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/ppa.html)). LVGL reports the only quantitative PPA figures I found: ~**30 %** average render-time saving on fill/image draw tasks, up to **9×** for pure fills at integer multiples of the display size, ~**40 %** saving when PPA does rotation instead of software, and that image *blending* shows "no significant gains" because of DMA-2D memory bandwidth ([LVGL PPA docs](https://lvgl.io/docs/open/9.4/details/integration/chip_vendors/espressif/hardware_accelerator_ppa.html)).
  - **2D-DMA**: dedicated 2D image DMA — 1 AXI master, unaligned start addresses, 4 mem→peripheral + 3 peripheral→memory channels, **macroblock reordering**, **colour-space conversion**, supports PPA and JPEG codec, memory-to-memory with CSC ([DS §4.1.2.3](https://documentation.espressif.com/esp32-p4_datasheet_en.html)).
  - **JPEG codec**: baseline JPEG, encoder and decoder, still up to **4K**, dynamic **1080p@40 fps / 720p@70 fps** (excluding header time) ([DS §4.2.1.1](https://documentation.espressif.com/esp32-p4_datasheet_en.html)).
  - **H.264 encoder**: baseline, **1080p@30 fps** max (YUV420), I/P frames, GOP + dual-stream, ROI ([DS §4.2.1.5](https://documentation.espressif.com/esp32-p4_datasheet_en.html)).
  - Software (non-PPA) image-effect throughput on P4 for reference, 1920×1080 RGB565: crop to full 1920×1080 = **21.3 fps**, rotate 0° = 20.9 fps / 90° = 14.4 fps, scale 1920×1080→1920×1080 = 4.04 fps, RGB565→RGB888 colour convert ≈ 26.3 fps ([Espressif esp_image_effects perf doc](https://github.com/espressif/esp-adf-libs/blob/master/esp_image_effects/doc/PERFORMANCE_ESP32P4.md)). *These are C-library figures, not PPA.*
  - Revision note: **PPA in chip rev3.0 adds YUV422 colour conversion** that rev1.x lacks (Espressif collaborator, [esp-idf issue #18611](https://github.com/espressif/esp-idf/issues/18611)).

## 5. I2S

- **Three HP I2S controllers + one LP I2S.** ESP-IDF states *"ESP32-P4 contains three I2S peripheral(s)"*; the datasheet lists `I2S` ×3 plus `LP I2S` ([IDF I2S](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/i2s.html), [DS §4.2.2.6–4.2.2.7](https://documentation.espressif.com/esp32-p4_datasheet_en.html)).
- **Modes/bit depth:** master **and** slave; full-duplex and half-duplex; separate TX and RX units that *"can work independently or simultaneously"*; TDM Philips/MSB/PCM, PDM; **TDM up to 16 channels**; **8/16/24/32-bit** data width; APLL up to **125 MHz**; **DMA = GDMA-AHB only** ([DS §4.2.2.6](https://documentation.espressif.com/esp32-p4_datasheet_en.html)).
- **Sample rate:** Espressif publishes **no single "max sample rate" number**. The IDF docs use a **144 000 Hz** worked example for DMA sizing, state PCM sample rates "normally below 100 kHz, typically 16 kHz–48 kHz" for the PDM path, and enforce `dma_buffer_size = dma_frame_num × slot_num × data_bit_width / 8 ≤ 4092` bytes ([IDF I2S](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/i2s.html)). The hardware limits are the APLL (≤125 MHz) and BCLK/MCLK division, so higher PCM rates are a clock-division question, not a published ceiling — see UNCERTAINTIES.
- **I2S in + display out simultaneously:** **not documented by Espressif as a validated combination** — no doc I found addresses it either way. Structurally the paths are separate: I2S uses **GDMA-AHB**, while LCD_CAM uses GDMA and MIPI-DSI uses the **VDMA** controller (AXI3, 2 AXI masters, handshake with MIPI DSI and ISP) ([DS §4.1.2.1–4.1.2.2](https://documentation.espressif.com/esp32-p4_datasheet_en.html)). Real-world evidence that audio-I2S + MIPI-DSI work concurrently exists (see §7), but it is I2S **playback**, not capture.
- LP I2S is RX-only, 16-bit, up to 2 TDM channels, intended for VAD/low-power capture ([DS §4.2.2.7](https://documentation.espressif.com/esp32-p4_datasheet_en.html)).

## 6. ESP-DSP FFT benchmarks

The published ESP-DSP table **does contain ESP32-P4 columns** and labels each column separately (ESP32 / ESP32S3 / ESP32P4 for the assembly implementation; separate "ANSI Xtensa" / "ANSI Risc-V" columns for the portable C implementation). Values are **CPU cycle counts**; the docs do not state the clock frequency used ([ESP-DSP benchmarks](https://docs.espressif.com/projects/esp-dsp/en/latest/esp32/esp-dsp-benchmarks.html)).

**ESP32-P4, optimized assembly, `-O2` (cycles):**

| Function | 64 | 128 | 256 | 512 | 1024 |
|---|---|---|---|---|---|
| `dsps_fft2r_fc32` (complex f32, radix-2) | 4 989 | 11 404 | 25 684 | 57 181 | **126 053** |
| `dsps_fft4r_fc32` (complex f32, radix-4) | 3 483 | — | 18 052 | — | **89 820** |
| `dsps_fft2r_sc16` (complex s16, radix-2) | 897 | 1 832 | 3 873 | 8 234 | **17 523** |

Converted at an **assumed 400 MHz** (my arithmetic, not published): 1024-pt complex-f32 radix-2 ≈ **315 µs**, radix-4 ≈ **225 µs**, s16 ≈ **44 µs**; 512-pt f32 ≈ **143 µs**; 256-pt f32 ≈ **64 µs**.

**Different silicon, for the clock-ratio caveat (ESP32-S3, `-O2`):** `dsps_fft2r_fc32` 1024 = 97 847 cycles vs P4 126 053; 512 = 44 594 vs 57 181; 256 = 20 139 vs 25 684; radix-4 1024 = 64 482 vs 89 820; s16 1024 = 15 623 vs 17 523. Note **P4 is slower in cycles than S3 for the same f32 kernels** (S3 has the Xtensa DSP/SIMD instructions optimized in esp-dsp), so the S3→P4 comparison must be done per-clock: S3 max 240 MHz vs P4 max 400 MHz ⇒ **~1.67× clock ratio** in P4's favour, which roughly offsets the cycle deficit. Do **not** apply an S3 number to P4 without this ratio.

Reference points from the same table for ESP32 (original): f32 radix-2 1024 = 113 205 cycles, s16 1024 = 226 154 cycles.

**Point-size coverage:** the published table only covers **64/128/256/512/1024 complex points** for `fft2r_fc32` and **64/256/1024** for `fft4r_fc32`. **There are no published 2048- or 4096-point numbers**, and no P4-specific real-FFT (`dsps_fft2r_fc32r` / `dsps_fftr_f32`) row. See UNCERTAINTIES.

## 7. Real-world evidence: ESP32-P4 + ~500×300-class display + audio visualizer

- **Closest published match — an actual P4 audio spectrum visualizer on a MIPI-DSI panel:** `dmatking/esp32-p4-webradio` runs an internet radio with a **live 32-band spectrum visualizer** on **M5Stack Tab5 (1280×720 landscape, ST7123 MIPI-DSI)** and **Waveshare ESP32-P4-WIFI6-Touch-LCD-4B (720×720, ST7703 MIPI-DSI)**. Details: spectrum is a **512-point FFT using esp-dsp** with 32 log-spaced bands; display uses **hardware double buffering (`num_fbs=2`, RGB565)**; `display_flush()` = cache-sync + `draw_bitmap` + buffer swap; on the Tab5 the UI is rendered into a logical **1280×720** buffer and the **PPA rotates it 90° into the framebuffer on every flush**; Wi-Fi comes from an **ESP32-C6 co-processor via `esp_hosted` over SDIO**. Built against **ESP-IDF v5.5.3** ([repo](https://github.com/dmatking/esp32-p4-webradio), [BUILDING.md](https://raw.githubusercontent.com/dmatking/esp32-p4-webradio/main/BUILDING.md)). **No fps figure is stated anywhere in the README or BUILDING.md.**
- **Real-World 800×800 RGB888 MIPI-DSI, 48 MHz pixel clock, 1.5 Gbps lane rate (ESPHome, IDF 5.5.4, LVGL 9.5, PPA enabled)** — dense-scroll workload measurements ([esphome issue #16873](https://github.com/esphome/esphome/issues/16873)):

  | Variant | LVGL loop total | Flush total | Avg flush |
  |---|---|---|---|
  | Full render baseline | 1.659 s | 362.7 ms | 15.1 ms |
  | Direct baseline | 1.612 s | 268.7 ms | 11.2 ms |
  | Async DSI staging | 1.811 s | 449.7 ms | 551 µs (~900 ms display copy) |
  | Unsafe zero-copy (corrupted) | 1.147 s | 31.0 ms | 37 µs |
  | **Zero-copy + C2M cache sync** | 1.230 s | **42.2 ms** | **51 µs** |

  i.e. ~10.6× faster flush than staging; the safe fast path requires the source buffer in internal RAM with DMA/cache-line alignment, otherwise a staging copy applies.
- **60 fps MIPI-DSI achieved at 640×512** (camera→display): a developer reports *"In esp-idf v6.0.1, I was able to directly write the camera frame into the display buffer while performing YUV422 → RGB888 conversion inside the MIPI DSI driver. The result was excellent — smooth image with no tearing and very low latency"* with double buffering and an `on_refresh_done` callback. **More relevant to your bandwidth question:** the same developer's CPU-based YUV2RGB at 640×512 caps at **~30 fps**, and he attributes it to memory bandwidth: *"the CPU spends a lot of time waiting for data from PSRAM and writing it back."* An earlier 384×288 pipeline (CSI → CPU YUV2RGB → PPA blend with LVGL buffer → PPA SRM ×2 → DSI) achieved **~40 fps with minor tearing** ([esp-idf issue #18611](https://github.com/espressif/esp-idf/issues/18611)).
- **P4 audio visualizer on a small RGB matrix (not the 500×300 class):** `3tawi/P4-RGB-Matrix-64x32-Esp32-Spectrum-Music-Visualizer-75-Mode` — ESP32-P4 driving a **64×32 RGB LED matrix** with 75 spectrum display modes ([repo](https://github.com/3tawi/P4-RGB-Matrix-64x32-Esp32-Spectrum-Music-Visualizer-75-Mode)). **No fps or panel-interface details in the README.** (The repository's only content is an `.ino` sketch and a YouTube link; the auto-generated About section is empty — low evidentiary value.)
- **Reference board with a 1024×600 7" MIPI-DSI panel + ESP32-C6:** Waveshare `ESP32-P4-WIFI6-Touch-LCD-7B` ([Waveshare docs](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-7B)) — useful as a bill-of-materials existence proof, but I found no published fps number for it.
- **Not found:** any blog/issue/demo of an ESP32-P4 driving a **~508×302-class** panel with an audio visualizer at a stated 30–60 fps. See UNCERTAINTIES.

## 8. Wireless

- **ESP32-P4 has no Wi-Fi or Bluetooth radio.** The datasheet's RF section, functional block diagram and feature list contain no radio; no Wi-Fi/BT peripheral or antenna pin exists ([DS](https://documentation.espressif.com/esp32-p4_datasheet_en.html)).
- **Espressif's standard companion story (quote):** *"If the application requires wireless connectivity, any product from the ESP32-C/S series can serve as a wireless companion chip for ESP32-P4, connecting via SPI/SDIO/UART interfaces using ESP-Hosted or ESP-AT solutions."* P4 can also be the Host MCU for ACK, AWS IoT ExpressLink, etc. ([product page](https://www.espressif.com/en/products/socs/esp32-p4)).
- **Concrete shipping implementations:** Espressif's own `ESP32-P4X-C5-Function-EV-Board` pairs P4 with an **ESP32-C5-MINI-1** module (2.4/5 GHz Wi-Fi 6 + BT 5 LE); the `ESP32-P4X-Function-EV-Board` and many third-party 7" boards pair P4 with **ESP32-C6** ([product page](https://www.espressif.com/en/products/socs/esp32-p4), [Waveshare ESP32-P4-WIFI6-Touch-LCD-7B](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-7B)).
- **Overhead / practical notes from a working project:** the C6 runs `esp_hosted` **slave** firmware; on Tab5 the host `esp_hosted` is pinned to **2.12.3**; boards commonly ship with old C6 firmware (v0.0.0, Wi-Fi works, no BT) and the app OTAs the C6 over **SDIO in ~25 s**, C6 rebooting once. So OTA/network on P4 = an extra chip + a firmware-version coupling + SDIO/SPI/UART pins ([BUILDING.md](https://raw.githubusercontent.com/dmatking/esp32-p4-webradio/main/BUILDING.md)).

---

## DERIVED ESTIMATES (my arithmetic — NOT sourced; label as estimates)

For your 508×302 target, RGB565 = **306 616 B/frame ≈ 300 KiB**.

- **DSI bandwidth:** raw scanout at 60 fps = **18.4 MB/s** (14.7 Mbit/s at 16 bpp… i.e. 147 Mbit/s), plus blanking ≈ 10.5 MHz pixel clock at 24 bpp ⇒ **~253 Mbit/s raw**, ≈**365 Mbit/s** after Espressif's ×1.25×1.15 overhead, ÷2 lanes ≈ **183 Mbit/s per lane**, ×1.2 margin ≈ **219 Mbit/s/lane**. **This is below Espressif's recommended ≥480 Mbps minimum lane rate** — a small panel may need a deliberately configured low lane rate, a higher colour depth/blanking, or the parallel RGB/SPI path instead. This is the single most non-obvious finding for your panel choice; the ≥480 Mbps figure is sourced, the arithmetic is mine.
- **PSRAM bandwidth share:** the read-modify-write persistence pass over the framebuffer costs 2×306 KB per frame = **36.8 MB/s** at 60 fps; adding 18.4 MB/s scanout (if the FB lives in PSRAM) gives **~55 MB/s**, i.e. **~46–55 % of the ~100–120 MB/s measured PSRAM stream bandwidth** — and DSI/RGB scanout bypasses cache, so it competes directly with CPU cache-line fetches, matching the documented "EDMA and CPU each get half" statement. Keeping the framebuffer in internal SRAM (≈592 KiB free without PSRAM; ≈569 KB with PSRAM per the sources) avoids this entirely but costs ~300 KiB of that budget.
- **FPS budget:** at the assumed 400 MHz, a 1024-pt complex f32 radix-2 FFT (~315 µs) is ~1.9 % of a 16.7 ms budget; even a 4096-pt transform would be roughly 4× that (~8 %). The FFT is *not* the bottleneck; the per-pixel persistence pass and PSRAM traffic are.
- **SPI panel ceiling:** 306 616 B/frame over 80 MHz SPI (≈10 MB/s raw) ≈ **32 fps theoretical maximum** before overhead — likely insufficient for a 60 fps target over SPI.

## UNCERTAINTIES (could not confirm)

1. **ESP-DSP has no 2048- or 4096-point rows**, no P4 real-FFT (`fc32r`/`fftr`) rows, and the docs **do not state the clock frequency** used for the cycle counts. My µs conversions assume 400 MHz. Also, no row covers radix-4 at 512 or 2048/4096.
2. **PSRAM clock figure is internally contradictory.** Datasheet §4.1.3.1 says in-package PSRAM max clock **250 MHz** (⇒ 8 Gbit/s), while datasheet Table 5-9 ("Memory Specifications", sourced from the memory vendor) prints **max clock frequency 80 MHz**. I could not reconcile these. Independent community measurements (~100–120 MB/s ≈ 0.8–1.0 Gbit/s) are **an order of magnitude below** the 8 Gbit/s theoretical figure, so treat 8 Gbit/s as a PHY ceiling that no cached CPU workload reaches.
3. **PSRAM 200 MHz is gated behind "experimental features" in menuconfig** (per an Espressif collaborator), so the *default* build's PSRAM clock is lower than 250 MHz. I did not confirm what the default is for a given IDF version.
4. **No PPA throughput spec (pixels/clock or Mpixel/s) exists in any Espressif document I found.** The only quantified PPA numbers are LVGL's relative speed-ups (~30 % general, up to 9× pure fill, ~40 % rotation) and the note that blending shows no significant gain due to DMA-2D memory bandwidth.
5. **I2S maximum PCM sample rate is not published.** Hardware limits found are APLL ≤125 MHz, 8/16/24/32-bit slots, ≤16 TDM slots, and the 4092-byte DMA buffer cap. A 192 kHz/32-bit figure would be a *derivation* from the clock tree, not a sourced spec.
6. **Simultaneous I2S-in + display-out is not documented by Espressif** as a supported/validated combination. The engines are separate (I2S on GDMA-AHB; DSI on VDMA), and a shipping project runs I2S-out + MIPI-DSI concurrently, but I found **no source confirming I2S RX + DSI/RGB simultaneously**, and no source quantifying whether the shared PSRAM path is the practical constraint.
7. **No real-world ESP32-P4 project driving a ~508×302 panel with an audio visualizer at a stated fps.** Closest matches are a 32-band esp-dsp visualizer on 720×720 / 1280×720 MIPI-DSI panels (no fps published), a 640×512 camera→DSI pipeline at 60 fps, and an 800×800 RGB888 LVGL workload with flush timings in ms (not fps). The claimed ~40 fps figure for 384×288 with PPA blending + software YUV2RGB is a single developer's report in an open issue, not a benchmark.
8. **ESP32-P4 core symmetry is only documented implicitly** — both HP cores are described as the same RISC-V design at up to 400 MHz; I found no Espressif statement of any *performance* asymmetry beyond CPU1's default-held reset. I did not verify from the TRM whether CPU1 has identical TCM/SPM access or trace capabilities.
9. **Default CPU frequency is 360 MHz**, not the 400 MHz headline, in at least one shipping board's boot log and the Arduino ESP32-P4 board definition. I did not confirm whether that is a board/BSP choice or an IDF default.
10. **Softly-sourced numbers flagged inline:** the memcpy/SIMD PSRAM bandwidth table comes from a community repo whose README carries a *"WARNING, THIS SOFTWARE HAS BEEN MAINLY AI-GENERATED"* notice — the methodology (64 KB buffers, 10 iterations, byte verification, cache invalidation) is described, but I could not independently reproduce it. The `3tawi` RGB-matrix visualizer repo has an empty description, 4 commits, and no fps data.
11. **Datasheet version:** the numbers above come from **v0.7, watermarked PRELIMINARY / pre-release**, with a revision history dated 2026-07-14. Two chip revisions exist in the field (`v1.x` and `v3.x`) and behaviour differs (e.g. PPA YUV422 conversion is rev3.0+ only); I did not map every peripheral number above to a specific silicon revision.
