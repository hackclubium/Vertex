#pragma once
#include <cstddef>
#include <cstdint>

// Standard CRC-32 (IEEE 802.3 / zlib polynomial 0xEDB88320) — the same
// checksum PNG chunks, gzip, and zip all use. Shared here rather than
// bundled privately into png.cpp since it'll be reused as-is for gzip's
// Content-Encoding trailer once the from-scratch HTTP client work reaches
// that stage.
uint32_t Crc32(const uint8_t* data, size_t size);
