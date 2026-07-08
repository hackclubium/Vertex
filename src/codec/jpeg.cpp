#include "codec/jpeg.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr int kMaxComponents = 4;
constexpr double kPi = 3.14159265358979323846;

// RFC/spec reference: ITU-T T.81. The zigzag order DQT/DCT coefficients are
// stored in — index i is the natural (row-major) position of the i-th
// zigzag-order coefficient.
const int kZigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

// Canonical Huffman table, built directly from DHT's own representation
// (count of codes per length 1..16, then symbols in order) — unlike DEFLATE,
// JPEG's DHT segment already IS this shape, no per-symbol-length
// intermediate step needed.
struct HuffTable {
    int counts[17] = { 0 }; // counts[1..16] used, counts[0] always 0
    std::vector<uint8_t> symbols;
    bool valid = false;
};

// JPEG packs bits most-significant-bit-first within each byte, and any 0xFF
// byte in the entropy-coded data is followed by a stuffed 0x00 to keep it
// from being mistaken for a marker — this reader undoes both transparently.
// A real marker (0xFF followed by a non-zero byte) stops bit supply; the
// caller (for restart markers) or the outer scan loop (for EOI) checks for
// it directly on the byte stream.
class JpegBitReader {
public:
    JpegBitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    size_t pos() const { return pos_; }

    // Discards any unread bits in the current byte — used before checking
    // for a restart marker, which always starts on a byte boundary.
    void AlignToByte() { bitsLeft_ = 0; }

    // True if data_[pos_] is the start of a real marker (0xFF, non-zero) —
    // meaning no more entropy-coded bits are available right now.
    bool AtMarker() const {
        return bitsLeft_ == 0 && pos_ + 1 < size_ && data_[pos_] == 0xFF && data_[pos_ + 1] != 0x00;
    }

    bool error() const { return error_; }

    int ReadBit() {
        if (bitsLeft_ == 0) {
            if (AtMarker() || pos_ >= size_) { error_ = true; return 0; }
            uint8_t b = data_[pos_++];
            if (b == 0xFF) {
                // Must be a stuffed 0x00 (AtMarker() already ruled out a
                // real marker before we got here) — consume the stuffing
                // byte; b itself is a literal 0xFF data byte.
                if (pos_ >= size_) { error_ = true; return 0; }
                pos_++;
            }
            curByte_ = b;
            bitsLeft_ = 8;
        }
        int bit = (curByte_ >> (bitsLeft_ - 1)) & 1;
        bitsLeft_--;
        return bit;
    }

    uint32_t ReadBits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) v = (v << 1) | (uint32_t)ReadBit();
        return v;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    uint8_t curByte_ = 0;
    int bitsLeft_ = 0;
    bool error_ = false;
};

// Same canonical-Huffman one-bit-at-a-time decode as inflate.cpp's
// DecodeSymbol (JPEG also reads/assembles Huffman codes MSB-first, matching
// its natural bit order) — kept as its own small copy since JPEG's table
// shape (counts+symbols straight from DHT) differs from DEFLATE's
// (per-symbol lengths), not worth sharing across two otherwise-independent
// format modules for ~10 lines.
int DecodeHuffSymbol(JpegBitReader& br, const HuffTable& table) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 16; len++) {
        code = (code << 1) | br.ReadBit();
        int count = table.counts[len];
        if (code - first < count) return table.symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
    }
    return -1;
}

// "Receive and extend" (ITU-T T.81 §F.2.2.1): a T-bit magnitude `v` decodes
// to a signed value — v as-is if its top bit is 1, else v - (2^T - 1).
int Extend(int v, int t) {
    if (t == 0) return 0;
    return (v < (1 << (t - 1))) ? (v - (1 << t) + 1) : v;
}

struct Component {
    int id = 0;
    int h = 1, v = 1;        // sampling factors
    int quantTable = 0;
    int dcTable = 0, acTable = 0;
    int dcPred = 0;           // per-scan DC predictor, reset at restarts
    int blocksPerLine = 0, blocksPerColumn = 0; // component's own block grid size
    std::vector<uint8_t> pixels; // decoded samples at this component's own (possibly subsampled) resolution
    int pixelsStride = 0;
};

double Idct1DBasis[8][8];
bool g_idctBasisInit = false;
void EnsureIdctBasis() {
    if (g_idctBasisInit) return;
    for (int x = 0; x < 8; x++)
        for (int k = 0; k < 8; k++)
            Idct1DBasis[x][k] = std::cos((2 * x + 1) * k * kPi / 16.0);
    g_idctBasisInit = true;
}

void Idct1D(const double in[8], double out[8]) {
    for (int x = 0; x < 8; x++) {
        double sum = 0;
        for (int k = 0; k < 8; k++) {
            double ck = (k == 0) ? (1.0 / std::sqrt(2.0)) : 1.0;
            sum += ck * in[k] * Idct1DBasis[x][k];
        }
        out[x] = 0.5 * sum;
    }
}

// Separable 2D IDCT: 1D IDCT down each column, then across each resulting row.
void Idct2D(const double block[64], double out[64]) {
    double tmp[64];
    for (int v = 0; v < 8; v++) {
        double col[8], colOut[8];
        for (int u = 0; u < 8; u++) col[u] = block[u * 8 + v];
        Idct1D(col, colOut);
        for (int x = 0; x < 8; x++) tmp[x * 8 + v] = colOut[x];
    }
    for (int x = 0; x < 8; x++) {
        double row[8], rowOut[8];
        for (int v = 0; v < 8; v++) row[v] = tmp[x * 8 + v];
        Idct1D(row, rowOut);
        for (int y = 0; y < 8; y++) out[x * 8 + y] = rowOut[y];
    }
}

uint8_t ClampByte(double v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)(v + 0.5);
}

} // namespace

DecodedImage DecodeJpeg(const uint8_t* data, size_t size) {
    DecodedImage result;
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return result; // SOI

    EnsureIdctBasis();

    uint16_t quantTables[4][64] = {};
    bool haveQuantTable[4] = { false, false, false, false };
    HuffTable dcTables[4], acTables[4];

    int width = 0, height = 0;
    int numComponents = 0;
    Component comps[kMaxComponents];
    bool haveSof = false;
    int restartInterval = 0;

    size_t pos = 2;
    bool sawSos = false;

    while (pos + 4 <= size && !sawSos) {
        if (data[pos] != 0xFF) return result; // expected a marker
        uint8_t marker = data[pos + 1];
        pos += 2;
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue; // no length field
        if (pos + 2 > size) return result;
        int segLen = ((int)data[pos] << 8) | data[pos + 1];
        if (segLen < 2 || pos + segLen > size) return result;
        const uint8_t* seg = data + pos + 2;
        int segDataLen = segLen - 2;

        if (marker == 0xDB) { // DQT
            int p = 0;
            while (p < segDataLen) {
                uint8_t pq = seg[p] >> 4;
                uint8_t tq = seg[p] & 0x0F;
                p++;
                if (tq > 3) return result;
                if (pq == 0) {
                    if (p + 64 > segDataLen) return result;
                    for (int i = 0; i < 64; i++) quantTables[tq][kZigzag[i]] = seg[p + i];
                    p += 64;
                } else {
                    if (p + 128 > segDataLen) return result;
                    for (int i = 0; i < 64; i++)
                        quantTables[tq][kZigzag[i]] = ((uint16_t)seg[p + i * 2] << 8) | seg[p + i * 2 + 1];
                    p += 128;
                }
                haveQuantTable[tq] = true;
            }
        } else if (marker == 0xC4) { // DHT
            int p = 0;
            while (p < segDataLen) {
                uint8_t tc = seg[p] >> 4;
                uint8_t th = seg[p] & 0x0F;
                p++;
                if (tc > 1 || th > 3 || p + 16 > segDataLen) return result;
                HuffTable table;
                int total = 0;
                for (int i = 1; i <= 16; i++) { table.counts[i] = seg[p + i - 1]; total += table.counts[i]; }
                p += 16;
                if (p + total > segDataLen) return result;
                table.symbols.assign(seg + p, seg + p + total);
                table.valid = true;
                p += total;
                (tc == 0 ? dcTables[th] : acTables[th]) = table;
            }
        } else if (marker == 0xC0 || marker == 0xC1) { // SOF0/SOF1 (baseline / extended sequential, both non-progressive)
            if (segDataLen < 6) return result;
            int precision = seg[0];
            height = ((int)seg[1] << 8) | seg[2];
            width = ((int)seg[3] << 8) | seg[4];
            numComponents = seg[5];
            if (precision != 8 || width <= 0 || height <= 0) return result;
            if (numComponents < 1 || numComponents > 3) return result; // 4-component CMYK not supported
            if (segDataLen < 6 + numComponents * 3) return result;
            for (int i = 0; i < numComponents; i++) {
                const uint8_t* c = seg + 6 + i * 3;
                comps[i].id = c[0];
                comps[i].h = c[1] >> 4;
                comps[i].v = c[1] & 0x0F;
                comps[i].quantTable = c[2];
                if (comps[i].h < 1 || comps[i].h > 4 || comps[i].v < 1 || comps[i].v > 4 || comps[i].quantTable > 3)
                    return result;
            }
            haveSof = true;
        } else if (marker == 0xC2) {
            return result; // SOF2 progressive — not supported
        } else if (marker == 0xDD) { // DRI
            if (segDataLen < 2) return result;
            restartInterval = ((int)seg[0] << 8) | seg[1];
        } else if (marker == 0xDA) { // SOS
            if (!haveSof) return result;
            if (segDataLen < 1) return result;
            int scanComponents = seg[0];
            if (scanComponents != numComponents || segDataLen < 1 + scanComponents * 2 + 3) return result;
            for (int i = 0; i < scanComponents; i++) {
                int selector = seg[1 + i * 2];
                int tables = seg[1 + i * 2 + 1];
                Component* comp = nullptr;
                for (int k = 0; k < numComponents; k++) if (comps[k].id == selector) comp = &comps[k];
                if (!comp) return result;
                comp->dcTable = tables >> 4;
                comp->acTable = tables & 0x0F;
                if (comp->dcTable > 3 || comp->acTable > 3) return result;
            }
            pos += segLen;
            sawSos = true;
            break;
        }
        // Other markers (APPn, COM, DNL, ...) are skipped by just advancing.

        pos += segLen;
    }

    if (!sawSos || !haveSof) return result;
    for (int i = 0; i < numComponents; i++) {
        if (!haveQuantTable[comps[i].quantTable]) return result;
        if (!dcTables[comps[i].dcTable].valid || !acTables[comps[i].acTable].valid) return result;
    }

    int hMax = 1, vMax = 1;
    for (int i = 0; i < numComponents; i++) { hMax = std::max(hMax, comps[i].h); vMax = std::max(vMax, comps[i].v); }

    const int mcuWidth = 8 * hMax;
    const int mcuHeight = 8 * vMax;
    const int mcusAcross = (width + mcuWidth - 1) / mcuWidth;
    const int mcusDown = (height + mcuHeight - 1) / mcuHeight;
    if ((uint64_t)mcusAcross * mcusDown > 20'000'000ull) return result; // sanity cap

    for (int i = 0; i < numComponents; i++) {
        comps[i].blocksPerLine = mcusAcross * comps[i].h;
        comps[i].blocksPerColumn = mcusDown * comps[i].v;
        comps[i].pixelsStride = comps[i].blocksPerLine * 8;
        comps[i].pixels.assign((size_t)comps[i].pixelsStride * (comps[i].blocksPerColumn * 8), 0);
    }

    JpegBitReader br(data + pos, size - pos);
    int mcuCount = 0;

    for (int mcuY = 0; mcuY < mcusDown; mcuY++) {
        for (int mcuX = 0; mcuX < mcusAcross; mcuX++) {
            if (restartInterval > 0 && mcuCount > 0 && mcuCount % restartInterval == 0) {
                br.AlignToByte();
                // A restart marker (FF D0-D7) must appear here in a
                // well-formed stream; consume it via the raw byte position
                // the bit reader is now sitting at.
                size_t p = pos + br.pos();
                if (p + 1 >= size || data[p] != 0xFF || data[p + 1] < 0xD0 || data[p + 1] > 0xD7)
                    return result;
                JpegBitReader fresh(data + p + 2, size - p - 2);
                br = fresh;
                for (int i = 0; i < numComponents; i++) comps[i].dcPred = 0;
            }

            for (int ci = 0; ci < numComponents; ci++) {
                Component& comp = comps[ci];
                for (int by = 0; by < comp.v; by++) {
                    for (int bx = 0; bx < comp.h; bx++) {
                        double coeffs[64] = { 0 };

                        int dcSym = DecodeHuffSymbol(br, dcTables[comp.dcTable]);
                        if (dcSym < 0 || dcSym > 16 || br.error()) return result;
                        int diff = dcSym ? Extend((int)br.ReadBits(dcSym), dcSym) : 0;
                        comp.dcPred += diff;
                        coeffs[0] = (double)comp.dcPred * quantTables[comp.quantTable][0];

                        int k = 1;
                        while (k < 64) {
                            int rs = DecodeHuffSymbol(br, acTables[comp.acTable]);
                            if (rs < 0 || br.error()) return result;
                            int run = rs >> 4, sizeBits = rs & 0x0F;
                            if (sizeBits == 0) {
                                if (run == 15) { k += 16; continue; } // ZRL
                                break; // EOB
                            }
                            k += run;
                            if (k >= 64) return result;
                            int val = Extend((int)br.ReadBits(sizeBits), sizeBits);
                            coeffs[kZigzag[k]] = (double)val * quantTables[comp.quantTable][kZigzag[k]];
                            k++;
                        }

                        double spatial[64];
                        Idct2D(coeffs, spatial);
                        int blockCol = mcuX * comp.h + bx;
                        int blockRow = mcuY * comp.v + by;
                        uint8_t* dst = comp.pixels.data() + (size_t)(blockRow * 8) * comp.pixelsStride + (size_t)(blockCol * 8);
                        for (int y = 0; y < 8; y++)
                            for (int x = 0; x < 8; x++)
                                dst[(size_t)y * comp.pixelsStride + x] = ClampByte(spatial[y * 8 + x] + 128.0);
                    }
                }
            }
            mcuCount++;
        }
    }

    // Upsample each component to full resolution and combine into RGBA.
    result.rgba.assign((size_t)width * height * 4, 255);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t samples[3];
            for (int ci = 0; ci < numComponents; ci++) {
                Component& comp = comps[ci];
                int sx = x * comp.h / hMax;
                if (sx >= comp.pixelsStride) sx = comp.pixelsStride - 1;
                int sy = y * comp.v / vMax;
                samples[ci] = comp.pixels[(size_t)sy * comp.pixelsStride + sx];
            }
            uint8_t* out = &result.rgba[((size_t)y * width + x) * 4];
            if (numComponents == 1) {
                out[0] = out[1] = out[2] = samples[0];
            } else {
                double Y = samples[0], cb = samples[1] - 128.0, cr = samples[2] - 128.0;
                out[0] = ClampByte(Y + 1.402 * cr);
                out[1] = ClampByte(Y - 0.344136 * cb - 0.714136 * cr);
                out[2] = ClampByte(Y + 1.772 * cb);
            }
            out[3] = 255;
        }
    }

    result.width = width;
    result.height = height;
    result.success = true;
    return result;
}
