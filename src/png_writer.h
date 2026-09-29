// png_writer.h —— 极简 PNG 编码器（8bit RGB），零依赖
//
// 与 Python 参考实现(vfd_spectrum.py 的 write_png)同一个思路: 不引入 zlib/Pillow。
// 这里用 deflate 的**存储块(stored block, BTYPE=00)**，不做压缩——
// 对 508x302 这种尺寸完全够用，而且代码短、可审计。
//
// 用途: 让 C++ 侧的渲染器也能出图，方便和 Python 参考实现做"黄金帧"对照。

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pngw {

inline uint32_t crc32Of(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

inline uint32_t adler32Of(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

inline void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

inline void writeChunk(std::vector<uint8_t>& out, const char tag[4],
                       const std::vector<uint8_t>& payload) {
    put32(out, (uint32_t)payload.size());
    const size_t start = out.size();
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    const uint32_t crc = crc32Of(out.data() + start, out.size() - start);
    put32(out, crc);
}

// rgb: w*h*3 字节。成功返回 true。
inline bool writePng(const char* path, const uint8_t* rgb, int w, int h) {
    // 原始扫描线: 每行前面加一个 filter 字节(0 = None)
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (1 + (size_t)w * 3));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        const uint8_t* row = rgb + (size_t)y * (size_t)w * 3;
        raw.insert(raw.end(), row, row + (size_t)w * 3);
    }

    // zlib 流: 头 + 若干存储块 + adler32
    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t n = (raw.size() - pos) > 65535 ? 65535 : (raw.size() - pos);
        const bool last = (pos + n) >= raw.size();
        z.push_back(last ? 1 : 0);                      // BFINAL + BTYPE=00
        z.push_back((uint8_t)(n & 0xFF));
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF));
        z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + (long)pos, raw.begin() + (long)(pos + n));
        pos += n;
    }
    put32(z, adler32Of(raw.data(), raw.size()));

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<uint8_t> ihdr;
    put32(ihdr, (uint32_t)w);
    put32(ihdr, (uint32_t)h);
    ihdr.push_back(8);      // bit depth
    ihdr.push_back(2);      // colour type = truecolour RGB
    ihdr.push_back(0);      // compression
    ihdr.push_back(0);      // filter
    ihdr.push_back(0);      // interlace
    writeChunk(out, "IHDR", ihdr);
    writeChunk(out, "IDAT", z);
    writeChunk(out, "IEND", {});

    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;
    const size_t wrote = std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return wrote == out.size();
}

}  // namespace pngw
