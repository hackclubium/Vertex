#include "codec/webp.h"
#include <array>
#include <cstring>

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
            // Lossy WebP still image = single VP8 key frame. Header parsing here
            // rejects malformed files early; entropy/IDCT decode intentionally
            // not faked as success.
            if (chunkSize < 10) return result;
            uint32_t tag = ReadU24LE(chunk);
            bool keyFrame = (tag & 1u) == 0;
            if (!keyFrame || chunk[3] != 0x9d || chunk[4] != 0x01 || chunk[5] != 0x2a) return result;
            uint32_t w = ReadU32LE(chunk + 6) & 0x3fff;
            uint32_t h = (ReadU32LE(chunk + 6) >> 16) & 0x3fff;
            if (w == 0 || h == 0 || (uint64_t)w * h > 64ull * 1000 * 1000) return result;
            return result;
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
