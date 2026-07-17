#include "codec/inflate.h"
#include "codec/crc32.h"
#include <vector>

namespace {

constexpr int kMaxBits = 15;

// Reads DEFLATE's bitstream. Two distinct bit orders are in play here, both
// handled by the same underlying ReadBit() (which just walks the byte
// stream least-significant-bit-first — the one true order of the raw
// stream): fixed-width fields (BTYPE, HLIT, extra bits, ...) treat the first
// bit read as the LOW-order bit of the value (ReadBits below), but Huffman
// codes treat the first bit read as the HIGH-order bit of the code (see
// DecodeSymbol) — this asymmetry is the single most common DEFLATE decoder
// bug, per RFC 1951 §3.1.1.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    int ReadBit() {
        if (bitPos_ == 0) {
            if (bytePos_ >= size_) { error_ = true; return 0; }
            curByte_ = data_[bytePos_++];
        }
        int bit = (curByte_ >> bitPos_) & 1;
        bitPos_ = (bitPos_ + 1) & 7;
        return bit;
    }

    uint32_t ReadBits(int n) {
        uint32_t result = 0;
        for (int i = 0; i < n; i++) result |= (uint32_t)ReadBit() << i;
        return result;
    }

    // Stored blocks are byte-aligned; discard any partial byte left in the
    // bit buffer before reading their length/data as whole bytes.
    void AlignToByte() { bitPos_ = 0; }

    uint8_t ReadByte() {
        if (bytePos_ >= size_) { error_ = true; return 0; }
        return data_[bytePos_++];
    }

    bool error() const { return error_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t bytePos_ = 0;
    int bitPos_ = 0;
    uint8_t curByte_ = 0;
    bool error_ = false;
};

// Canonical Huffman table built from a list of per-symbol code lengths
// (RFC 1951 §3.2.2), stored as (count of codes per length, symbols grouped
// by length then original index) rather than explicit bit-string codes —
// this is what lets DecodeSymbol below decode one bit at a time without
// needing to know any code's exact bit pattern up front.
struct HuffmanTable {
    std::vector<int> counts;   // counts[len] = number of codes of that length
    std::vector<uint16_t> symbols; // symbols, grouped by length ascending

    void Build(const uint8_t* lengths, int numSymbols) {
        counts.assign(kMaxBits + 1, 0);
        for (int i = 0; i < numSymbols; i++) {
            if (lengths[i] > 0) counts[lengths[i]]++;
        }

        std::vector<int> offsets(kMaxBits + 1, 0);
        for (int len = 1; len < kMaxBits; len++)
            offsets[len + 1] = offsets[len] + counts[len];
        int total = offsets[kMaxBits] + counts[kMaxBits];

        symbols.assign(total, 0);
        std::vector<int> nextOffset = offsets;
        for (int i = 0; i < numSymbols; i++) {
            if (lengths[i] > 0)
                symbols[nextOffset[lengths[i]]++] = (uint16_t)i;
        }
    }
};

// Decodes one Huffman-coded symbol, one bit at a time. Each new bit becomes
// the code's new low bit, and the running `code`/`first`/`index` triple is
// shifted up before the next bit is added — so the first bit read ends up
// as the code's highest-order bit once enough bits have accumulated to
// match a known code of that length. This is the standard canonical-Huffman
// decode technique (see RFC 1951 §3.2.2's own decode procedure).
int DecodeSymbol(BitReader& br, const HuffmanTable& table) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; len++) {
        code |= br.ReadBit();
        int count = table.counts[len];
        if (code - first < count) return table.symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

void BuildFixedTables(HuffmanTable& litTable, HuffmanTable& distTable) {
    uint8_t litLengths[288];
    for (int i = 0; i < 144; i++) litLengths[i] = 8;
    for (int i = 144; i < 256; i++) litLengths[i] = 9;
    for (int i = 256; i < 280; i++) litLengths[i] = 7;
    for (int i = 280; i < 288; i++) litLengths[i] = 8;
    litTable.Build(litLengths, 288);

    uint8_t distLengths[30];
    for (int i = 0; i < 30; i++) distLengths[i] = 5;
    distTable.Build(distLengths, 30);
}

const uint16_t kLengthBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
const uint8_t kLengthExtraBits[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
const uint16_t kDistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
const uint8_t kDistExtraBits[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
const uint8_t kCodeLengthOrder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

bool ReadDynamicTables(BitReader& br, HuffmanTable& litTable, HuffmanTable& distTable) {
    int hlit = (int)br.ReadBits(5) + 257;
    int hdist = (int)br.ReadBits(5) + 1;
    int hclen = (int)br.ReadBits(4) + 4;

    uint8_t clLengths[19] = { 0 };
    for (int i = 0; i < hclen; i++)
        clLengths[kCodeLengthOrder[i]] = (uint8_t)br.ReadBits(3);

    HuffmanTable clTable;
    clTable.Build(clLengths, 19);

    std::vector<uint8_t> lengths((size_t)(hlit + hdist), 0);
    int i = 0;
    while (i < hlit + hdist) {
        int sym = DecodeSymbol(br, clTable);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (i == 0) return false; // nothing previous to repeat
            int repeat = 3 + (int)br.ReadBits(2);
            uint8_t prev = lengths[i - 1];
            for (int r = 0; r < repeat && i < (int)lengths.size(); r++) lengths[i++] = prev;
        } else if (sym == 17) {
            int repeat = 3 + (int)br.ReadBits(3);
            for (int r = 0; r < repeat && i < (int)lengths.size(); r++) lengths[i++] = 0;
        } else if (sym == 18) {
            int repeat = 11 + (int)br.ReadBits(7);
            for (int r = 0; r < repeat && i < (int)lengths.size(); r++) lengths[i++] = 0;
        } else {
            return false;
        }
        if (br.error()) return false;
    }

    litTable.Build(lengths.data(), hlit);
    distTable.Build(lengths.data() + hlit, hdist);
    return true;
}

bool InflateBlock(BitReader& br, const HuffmanTable& litTable, const HuffmanTable& distTable, std::string& out, size_t maxOutputBytes) {
    for (;;) {
        int sym = DecodeSymbol(br, litTable);
        if (sym < 0 || br.error()) return false;
        if (sym < 256) {
            if (out.size() >= maxOutputBytes) return false;
            out.push_back((char)sym);
        } else if (sym == 256) {
            return true;
        } else {
            int lenIdx = sym - 257;
            if (lenIdx >= 29) return false;
            int length = kLengthBase[lenIdx] + (int)br.ReadBits(kLengthExtraBits[lenIdx]);
            int distSym = DecodeSymbol(br, distTable);
            if (distSym < 0 || distSym >= 30) return false;
            int distance = kDistBase[distSym] + (int)br.ReadBits(kDistExtraBits[distSym]);
            if ((size_t)distance > out.size()) return false; // back-reference before start of output
            size_t start = out.size() - (size_t)distance;
            // Byte-by-byte (not a bulk copy): length can exceed distance —
            // e.g. distance=1 is a run-length repeat of the last byte — and
            // each iteration must see bytes appended by earlier iterations
            // of this same copy for that self-overlap to work correctly.
            if (out.size() + (size_t)length > maxOutputBytes) return false;
            for (int k = 0; k < length; k++) out.push_back(out[start + (size_t)k]);
        }
        if (br.error()) return false;
    }
}

} // namespace

bool Inflate(const uint8_t* data, size_t size, std::string& out) {
    constexpr size_t kMaxDecompressedSize = 256 * 1024 * 1024; // 256 MB limit
    return Inflate(data, size, out, kMaxDecompressedSize);
}

bool Inflate(const uint8_t* data, size_t size, std::string& out, size_t maxOutputBytes) {
    BitReader br(data, size);
    bool final = false;
    while (!final) {
        final = br.ReadBits(1) != 0;
        uint32_t btype = br.ReadBits(2);
        if (br.error()) return false;

        if (btype == 0) {
            br.AlignToByte();
            uint16_t len = (uint16_t)br.ReadByte() | ((uint16_t)br.ReadByte() << 8);
            uint16_t nlen = (uint16_t)br.ReadByte() | ((uint16_t)br.ReadByte() << 8);
            if (br.error() || (uint16_t)(~len) != nlen) return false;
            size_t storedEnd = out.size() + (size_t)len;
            if (storedEnd > maxOutputBytes) return false;
            while (out.size() < storedEnd) {
                out.push_back((char)br.ReadByte());
                if (br.error()) return false;
            }
        } else if (btype == 1 || btype == 2) {
            HuffmanTable litTable, distTable;
            if (btype == 1) {
                BuildFixedTables(litTable, distTable);
            } else {
                if (!ReadDynamicTables(br, litTable, distTable)) return false;
            }
            if (!InflateBlock(br, litTable, distTable, out, maxOutputBytes)) return false;
        } else {
            return false; // btype == 3 is reserved/invalid
        }
    }
    return true;
}

uint32_t Adler32(const uint8_t* data, size_t size) {
    constexpr uint32_t kModAdler = 65521;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < size; i++) {
        a = (a + data[i]) % kModAdler;
        b = (b + a) % kModAdler;
    }
    return (b << 16) | a;
}

bool ZlibInflate(const uint8_t* data, size_t size, std::string& out) {
    constexpr size_t kMaxDecompressedSize = 256 * 1024 * 1024;
    return ZlibInflate(data, size, out, kMaxDecompressedSize);
}

bool ZlibInflate(const uint8_t* data, size_t size, std::string& out, size_t maxOutputBytes) {
    if (size < 6) return false; // 2-byte header + at least an empty deflate block + 4-byte trailer
    uint8_t cmf = data[0], flg = data[1];
    if ((cmf & 0x0F) != 8) return false;       // compression method must be DEFLATE
    if (((cmf << 8) | flg) % 31 != 0) return false; // header check bits (RFC 1950 §2.2)
    if (flg & 0x20) return false;              // FDICT set — preset dictionary not supported

    if (!Inflate(data + 2, size - 6, out, maxOutputBytes)) return false;

    uint32_t expected = ((uint32_t)data[size - 4] << 24) | ((uint32_t)data[size - 3] << 16) |
                         ((uint32_t)data[size - 2] << 8) | (uint32_t)data[size - 1];
    return Adler32((const uint8_t*)out.data(), out.size()) == expected;
}

bool GzipInflate(const uint8_t* data, size_t size, std::string& out) {
    constexpr size_t kMaxDecompressedSize = 256 * 1024 * 1024;
    return GzipInflate(data, size, out, kMaxDecompressedSize);
}

bool GzipInflate(const uint8_t* data, size_t size, std::string& out, size_t maxOutputBytes) {
    if (size < 18) return false; // 10-byte header + empty deflate block + 8-byte trailer, minimum
    if (data[0] != 0x1F || data[1] != 0x8B) return false; // magic
    if (data[2] != 8) return false; // compression method must be DEFLATE
    uint8_t flg = data[3];
    size_t pos = 10;

    if (flg & 0x04) { // FEXTRA
        if (pos + 2 > size) return false;
        uint16_t xlen = (uint16_t)data[pos] | ((uint16_t)data[pos + 1] << 8);
        pos += 2;
        if (pos + xlen > size) return false;
        pos += xlen;
    }
    if (flg & 0x08) { // FNAME (NUL-terminated)
        while (pos < size && data[pos] != 0) pos++;
        if (pos >= size) return false;
        pos++;
    }
    if (flg & 0x10) { // FCOMMENT (NUL-terminated)
        while (pos < size && data[pos] != 0) pos++;
        if (pos >= size) return false;
        pos++;
    }
    if (flg & 0x02) pos += 2; // FHCRC (header CRC16, not verified)
    if (pos + 8 > size) return false; // must leave room for the 8-byte trailer

    if (!Inflate(data + pos, size - pos - 8, out, maxOutputBytes)) return false;

    uint32_t expectedCrc = (uint32_t)data[size - 8] | ((uint32_t)data[size - 7] << 8) |
                            ((uint32_t)data[size - 6] << 16) | ((uint32_t)data[size - 5] << 24);
    uint32_t expectedSize = (uint32_t)data[size - 4] | ((uint32_t)data[size - 3] << 8) |
                             ((uint32_t)data[size - 2] << 16) | ((uint32_t)data[size - 1] << 24);
    if (Crc32((const uint8_t*)out.data(), out.size()) != expectedCrc) return false;
    return (uint32_t)(out.size() & 0xFFFFFFFFu) == expectedSize;
}
