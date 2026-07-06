#pragma once
//
// png.h — hand-rolled PNG decoder.
//
// Part of Vertex's zero-third-party-dependency push (replaces stb_image for
// this format). Built on the just-shipped ZlibInflate/Crc32.
//
// Scope: non-interlaced images only (Adam7 interlacing is not implemented —
// rare in real-world web content, deferred rather than blocking this on a
// much rarer case). All 5 PNG color types (grayscale, truecolor, indexed,
// grayscale+alpha, truecolor+alpha) and all their valid bit depths
// (1/2/4/8/16) are supported, including tRNS-chunk transparency. No color
// management (gAMA/cHRM/iCCP are ignored, matching how most lightweight
// decoders treat sample values as already being final display values).
//
#include <cstddef>
#include <cstdint>
#include <vector>

struct DecodedImage {
    bool success = false;
    int width = 0;
    int height = 0;
    // width*height*4 bytes, row-major top-to-bottom, straight (not
    // premultiplied) RGBA — same convention stb_image's callers already
    // expect, so this is a drop-in replacement at the call site.
    std::vector<uint8_t> rgba;
};

DecodedImage DecodePng(const uint8_t* data, size_t size);
