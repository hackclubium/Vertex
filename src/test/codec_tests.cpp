#include "test/fixture.h"
#include "codec/inflate.h"

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

    return result;
}
