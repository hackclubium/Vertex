#include "test/fixture.h"
#include "codec/inflate.h"
#include "codec/png.h"

#include <cctype>
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

} // namespace

TestResult RunCodecTests() {
    TestResult result;

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

    return result;
}
