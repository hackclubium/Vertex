#include "fixture.h"
#include "codec/inflate.h"
#include "codec/png.h"
#include "codec/jpeg.h"
#include "codec/webp.h"
#include "codec/sha256.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <vector>

namespace {

std::vector<uint8_t> HexToBytes(const std::string& hex) {
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        return 0;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back((uint8_t)((val(hex[i]) << 4) | val(hex[i + 1])));
    return out;
}

// JPEG is lossy, so comparisons against a reference decode use a small
// tolerance rather than byte-exact equality — `refChannels` is 3 for an RGB
// reference (djpeg's default PPM output) or 1 for a grayscale reference
// (djpeg's PGM output).
std::string JpegDiffSummary(const DecodedImage& img, const std::vector<uint8_t>& ref,
                             int refWidth, int refHeight, int refChannels) {
    if (!img.success) return "fail";
    if (img.width != refWidth || img.height != refHeight)
        return "size-mismatch:" + std::to_string(img.width) + "x" + std::to_string(img.height);
    int maxDiff = 0;
    for (int y = 0; y < refHeight; y++) {
        for (int x = 0; x < refWidth; x++) {
            uint8_t rr, rg, rb;
            if (refChannels == 3) {
                const uint8_t* p = &ref[((size_t)y * refWidth + x) * 3];
                rr = p[0]; rg = p[1]; rb = p[2];
            } else {
                rr = rg = rb = ref[(size_t)y * refWidth + x];
            }
            const uint8_t* out = &img.rgba[((size_t)y * img.width + x) * 4];
            maxDiff = std::max({ maxDiff, std::abs(out[0] - rr), std::abs(out[1] - rg), std::abs(out[2] - rb) });
        }
    }
    return "maxDiff=" + std::to_string(maxDiff);
}

std::string RgbaSummary(const DecodedImage& img) {
    if (!img.success) return "fail";
    std::string out = std::to_string(img.width) + "x" + std::to_string(img.height) + ":";
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
        if (i) out += " ";
        out += std::to_string(img.rgba[i]) + "," + std::to_string(img.rgba[i + 1]) + "," +
               std::to_string(img.rgba[i + 2]) + "," + std::to_string(img.rgba[i + 3]);
    }
    return out;
}

std::vector<uint8_t> MakeSolidVp8LWebp(int width, int height, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    struct Bits {
        std::vector<uint8_t> out;
        int bit = 0;
        void Write(uint32_t v, int n) {
            for (int i = 0; i < n; ++i) {
                if (bit == 0) out.push_back(0);
                out.back() |= (uint8_t)(((v >> i) & 1u) << bit);
                bit = (bit + 1) & 7;
            }
        }
        void SimpleSymbol(uint8_t v) {
            Write(1, 1); // simple prefix code
            Write(0, 1); // one symbol
            Write(1, 1); // first symbol is 8 bits
            Write(v, 8);
        }
    } bits;
    bits.Write(0, 1); // no transforms
    bits.Write(0, 1); // no color cache
    bits.Write(0, 1); // one meta-prefix group
    bits.SimpleSymbol(g);
    bits.SimpleSymbol(r);
    bits.SimpleSymbol(b);
    bits.SimpleSymbol(a);
    bits.SimpleSymbol(0); // unused distance code

    std::vector<uint8_t> chunk;
    chunk.push_back(0x2f);
    uint32_t header = (uint32_t)(width - 1) | ((uint32_t)(height - 1) << 14);
    for (int i = 0; i < 4; ++i) chunk.push_back((uint8_t)(header >> (i * 8)));
    chunk.insert(chunk.end(), bits.out.begin(), bits.out.end());

    std::vector<uint8_t> webp = {'R','I','F','F',0,0,0,0,'W','E','B','P','V','P','8','L',0,0,0,0};
    uint32_t chunkSize = (uint32_t)chunk.size();
    for (int i = 0; i < 4; ++i) webp[16 + i] = (uint8_t)(chunkSize >> (i * 8));
    webp.insert(webp.end(), chunk.begin(), chunk.end());
    if (chunkSize & 1u) webp.push_back(0);
    uint32_t riffSize = (uint32_t)webp.size() - 8;
    for (int i = 0; i < 4; ++i) webp[4 + i] = (uint8_t)(riffSize >> (i * 8));
    return webp;
}

void AppendChunk(std::vector<uint8_t>& webp, const char fourcc[4], const std::vector<uint8_t>& chunk) {
    webp.insert(webp.end(), fourcc, fourcc + 4);
    uint32_t chunkSize = (uint32_t)chunk.size();
    for (int i = 0; i < 4; ++i) webp.push_back((uint8_t)(chunkSize >> (i * 8)));
    webp.insert(webp.end(), chunk.begin(), chunk.end());
    if (chunkSize & 1u) webp.push_back(0);
}

void FinishRiffSize(std::vector<uint8_t>& webp) {
    uint32_t riffSize = (uint32_t)webp.size() - 8;
    for (int i = 0; i < 4; ++i) webp[4 + i] = (uint8_t)(riffSize >> (i * 8));
}

std::vector<uint8_t> MakeAlphaOnlyVp8LStream(uint8_t alpha) {
    struct Bits {
        std::vector<uint8_t> out;
        int bit = 0;
        void Write(uint32_t v, int n) {
            for (int i = 0; i < n; ++i) {
                if (bit == 0) out.push_back(0);
                out.back() |= (uint8_t)(((v >> i) & 1u) << bit);
                bit = (bit + 1) & 7;
            }
        }
        void SimpleSymbol(uint8_t v) {
            Write(1, 1);
            Write(0, 1);
            Write(1, 1);
            Write(v, 8);
        }
    } bits;
    bits.Write(0, 1);
    bits.Write(0, 1);
    bits.Write(0, 1);
    bits.SimpleSymbol(alpha); // green channel stores alpha for ALPH lossless streams
    bits.SimpleSymbol(0);
    bits.SimpleSymbol(0);
    bits.SimpleSymbol(255);
    bits.SimpleSymbol(0);
    return bits.out;
}

std::vector<uint8_t> MakeExtendedVp8LWebp(int width, int height, uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                                          const std::vector<uint8_t>& alph) {
    auto simple = MakeSolidVp8LWebp(width, height, r, g, b, a);
    std::vector<uint8_t> vp8l(simple.begin() + 20, simple.end());
    if (vp8l.size() && vp8l.back() == 0 && ((simple[16] | (simple[17] << 8) | (simple[18] << 16) | (simple[19] << 24)) & 1)) vp8l.pop_back();

    std::vector<uint8_t> webp = {'R','I','F','F',0,0,0,0,'W','E','B','P'};
    std::vector<uint8_t> vp8x(10, 0);
    vp8x[0] = alph.empty() ? 0 : 0x10;
    uint32_t wm1 = (uint32_t)(width - 1), hm1 = (uint32_t)(height - 1);
    for (int i = 0; i < 3; ++i) {
        vp8x[4 + i] = (uint8_t)(wm1 >> (i * 8));
        vp8x[7 + i] = (uint8_t)(hm1 >> (i * 8));
    }
    AppendChunk(webp, "VP8X", vp8x);
    if (!alph.empty()) AppendChunk(webp, "ALPH", alph);
    AppendChunk(webp, "VP8L", vp8l);
    FinishRiffSize(webp);
    return webp;
}

} // namespace

TestResult RunCodecTests() {
    TestResult result;

    ExpectEqual("codec/sha256/known-vector",
        Sha256Hex(std::string("abc")) + "\n",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n",
        result);

    // All hex vectors below were produced by Python's stdlib zlib (an
    // independent, industry-standard implementation), not by Vertex's own
    // encoder — this is a genuine interoperability test, the same spirit as
    // testing the WebSocket handshake against RFC 6455's own worked example.

    {
        // zlib.compressobj(Z_NO_COMPRESSION) — forces a stored (BTYPE=00) block.
        auto bytes = HexToBytes(
            "7801013200cdff73746f72656420626c6f636b207465737420646174612c206e6f20636f6d7072657373696f6e20617420616c6c2068657265d97e1264");
        std::string out;
        bool ok = ZlibInflate(bytes.data(), bytes.size(), out);
        ExpectEqual("codec/inflate/stored-block",
            (ok ? "ok:" : "fail:") + out + "\n",
            "ok:stored block test data, no compression at all here\n",
            result);
    }

    {
        // zlib.compressobj(strategy=Z_FIXED) — forces a fixed-Huffman (BTYPE=01) block.
        auto bytes = HexToBytes(
            "78014bcbac484d51c8284d4bcb4dcc53282e294a2c494daf5428492d2e512848acccc94f4cd151284a2d48058aa760320000da17fd");
        std::string out;
        bool ok = ZlibInflate(bytes.data(), bytes.size(), out);
        ExpectEqual("codec/inflate/fixed-huffman-block",
            (ok ? "ok:" : "fail:") + out + "\n",
            "ok:fixed huffman strategy test payload, repeated repeated repeated\n",
            result);
    }

    {
        // zlib.compress(..., 9) on enough entropy that zlib's own heuristics
        // choose a dynamic-Huffman (BTYPE=10) block — exercises the HLIT/
        // HDIST/HCLEN code-length-alphabet path, not just fixed tables.
        auto bytes = HexToBytes(
            "78da5d505b12802008bc8a57b3c4ac7c946959a76ff035ea0703c32ecbc264c"
            "da389f09c2baac9e9d779279b57c795eb1bac8330246616c24de8294e400430"
            "8a1ef6329c4471aa5f26e9f7465e09065c5207550f4730b718f6064b0d1225b"
            "16868697df598d9c9c2147fd05cfd03029066cd");
        std::string out;
        bool ok = ZlibInflate(bytes.data(), bytes.size(), out);
        ExpectEqual("codec/inflate/dynamic-huffman-block",
            (ok ? "ok:" : "fail:") + out + "\n",
            "ok:brown huffman quick jumps quick vertex vertex vertex dog fox "
            "quick vertex the dog dog huffman the vertex jumps fox huffman "
            "quick lazy the the the deflate the dog fox dog the deflate fox "
            "vertex vertex deflate fox lazy fox fox vertex jumps the dog "
            "deflate quick brown jumps quick\n",
            result);
    }

    {
        // gzip.compress(...) from Python's stdlib — a real, independent gzip
        // encoder, exercising the header/flags parsing + CRC-32/ISIZE
        // trailer verification GzipInflate adds on top of the shared
        // Inflate() core (already covered by the zlib-wrapper tests above).
        auto bytes = HexToBytes(
            "1f8b08000000000002ffcb48cdc9c95748afca2c50482bcacf5528a82cc9c8c"
            "f03009c07368816000000");
        std::string out;
        bool ok = GzipInflate(bytes.data(), bytes.size(), out);
        ExpectEqual("codec/inflate/gzip-wrapper",
            (ok ? "ok:" : "fail:") + out + "\n",
            "ok:hello gzip from python\n",
            result);
    }

    {
        // A long run of a single repeated byte forces an LZ77 back-reference
        // whose length exceeds its distance (distance=1) — the classic
        // self-overlapping-copy case that a naive bulk memcpy would break.
        auto bytes = HexToBytes("78da4b4ca40c0000148d1841");
        std::string out;
        bool ok = ZlibInflate(bytes.data(), bytes.size(), out);
        ExpectEqual("codec/inflate/self-overlapping-back-reference",
            (ok ? "ok:" : "fail:") + std::to_string(out.size()) + ":" + out + "\n",
            "ok:64:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n",
            result);
    }

    {
        // zlib.compress(b'') — the empty-input edge case.
        auto bytes = HexToBytes("78da030000000001");
        std::string out;
        bool ok = ZlibInflate(bytes.data(), bytes.size(), out);
        ExpectEqual("codec/inflate/empty-input",
            (ok ? "ok:" : "fail:") + std::to_string(out.size()) + "\n",
            "ok:0\n",
            result);
    }

    {
        // Raw DEFLATE (no zlib header/trailer) — zlib.compressobj(wbits=-15).
        auto bytes = HexToBytes("2b4a2c5748494dcb492c495528492d2e512848acccc94f4c0100");
        std::string out;
        bool ok = Inflate(bytes.data(), bytes.size(), out);
        ExpectEqual("codec/inflate/raw-deflate-no-wrapper",
            (ok ? "ok:" : "fail:") + out + "\n",
            "ok:raw deflate test payload\n",
            result);
    }

    {
        // Corrupt input (truncated mid-stream) must fail cleanly, not crash
        // or read out of bounds — decompressing attacker-controlled bytes
        // from the network/a file has to be safe against garbage input.
        auto bytes = HexToBytes("78dacb48cdc9c957284b2d2a49ad0000"); // short zlib stream, truncated
        std::string out;
        bool ok = ZlibInflate(bytes.data(), bytes.size(), out);
        auto truncated = HexToBytes("78da");
        std::string out2;
        bool ok2 = ZlibInflate(truncated.data(), truncated.size(), out2);
        ExpectEqual("codec/inflate/truncated-input-fails-safely",
            std::string(ok ? "unexpected-ok " : "rejected ") + (ok2 ? "unexpected-ok\n" : "rejected\n"),
            "rejected rejected\n",
            result);
    }

    {
        // Adler-32 mismatch (trailer doesn't match the decompressed bytes)
        // must be caught — verifies the checksum is actually being checked,
        // not just parsed and ignored.
        auto bytes = HexToBytes("78dacb48cdc9c957284b2d2a49ad00001ec204d4"); // last byte flipped vs. the valid "short" vector
        std::string out;
        bool ok = ZlibInflate(bytes.data(), bytes.size(), out);
        ExpectEqual("codec/inflate/adler32-mismatch-is-rejected",
            std::string(ok ? "unexpected-ok\n" : "rejected\n"),
            "rejected\n",
            result);
    }

    // The PNG vectors below were constructed with Python stdlib (struct +
    // zlib.compress + zlib.crc32 — a real, independent DEFLATE encoder and
    // CRC-32 implementation, not Vertex's own), each with hand-picked pixel
    // values so the expected decoded output is known exactly.

    {
        // 2x2 truecolor (color type 2, depth 8): red, green / blue, white.
        auto bytes = HexToBytes(
            "89504e470d0a1a0a0000000d4948445200000002000000020802000000fdd4"
            "9a73000000124944415478da63f8cfc0c000c20cff8100001fee05fbf1abba"
            "770000000049454e44ae426082");
        auto img = DecodePng(bytes.data(), bytes.size());
        ExpectEqual("codec/png/truecolor-8bit",
            RgbaSummary(img) + "\n",
            "2x2:255,0,0,255 0,255,0,255 0,0,255,255 255,255,255,255\n",
            result);
    }

    {
        // 2x2 grayscale (color type 0, depth 8): 0, 85 / 170, 255.
        auto bytes = HexToBytes(
            "89504e470d0a1a0a0000000d494844520000000200000002080000000057dd"
            "52f80000000e4944415478da6360086558f51f0003ad01ff7a93847f000000"
            "0049454e44ae426082");
        auto img = DecodePng(bytes.data(), bytes.size());
        ExpectEqual("codec/png/grayscale-8bit",
            RgbaSummary(img) + "\n",
            "2x2:0,0,0,255 85,85,85,255 170,170,170,255 255,255,255,255\n",
            result);
    }

    {
        // 2x2 indexed color (color type 3, depth 8) with a 4-entry palette
        // (red/green/blue/yellow) and a tRNS chunk giving each entry a
        // different alpha (255/128/0/255) — exercises both PLTE lookup and
        // tRNS-per-index transparency.
        auto bytes = HexToBytes(
            "89504e470d0a1a0a0000000d49484452000000020000000208030000004568"
            "fd160000000c504c5445ff000000ff000000ffffff00d6028f7b0000000474"
            "524e53ff8000ffa1a194660000000e4944415478da6360606460620600001100"
            "0783ca64640000000049454e44ae426082");
        auto img = DecodePng(bytes.data(), bytes.size());
        ExpectEqual("codec/png/indexed-with-trns",
            RgbaSummary(img) + "\n",
            "2x2:255,0,0,255 0,255,0,128 0,0,255,0 255,255,0,255\n",
            result);
    }

    {
        // 2x2 truecolor+alpha (color type 6, depth 8).
        auto bytes = HexToBytes(
            "89504e470d0a1a0a0000000d494844520000000200000002080600000072b6"
            "0d24000000154944415478da63f8cfc0f01f081b18c0f4ffff0e003f1807ba"
            "92a45f250000000049454e44ae426082");
        auto img = DecodePng(bytes.data(), bytes.size());
        ExpectEqual("codec/png/truecolor-alpha-8bit",
            RgbaSummary(img) + "\n",
            "2x2:255,0,0,255 0,255,0,128 0,0,255,0 255,255,255,64\n",
            result);
    }

    {
        // 8x1 grayscale at 1-bit depth: byte 0b10110010 packed MSB-first —
        // exercises sub-byte sample unpacking, not just whole-byte samples.
        auto bytes = HexToBytes(
            "89504e470d0a1a0a0000000d4948445200000008000000010100000000cb7bd2"
            "ee0000000a4944415478da63d8040000b400b38990cd2f0000000049454e44ae426082");
        auto img = DecodePng(bytes.data(), bytes.size());
        ExpectEqual("codec/png/grayscale-1bit-packing",
            RgbaSummary(img) + "\n",
            "8x1:255,255,255,255 0,0,0,255 255,255,255,255 255,255,255,255 "
            "0,0,0,255 0,0,0,255 255,255,255,255 0,0,0,255\n",
            result);
    }

    {
        // Not a PNG at all (no signature) and a truncated real PNG must
        // both fail cleanly rather than crash — decoding attacker-supplied
        // image bytes has to be safe against garbage/incomplete input.
        auto notPng = HexToBytes("00112233");
        auto badImg = DecodePng(notPng.data(), notPng.size());
        auto truncated = HexToBytes("89504e470d0a1a0a0000000d4948445200000002");
        auto truncImg = DecodePng(truncated.data(), truncated.size());
        ExpectEqual("codec/png/malformed-input-fails-safely",
            std::string(badImg.success ? "unexpected-ok " : "rejected ") +
                (truncImg.success ? "unexpected-ok\n" : "rejected\n"),
            "rejected rejected\n",
            result);
    }

    // The JPEG vectors below were produced by libjpeg-turbo's cjpeg (a real,
    // independent encoder) at three chroma subsampling ratios plus a
    // grayscale case, with expected pixels taken from libjpeg-turbo's own
    // djpeg decode of those exact files — a genuine interoperability check,
    // not just internal self-consistency. Since JPEG is lossy, comparisons
    // use a small tolerance rather than byte-exact equality; the 4:2:0/4:2:2
    // references were decoded with djpeg's `-nosmooth` flag specifically,
    // since Vertex's chroma upsampling is nearest-neighbor, not djpeg's
    // smoother default — that's the correct apples-to-apples comparison for
    // the upsampling method actually implemented (see codec/jpeg.h).

    {
        // 16x16, 4:4:4 (no chroma subsampling) — every component has its own
        // full-resolution block per MCU, so this exercises the baseline
        // pipeline (Huffman, dequant, IDCT, YCbCr->RGB) without exercising
        // multi-block-per-component MCUs or upsampling at all.
        auto jpg = HexToBytes(
            "ffd8ffe000104a46494600010100000100010000ffdb00430003020203020203030303040303"
            "04050805050404050a070706080c0a0c0c0b0a0b0b0d0e12100d0e110e0b0b10161011131415"
            "15150c0f171816141812141514ffdb00430103040405040509050509140d0b0d141414141414"
            "1414141414141414141414141414141414141414141414141414141414141414141414141414"
            "141414141414ffc00011080010001003011100021101031101ffc4001f000001050101010101"
            "0100000000000000000102030405060708090a0bffc400b51000020103030204030505040400"
            "00017d01020300041105122131410613516107227114328191a1082342b1c11552d1f0243362"
            "7282090a161718191a25262728292a3435363738393a434445464748494a535455565758595a"
            "636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5a6"
            "a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7"
            "e8e9eaf1f2f3f4f5f6f7f8f9faffc4001f010003010101010101010101000000000000010203"
            "0405060708090a0bffc400b51100020102040403040705040400010277000102031104052131"
            "061241510761711322328108144291a1b1c109233352f0156272d10a162434e125f11718191a"
            "262728292a35363738393a434445464748494a535455565758595a636465666768696a737475"
            "767778797a82838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7"
            "b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9"
            "faffda000c03010002110311003f00f9febf243fd0b3deebf3a3fc2c3e21aff6acfe9d3f6dab"
            "fe7a4fd58fffd9");
        auto ref = HexToBytes(
            "dc1414dc1414dc1414dc1414dc1414dc1414dc1414dc141415c81415c81415c81415c81415c8"
            "1415c81415c81415c814dc1414dc1414dc1414dc1414dc1414dc1414dc1414dc141415c81415"
            "c81415c81415c81415c81415c81415c81415c814dc1414dc1414dc1414dc1414dc1414dc1414"
            "dc1414dc141415c81415c81415c81415c81415c81415c81415c81415c814dc1414dc1414dc14"
            "14dc1414dc1414dc1414dc1414dc141415c81415c81415c81415c81415c81415c81415c81415"
            "c814dc1414dc1414dc1414dc1414dc1414dc1414dc1414dc141415c81415c81415c81415c814"
            "15c81415c81415c81415c814dc1414dc1414dc1414dc1414dc1414dc1414dc1414dc141415c8"
            "1415c81415c81415c81415c81415c81415c81415c814dc1414dc1414dc1414dc1414dc1414dc"
            "1414dc1414dc141415c81415c81415c81415c81415c81415c81415c81415c814dc1414dc1414"
            "dc1414dc1414dc1414dc1414dc1414dc141415c81415c81415c81415c81415c81415c81415c8"
            "1415c8141514dc1514dc1514dc1514dc1514dc1514dc1514dc1514dce5e61ee5e61ee5e61ee5"
            "e61ee5e61ee5e61ee5e61ee5e61e1514dc1514dc1514dc1514dc1514dc1514dc1514dc1514dc"
            "e5e61ee5e61ee5e61ee5e61ee5e61ee5e61ee5e61ee5e61e1514dc1514dc1514dc1514dc1514"
            "dc1514dc1514dc1514dce5e61ee5e61ee5e61ee5e61ee5e61ee5e61ee5e61ee5e61e1514dc15"
            "14dc1514dc1514dc1514dc1514dc1514dc1514dce5e61ee5e61ee5e61ee5e61ee5e61ee5e61e"
            "e5e61ee5e61e1514dc1514dc1514dc1514dc1514dc1514dc1514dc1514dce5e61ee5e61ee5e6"
            "1ee5e61ee5e61ee5e61ee5e61ee5e61e1514dc1514dc1514dc1514dc1514dc1514dc1514dc15"
            "14dce5e61ee5e61ee5e61ee5e61ee5e61ee5e61ee5e61ee5e61e1514dc1514dc1514dc1514dc"
            "1514dc1514dc1514dc1514dce5e61ee5e61ee5e61ee5e61ee5e61ee5e61ee5e61ee5e61e1514"
            "dc1514dc1514dc1514dc1514dc1514dc1514dc1514dce5e61ee5e61ee5e61ee5e61ee5e61ee5"
            "e61ee5e61ee5e61e");
        auto img = DecodeJpeg(jpg.data(), jpg.size());
        ExpectEqual("codec/jpeg/no-subsampling-444",
            JpegDiffSummary(img, ref, 16, 16, 3) + "\n",
            "maxDiff=0\n",
            result);
    }

    {
        // 16x16, 4:2:0 — luma has 4 blocks per MCU (2x2), chroma 1 each;
        // exercises multi-block-per-component decode + 2x2 upsampling.
        auto jpg = HexToBytes(
            "ffd8ffe000104a46494600010100000100010000ffdb00430003020203020203030303040303"
            "04050805050404050a070706080c0a0c0c0b0a0b0b0d0e12100d0e110e0b0b10161011131415"
            "15150c0f171816141812141514ffdb00430103040405040509050509140d0b0d141414141414"
            "1414141414141414141414141414141414141414141414141414141414141414141414141414"
            "141414141414ffc00011080010001003012200021101031101ffc4001f000001050101010101"
            "0100000000000000000102030405060708090a0bffc400b51000020103030204030505040400"
            "00017d01020300041105122131410613516107227114328191a1082342b1c11552d1f0243362"
            "7282090a161718191a25262728292a3435363738393a434445464748494a535455565758595a"
            "636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5a6"
            "a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7"
            "e8e9eaf1f2f3f4f5f6f7f8f9faffc4001f010003010101010101010101000000000000010203"
            "0405060708090a0bffc400b51100020102040403040705040400010277000102031104052131"
            "061241510761711322328108144291a1b1c109233352f0156272d10a162434e125f11718191a"
            "262728292a35363738393a434445464748494a535455565758595a636465666768696a737475"
            "767778797a82838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7"
            "b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9"
            "faffda000c03010002110311003f00f9febdeebe21afdb6af80f19b853fe21c7f67feffeb1f5"
            "8f6bf67939793d9ff7a77bf3f95add6fa7a3e20e2ffe22afd57ddfab7d5b9fafb4e6f69c9fe0"
            "b5b93cef7e96d7ffd9");
        auto ref = HexToBytes(
            "e21019e21019dc1414dc1414dc131bdc131bdf1316df131613c91413c91418c80e18c80e16c7"
            "1516c71512ca1012ca10e21019e21019dc1414dc1414dc131bdc131bdf1316df131613c91413"
            "c91418c80e18c80e16c71516c71512ca1012ca10d71714d71714d81809d81809d9170ed9170e"
            "d81712d8171212ca1212ca1211ca1511ca1512c81b12c81b13ca1013ca10d71714d71714d818"
            "09d81809d9170ed9170ed81712d8171212ca1212ca1211ca1511ca1512c81b12c81b13ca1013"
            "ca10df1219df1219e3120ee3120ee01212e01212d91519d915191dc50e1dc50e16c71516c715"
            "13c81b13c81b18c80e18c80edf1219df1219e3120ee3120ee01212e01212d91519d915191dc5"
            "0e1dc50e16c71516c71513c81b13c81b18c80e18c80edc1512dc1512db160edb160ed21916d2"
            "1916de1609de160913c81913c8191fc50b1fc50b16c71416c71415c91015c910dc1512dc1512"
            "db160edb160ed21916d21916de1609de160913c81913c8191fc50b1fc50b16c71416c71415c9"
            "1015c9101514de1514de1912dc1912dc1a11da1a11da1710e91710e9e4e817e4e817e0e725e0"
            "e725e1e721e1e721e5e620e5e6201514de1514de1912dc1912dc1a11da1a11da1710e91710e9"
            "e4e817e4e817e0e725e0e725e1e721e1e721e5e620e5e6201216d71216d71215de1215de0e17"
            "de0e17de1c11d71c11d7daeb21daeb21e8e51ae8e51ae4e71ae4e71ae4e623e4e6231216d712"
            "16d71215de1215de0e17de0e17de1c11d71c11d7daeb21daeb21e8e51ae8e51ae4e71ae4e71a"
            "e4e623e4e6231a11dc1a11dc1d0ee31d0ee31513e21513e21d0fde1d0fdee5e61ee5e61eeee2"
            "1aeee21ae5e71ae5e71ae7e520e7e5201a11dc1a11dc1d0ee31d0ee31513e21513e21d0fde1d"
            "0fdee5e61ee5e61eeee21aeee21ae5e71ae5e71ae7e520e7e5200f18d70f18d71713d91713d9"
            "1216d71216d71613dc1613dce3e81ce3e81ce7e521e7e521e1e820e1e820e8e421e8e4210f18"
            "d70f18d71713d91713d91216d71216d71613dc1613dce3e81ce3e81ce7e521e7e521e1e820e1"
            "e820e8e421e8e421");
        auto img = DecodeJpeg(jpg.data(), jpg.size());
        ExpectEqual("codec/jpeg/subsampled-420",
            JpegDiffSummary(img, ref, 16, 16, 3) + "\n",
            "maxDiff=1\n",
            result);
    }

    {
        // 16x16, 4:2:2 — luma has 2 horizontal blocks per MCU, chroma 1
        // each; exercises the non-square (asymmetric H/V) subsampling case.
        auto jpg = HexToBytes(
            "ffd8ffe000104a46494600010100000100010000ffdb00430003020203020203030303040303"
            "04050805050404050a070706080c0a0c0c0b0a0b0b0d0e12100d0e110e0b0b10161011131415"
            "15150c0f171816141812141514ffdb00430103040405040509050509140d0b0d141414141414"
            "1414141414141414141414141414141414141414141414141414141414141414141414141414"
            "141414141414ffc00011080010001003012100021101031101ffc4001f000001050101010101"
            "0100000000000000000102030405060708090a0bffc400b51000020103030204030505040400"
            "00017d01020300041105122131410613516107227114328191a1082342b1c11552d1f0243362"
            "7282090a161718191a25262728292a3435363738393a434445464748494a535455565758595a"
            "636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5a6"
            "a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7"
            "e8e9eaf1f2f3f4f5f6f7f8f9faffc4001f010003010101010101010101000000000000010203"
            "0405060708090a0bffc400b51100020102040403040705040400010277000102031104052131"
            "061241510761711322328108144291a1b1c109233352f0156272d10a162434e125f11718191a"
            "262728292a35363738393a434445464748494a535455565758595a636465666768696a737475"
            "767778797a82838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7"
            "b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9"
            "faffda000c03010002110311003f00f9febdeebf02ce3fe5dfcff43e87e93fff00326ffb8fff"
            "00b84f886bf6dabf7dfa5aff00cc8ffee67ff701f3d917fcbdf97ea7ffd9");
        auto ref = HexToBytes(
            "db1514db1514de1316de1316de1316de1316d91614d9161418c71418c71413c91413c91413c9"
            "1213c91216c71416c714db1514db1514de1316de1316de1316de1316d91614d9161418c71418"
            "c71413c91413c91413c91213c91216c71416c714db1514db1514de1316de1316de1316de1316"
            "d91614d9161418c71418c71413c91413c91413c91213c91216c71416c714db1514db1514de13"
            "16de1316de1316de1316d91614d9161418c71418c71413c91413c91413c91213c91216c71416"
            "c714db1514db1514de1316de1316de1316de1316d91614d9161418c71418c71413c91413c914"
            "13c91213c91216c71416c714db1514db1514de1316de1316de1316de1316d91614d9161418c7"
            "1418c71413c91413c91413c91213c91216c71416c714db1514db1514de1316de1316de1316de"
            "1316d91614d9161418c71418c71413c91413c91413c91213c91216c71416c714db1514db1514"
            "de1316de1316de1316de1316d91614d9161418c71418c71413c91413c91413c91213c91216c7"
            "1416c7141613de1613de1315d91315d91315dc1315dc1613dc1613dce4e71ee4e71ee7e51ee7"
            "e51ee7e521e7e521e4e71ce4e71c1613de1613de1315d91315d91315dc1315dc1613dc1613dc"
            "e4e71ee4e71ee7e51ee7e51ee7e521e7e521e4e71ce4e71c1613de1613de1315d91315d91315"
            "dc1315dc1613dc1613dce4e71ee4e71ee7e51ee7e51ee7e521e7e521e4e71ce4e71c1613de16"
            "13de1315d91315d91315dc1315dc1613dc1613dce4e71ee4e71ee7e51ee7e51ee7e521e7e521"
            "e4e71ce4e71c1613de1613de1315d91315d91315dc1315dc1613dc1613dce4e71ee4e71ee7e5"
            "1ee7e51ee7e521e7e521e4e71ce4e71c1613de1613de1315d91315d91315dc1315dc1613dc16"
            "13dce4e71ee4e71ee7e51ee7e51ee7e521e7e521e4e71ce4e71c1613de1613de1315d91315d9"
            "1315dc1315dc1613dc1613dce4e71ee4e71ee7e51ee7e51ee7e521e7e521e4e71ce4e71c1613"
            "de1613de1315d91315d91315dc1315dc1613dc1613dce4e71ee4e71ee7e51ee7e51ee7e521e7"
            "e521e4e71ce4e71c");
        auto img = DecodeJpeg(jpg.data(), jpg.size());
        ExpectEqual("codec/jpeg/subsampled-422",
            JpegDiffSummary(img, ref, 16, 16, 3) + "\n",
            "maxDiff=0\n",
            result);
    }

    {
        // 16x16 grayscale (1 component) — the color-conversion bypass path.
        auto jpg = HexToBytes(
            "ffd8ffe000104a46494600010100000100010000ffdb00430003020203020203030303040303"
            "04050805050404050a070706080c0a0c0c0b0a0b0b0d0e12100d0e110e0b0b10161011131415"
            "15150c0f171816141812141514ffc0000b080010001001011100ffc4001f0000010501010101"
            "010100000000000000000102030405060708090a0bffc400b510000201030302040305050404"
            "0000017d01020300041105122131410613516107227114328191a1082342b1c11552d1f02433"
            "627282090a161718191a25262728292a3435363738393a434445464748494a53545556575859"
            "5a636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5"
            "a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6"
            "e7e8e9eaf1f2f3f4f5f6f7f8f9faffda0008010100003f00f9febdeebe21afdb6affd9");
        auto ref = HexToBytes(
            "50505050505050507e7e7e7e7e7e7e7e50505050505050507e7e7e7e7e7e7e7e505050505050"
            "50507e7e7e7e7e7e7e7e50505050505050507e7e7e7e7e7e7e7e50505050505050507e7e7e7e"
            "7e7e7e7e50505050505050507e7e7e7e7e7e7e7e50505050505050507e7e7e7e7e7e7e7e5050"
            "5050505050507e7e7e7e7e7e7e7e2b2b2b2b2b2b2b2bcfcfcfcfcfcfcfcf2b2b2b2b2b2b2b2b"
            "cfcfcfcfcfcfcfcf2b2b2b2b2b2b2b2bcfcfcfcfcfcfcfcf2b2b2b2b2b2b2b2bcfcfcfcfcfcf"
            "cfcf2b2b2b2b2b2b2b2bcfcfcfcfcfcfcfcf2b2b2b2b2b2b2b2bcfcfcfcfcfcfcfcf2b2b2b2b"
            "2b2b2b2bcfcfcfcfcfcfcfcf2b2b2b2b2b2b2b2bcfcfcfcfcfcfcfcf");
        auto img = DecodeJpeg(jpg.data(), jpg.size());
        ExpectEqual("codec/jpeg/grayscale",
            JpegDiffSummary(img, ref, 16, 16, 1) + "\n",
            "maxDiff=0\n",
            result);
    }

    {
        // 16x16, progressive (SOF2), produced by libjpeg-turbo's cjpeg
        // -progressive with 4:2:0 subsampling — exercises the separate
        // DC/AC scan decode path (spectral selection + successive
        // approximation across multiple scans) instead of baseline's single
        // full-spectrum Huffman pass. Reference decoded the same way as the
        // baseline vectors above (djpeg -nosmooth).
        auto jpg = HexToBytes(
            "ffd8ffe000104a46494600010100000100010000ffdb004300060405060504060605060707"
            "06080a100a0a09090a140e0f0c1017141818171416161a1d251f1a1b231c1616202c202326"
            "27292a29191f2d302d283025282928ffdb0043010707070a080a130a0a13281a161a282828"
            "28282828282828282828282828282828282828282828282828282828282828282828282828"
            "28282828282828282828ffc20011080010001003012200021101031101ffc4001500010100"
            "000000000000000000000000000506ffc40014010100000000000000000000000000000000"
            "ffda000c030100021003100000018f79978fffc40016100003000000000000000000000000"
            "0000000304ffda00080101000105024cc2661330998fffc400151101010000000000000000"
            "0000000000000500ffda0008010301013f01313bffc4001511010100000000000000000000"
            "000000000200ffda0008010201013f010effc4001410010000000000000000000000000000"
            "0020ffda0008010100063f021fffc400161000030000000000000000000000000000002131"
            "ffda0008010100013f219a2689a268ffda000c0301000200030000001053ffc40016110003"
            "0000000000000000000000000000002131ffda0008010301013f10833fffc4001411010000"
            "0000000000000000000000000000ffda0008010201013f107fffc400151001010000000000"
            "00000000000000000000f1ffda0008010100013f1082828282ffd9");
        auto ref = HexToBytes(
            "00000005050519040d1f0a133a021d4008235d022e6207337c023f8107449e0150a40756be"
            "0060c30565d9046ede09730b0b0b101010240f182a151e450d284b132e680d396d123e870d"
            "4a8c124fa90c5baf1261c90b6bce1070e40f79e9147e061b120b2017202020262626411f2f"
            "462434621f40672445821e52872357a41e63a92368c41c71ca2277df2181e4268610251c15"
            "2a212a2a2a2f2f2f4a2838502e3e6c294a712e4f8c285c912d61ad276cb32d72ce267bd42c"
            "81e82a8aed2f8f063e230b4328204232264838414141464646634152684657824064874569"
            "a44076aa467cc43e83ca4489e04392e5489710482d154d322a4c3c2f51414a4a4a5050506d"
            "4b5c7250618c4a6e914f73ad497fb34f85ce488dd44e93e94c9bee51a0035e320863371e61"
            "3e2467443e604f446655606060656565816071856475a15f83a76589c25e92c76397dd63a0"
            "e268a50e693d136e42296c492f724f496b5a4f71606b6b6b7070708c6b7c906f80ac6a8eb2"
            "7094cd699dd26ea2e86eabed73b0037d400882451e824e2488543e805c4486626081706586"
            "75808080858585a28091a78596c17ea1c784a7de83afe287b30e884b138d50298d592f935f"
            "498b674f916d6b8c7b7091808b8b8b909090ad8b9cb290a1cc89acd28fb2e98ebaed92be02"
            "9f5007a4551ca25d22a8633da16b43a7715fa17d64a6827fa19084a695a0a0a0a6a6a6c19f"
            "afc7a5b5dba3bee0a8c30ba85910ad5e26ac672cb26d47ab754cb07a69ab876eb08c88aa99"
            "8daf9eaaaaaab0b0b0caa8b8d0aebee5adc8eab2cd03c16108c6661dc57022ca753dc37e43"
            "c98460c49065c99580c3a285c8a7a0c2b2a6c8b8c1c1c1c7c7c7dbc6cfe0cbd40dcb6b12d0"
            "7026ce792cd47f47cd884dd38e69cd996ed29e89ccab8ed1b0aaccbcb0d2c2cbcbcbd0d0d0"
            "e5d0d9ead5de07dc720ce17722e08028e68641de8f47e49564dea169e3a683deb288e3b7a6"
            "dec3abe3c8c6dbd2cce1d8e1e1e1e6e6e612e77d17ec822deb8b33f1914ce99a52efa06fe9"
            "ac74eeb18ee9bd93eec2b1e9ceb6eed3d1e6ddd7ece3ecececf1f1f1");
        auto img = DecodeJpeg(jpg.data(), jpg.size());
        // maxDiff=2 here (vs. 1 elsewhere) is IDCT rounding noise, not a decode
        // bug: entropy-decoded coefficients match libjpeg's raw
        // jpeg_read_coefficients() output exactly for all 3 components. Our
        // exact-float IDCT vs. libjpeg's fixed-point ISLOW IDCT puts one Cb
        // pixel 1 unit off, which the 1.772 Cb->Blue conversion factor
        // amplifies to a 2-unit Blue diff.
        ExpectEqual("codec/jpeg/progressive",
            JpegDiffSummary(img, ref, 16, 16, 3) + "\n",
            "maxDiff=2\n",
            result);
    }

    {
        // Not a JPEG at all, and a truncated real JPEG, must both fail
        // cleanly — decoding attacker-supplied image bytes has to be safe
        // against garbage/incomplete input.
        auto notJpeg = HexToBytes("00112233");
        auto badImg = DecodeJpeg(notJpeg.data(), notJpeg.size());
        auto truncated = HexToBytes("ffd8ffe000104a46494600010100000100010000ffdb0043");
        auto truncImg = DecodeJpeg(truncated.data(), truncated.size());
        ExpectEqual("codec/jpeg/malformed-input-fails-safely",
            std::string(badImg.success ? "unexpected-ok " : "rejected ") +
                (truncImg.success ? "unexpected-ok\n" : "rejected\n"),
            "rejected rejected\n",
            result);
    }

    {
        auto webp = MakeSolidVp8LWebp(2, 2, 17, 34, 51, 68);
        auto img = DecodeWebp(webp.data(), webp.size());
        ExpectEqual("codec/webp/vp8l-solid-color",
            RgbaSummary(img) + "\n",
            "2x2:17,34,51,68 17,34,51,68 17,34,51,68 17,34,51,68\n",
            result);
    }

    {
        // dwebp reference for a tiny real lossy VP8 keyframe with residual coefficients.
        auto webp = HexToBytes(
            "524946462600000057454250565038201a0000005001009d012a10001000"
            "00000025a400000000005f7722048000");
        std::vector<uint8_t> ref((size_t)16 * 16 * 3, 0x82);
        auto img = DecodeWebp(webp.data(), webp.size());
        ExpectEqual("codec/webp/vp8-lossy-residuals",
            JpegDiffSummary(img, ref, 16, 16, 3) + "\n",
            "maxDiff=0\n",
            result);
    }

    {
        std::vector<uint8_t> alph = {0, 0, 64, 128, 255};
        auto webp = MakeExtendedVp8LWebp(2, 2, 17, 34, 51, 255, alph);
        auto img = DecodeWebp(webp.data(), webp.size());
        ExpectEqual("codec/webp/vp8x-raw-alpha",
            RgbaSummary(img) + "\n",
            "2x2:17,34,51,0 17,34,51,64 17,34,51,128 17,34,51,255\n",
            result);
    }

    {
        std::vector<uint8_t> alph = {1};
        auto stream = MakeAlphaOnlyVp8LStream(99);
        alph.insert(alph.end(), stream.begin(), stream.end());
        auto webp = MakeExtendedVp8LWebp(2, 2, 17, 34, 51, 255, alph);
        auto img = DecodeWebp(webp.data(), webp.size());
        ExpectEqual("codec/webp/vp8x-compressed-alpha-solid",
            RgbaSummary(img) + "\n",
            "2x2:17,34,51,99 17,34,51,99 17,34,51,99 17,34,51,99\n",
            result);
    }

    {
        // WebP decoder is hand-rolled and rejects unsupported/truncated payloads
        // instead of pretending to decode pixels.
        auto notWebp = HexToBytes("00112233");
        auto badImg = DecodeWebp(notWebp.data(), notWebp.size());
        auto tinyVp8 = HexToBytes(
            "5249464612000000574542505650382006000000"
            "0000009d012a01000100");
        auto vp8Img = DecodeWebp(tinyVp8.data(), tinyVp8.size());
        std::vector<uint8_t> shortAlph = {0, 1, 2, 3};
        auto badAlpha = MakeExtendedVp8LWebp(2, 2, 17, 34, 51, 255, shortAlph);
        auto badAlphaImg = DecodeWebp(badAlpha.data(), badAlpha.size());
        ExpectEqual("codec/webp/unsupported-fails-safely",
            std::string(badImg.success ? "unexpected-ok " : "rejected ") +
                (vp8Img.success ? "unexpected-ok " : "rejected ") +
                (badAlphaImg.success ? "unexpected-ok\n" : "rejected\n"),
            "rejected rejected rejected\n",
            result);
    }

    return result;
}
