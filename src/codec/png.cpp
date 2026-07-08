#include "codec/png.h"
#include "codec/inflate.h"
#include "codec/crc32.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint8_t kPngSignature[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };

// A generous but bounded cap on total pixels, so a maliciously/corruptly
// huge width*height in an IHDR chunk can't be used to force an enormous
// allocation before any real image data has even been validated.
constexpr uint64_t kMaxPixels = 64ull * 1000 * 1000; // 64 megapixels

uint32_t ReadU32BE(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int PaethPredictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

bool BitDepthValidForColorType(uint8_t colorType, uint8_t bitDepth) {
    switch (colorType) {
    case 0: return bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8 || bitDepth == 16;
    case 2: return bitDepth == 8 || bitDepth == 16;
    case 3: return bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8;
    case 4: return bitDepth == 8 || bitDepth == 16;
    case 6: return bitDepth == 8 || bitDepth == 16;
    default: return false;
    }
}

int ChannelsForColorType(uint8_t colorType) {
    switch (colorType) {
    case 0: return 1;
    case 2: return 3;
    case 3: return 1;
    case 4: return 2;
    case 6: return 4;
    default: return 0;
    }
}

} // namespace

DecodedImage DecodePng(const uint8_t* data, size_t size) {
    DecodedImage result;
    if (size < 8 || memcmp(data, kPngSignature, 8) != 0) return result;

    size_t pos = 8;
    uint32_t width = 0, height = 0;
    uint8_t bitDepth = 0, colorType = 0, interlace = 0;
    bool haveIhdr = false;
    std::vector<uint8_t> palette; // flat RGB triples
    std::vector<uint8_t> trns;
    std::string idatConcat;

    while (pos + 8 <= size) {
        uint32_t len = ReadU32BE(data + pos);
        if (pos + 8 + (size_t)len + 4 > size) return result; // truncated/corrupt chunk

        const uint8_t* type = data + pos + 4;
        const uint8_t* chunkData = data + pos + 8;
        const uint8_t* crcField = data + pos + 8 + len;

        std::vector<uint8_t> crcInput(4 + (size_t)len);
        memcpy(crcInput.data(), type, 4);
        if (len) memcpy(crcInput.data() + 4, chunkData, len);
        if (Crc32(crcInput.data(), crcInput.size()) != ReadU32BE(crcField)) return result;

        if (memcmp(type, "IHDR", 4) == 0) {
            if (len != 13 || haveIhdr) return result;
            width = ReadU32BE(chunkData);
            height = ReadU32BE(chunkData + 4);
            bitDepth = chunkData[8];
            colorType = chunkData[9];
            uint8_t compression = chunkData[10];
            uint8_t filterMethod = chunkData[11];
            interlace = chunkData[12];
            if (compression != 0 || filterMethod != 0) return result;
            if (width == 0 || height == 0) return result;
            if ((uint64_t)width * (uint64_t)height > kMaxPixels) return result;
            if (!BitDepthValidForColorType(colorType, bitDepth)) return result;
            haveIhdr = true;
        } else if (memcmp(type, "PLTE", 4) == 0) {
            if (len % 3 != 0) return result;
            palette.assign(chunkData, chunkData + len);
        } else if (memcmp(type, "tRNS", 4) == 0) {
            trns.assign(chunkData, chunkData + len);
        } else if (memcmp(type, "IDAT", 4) == 0) {
            idatConcat.append((const char*)chunkData, len);
        } else if (memcmp(type, "IEND", 4) == 0) {
            pos += 8 + len + 4;
            break;
        }
        // Any other chunk (ancillary — gAMA, cHRM, iCCP, tEXt, ...) is skipped.

        pos += 8 + (size_t)len + 4;
    }

    if (!haveIhdr || idatConcat.empty()) return result;
    if (interlace != 0) return result; // Adam7 not supported
    if (colorType == 3 && palette.empty()) return result; // indexed color requires a palette

    const int channels = ChannelsForColorType(colorType);
    if (channels == 0) return result;

    std::string inflated;
    if (!ZlibInflate((const uint8_t*)idatConcat.data(), idatConcat.size(), inflated)) return result;

    const int bitsPerPixel = channels * bitDepth;
    const int bytesPerScanline = (bitsPerPixel * (int)width + 7) / 8;
    const int bpp = std::max(1, (bitsPerPixel + 7) / 8);
    const size_t expectedSize = (size_t)(bytesPerScanline + 1) * height;
    if (inflated.size() < expectedSize) return result;

    // Defilter (RFC-less, but PNG spec §6) every scanline into a flat,
    // filter-byte-free buffer at the image's native bit depth/channel count.
    std::vector<uint8_t> raw((size_t)bytesPerScanline * height);
    std::vector<uint8_t> prevRow(bytesPerScanline, 0);
    size_t inPos = 0;
    for (uint32_t y = 0; y < height; y++) {
        uint8_t filterType = (uint8_t)inflated[inPos++];
        uint8_t* curRow = raw.data() + (size_t)y * bytesPerScanline;
        const uint8_t* filt = (const uint8_t*)inflated.data() + inPos;
        for (int x = 0; x < bytesPerScanline; x++) {
            int a = (x >= bpp) ? curRow[x - bpp] : 0;
            int b = prevRow[x];
            int c = (x >= bpp) ? prevRow[x - bpp] : 0;
            int val = filt[x];
            switch (filterType) {
            case 0: break;
            case 1: val += a; break;
            case 2: val += b; break;
            case 3: val += (a + b) / 2; break;
            case 4: val += PaethPredictor(a, b, c); break;
            default: return result;
            }
            curRow[x] = (uint8_t)(val & 0xFF);
        }
        inPos += bytesPerScanline;
        prevRow.assign(curRow, curRow + bytesPerScanline);
    }

    // Unpack native samples (possibly sub-byte-packed, possibly 16-bit) into
    // straight RGBA8.
    result.rgba.assign((size_t)width * height * 4, 255);
    const int maxRaw = (bitDepth == 16) ? 65535 : ((1 << bitDepth) - 1);
    auto getSample = [&](const uint8_t* row, int sampleIndex) -> uint32_t {
        if (bitDepth == 16) {
            return ((uint32_t)row[sampleIndex * 2] << 8) | row[sampleIndex * 2 + 1];
        } else if (bitDepth == 8) {
            return row[sampleIndex];
        } else {
            // Sub-byte samples are packed most-significant-bits-first
            // within each byte (PNG spec §7.2).
            int samplesPerByte = 8 / bitDepth;
            int byteIdx = sampleIndex / samplesPerByte;
            int sampleInByte = sampleIndex % samplesPerByte;
            int shift = 8 - bitDepth - sampleInByte * bitDepth;
            return (row[byteIdx] >> shift) & ((1 << bitDepth) - 1);
        }
    };
    // Exact for every bit depth PNG defines at 8-bit output: 255 is evenly
    // divisible by 1, 3, 15, and 255 (the max raw values for depths 1/2/4/8),
    // so this scales without rounding error.
    auto scale8 = [&](uint32_t raw) -> uint8_t {
        return (bitDepth == 16) ? (uint8_t)(raw >> 8) : (uint8_t)(raw * 255 / maxRaw);
    };

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = raw.data() + (size_t)y * bytesPerScanline;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t* out = &result.rgba[((size_t)y * width + x) * 4];
            switch (colorType) {
            case 0: { // grayscale
                uint32_t g = getSample(row, x);
                uint8_t g8 = scale8(g);
                out[0] = out[1] = out[2] = g8;
                out[3] = 255;
                if (trns.size() >= 2) {
                    uint32_t trnsVal = (bitDepth == 16) 
                        ? (((uint32_t)trns[0] << 8) | trns[1])
                        : trns[1];
                    if (g == trnsVal) out[3] = 0;
                }
                break;
            }
            case 2: { // truecolor
                uint32_t r = getSample(row, x * 3 + 0);
                uint32_t g = getSample(row, x * 3 + 1);
                uint32_t b = getSample(row, x * 3 + 2);
                out[0] = scale8(r); out[1] = scale8(g); out[2] = scale8(b); out[3] = 255;
                if (trns.size() >= 6) {
                    uint32_t tr, tg, tb;
                    if (bitDepth == 16) {
                        tr = ((uint32_t)trns[0] << 8) | trns[1];
                        tg = ((uint32_t)trns[2] << 8) | trns[3];
                        tb = ((uint32_t)trns[4] << 8) | trns[5];
                    } else {
                        tr = trns[1];
                        tg = trns[3];
                        tb = trns[5];
                    }
                    if (r == tr && g == tg && b == tb) out[3] = 0;
                }
                break;
            }
            case 3: { // indexed
                uint32_t idx = getSample(row, x);
                if ((idx + 1) * 3 > palette.size()) return DecodedImage{}; // corrupt: index beyond palette
                out[0] = palette[idx * 3 + 0];
                out[1] = palette[idx * 3 + 1];
                out[2] = palette[idx * 3 + 2];
                out[3] = (idx < trns.size()) ? trns[idx] : (uint8_t)255;
                break;
            }
            case 4: { // grayscale + alpha
                uint32_t g = getSample(row, x * 2 + 0);
                uint32_t a = getSample(row, x * 2 + 1);
                uint8_t g8 = scale8(g);
                out[0] = out[1] = out[2] = g8;
                out[3] = scale8(a);
                break;
            }
            case 6: { // truecolor + alpha
                uint32_t r = getSample(row, x * 4 + 0);
                uint32_t g = getSample(row, x * 4 + 1);
                uint32_t b = getSample(row, x * 4 + 2);
                uint32_t a = getSample(row, x * 4 + 3);
                out[0] = scale8(r); out[1] = scale8(g); out[2] = scale8(b); out[3] = scale8(a);
                break;
            }
            }
        }
    }

    result.width = (int)width;
    result.height = (int)height;
    result.success = true;
    return result;
}
