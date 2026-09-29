// render_bench.cpp —— VFD 渲染器的性能基准 + 黄金帧导出
//
// 为什么要有它：docs/508x302-feasibility.md 里那些毫秒数是一次性手工测出来的，
// 改一次代码就得重测一次、而且没法验证"改动前后画面是否完全一致"。这个工具把两件事
// 变成可重复的：
//   1) 分阶段计时（清屏 / 柱体 / 峰值帽 / 磷光暂留 / 出图），逐阶段给 avg/min/max；
//   2) 用**确定性合成频谱**跑 N 帧后导出 PNG 或裸 RGB，供改动前后逐字节对照
//      （`--raw` 写裸数据，配 `fc /b` 或 cmp 就是"黄金帧"回归）。
//
// 用法:
//   render_bench [--cell 4] [--dot 3] [--cols 127] [--rows 75] [--bands 512]
//                [--frames 240] [--tau 0.07] [--warmup 30]
//                [--render-only 200] [--png out.png] [--raw out.bin]
//                [--header] [--csv] [--quiet]

#include "png_writer.h"
#include "vfd_render.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double nowMs() {
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

// 确定性合成频谱：4 个缓慢滑动的高斯峰 + 底噪 + 逐带抖动。
// 刻意不用 rand()：同一 frame 序号永远给出同一组频段，黄金帧才可比。
void synthBands(int frame, int n, float* out) {
    for (int i = 0; i < n; ++i) {
        const double t = (n > 1) ? (double)i / (double)(n - 1) : 0.0;
        double v = 0.02 + 0.01 * std::sin(3.1 * i + 0.05 * frame);
        for (int k = 0; k < 4; ++k) {
            const double c = 0.14 + 0.23 * (double)k + 0.05 * std::sin(0.031 * frame + 1.7 * k);
            const double w = 0.010 + 0.009 * (double)k;
            const double d = t - c;
            v += (0.88 - 0.13 * (double)k) * std::exp(-(d * d) / (2.0 * w * w));
        }
        out[i] = (float)(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
    }
}

struct Stat {
    double sum = 0.0, min = 1e30, max = 0.0;
    long count = 0;
    void add(double ms) {
        sum += ms;
        if (ms < min) min = ms;
        if (ms > max) max = ms;
        ++count;
    }
    double avg() const { return count ? sum / (double)count : 0.0; }
};

void report(const char* name, const Stat& s, int frames) {
    std::printf("  %-22s avg %8.3f ms  min %8.3f  max %8.3f   (%d frames)\n", name, s.avg(),
                s.count ? s.min : 0.0, s.max, frames);
}

}  // namespace

int main(int argc, char** argv) {
    int cell = 4;
    int dot = 3;
    int cols = 127;
    int rows = 75;
    int bands = 512;
    int frames = 240;
    int warmup = 30;
    int renderOnly = 0;
    double tau = 0.07;
    const char* pngPath = nullptr;
    const char* rawPath = nullptr;
    bool withHeader = false;
    bool csv = false;
    bool quiet = false;
    int theme = 0;
    bool rowsSet = false, colsSet = false;

    for (int i = 1; i < argc; ++i) {
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "render_bench: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (std::strcmp(argv[i], "--cell") == 0) {
            cell = std::atoi(next("--cell"));
        } else if (std::strcmp(argv[i], "--dot") == 0) {
            dot = std::atoi(next("--dot"));
        } else if (std::strcmp(argv[i], "--cols") == 0) {
            cols = std::atoi(next("--cols"));
            colsSet = true;
        } else if (std::strcmp(argv[i], "--rows") == 0) {
            rows = std::atoi(next("--rows"));
            rowsSet = true;
        } else if (std::strcmp(argv[i], "--bands") == 0) {
            bands = std::atoi(next("--bands"));
        } else if (std::strcmp(argv[i], "--frames") == 0) {
            frames = std::atoi(next("--frames"));
        } else if (std::strcmp(argv[i], "--warmup") == 0) {
            warmup = std::atoi(next("--warmup"));
        } else if (std::strcmp(argv[i], "--render-only") == 0) {
            renderOnly = std::atoi(next("--render-only"));
        } else if (std::strcmp(argv[i], "--tau") == 0) {
            tau = std::atof(next("--tau"));
        } else if (std::strcmp(argv[i], "--png") == 0) {
            pngPath = next("--png");
        } else if (std::strcmp(argv[i], "--raw") == 0) {
            rawPath = next("--raw");
        } else if (std::strcmp(argv[i], "--header") == 0) {
            withHeader = true;
        } else if (std::strcmp(argv[i], "--csv") == 0) {
            csv = true;
        } else if (std::strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (std::strcmp(argv[i], "--theme") == 0) {
            theme = std::atoi(next("--theme"));       // 0..4 = 黄绿/深绿/蓝/粉/橘
        } else {
            std::fprintf(stderr, "render_bench: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    if (cell < 1) cell = 1;
    if (dot < 1 || dot > cell) dot = cell;
    // 默认布局与调用方给出的目标分辨率一致：508x360 下 cell=4 -> 127x90
    // （360 = 面板把整块显示区加高 20%、多出来的高度全给频谱之后的新高度）
    if (!colsSet) cols = 508 / cell;
    if (!rowsSet) rows = 360 / cell;
    if (bands < 8) bands = 8;

    vfdrender::Screen screen(cols, rows, cell, dot);
    screen.setTheme((vfdrender::Theme)(((theme % vfdrender::Screen::themeCount())
                                        + vfdrender::Screen::themeCount())
                                       % vfdrender::Screen::themeCount()));
    if (withHeader) {
        screen.setHeader("SPECTRUM", "192000HZ", "32BIT 2CH");
    }
    std::vector<float> bandsBuf((size_t)bands, 0.0f);
    std::vector<uint8_t> rgb;

    std::printf("render_bench: %dx%d px, grid %dx%d, cell=%d dot=%d, bands=%d, tau=%.3f\n",
                screen.width(), screen.height(), cols, rows, cell, dot, bands, tau);
    std::fflush(stdout);

    Stat sClear, sBars, sPeaks, sPersist, sRender, sTotal;

    const double dt = 1.0 / 30.0;
    const int total = warmup + frames;
    for (int f = 0; f < total; ++f) {
        synthBands(f, bands, bandsBuf.data());
        const bool measure = (f >= warmup);

        double t0 = nowMs();
        screen.clearDynamic();
        double t1 = nowMs();
        screen.drawBars(bandsBuf.data(), bands);
        double t2 = nowMs();
        screen.drawPeaks(bandsBuf.data(), bands);
        double t3 = nowMs();
        screen.applyPersistence((float)dt, (float)tau);
        double t4 = nowMs();
        screen.render(rgb);
        double t5 = nowMs();

        if (measure) {
            sClear.add(t1 - t0);
            sBars.add(t2 - t1);
            sPeaks.add(t3 - t2);
            sPersist.add(t4 - t3);
            sRender.add(t5 - t4);
            sTotal.add(t5 - t0);
        }
    }

    if (!quiet) {
        std::printf("--- per frame (draw path, %d frames after %d warmup) ---\n", frames, warmup);
        report("clearDynamic", sClear, frames);
        report("drawBars", sBars, frames);
        report("drawPeaks", sPeaks, frames);
        report("applyPersistence", sPersist, frames);
        report("render (out image)", sRender, frames);
        report("TOTAL", sTotal, frames);
    }

    if (csv) {
        std::printf("csv,clearDynamic,%.4f,drawBars,%.4f,drawPeaks,%.4f,applyPersistence,%.4f,"
                    "render,%.4f,total,%.4f,cell,%d,cols,%d,rows,%d,bands,%d\n",
                    sClear.avg(), sBars.avg(), sPeaks.avg(), sPersist.avg(), sRender.avg(),
                    sTotal.avg(), cell, cols, rows, bands);
    }

    if (renderOnly > 0) {
        Stat sR;
        for (int i = 0; i < renderOnly; ++i) {
            const double t0 = nowMs();
            screen.render(rgb);
            sR.add(nowMs() - t0);
        }
        if (!quiet) {
            std::printf("--- render() only, %d iterations (state frozen) ---\n", renderOnly);
            report("render", sR, renderOnly);
        }
        std::printf("RENDER_ONLY avg %.4f ms  min %.4f  max %.4f\n", sR.avg(), sR.min, sR.max);
    }

    if (pngPath != nullptr) {
        const bool ok = pngw::writePng(pngPath, rgb.data(), screen.width(), screen.height());
        std::printf("png     -> %s [%s]\n", pngPath, ok ? "ok" : "FAILED");
    }
    if (rawPath != nullptr) {
        std::FILE* fh = std::fopen(rawPath, "wb");
        bool ok = false;
        if (fh != nullptr) {
            ok = std::fwrite(rgb.data(), 1, rgb.size(), fh) == rgb.size();
            std::fclose(fh);
        }
        std::printf("raw     -> %s (%zu bytes) [%s]\n", rawPath, rgb.size(), ok ? "ok" : "FAILED");
    }
    return 0;
}
