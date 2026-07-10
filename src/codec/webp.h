#pragma once
// Hand-rolled WebP decoder. VP8L supports the deliberately tiny literal-only
// single-color subset; unsupported VP8/VP8L/VP8X variants fail safely.
#include "codec/image.h"
#include <cstddef>

DecodedImage DecodeWebp(const uint8_t* data, size_t size);
