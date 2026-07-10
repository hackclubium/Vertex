#include "codec/webp.h"
#include <array>
#include <algorithm>
#include <cstdint>
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

uint32_t ReverseBits(uint32_t v, int n) {
    uint32_t out = 0;
    for (int i = 0; i < n; ++i) out = (out << 1) | ((v >> i) & 1u);
    return out;
}

struct HuffmanCode {
    std::vector<int> symbol;
    std::vector<int> length;
    std::vector<uint32_t> code;
    int maxLength = 0;

    bool Build(const std::vector<int>& lengths) {
        symbol.clear(); length.clear(); code.clear(); maxLength = 0;
        int maxLen = 0;
        for (int n : lengths) maxLen = std::max(maxLen, n);
        if (maxLen == 0 && !lengths.empty()) {
            symbol.push_back(0); length.push_back(0); code.push_back(0); maxLength = 0;
            return true;
        }
        if (maxLen <= 0 || maxLen > 15) return false;
        std::vector<int> count(maxLen + 1, 0);
        for (int n : lengths) {
            if (n < 0 || n > 15) return false;
            if (n) count[n]++;
        }
        int live = 0;
        for (int n : count) live += n;
        if (live == 1) {
            for (int i = 0; i < (int)lengths.size(); ++i) if (lengths[i]) {
                symbol.push_back(i); length.push_back(0); code.push_back(0); maxLength = 0;
                return true;
            }
        }
        int left = 1;
        for (int bits = 1; bits <= maxLen; ++bits) {
            left <<= 1;
            left -= count[bits];
            if (left < 0) return false;
        }
        std::vector<int> next(maxLen + 1, 0);
        int c = 0;
        for (int bits = 1; bits <= maxLen; ++bits) {
            c = (c + count[bits - 1]) << 1;
            next[bits] = c;
        }
        for (int i = 0; i < (int)lengths.size(); ++i) {
            int n = lengths[i];
            if (!n) continue;
            symbol.push_back(i);
            length.push_back(n);
            code.push_back(ReverseBits((uint32_t)next[n]++, n));
            maxLength = std::max(maxLength, n);
        }
        return true;
    }

    bool Decode(BitReader& br, int& out) const {
        if (symbol.empty()) return false;
        if (maxLength == 0) { out = symbol[0]; return true; }
        uint32_t bits = 0;
        for (int n = 1; n <= maxLength; ++n) {
            uint32_t bit = 0;
            if (!br.ReadBits(1, bit)) return false;
            bits |= bit << (n - 1);
            for (size_t i = 0; i < symbol.size(); ++i) {
                if (length[i] == n && code[i] == bits) {
                    out = symbol[i];
                    return true;
                }
            }
        }
        return false;
    }
};

bool ReadHuffmanCode(BitReader& br, int alphabetSize, HuffmanCode& out) {
    if (alphabetSize <= 0 || alphabetSize > 4096) return false;
    uint32_t simple = 0;
    if (!br.ReadBits(1, simple)) return false;
    std::vector<int> lengths((size_t)alphabetSize, 0);
    if (simple) {
        uint32_t numSymbolsMinusOne = 0, firstIs8Bits = 0, value = 0;
        if (!br.ReadBits(1, numSymbolsMinusOne) || !br.ReadBits(1, firstIs8Bits)) return false;
        if (!br.ReadBits(firstIs8Bits ? 8 : 1, value) || value >= (uint32_t)alphabetSize) return false;
        int first = (int)value;
        if (numSymbolsMinusOne) {
            if (!br.ReadBits(8, value) || value >= (uint32_t)alphabetSize || lengths[value]) return false;
            if ((int)value == first) return false;
            out.symbol = { first, (int)value };
            out.length = { 1, 1 };
            out.code = { 0, 1 };
            out.maxLength = 1;
            return true;
        }
        lengths[(size_t)first] = 1;
        return out.Build(lengths);
    }

    static const int order[19] = { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    uint32_t n = 0;
    if (!br.ReadBits(4, n)) return false;
    int codeLengthCount = (int)n + 4;
    std::vector<int> codeLengthLengths(19, 0);
    for (int i = 0; i < codeLengthCount; ++i) {
        uint32_t v = 0;
        if (!br.ReadBits(3, v)) return false;
        codeLengthLengths[order[i]] = (int)v;
    }
    HuffmanCode codeLengthCode;
    if (!codeLengthCode.Build(codeLengthLengths)) return false;
    uint32_t limited = 0;
    int maxSymbol = alphabetSize;
    if (!br.ReadBits(1, limited)) return false;
    if (limited) {
        uint32_t nbitsFlag = 0, v = 0;
        if (!br.ReadBits(3, nbitsFlag)) return false;
        int nbits = 2 + 2 * (int)nbitsFlag;
        if (!br.ReadBits(nbits, v)) return false;
        maxSymbol = 2 + (int)v;
        if (maxSymbol > alphabetSize) return false;
    }
    int pos = 0;
    while (pos < maxSymbol) {
        int sym = 0;
        if (!codeLengthCode.Decode(br, sym)) return false;
        if (sym < 16) lengths[(size_t)pos++] = sym;
        else if (sym == 16) {
            int prev = pos == 0 ? 8 : lengths[(size_t)pos - 1];
            uint32_t extra = 0;
            if (!br.ReadBits(2, extra)) return false;
            int repeat = 3 + (int)extra;
            int end = std::min(maxSymbol, pos + repeat);
            std::fill(lengths.begin() + pos, lengths.begin() + end, prev);
            pos = end;
        } else if (sym == 17) {
            uint32_t extra = 0;
            if (!br.ReadBits(3, extra)) return false;
            int repeat = 3 + (int)extra;
            pos = std::min(maxSymbol, pos + repeat);
        } else if (sym == 18) {
            uint32_t extra = 0;
            if (!br.ReadBits(7, extra)) return false;
            int repeat = 11 + (int)extra;
            pos = std::min(maxSymbol, pos + repeat);
        } else return false;
    }
    return out.Build(lengths);
}

bool FailVp8l(const char* where) {
    (void)where;
    return false;
}

int Vp8lExtraBits(int prefix) {
    return prefix < 4 ? 0 : (prefix - 2) >> 1;
}

bool ReadVp8lPrefix(BitReader& br, int prefix, int& value) {
    if (prefix < 0 || prefix >= 40) return false;
    int extraBits = Vp8lExtraBits(prefix);
    uint32_t extra = 0;
    if (extraBits && !br.ReadBits(extraBits, extra)) return false;
    if (prefix < 4) value = prefix + 1;
    else value = (((2 + (prefix & 1)) << extraBits) + (int)extra) + 1;
    return true;
}

int Vp8lDistanceToPlaneDistance(int code, int width) {
    static const int8_t map[120][2] = {
        {0,1},{1,0},{1,1},{-1,1},{0,2},{2,0},{1,2},{-1,2},{2,1},{-2,1},{2,2},{-2,2},
        {0,3},{3,0},{1,3},{-1,3},{3,1},{-3,1},{2,3},{-2,3},{3,2},{-3,2},{0,4},{4,0},
        {1,4},{-1,4},{4,1},{-4,1},{3,3},{-3,3},{2,4},{-2,4},{4,2},{-4,2},{0,5},{3,4},
        {-3,4},{4,3},{-4,3},{5,0},{1,5},{-1,5},{5,1},{-5,1},{2,5},{-2,5},{5,2},{-5,2},
        {4,4},{-4,4},{3,5},{-3,5},{5,3},{-5,3},{0,6},{6,0},{1,6},{-1,6},{6,1},{-6,1},
        {2,6},{-2,6},{6,2},{-6,2},{4,5},{-4,5},{5,4},{-5,4},{3,6},{-3,6},{6,3},{-6,3},
        {0,7},{7,0},{1,7},{-1,7},{5,5},{-5,5},{7,1},{-7,1},{4,6},{-4,6},{6,4},{-6,4},
        {2,7},{-2,7},{7,2},{-7,2},{3,7},{-3,7},{7,3},{-7,3},{5,6},{-5,6},{6,5},{-6,5},
        {8,0},{4,7},{-4,7},{7,4},{-7,4},{8,1},{8,2},{6,6},{-6,6},{8,3},{5,7},{-5,7},
        {7,5},{-7,5},{8,4},{6,7},{-6,7},{7,6},{-7,6},{8,5},{7,7},{-7,7},{8,6},{8,7}
    };
    if (code <= 0) return 0;
    if (code > 120) return code - 120;
    int dist = (int)map[code - 1][0] + (int)map[code - 1][1] * width;
    return std::max(1, dist);
}

uint32_t PackArgb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint32_t ColorCacheHash(uint32_t argb, int bits) {
    return (argb * 0x1e35a7bdu) >> (32 - bits);
}

uint8_t Add8(uint8_t a, uint8_t b) { return (uint8_t)((int)a + (int)b); }
uint8_t Avg2(uint8_t a, uint8_t b) { return (uint8_t)(((int)a + (int)b) >> 1); }

uint32_t AddPixels(uint32_t a, uint32_t b) {
    return PackArgb(Add8(a >> 24, b >> 24), Add8(a >> 16, b >> 16), Add8(a >> 8, b >> 8), Add8(a, b));
}

uint32_t AvgPixels(uint32_t a, uint32_t b) {
    return PackArgb(Avg2(a >> 24, b >> 24), Avg2(a >> 16, b >> 16), Avg2(a >> 8, b >> 8), Avg2(a, b));
}

int ClampedAdd(int a, int b, int c) { return std::max(0, std::min(255, a + b - c)); }

uint32_t ClampedAddPixels(uint32_t a, uint32_t b, uint32_t c) {
    return PackArgb((uint8_t)ClampedAdd(a >> 24, b >> 24, c >> 24),
                    (uint8_t)ClampedAdd(a >> 16, b >> 16, c >> 16),
                    (uint8_t)ClampedAdd(a >> 8, b >> 8, c >> 8),
                    (uint8_t)ClampedAdd(a, b, c));
}

int ColorDistance(uint32_t a, uint32_t b) {
    return std::abs((int)(uint8_t)(a >> 24) - (int)(uint8_t)(b >> 24)) +
           std::abs((int)(uint8_t)(a >> 16) - (int)(uint8_t)(b >> 16)) +
           std::abs((int)(uint8_t)(a >> 8) - (int)(uint8_t)(b >> 8)) +
           std::abs((int)(uint8_t)a - (int)(uint8_t)b);
}

uint32_t SelectPixel(uint32_t left, uint32_t top, uint32_t topLeft) {
    uint32_t pred = ClampedAddPixels(left, top, topLeft);
    return ColorDistance(pred, left) < ColorDistance(pred, top) ? left : top;
}

int Clamp8(int v) { return std::max(0, std::min(255, v)); }

uint32_t ClampedHalfPixels(uint32_t a, uint32_t b) {
    return PackArgb((uint8_t)Clamp8((int)(uint8_t)(a >> 24) + ((int)(uint8_t)(a >> 24) - (int)(uint8_t)(b >> 24)) / 2),
                    (uint8_t)Clamp8((int)(uint8_t)(a >> 16) + ((int)(uint8_t)(a >> 16) - (int)(uint8_t)(b >> 16)) / 2),
                    (uint8_t)Clamp8((int)(uint8_t)(a >> 8) + ((int)(uint8_t)(a >> 8) - (int)(uint8_t)(b >> 8)) / 2),
                    (uint8_t)Clamp8((int)(uint8_t)a + ((int)(uint8_t)a - (int)(uint8_t)b) / 2));
}

int8_t S8(uint32_t v) { return (int8_t)(uint8_t)v; }
uint8_t ColorTransformDelta(int8_t t, uint8_t c) { return (uint8_t)(((int)t * (int)S8(c)) >> 5); }

struct Vp8lTransform {
    int type = 0;
    int bits = 0;
    int width = 0;
    std::vector<uint32_t> data;
};

struct Vp8lPrefixGroup {
    std::array<HuffmanCode, 5> codes;
};

uint32_t PredictorPixel(int mode, uint32_t left, uint32_t top, uint32_t topLeft, uint32_t topRight) {
    static const uint32_t black = 0xff000000u;
    switch (mode) {
    case 0: return black;
    case 1: return left;
    case 2: return top;
    case 3: return topRight;
    case 4: return topLeft;
    case 5: return AvgPixels(AvgPixels(left, topRight), top);
    case 6: return AvgPixels(left, topLeft);
    case 7: return AvgPixels(left, top);
    case 8: return AvgPixels(topLeft, top);
    case 9: return AvgPixels(top, topRight);
    case 10: return AvgPixels(AvgPixels(left, topLeft), AvgPixels(top, topRight));
    case 11: return SelectPixel(left, top, topLeft);
    case 12: return ClampedAddPixels(left, top, topLeft);
    case 13: return ClampedHalfPixels(AvgPixels(left, top), topLeft);
    default: return black;
    }
}

bool DecodeVp8lImage(BitReader& br, int width, int height, bool allowMeta, std::vector<uint32_t>& pixels) {
    if (width <= 0 || height <= 0 || (uint64_t)width * height > 64ull * 1000 * 1000) return FailVp8l("size");
    uint32_t cacheFlag = 0;
    if (!br.ReadBits(1, cacheFlag)) return FailVp8l("cacheflag");
    int cacheBits = 0;
    if (cacheFlag) {
        uint32_t v = 0;
        if (!br.ReadBits(4, v) || v < 1 || v > 11) return FailVp8l("cachebits");
        cacheBits = (int)v;
    }
    int prefixBits = 0;
    int prefixWidth = 1;
    std::vector<uint32_t> entropyImage;
    int numGroups = 1;
    if (allowMeta) {
        uint32_t meta = 0;
        if (!br.ReadBits(1, meta)) return FailVp8l("metaflag");
        if (meta) {
            uint32_t bits = 0;
            if (!br.ReadBits(3, bits)) return FailVp8l("prefixbits");
            prefixBits = (int)bits + 2;
            prefixWidth = (width + (1 << prefixBits) - 1) >> prefixBits;
            int prefixHeight = (height + (1 << prefixBits) - 1) >> prefixBits;
            if (!DecodeVp8lImage(br, prefixWidth, prefixHeight, false, entropyImage)) return FailVp8l("entropyimage");
            for (uint32_t p : entropyImage) {
                int group = (int)((p >> 8) & 0xffffu);
                if (group > 255) return FailVp8l("groupcap"); // fail-safe cap for tiny in-tree decoder.
                numGroups = std::max(numGroups, group + 1);
            }
        }
    }

    int cacheSize = cacheBits ? (1 << cacheBits) : 0;
    std::vector<Vp8lPrefixGroup> groups((size_t)numGroups);
    const std::array<int, 5> alphabetSizes{ 256 + 24 + cacheSize, 256, 256, 256, 40 };
    for (int g = 0; g < numGroups; ++g)
        for (int i = 0; i < 5; ++i)
            if (!ReadHuffmanCode(br, alphabetSizes[i], groups[(size_t)g].codes[i])) return FailVp8l("huff");

    pixels.clear();
    pixels.reserve((size_t)width * height);
    std::vector<uint32_t> cache((size_t)cacheSize, 0);
    auto pushPixel = [&](uint32_t argb) -> bool {
        if (pixels.size() >= (size_t)width * height) return false;
        pixels.push_back(argb);
        if (cacheSize) cache[ColorCacheHash(argb, cacheBits)] = argb;
        return true;
    };

    while (pixels.size() < (size_t)width * height) {
        size_t pos = pixels.size();
        int groupIndex = 0;
        if (!entropyImage.empty()) {
            int x = (int)(pos % (size_t)width);
            int y = (int)(pos / (size_t)width);
            groupIndex = (int)((entropyImage[(size_t)(y >> prefixBits) * prefixWidth + (x >> prefixBits)] >> 8) & 0xffffu);
            if (groupIndex < 0 || groupIndex >= numGroups) return FailVp8l("groupindex");
        }
        const auto& codes = groups[(size_t)groupIndex].codes;
        int green = 0;
        if (!codes[0].Decode(br, green)) return FailVp8l("green");
        if (green < 256) {
            int red = 0, blue = 0, alpha = 0;
            if (!codes[1].Decode(br, red) || !codes[2].Decode(br, blue) || !codes[3].Decode(br, alpha)) return FailVp8l("rgba");
            if (red > 255 || blue > 255 || alpha > 255) return FailVp8l("literalrange");
            if (!pushPixel(PackArgb((uint8_t)alpha, (uint8_t)red, (uint8_t)green, (uint8_t)blue))) return FailVp8l("pushlit");
        } else if (green < 280) {
            int length = 0, distPrefix = 0, distance = 0;
            if (!ReadVp8lPrefix(br, green - 256, length) || !codes[4].Decode(br, distPrefix) || !ReadVp8lPrefix(br, distPrefix, distance)) return FailVp8l("lzprefix");
            distance = Vp8lDistanceToPlaneDistance(distance, width);
            if (distance <= 0 || (size_t)distance > pixels.size()) return FailVp8l("distance");
            for (int i = 0; i < length; ++i) {
                uint32_t p = pixels[pixels.size() - (size_t)distance];
                if (!pushPixel(p)) return FailVp8l("pushlz");
            }
        } else {
            int index = green - 280;
            if (index < 0 || index >= cacheSize) return FailVp8l("cacheindex");
            if (!pushPixel(cache[(size_t)index])) return FailVp8l("pushcache");
        }
    }
    return true;
}

bool ApplyVp8lTransforms(std::vector<Vp8lTransform>& transforms, int width, int height, std::vector<uint32_t>& pixels) {
    for (int ti = (int)transforms.size() - 1; ti >= 0; --ti) {
        const Vp8lTransform& t = transforms[(size_t)ti];
        if (t.type == 2) {
            for (uint32_t& p : pixels) {
                uint8_t a = (uint8_t)(p >> 24), r = (uint8_t)(p >> 16), g = (uint8_t)(p >> 8), b = (uint8_t)p;
                p = PackArgb(a, Add8(r, g), g, Add8(b, g));
            }
        } else if (t.type == 1) {
            if (t.width <= 0 || t.data.empty()) return false;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                size_t i = (size_t)y * width + x;
                uint32_t c = t.data[(size_t)(y >> t.bits) * t.width + (x >> t.bits)];
                uint8_t a = (uint8_t)(pixels[i] >> 24), r = (uint8_t)(pixels[i] >> 16), g = (uint8_t)(pixels[i] >> 8), b = (uint8_t)pixels[i];
                b = Add8(b, ColorTransformDelta(S8(c), g));
                r = Add8(r, ColorTransformDelta(S8(c >> 8), g));
                r = Add8(r, ColorTransformDelta(S8(c >> 16), b));
                pixels[i] = PackArgb(a, r, g, b);
            }
        } else if (t.type == 0) {
            if (t.width <= 0 || t.data.empty()) return false;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                size_t i = (size_t)y * width + x;
                uint32_t left = x ? pixels[i - 1] : 0xff000000u;
                uint32_t top = y ? pixels[i - (size_t)width] : 0xff000000u;
                uint32_t topLeft = (x && y) ? pixels[i - (size_t)width - 1] : 0xff000000u;
                uint32_t topRight = (y && x + 1 < width) ? pixels[i - (size_t)width + 1] : top;
                int mode = (int)(t.data[(size_t)(y >> t.bits) * t.width + (x >> t.bits)] >> 8) & 15;
                pixels[i] = AddPixels(pixels[i], PredictorPixel(mode, left, top, topLeft, topRight));
            }
        } else if (t.type == 3) {
            if (t.data.empty()) return false;
            std::vector<uint32_t> out((size_t)width * height);
            int bitsPerPixel = t.bits == 0 ? 8 : (t.bits == 1 ? 4 : (t.bits == 2 ? 2 : 1));
            int mask = (1 << bitsPerPixel) - 1;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                uint32_t packed = pixels[(size_t)y * ((width + (1 << t.bits) - 1) >> t.bits) + (x >> t.bits)];
                int shift = bitsPerPixel * (x & ((1 << t.bits) - 1));
                int idx = (int)((packed >> (8 + shift)) & mask);
                out[(size_t)y * width + x] = idx < (int)t.data.size() ? t.data[(size_t)idx] : 0;
            }
            pixels.swap(out);
        } else return false;
    }
    return pixels.size() == (size_t)width * height;
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
        else ok = false;
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

uint8_t Clip8(int v);

static const uint8_t kVp8Zigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static const uint8_t kVp8Bands[17] = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0};
static const uint8_t kVp8DcTable[128] = {
    4,5,6,7,8,9,10,10,11,12,13,14,15,16,17,17,18,19,20,20,21,21,22,22,23,23,24,25,25,26,27,28,
    29,30,31,32,33,34,35,36,37,37,38,39,40,41,42,43,44,45,46,46,47,48,49,50,51,52,53,54,55,56,
    57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,76,77,78,79,80,81,82,83,84,85,
    86,87,88,89,91,93,95,96,98,100,101,102,104,106,108,110,112,114,116,118,122,124,126,128,130,132,134,136,
    138,140,143,145,148,151,154,157
};
static const uint16_t kVp8AcTable[128] = {
    4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,
    36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,60,62,64,66,68,70,72,
    74,76,78,80,82,84,86,88,90,92,94,96,98,100,102,104,106,108,110,112,114,116,119,122,125,128,131,134,137,140,
    143,146,149,152,155,158,161,164,167,170,173,177,181,185,189,193,197,201,205,209,213,217,221,225,229,234,239,245,
    249,254,259,264,269,274,279,284
};
static const uint8_t kVp8CoefProbs[4][8][3][11] = {
{{{128,128,128,128,128,128,128,128,128,128,128},{128,128,128,128,128,128,128,128,128,128,128},{128,128,128,128,128,128,128,128,128,128,128}},{{253,136,254,255,228,219,128,128,128,128,128},{189,129,242,255,227,213,255,219,128,128,128},{106,126,227,252,214,209,255,255,128,128,128}},{{1,98,248,255,236,226,255,255,128,128,128},{181,133,238,254,221,234,255,154,128,128,128},{78,134,202,247,198,180,255,219,128,128,128}},{{1,185,249,255,243,255,128,128,128,128,128},{184,150,247,255,236,224,128,128,128,128,128},{77,110,216,255,236,230,128,128,128,128,128}},{{1,101,251,255,241,255,128,128,128,128,128},{170,139,241,252,236,209,255,255,128,128,128},{37,116,196,243,228,255,255,255,128,128,128}},{{1,204,254,255,245,255,128,128,128,128,128},{207,160,250,255,238,128,128,128,128,128,128},{102,103,231,255,211,171,128,128,128,128,128}},{{1,152,252,255,240,255,128,128,128,128,128},{177,135,243,255,234,225,128,128,128,128,128},{80,129,211,255,194,224,128,128,128,128,128}},{{1,1,255,128,128,128,128,128,128,128,128},{246,1,255,128,128,128,128,128,128,128,128},{255,128,128,128,128,128,128,128,128,128,128}}},
{{{198,35,237,223,193,187,162,160,145,155,62},{131,45,198,221,172,176,220,157,252,221,1},{68,47,146,208,149,167,221,162,255,223,128}},{{1,149,241,255,221,224,255,255,128,128,128},{184,141,234,253,222,220,255,199,128,128,128},{81,99,181,242,176,190,249,202,255,255,128}},{{1,129,232,253,214,197,242,196,255,255,128},{99,121,210,250,201,198,255,202,128,128,128},{23,91,163,242,170,187,247,210,255,255,128}},{{1,200,246,255,234,255,128,128,128,128,128},{109,178,241,255,231,245,255,255,128,128,128},{44,130,201,253,205,192,255,255,128,128,128}},{{1,132,239,251,219,209,255,165,128,128,128},{94,136,225,251,218,190,255,255,128,128,128},{22,100,174,245,186,161,255,199,128,128,128}},{{1,182,249,255,232,235,128,128,128,128,128},{124,143,241,255,227,234,128,128,128,128,128},{35,77,181,251,193,211,255,205,128,128,128}},{{1,157,247,255,236,231,255,255,128,128,128},{121,141,235,255,225,227,255,255,128,128,128},{45,99,188,251,195,217,255,224,128,128,128}},{{1,1,251,255,213,255,128,128,128,128,128},{203,1,248,255,255,128,128,128,128,128,128},{137,1,177,255,224,255,128,128,128,128,128}}},
{{{253,9,248,251,207,208,255,192,128,128,128},{175,13,224,243,193,185,249,198,255,255,128},{73,17,171,221,161,179,236,167,255,234,128}},{{1,95,247,253,212,183,255,255,128,128,128},{239,90,244,250,211,209,255,255,128,128,128},{155,77,195,248,188,195,255,255,128,128,128}},{{1,24,239,251,218,219,255,205,128,128,128},{201,51,219,255,196,186,128,128,128,128,128},{69,46,190,239,201,218,255,228,128,128,128}},{{1,191,251,255,255,128,128,128,128,128,128},{223,165,249,255,213,255,128,128,128,128,128},{141,124,248,255,255,128,128,128,128,128,128}},{{1,16,248,255,255,128,128,128,128,128,128},{190,36,230,255,236,255,128,128,128,128,128},{149,1,255,128,128,128,128,128,128,128,128}},{{1,226,255,128,128,128,128,128,128,128,128},{247,192,255,128,128,128,128,128,128,128,128},{240,128,255,128,128,128,128,128,128,128,128}},{{1,134,252,255,255,128,128,128,128,128,128},{213,62,250,255,255,128,128,128,128,128,128},{55,93,255,128,128,128,128,128,128,128,128}},{{128,128,128,128,128,128,128,128,128,128,128},{128,128,128,128,128,128,128,128,128,128,128},{128,128,128,128,128,128,128,128,128,128,128}}},
{{{202,24,213,235,186,191,220,160,240,175,255},{126,38,182,232,169,184,228,174,255,187,128},{61,46,138,219,151,178,240,170,255,216,128}},{{1,112,230,250,199,191,247,159,255,255,128},{166,109,228,252,211,215,255,174,128,128,128},{39,77,162,232,172,180,245,178,255,255,128}},{{1,52,220,246,198,199,249,220,255,255,128},{124,74,191,243,183,193,250,221,255,255,128},{24,71,130,219,154,170,243,182,255,255,128}},{{1,182,225,249,219,240,255,224,128,128,128},{149,150,226,252,216,205,255,171,128,128,128},{28,108,170,242,183,194,254,223,255,255,128}},{{1,81,230,252,204,203,255,192,128,128,128},{123,102,209,247,188,196,255,233,128,128,128},{20,95,153,243,164,173,255,203,128,128,128}},{{1,222,248,255,216,213,128,128,128,128,128},{168,175,246,252,235,205,255,255,128,128,128},{47,116,215,255,211,212,255,255,128,128,128}},{{1,121,236,253,212,214,255,255,128,128,128},{141,84,213,252,201,202,255,219,128,128,128},{42,80,160,240,162,185,255,205,128,128,128}},{{1,1,255,128,128,128,128,128,128,128,128},{244,1,255,128,128,128,128,128,128,128,128},{238,1,255,128,128,128,128,128,128,128,128}}}
};

struct Vp8Quant { int y1dc, y1ac, y2dc, y2ac, uvdc, uvac; };

int ClampQ(int v, int max) { return std::max(0, std::min(max, v)); }
int ReadSignedLit(BoolReader& br, int n) { int v = (int)br.Lit(n); return br.Bit() ? -v : v; }

bool ReadVp8Quant(BoolReader& br, Vp8Quant& q) {
    int base = (int)br.Lit(7);
    int dy1dc = br.Bit() ? ReadSignedLit(br, 4) : 0;
    int dy2dc = br.Bit() ? ReadSignedLit(br, 4) : 0;
    int dy2ac = br.Bit() ? ReadSignedLit(br, 4) : 0;
    int duvdc = br.Bit() ? ReadSignedLit(br, 4) : 0;
    int duvac = br.Bit() ? ReadSignedLit(br, 4) : 0;
    q.y1dc = kVp8DcTable[ClampQ(base + dy1dc, 127)];
    q.y1ac = kVp8AcTable[ClampQ(base, 127)];
    q.y2dc = kVp8DcTable[ClampQ(base + dy2dc, 127)] * 2;
    q.y2ac = std::max(8, (kVp8AcTable[ClampQ(base + dy2ac, 127)] * 101581) >> 16);
    q.uvdc = kVp8DcTable[ClampQ(base + duvdc, 117)];
    q.uvac = kVp8AcTable[ClampQ(base + duvac, 127)];
    return br.ok;
}

int ReadLargeCoeff(BoolReader& br, const uint8_t* p) {
    static const uint8_t c3[] = {173,148,140,0}, c4[] = {176,155,140,135,0}, c5[] = {180,157,141,134,130,0};
    static const uint8_t c6[] = {254,254,243,230,196,177,153,140,133,130,129,0};
    static const uint8_t* cs[] = {c3, c4, c5, c6};
    if (!br.Bit(p[3])) return !br.Bit(p[4]) ? 2 : 3 + br.Bit(p[5]);
    if (!br.Bit(p[6])) return !br.Bit(p[7]) ? 5 + br.Bit(159) : 7 + 2 * br.Bit(165) + br.Bit(145);
    int cat = 2 * br.Bit(p[8]); cat += br.Bit(p[9 + (cat >> 1)]);
    int v = 0; for (const uint8_t* t = cs[cat]; *t; ++t) v += v + br.Bit(*t);
    return v + 3 + (8 << cat);
}

int ReadCoeffs(BoolReader& br, int type, int ctx, int dcq, int acq, int first, int16_t* out) {
    for (int n = first; n < 16; ++n) {
        const uint8_t* p = kVp8CoefProbs[type][kVp8Bands[n]][ctx];
        if (!br.Bit(p[0])) return n;
        while (!br.Bit(p[1])) {
            if (++n == 16) return 16;
            p = kVp8CoefProbs[type][kVp8Bands[n]][0];
        }
        int v = !br.Bit(p[2]) ? 1 : ReadLargeCoeff(br, p);
        out[kVp8Zigzag[n]] = (int16_t)((br.Bit() ? -v : v) * (n ? acq : dcq));
        ctx = (v == 1) ? 1 : 2;
    }
    return 16;
}

void InverseWht(const int16_t* in, int16_t* out) {
    int tmp[16];
    for (int i = 0; i < 4; ++i) {
        int a0 = in[i] + in[12 + i], a1 = in[4 + i] + in[8 + i], a2 = in[4 + i] - in[8 + i], a3 = in[i] - in[12 + i];
        tmp[i] = a0 + a1; tmp[8 + i] = a0 - a1; tmp[4 + i] = a3 + a2; tmp[12 + i] = a3 - a2;
    }
    for (int i = 0; i < 4; ++i) {
        int dc = tmp[i * 4] + 3, a0 = dc + tmp[3 + i * 4], a1 = tmp[1 + i * 4] + tmp[2 + i * 4];
        int a2 = tmp[1 + i * 4] - tmp[2 + i * 4], a3 = dc - tmp[3 + i * 4];
        out[i * 4 + 0] = (int16_t)((a0 + a1) >> 3); out[i * 4 + 1] = (int16_t)((a3 + a2) >> 3);
        out[i * 4 + 2] = (int16_t)((a0 - a1) >> 3); out[i * 4 + 3] = (int16_t)((a3 - a2) >> 3);
    }
}

void AddIdct4(const int16_t* in, uint8_t* dst, int stride) {
    int tmp[16];
    for (int i = 0; i < 4; ++i) {
        int a = in[i] + in[8 + i], b = in[i] - in[8 + i];
        int c = ((in[4 + i] * 35468) >> 16) - ((in[12 + i] * 20091) >> 16);
        int d = ((in[4 + i] * 20091) >> 16) + ((in[12 + i] * 35468) >> 16);
        tmp[i] = a + d; tmp[4 + i] = b + c; tmp[8 + i] = b - c; tmp[12 + i] = a - d;
    }
    for (int y = 0; y < 4; ++y) {
        int dc = tmp[y * 4] + 4, a = dc + tmp[y * 4 + 2], b = dc - tmp[y * 4 + 2];
        int c = ((tmp[y * 4 + 1] * 35468) >> 16) - ((tmp[y * 4 + 3] * 20091) >> 16);
        int d = ((tmp[y * 4 + 1] * 20091) >> 16) + ((tmp[y * 4 + 3] * 35468) >> 16);
        dst[0] = Clip8(dst[0] + ((a + d) >> 3)); dst[1] = Clip8(dst[1] + ((b + c) >> 3));
        dst[2] = Clip8(dst[2] + ((b - c) >> 3)); dst[3] = Clip8(dst[3] + ((a - d) >> 3));
        dst += stride;
    }
}

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
    Vp8Quant q;
    if (!ReadVp8Quant(br, q)) return result;
    (void)br.Bit(); // refresh entropy probs
    for (int i = 0; i < 4 * 8 * 3 * 11; ++i) if (br.Bit(252)) return result; // updated coefficient probabilities unsupported
    int mbNoCoeffSkip = br.Bit();
    int probSkip = mbNoCoeffSkip ? (int)br.Lit(8) : 0;

    static const uint8_t yProb[] = { 145, 156, 163, 128 };
    static const uint8_t uvProb[] = { 142, 114, 183, 128 };

    std::vector<uint8_t> yPlane((size_t)w * h, 128), uPlane((size_t)((w + 1) / 2) * ((h + 1) / 2), 128), vPlane(uPlane.size(), 128);
    int mbw = (w + 15) / 16;
    int mbh = (h + 15) / 16;
    if (chunkSize <= 10 + firstPartSize) return result;
    BoolReader tr;
    if (!tr.Init(chunk + 10 + firstPartSize, chunkSize - 10 - firstPartSize)) return result;
    std::vector<uint8_t> topNz((size_t)mbw * 16, 0), topUNz((size_t)mbw * 4, 0), topVNz((size_t)mbw * 4, 0), topY2Nz((size_t)mbw, 0);
    for (int my = 0; my < mbh; ++my) {
        uint8_t leftNz[16] = {}, leftUNz[4] = {}, leftVNz[4] = {}, leftY2Nz = 0;
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
            int16_t yCoeff[16][16] = {}, y2[16] = {}, y2out[16] = {}, uCoeff[4][16] = {}, vCoeff[4][16] = {};
            if (!skip) {
                int y2ctx = leftY2Nz + topY2Nz[(size_t)mx];
                int y2nz = ReadCoeffs(tr, 1, y2ctx, q.y2dc, q.y2ac, 0, y2);
                leftY2Nz = topY2Nz[(size_t)mx] = (uint8_t)(y2nz > 0);
                if (y2nz > 0) InverseWht(y2, y2out);
                for (int by = 0; by < 4; ++by) for (int bx = 0; bx < 4; ++bx) {
                    int bi = by * 4 + bx;
                    yCoeff[bi][0] = y2out[bi];
                    int ctx = leftNz[by * 4 + bx] + topNz[(size_t)mx * 16 + bx * 4 + by];
                    int nz = ReadCoeffs(tr, 0, ctx, q.y1dc, q.y1ac, 1, yCoeff[bi]);
                    leftNz[by * 4 + bx] = topNz[(size_t)mx * 16 + bx * 4 + by] = (uint8_t)(nz > 1 || yCoeff[bi][0]);
                }
                for (int ch = 0; ch < 2; ++ch) for (int by = 0; by < 2; ++by) for (int bx = 0; bx < 2; ++bx) {
                    int bi = by * 2 + bx;
                    uint8_t* left = ch ? leftVNz : leftUNz;
                    std::vector<uint8_t>& top = ch ? topVNz : topUNz;
                    int ctx = left[by * 2 + bx] + top[(size_t)mx * 4 + bx * 2 + by];
                    int nz = ReadCoeffs(tr, 2, ctx, q.uvdc, q.uvac, 0, ch ? vCoeff[bi] : uCoeff[bi]);
                    left[by * 2 + bx] = top[(size_t)mx * 4 + bx * 2 + by] = (uint8_t)(nz > 0);
                }
            }
            for (int by = 0; by < 4; ++by) for (int bx = 0; bx < 4; ++bx) {
                int px = x + bx * 4, py = yy + by * 4;
                if (px < w && py < h) {
                    uint8_t block[4 * 4] = {};
                    int rows = std::min(4, h - py), cols = std::min(4, w - px);
                    for (int r = 0; r < rows; ++r) std::memcpy(block + r * 4, &yPlane[(size_t)(py + r) * w + px], cols);
                    AddIdct4(yCoeff[by * 4 + bx], block, 4);
                    for (int r = 0; r < rows; ++r) std::memcpy(&yPlane[(size_t)(py + r) * w + px], block + r * 4, cols);
                }
            }
            for (int ch = 0; ch < 2; ++ch) {
                std::vector<uint8_t>& plane = ch ? vPlane : uPlane;
                int16_t (*coeff)[16] = ch ? vCoeff : uCoeff;
                for (int by = 0; by < 2; ++by) for (int bx = 0; bx < 2; ++bx) {
                    int px = cx + bx * 4, py = cy + by * 4;
                    if (px < cw && py < (h + 1) / 2) {
                        uint8_t block[4 * 4] = {};
                        int rows = std::min(4, (h + 1) / 2 - py), cols = std::min(4, cw - px);
                        for (int r = 0; r < rows; ++r) std::memcpy(block + r * 4, &plane[(size_t)(py + r) * cw + px], cols);
                        AddIdct4(coeff[by * 2 + bx], block, 4);
                        for (int r = 0; r < rows; ++r) std::memcpy(&plane[(size_t)(py + r) * cw + px], block + r * 4, cols);
                    }
                }
            }
        }
    }
    if (!br.ok || !tr.ok) return result;
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
    std::vector<Vp8lTransform> transforms;
    int imageWidth = w, imageHeight = h;
    uint32_t bit = 0;
    while (true) {
        if (!br.ReadBits(1, bit)) return result;
        if (!bit) break;
        uint32_t type = 0;
        if (!br.ReadBits(2, type)) return result;
        Vp8lTransform transform;
        transform.type = (int)type;
        if (type == 0 || type == 1) {
            uint32_t bits = 0;
            if (!br.ReadBits(3, bits)) return result;
            transform.bits = (int)bits + 2;
            transform.width = (imageWidth + (1 << transform.bits) - 1) >> transform.bits;
            int th = (imageHeight + (1 << transform.bits) - 1) >> transform.bits;
            if (!DecodeVp8lImage(br, transform.width, th, false, transform.data)) return result;
        } else if (type == 2) {
            // no payload
        } else if (type == 3) {
            uint32_t paletteSizeMinusOne = 0;
            if (!br.ReadBits(8, paletteSizeMinusOne)) return result;
            int paletteSize = (int)paletteSizeMinusOne + 1;
            transform.bits = paletteSize <= 2 ? 3 : (paletteSize <= 4 ? 2 : (paletteSize <= 16 ? 1 : 0));
            if (!DecodeVp8lImage(br, paletteSize, 1, false, transform.data)) return result;
            for (size_t i = 1; i < transform.data.size(); ++i) transform.data[i] = AddPixels(transform.data[i], transform.data[i - 1]);
            imageWidth = (imageWidth + (1 << transform.bits) - 1) >> transform.bits;
        } else return result;
        transforms.push_back(std::move(transform));
    }

    std::vector<uint32_t> pixels;
    if (!DecodeVp8lImage(br, imageWidth, imageHeight, true, pixels)) return result;
    if (!ApplyVp8lTransforms(transforms, w, h, pixels)) return result;

    result.width = w;
    result.height = h;
    result.rgba.resize((size_t)w * (size_t)h * 4);
    for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) {
        uint32_t p = pixels[i];
        result.rgba[i * 4 + 0] = (uint8_t)(p >> 16);
        result.rgba[i * 4 + 1] = (uint8_t)(p >> 8);
        result.rgba[i * 4 + 2] = (uint8_t)p;
        result.rgba[i * 4 + 3] = (uint8_t)(p >> 24);
    }
    result.success = true;
    return result;
}

bool DecodeVp8LosslessAlpha(const uint8_t* chunk, size_t chunkSize, int w, int h, std::vector<uint8_t>& alpha) {
    if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > 64ull * 1000 * 1000) return false;
    BitReader br{chunk, chunkSize, 0};
    uint32_t bit = 0;
    if (!br.ReadBits(1, bit) || bit != 0) return false;
    if (!br.ReadBits(1, bit) || bit != 0) return false;
    if (!br.ReadBits(1, bit) || bit != 0) return false;

    std::array<int, 5> symbols{};
    const std::array<int, 5> alphabetSizes{256 + 24, 256, 256, 256, 40};
    for (size_t i = 0; i < symbols.size(); ++i) {
        if (!ReadSimpleSingleSymbol(br, alphabetSizes[i], symbols[i])) return false;
    }
    if (symbols[0] >= 256) return false;
    alpha.assign((size_t)w * (size_t)h, (uint8_t)symbols[0]);
    return true;
}

bool ApplyAlphaFilter(std::vector<uint8_t>& alpha, int w, int h, int filter) {
    if (filter < 0 || filter > 3) return false;
    if (filter == 0) return true;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int left = x ? alpha[(size_t)y * w + x - 1] : 0;
            int top = y ? alpha[(size_t)(y - 1) * w + x] : 0;
            int topLeft = (x && y) ? alpha[(size_t)(y - 1) * w + x - 1] : 0;
            int pred = 0;
            if (filter == 1) pred = x ? left : top;
            else if (filter == 2) pred = y ? top : left;
            else pred = (x && y) ? Clip8(left + top - topLeft) : (x ? left : top);
            alpha[(size_t)y * w + x] = (uint8_t)((alpha[(size_t)y * w + x] + pred) & 255);
        }
    }
    return true;
}

bool DecodeAlph(const uint8_t* chunk, size_t chunkSize, int w, int h, std::vector<uint8_t>& alpha) {
    if (chunkSize < 1 || w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > 64ull * 1000 * 1000) return false;
    uint8_t header = chunk[0];
    int compression = header & 3;
    int filter = (header >> 2) & 3;
    if ((header >> 6) != 0) return false;
    const uint8_t* payload = chunk + 1;
    size_t payloadSize = chunkSize - 1;
    if (compression == 0) {
        if (payloadSize != (size_t)w * (size_t)h) return false;
        alpha.assign(payload, payload + payloadSize);
    } else if (compression == 1) {
        if (!DecodeVp8LosslessAlpha(payload, payloadSize, w, h, alpha)) return false;
    } else {
        return false;
    }
    return ApplyAlphaFilter(alpha, w, h, filter);
}

void ComposeAlpha(DecodedImage& img, const std::vector<uint8_t>& alpha) {
    if (!img.success || alpha.size() != (size_t)img.width * (size_t)img.height) return;
    for (size_t i = 0; i < alpha.size(); ++i) img.rgba[i * 4 + 3] = alpha[i];
}

} // namespace

DecodedImage DecodeWebp(const uint8_t* data, size_t size) {
    DecodedImage result;
    if (!data || size < 12) return result;
    if (!Match(data, "RIFF") || !Match(data + 8, "WEBP")) return result;
    uint32_t riffSize = ReadU32LE(data + 4);
    if ((uint64_t)riffSize + 8 > size) return result;

    size_t end = (size_t)riffSize + 8;
    size_t pos = 12;
    bool extended = false;
    int canvasW = 0, canvasH = 0;
    const uint8_t* alphaChunk = nullptr;
    size_t alphaSize = 0;
    while (pos + 8 <= end) {
        const uint8_t* fourcc = data + pos;
        uint32_t chunkSize = ReadU32LE(data + pos + 4);
        pos += 8;
        if (pos + chunkSize > end) return result;
        const uint8_t* chunk = data + pos;

        if (Match(fourcc, "VP8 ")) {
            result = DecodeVp8Lossy(chunk, chunkSize);
            break;
        }

        if (Match(fourcc, "VP8L")) {
            result = DecodeVp8Lossless(chunk, chunkSize);
            break;
        }

        if (Match(fourcc, "VP8X")) {
            if (chunkSize < 10) return result;
            if (extended) return result;
            uint32_t w = ReadU24LE(chunk + 4) + 1;
            uint32_t h = ReadU24LE(chunk + 7) + 1;
            if (w == 0 || h == 0 || (uint64_t)w * h > 64ull * 1000 * 1000) return result;
            if (chunk[0] & 0x02) return result; // animation unsupported
            extended = true;
            canvasW = (int)w;
            canvasH = (int)h;
        } else if (Match(fourcc, "ALPH")) {
            if (alphaChunk) return result;
            alphaChunk = chunk;
            alphaSize = chunkSize;
        }

        pos += chunkSize + (chunkSize & 1u);
    }
    if (!result.success) return result;
    if (extended && (result.width != canvasW || result.height != canvasH)) return DecodedImage{};
    if (alphaChunk && !extended) return DecodedImage{};
    if (alphaChunk) {
        std::vector<uint8_t> alpha;
        if (!DecodeAlph(alphaChunk, alphaSize, result.width, result.height, alpha)) return DecodedImage{};
        ComposeAlpha(result, alpha);
    }
    return result;
}
