#pragma once
// Hand-rolled WebP decoder. VP8 supports simple lossy keyframes whose macroblocks
// skip residual coefficients; VP8L supports a tiny literal-only single-color subset.
#include "codec/image.h"
#include <cstddef>

DecodedImage DecodeWebp(const uint8_t* data, size_t size);
