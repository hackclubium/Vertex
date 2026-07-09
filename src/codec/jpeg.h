#pragma once
//
// jpeg.h — hand-rolled baseline sequential JPEG decoder.
//
// Part of Vertex's zero-third-party-dependency push (replaces stb_image for
// this format). Marker parsing, JPEG-style Huffman tables, DC/AC coefficient
// decoding, dequantization, a separable 2D IDCT, chroma upsampling, and
// YCbCr->RGB are all implemented here.
//
// Scope: baseline (SOF0/SOF1) and progressive (SOF2) DCT scans. Arithmetic
// coding is NOT supported — vanishingly rare relative to Huffman coding, and
// not something any mainstream encoder emits by default. 1-component
// (grayscale) and 3-component (YCbCr) images are supported with any of the
// common chroma subsampling ratios (4:4:4, 4:2:2, 4:2:0); 4-component
// (CMYK/Adobe) JPEGs are not supported — matching the same "defer the rare
// case" scoping already used for PNG's Adam7 interlacing.
// Chroma upsampling is nearest-neighbor, not the smoother bilinear-ish
// ("fancy") upsampling most decoders (including libjpeg's default) use —
// deliberately simple for now; verified against libjpeg-turbo's own
// `-nosmooth` (plain upsampling) reference output, which is the correct
// apples-to-apples comparison given that choice.
//
#include "codec/image.h"
#include <cstddef>

DecodedImage DecodeJpeg(const uint8_t* data, size_t size);
