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
    JpegBitReader() = default;
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
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
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
    int dcPred = 0;           // per-scan DC predictor, reset at scan start and at restarts
    int blocksPerLine = 0, blocksPerColumn = 0; // component's own MCU-aligned block grid size
    std::vector<int32_t> coeffs; // one full (unzigzagged, undequantized) 64-coefficient block per grid cell
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

// Decodes one 8x8 block's coefficients for a single scan pass into `coeff`
// (natural order, not zigzag; undequantized). Dispatches on (Ss, Se, Ah) per
// ITU-T T.81 §G.1:
//   - Ss==0 && Se==63: a non-progressive (baseline) full-spectrum scan — DC
//     then AC, one Huffman-decoded pass, no successive approximation.
//   - Ss==0 (Se==0 implied): a progressive DC scan — first pass decodes the
//     diff normally then left-shifts by Al; refinement passes just OR in one
//     more bit.
//   - Ss>0: a progressive AC scan (always single-component/non-interleaved)
//     — first pass uses an EOB-run scheme to skip all-zero trailing blocks
//     cheaply; refinement passes both correct existing nonzero coefficients
//     and place newly-significant ones, per §G.1.2.3.
// `eobrun` is scan-scoped state (persists across blocks, reset at restarts).
bool DecodeBlock(JpegBitReader& br, Component& comp, int Ss, int Se, int Ah, int Al,
                  HuffTable dcTables[4], HuffTable acTables[4], int& eobrun, int32_t* coeff) {
    if (Ss == 0 && Se == 63) {
        int dcSym = DecodeHuffSymbol(br, dcTables[comp.dcTable]);
        if (dcSym < 0 || dcSym > 16 || br.error()) return false;
        int diff = dcSym ? Extend((int)br.ReadBits(dcSym), dcSym) : 0;
        comp.dcPred += diff;
        coeff[0] = comp.dcPred;
        int k = 1;
        while (k < 64) {
            int rs = DecodeHuffSymbol(br, acTables[comp.acTable]);
            if (rs < 0 || br.error()) return false;
            int run = rs >> 4, sizeBits = rs & 0x0F;
            if (sizeBits == 0) {
                if (run == 15) { k += 16; continue; } // ZRL
                break; // EOB
            }
            k += run;
            if (k >= 64) return false;
            coeff[kZigzag[k]] = Extend((int)br.ReadBits(sizeBits), sizeBits);
            k++;
        }
        return true;
    }

    if (Ss == 0) { // progressive DC
        if (Ah == 0) {
            int dcSym = DecodeHuffSymbol(br, dcTables[comp.dcTable]);
            if (dcSym < 0 || dcSym > 16 || br.error()) return false;
            int diff = dcSym ? Extend((int)br.ReadBits(dcSym), dcSym) : 0;
            comp.dcPred += diff;
            coeff[0] = comp.dcPred << Al;
        } else {
            int bit = br.ReadBit();
            if (bit) coeff[0] |= (1 << Al);
        }
        return !br.error();
    }

    // progressive AC
    int p1 = 1 << Al;
    int m1 = -1 << Al;
    if (Ah == 0) {
        if (eobrun > 0) { eobrun--; return true; } // block fully covered by a prior EOB run
        int k = Ss;
        while (k <= Se) {
            int rs = DecodeHuffSymbol(br, acTables[comp.acTable]);
            if (rs < 0 || br.error()) return false;
            int run = rs >> 4, sizeBits = rs & 0x0F;
            if (sizeBits == 0) {
                if (run < 15) {
                    eobrun = (1 << run) - 1;
                    if (run) eobrun += (int)br.ReadBits(run);
                    break;
                }
                k += 16; // ZRL
                continue;
            }
            k += run;
            if (k > Se) return false;
            int raw = Extend((int)br.ReadBits(sizeBits), sizeBits);
            coeff[kZigzag[k]] = raw << Al;
            k++;
        }
        return !br.error();
    }

    // progressive AC refinement
    int k = Ss;
    if (eobrun == 0) {
        while (k <= Se) {
            int rs = DecodeHuffSymbol(br, acTables[comp.acTable]);
            if (rs < 0 || br.error()) return false;
            int run = rs >> 4, sizeBits = rs & 0x0F;
            int newVal = 0;
            if (sizeBits == 0) {
                if (run < 15) {
                    // Refinement EOB includes THIS block's correction pass (done
                    // by the eobrun>0 tail below before eobrun is decremented),
                    // unlike the first-pass branch where this block is already
                    // fully consumed — so the count is 1<<run, not 1<<run - 1.
                    eobrun = 1 << run;
                    if (run) eobrun += (int)br.ReadBits(run);
                    break;
                }
                // run == 15 (ZRL): skip 16 zero-history coefficients, refining any
                // already-nonzero ones encountered along the way (handled below).
            } else {
                // sizeBits is always 1 in a refinement scan (the new coefficient's sign).
                newVal = br.ReadBit() ? p1 : m1;
            }
            while (k <= Se) {
                int zi = kZigzag[k];
                if (coeff[zi] != 0) {
                    int refBit = br.ReadBit();
                    if (refBit && (coeff[zi] & p1) == 0)
                        coeff[zi] += (coeff[zi] >= 0) ? p1 : m1;
                } else {
                    if (run == 0) {
                        if (sizeBits) coeff[zi] = newVal;
                        k++;
                        break;
                    }
                    run--;
                }
                k++;
            }
            if (br.error()) return false;
        }
    }
    if (eobrun > 0) {
        while (k <= Se) {
            int zi = kZigzag[k];
            if (coeff[zi] != 0) {
                int refBit = br.ReadBit();
                if (refBit && (coeff[zi] & p1) == 0)
                    coeff[zi] += (coeff[zi] >= 0) ? p1 : m1;
            }
            k++;
        }
        eobrun--;
    }
    return !br.error();
}

// Decodes one entropy-coded scan (one SOS's worth of compressed data) into
// each named component's coefficient storage, handling restart markers.
// `scanComps` covers the whole MCU (interleaved, >1 component) or a single
// component's own real-coverage block grid (non-interleaved) per §A.2.2/A.2.3
// — the caller picks blocksAcross/blocksDown accordingly.
bool DecodeScan(const uint8_t* data, size_t size, size_t scanStart,
                 Component** scanComps, int numScanComps,
                 int Ss, int Se, int Ah, int Al, int restartInterval,
                 int blocksAcross, int blocksDown,
                 HuffTable dcTables[4], HuffTable acTables[4], size_t& outPos) {
    for (int i = 0; i < numScanComps; i++) scanComps[i]->dcPred = 0;
    int eobrun = 0;
    size_t base = scanStart;
    JpegBitReader br(data + base, size - base);
    int unitCount = 0;

    for (int uy = 0; uy < blocksDown; uy++) {
        for (int ux = 0; ux < blocksAcross; ux++) {
            if (restartInterval > 0 && unitCount > 0 && unitCount % restartInterval == 0) {
                br.AlignToByte();
                size_t p = base + br.pos();
                if (p + 1 >= size || data[p] != 0xFF || data[p + 1] < 0xD0 || data[p + 1] > 0xD7) return false;
                base = p + 2;
                br = JpegBitReader(data + base, size - base);
                for (int i = 0; i < numScanComps; i++) scanComps[i]->dcPred = 0;
                eobrun = 0;
            }

            if (numScanComps > 1) {
                for (int ci = 0; ci < numScanComps; ci++) {
                    Component& comp = *scanComps[ci];
                    for (int by = 0; by < comp.v; by++) {
                        for (int bx = 0; bx < comp.h; bx++) {
                            int blockCol = ux * comp.h + bx;
                            int blockRow = uy * comp.v + by;
                            int32_t* coeff = comp.coeffs.data() + (size_t)(blockRow * comp.blocksPerLine + blockCol) * 64;
                            if (!DecodeBlock(br, comp, Ss, Se, Ah, Al, dcTables, acTables, eobrun, coeff)) return false;
                        }
                    }
                }
            } else {
                Component& comp = *scanComps[0];
                int32_t* coeff = comp.coeffs.data() + (size_t)(uy * comp.blocksPerLine + ux) * 64;
                if (!DecodeBlock(br, comp, Ss, Se, Ah, Al, dcTables, acTables, eobrun, coeff)) return false;
            }
            unitCount++;
        }
    }
    br.AlignToByte();
    outPos = base + br.pos();
    return true;
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
    bool progressive = false;
    int restartInterval = 0;
    int hMax = 1, vMax = 1, mcusAcross = 0, mcusDown = 0;
    int scanCount = 0;

    size_t pos = 2;

    while (pos + 2 <= size) {
        if (data[pos] != 0xFF) return result; // expected a marker
        uint8_t marker = data[pos + 1];
        pos += 2;
        if (marker == 0xD9) break; // EOI
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue; // no length field
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
        } else if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            // SOF0/SOF1 (baseline / extended sequential) or SOF2 (progressive) —
            // all three share the same header layout.
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
            progressive = (marker == 0xC2);
            haveSof = true;

            hMax = 1; vMax = 1;
            for (int i = 0; i < numComponents; i++) { hMax = std::max(hMax, comps[i].h); vMax = std::max(vMax, comps[i].v); }
            const int mcuWidth = 8 * hMax;
            const int mcuHeight = 8 * vMax;
            mcusAcross = (width + mcuWidth - 1) / mcuWidth;
            mcusDown = (height + mcuHeight - 1) / mcuHeight;
            if ((uint64_t)mcusAcross * mcusDown > 20'000'000ull) return result; // sanity cap
            for (int i = 0; i < numComponents; i++) {
                comps[i].blocksPerLine = mcusAcross * comps[i].h;
                comps[i].blocksPerColumn = mcusDown * comps[i].v;
                comps[i].pixelsStride = comps[i].blocksPerLine * 8;
                comps[i].pixels.assign((size_t)comps[i].pixelsStride * (comps[i].blocksPerColumn * 8), 0);
                comps[i].coeffs.assign((size_t)comps[i].blocksPerLine * comps[i].blocksPerColumn * 64, 0);
            }
        } else if (marker == 0xDD) { // DRI
            if (segDataLen < 2) return result;
            restartInterval = ((int)seg[0] << 8) | seg[1];
        } else if (marker == 0xDA) { // SOS
            if (!haveSof) return result;
            if (++scanCount > 200) return result; // defensive cap on pathological scan counts
            if (segDataLen < 1) return result;
            int scanComponents = seg[0];
            if (scanComponents < 1 || scanComponents > numComponents || segDataLen < 1 + scanComponents * 2 + 3)
                return result;
            Component* scanComps[kMaxComponents];
            for (int i = 0; i < scanComponents; i++) {
                int selector = seg[1 + i * 2];
                int tables = seg[1 + i * 2 + 1];
                Component* comp = nullptr;
                for (int k = 0; k < numComponents; k++) if (comps[k].id == selector) comp = &comps[k];
                if (!comp) return result;
                comp->dcTable = tables >> 4;
                comp->acTable = tables & 0x0F;
                if (comp->dcTable > 3 || comp->acTable > 3) return result;
                scanComps[i] = comp;
            }
            int Ss = seg[1 + scanComponents * 2];
            int Se = seg[1 + scanComponents * 2 + 1];
            int AhAl = seg[1 + scanComponents * 2 + 2];
            int Ah = AhAl >> 4, Al = AhAl & 0x0F;
            if (Ss < 0 || Se > 63 || Ss > Se) return result;

            if (!progressive) {
                if (Ss != 0 || Se != 63 || Ah != 0 || Al != 0 || scanComponents != numComponents) return result;
            } else {
                if (Ss == 0) { if (Se != 0) return result; } // DC scans cover only coefficient 0
                else if (scanComponents != 1) return result; // AC scans are always non-interleaved
            }
            for (int i = 0; i < scanComponents; i++) {
                Component& c = *scanComps[i];
                if (!haveQuantTable[c.quantTable]) return result;
                if (Ss == 0 && !dcTables[c.dcTable].valid) return result;
                if (Se > 0 && !acTables[c.acTable].valid) return result;
            }

            pos += segLen;
            int blocksAcross, blocksDown;
            if (scanComponents > 1) {
                blocksAcross = mcusAcross;
                blocksDown = mcusDown;
            } else {
                Component& c = *scanComps[0];
                int compWidthPx = (width * c.h + hMax - 1) / hMax;
                int compHeightPx = (height * c.v + vMax - 1) / vMax;
                blocksAcross = (compWidthPx + 7) / 8;
                blocksDown = (compHeightPx + 7) / 8;
            }
            size_t newPos;
            if (!DecodeScan(data, size, pos, scanComps, scanComponents, Ss, Se, Ah, Al,
                             restartInterval, blocksAcross, blocksDown, dcTables, acTables, newPos))
                return result;
            pos = newPos;
            continue;
        }
        // Other markers (APPn, COM, DNL, ...) are skipped by just advancing.

        pos += segLen;
    }

    if (!haveSof) return result;

    // Dequantize + IDCT every block now that all scans have refined it.
    for (int ci = 0; ci < numComponents; ci++) {
        Component& comp = comps[ci];
        const uint16_t* qt = quantTables[comp.quantTable];
        for (int blockRow = 0; blockRow < comp.blocksPerColumn; blockRow++) {
            for (int blockCol = 0; blockCol < comp.blocksPerLine; blockCol++) {
                const int32_t* coeff = comp.coeffs.data() + (size_t)(blockRow * comp.blocksPerLine + blockCol) * 64;
                double dequant[64];
                for (int i = 0; i < 64; i++) dequant[i] = (double)coeff[i] * qt[i];
                double spatial[64];
                Idct2D(dequant, spatial);
                uint8_t* dst = comp.pixels.data() + (size_t)(blockRow * 8) * comp.pixelsStride + (size_t)(blockCol * 8);
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                        dst[(size_t)y * comp.pixelsStride + x] = ClampByte(spatial[y * 8 + x] + 128.0);
            }
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
