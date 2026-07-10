#include "codec/webp.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <vector>

namespace {

uint32_t ReadU32LE(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t ReadU24LE(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

bool Match(const uint8_t* p, const char* s) {
    return std::memcmp(p, s, 4) == 0;
}

struct BitReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t bitPos = 0;

    bool ReadBits(int n, uint32_t& out) {
        if (n < 0 || n > 24 || bitPos + (size_t)n > size * 8) return false;
        out = 0;
        for (int i = 0; i < n; ++i) {
            out |= ((uint32_t)((data[bitPos >> 3] >> (bitPos & 7)) & 1u)) << i;
            ++bitPos;
        }
        return true;
    }
};

bool ReadSimpleSingleSymbol(BitReader& br, int alphabetSize, int& symbol) {
    uint32_t bit = 0;
    if (!br.ReadBits(1, bit) || bit != 1) return false; // normal code unsupported
    uint32_t numSymbolsMinusOne = 0;
    if (!br.ReadBits(1, numSymbolsMinusOne) || numSymbolsMinusOne != 0) return false;
    uint32_t firstIs8Bits = 0;
    if (!br.ReadBits(1, firstIs8Bits)) return false;
    uint32_t value = 0;
    if (!br.ReadBits(firstIs8Bits ? 8 : 1, value)) return false;
    if (value >= (uint32_t)alphabetSize) return false;
    symbol = (int)value;
    return true;
}

struct BoolReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    uint32_t range = 255;
    uint32_t value = 0;
    int bitCount = 0;
    bool ok = true;

    bool Init(const uint8_t* p, size_t n) {
        if (n < 2) return false;
        data = p;
        size = n;
        pos = 2;
        range = 255;
        value = ((uint32_t)data[0] << 8) | data[1];
        bitCount = 8;
        ok = true;
        return ok;
    }

    void Fill() {
        if (pos < size) value |= data[pos++];
        bitCount = 8;
    }

    int Bit(int prob = 128) {
        uint32_t split = 1 + (((range - 1) * (uint32_t)prob) >> 8);
        uint32_t bigsplit = split << 8;
        int bit = value >= bigsplit;
        if (bit) {
            range -= split;
            value -= bigsplit;
        } else {
            range = split;
        }
        while (range < 128) {
            range <<= 1;
            value <<= 1;
            --bitCount;
            if (bitCount == 0) Fill();
        }
        return bit;
    }

    uint32_t Lit(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | (uint32_t)Bit();
        return v;
    }
};

int ReadVp8Mode(BoolReader& br, const uint8_t* probs) {
    if (!br.Bit(probs[0])) return 0;
    if (!br.Bit(probs[1])) return 1;
    if (!br.Bit(probs[2])) return 2;
    return br.Bit(probs[3]) ? 4 : 3;
}

uint8_t Clip8(int v) {
    return (uint8_t)std::max(0, std::min(255, v));
}

void PutRgb(std::vector<uint8_t>& rgba, int w, int x, int y, int yy, int u, int v) {
    int c = yy - 16;
    int d = u - 128;
    int e = v - 128;
    uint8_t* p = &rgba[((size_t)y * w + x) * 4];
    p[0] = Clip8((298 * c + 409 * e + 128) >> 8);
    p[1] = Clip8((298 * c - 100 * d - 208 * e + 128) >> 8);
    p[2] = Clip8((298 * c + 516 * d + 128) >> 8);
    p[3] = 255;
}

void Predict16(int mode, const std::vector<uint8_t>& plane, int stride, int x, int y, int bw, int bh, uint8_t* out) {
    for (int yy = 0; yy < bh; ++yy) {
        for (int xx = 0; xx < bw; ++xx) {
            int left = x ? plane[(size_t)(y + yy) * stride + x - 1] : 129;
            int top = y ? plane[(size_t)(y - 1) * stride + x + xx] : 127;
            int topLeft = (x && y) ? plane[(size_t)(y - 1) * stride + x - 1] : 127;
            int v = 128;
            if (mode == 0) {
                int sum = 0;
                for (int i = 0; i < bw; ++i) sum += y ? plane[(size_t)(y - 1) * stride + x + i] : 127;
                for (int i = 0; i < bh; ++i) sum += x ? plane[(size_t)(y + i) * stride + x - 1] : 129;
                v = (sum + bw) / (bw + bh);
            } else if (mode == 1) v = top;
            else if (mode == 2) v = left;
            else if (mode == 3) v = std::max(0, std::min(255, left + top - topLeft));
            out[yy * bw + xx] = (uint8_t)v;
        }
    }
}

bool ReadAllEobTokens(const uint8_t* data, size_t size, int macroblocks) {
    BoolReader br;
    if (!br.Init(data, size)) return false;
    for (int mb = 0; mb < macroblocks; ++mb) {
        if (br.Bit(202)) return false; // Y2 block has non-zero coefficient.
        for (int i = 0; i < 16; ++i) if (br.Bit(195)) return false; // Y blocks.
        for (int i = 0; i < 8; ++i) if (br.Bit(211)) return false; // U/V blocks.
    }
    return br.ok;
}

DecodedImage DecodeVp8Lossy(const uint8_t* chunk, size_t chunkSize) {
    DecodedImage result;
    if (chunkSize < 10) return result;
    uint32_t tag = ReadU24LE(chunk);
    if ((tag & 1u) != 0 || ((tag >> 1) & 7u) > 3 || chunk[3] != 0x9d || chunk[4] != 0x01 || chunk[5] != 0x2a) return result;
    size_t firstPartSize = (tag >> 5) & 0x7ffffu;
    int w = (int)(ReadU32LE(chunk + 6) & 0x3fff);
    int h = (int)((ReadU32LE(chunk + 6) >> 16) & 0x3fff);
    if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > 64ull * 1000 * 1000) return result;
    if (10 + firstPartSize > chunkSize) return result;

    BoolReader br;
    if (!br.Init(chunk + 10, firstPartSize)) return result;
    (void)br.Bit(); // color space
    (void)br.Bit(); // pixel clamp type
    if (br.Bit()) return result; // segmentation unsupported
    (void)br.Bit(); // simple loop filter
    (void)br.Lit(6);
    (void)br.Lit(3);
    if (br.Bit()) {
        if (br.Bit()) {
            for (int i = 0; i < 4; ++i) if (br.Bit()) (void)br.Lit(6 + br.Bit());
            for (int i = 0; i < 4; ++i) if (br.Bit()) (void)br.Lit(6 + br.Bit());
        }
    }
    if (br.Lit(2) != 0) return result; // token partitions unsupported
    (void)br.Lit(7);
    for (int i = 0; i < 5; ++i) if (br.Bit()) (void)br.Lit(4 + br.Bit());
    (void)br.Bit(); // refresh entropy probs
    for (int i = 0; i < 4 * 8 * 3 * 11; ++i) if (br.Bit(252)) (void)br.Lit(8);
    int mbNoCoeffSkip = br.Bit();
    int probSkip = mbNoCoeffSkip ? (int)br.Lit(8) : 0;

    static const uint8_t yProb[] = { 145, 156, 163, 128 };
    static const uint8_t uvProb[] = { 142, 114, 183, 128 };

    std::vector<uint8_t> yPlane((size_t)w * h, 128), uPlane((size_t)((w + 1) / 2) * ((h + 1) / 2), 128), vPlane(uPlane.size(), 128);
    int mbw = (w + 15) / 16;
    int mbh = (h + 15) / 16;
    for (int my = 0; my < mbh; ++my) {
        for (int mx = 0; mx < mbw; ++mx) {
            int skip = mbNoCoeffSkip ? br.Bit(probSkip) : 0;
            int yMode = ReadVp8Mode(br, yProb);
            int uvMode = ReadVp8Mode(br, uvProb);
            if (!br.ok || yMode == 4) return result; // 4x4 mode unsupported
            if (!skip && chunkSize <= 10 + firstPartSize) return result;
            int x = mx * 16, yy = my * 16, bw = std::min(16, w - x), bh = std::min(16, h - yy);
            uint8_t pred[16 * 16];
            Predict16(yMode, yPlane, w, x, yy, bw, bh, pred);
            for (int r = 0; r < bh; ++r) std::memcpy(&yPlane[(size_t)(yy + r) * w + x], pred + r * bw, bw);
            int cw = (w + 1) / 2, cx = mx * 8, cy = my * 8, cbw = std::min(8, cw - cx), cbh = std::min(8, (h + 1) / 2 - cy);
            uint8_t cpred[8 * 8];
            Predict16(uvMode, uPlane, cw, cx, cy, cbw, cbh, cpred);
            for (int r = 0; r < cbh; ++r) std::memcpy(&uPlane[(size_t)(cy + r) * cw + cx], cpred + r * cbw, cbw);
            Predict16(uvMode, vPlane, cw, cx, cy, cbw, cbh, cpred);
            for (int r = 0; r < cbh; ++r) std::memcpy(&vPlane[(size_t)(cy + r) * cw + cx], cpred + r * cbw, cbw);
        }
    }
    if (!br.ok) return result;
    if (chunkSize <= 10 + firstPartSize) return result;
    if (!ReadAllEobTokens(chunk + 10 + firstPartSize, chunkSize - 10 - firstPartSize, mbw * mbh)) return result;
    result.width = w;
    result.height = h;
    result.rgba.resize((size_t)w * h * 4);
    int cw = (w + 1) / 2;
    for (int yy = 0; yy < h; ++yy)
        for (int x = 0; x < w; ++x)
            PutRgb(result.rgba, w, x, yy, yPlane[(size_t)yy * w + x], uPlane[(size_t)(yy / 2) * cw + x / 2], vPlane[(size_t)(yy / 2) * cw + x / 2]);
    result.success = true;
    return result;
}

DecodedImage DecodeVp8Lossless(const uint8_t* chunk, size_t chunkSize) {
    DecodedImage result;
    if (chunkSize < 5 || chunk[0] != 0x2f) return result;
    uint32_t bits = ReadU32LE(chunk + 1);
    int w = (int)((bits & 0x3fffu) + 1);
    int h = (int)(((bits >> 14) & 0x3fffu) + 1);
    int version = (int)((bits >> 29) & 7u);
    if (version != 0 || w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > 64ull * 1000 * 1000) return result;

    BitReader br{chunk + 5, chunkSize - 5, 0};
    uint32_t bit = 0;
    if (!br.ReadBits(1, bit) || bit != 0) return result; // transforms unsupported
    if (!br.ReadBits(1, bit) || bit != 0) return result; // color cache unsupported
    if (!br.ReadBits(1, bit) || bit != 0) return result; // multiple meta-prefix groups unsupported

    std::array<int, 5> symbols{};
    const std::array<int, 5> alphabetSizes{256 + 24, 256, 256, 256, 40};
    for (size_t i = 0; i < symbols.size(); ++i) {
        if (!ReadSimpleSingleSymbol(br, alphabetSizes[i], symbols[i])) return result;
    }
    if (symbols[0] >= 256) return result; // LZ77/cache path unsupported

    result.width = w;
    result.height = h;
    result.rgba.resize((size_t)w * (size_t)h * 4);
    for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) {
        result.rgba[i * 4 + 0] = (uint8_t)symbols[1];
        result.rgba[i * 4 + 1] = (uint8_t)symbols[0];
        result.rgba[i * 4 + 2] = (uint8_t)symbols[2];
        result.rgba[i * 4 + 3] = (uint8_t)symbols[3];
    }
    result.success = true;
    return result;
}

} // namespace

DecodedImage DecodeWebp(const uint8_t* data, size_t size) {
    DecodedImage result;
    if (!data || size < 12) return result;
    if (!Match(data, "RIFF") || !Match(data + 8, "WEBP")) return result;
    uint32_t riffSize = ReadU32LE(data + 4);
    if ((uint64_t)riffSize + 8 > size) return result;

    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t* fourcc = data + pos;
        uint32_t chunkSize = ReadU32LE(data + pos + 4);
        pos += 8;
        if (pos + chunkSize > size) return result;
        const uint8_t* chunk = data + pos;

        if (Match(fourcc, "VP8 ")) {
            return DecodeVp8Lossy(chunk, chunkSize);
        }

        if (Match(fourcc, "VP8L")) {
            return DecodeVp8Lossless(chunk, chunkSize);
        }

        if (Match(fourcc, "VP8X")) {
            if (chunkSize < 10) return result;
            uint32_t w = ReadU24LE(chunk + 4) + 1;
            uint32_t h = ReadU24LE(chunk + 7) + 1;
            if (w == 0 || h == 0 || (uint64_t)w * h > 64ull * 1000 * 1000) return result;
        }

        pos += chunkSize + (chunkSize & 1u);
    }
    return result;
}
