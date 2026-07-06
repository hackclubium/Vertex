#pragma once
//
// inflate.h — hand-rolled DEFLATE (RFC 1951) decompressor.
//
// Part of Vertex's zero-third-party-dependency push: this replaces the
// decompression zlib/libcurl currently do internally. Shared infrastructure
// for two consumers: PNG decoding (IDAT streams are zlib-wrapped DEFLATE)
// and, later, HTTP's Content-Encoding: gzip (gzip-wrapped DEFLATE — that
// wrapper isn't implemented yet, only the zlib one PNG needs).
//
#include <cstdint>
#include <string>

// Decompresses a raw DEFLATE bitstream (no zlib/gzip wrapper) per RFC 1951.
// Returns false on any malformed input (bad block type, corrupt stored-block
// length, a back-reference pointing before the start of output, etc.) —
// never throws, since decompressing attacker-controlled network/file bytes
// must not be able to crash the browser.
bool Inflate(const uint8_t* data, size_t size, std::string& out);

// RFC 1950 zlib format: a 2-byte header, a raw DEFLATE stream, then a 4-byte
// big-endian Adler-32 checksum of the decompressed data. This is exactly the
// format PNG's IDAT chunks use.
bool ZlibInflate(const uint8_t* data, size_t size, std::string& out);

uint32_t Adler32(const uint8_t* data, size_t size);
