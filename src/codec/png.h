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
#include "codec/image.h"
#include <cstddef>

DecodedImage DecodePng(const uint8_t* data, size_t size);
