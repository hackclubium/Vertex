#pragma once
// Hand-rolled WebP decoder for still images. VP8/VP8L cover common small streams;
// unsupported extended/animated variants fail safely.
#include "codec/image.h"
#include <cstddef>

DecodedImage DecodeWebp(const uint8_t* data, size_t size);
