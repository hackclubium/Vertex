#include "network/text_decode.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <cstdint>

namespace {

std::string LowerAscii(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::string Trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
        [](unsigned char c) { return std::isspace(c); });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
        [](unsigned char c) { return std::isspace(c); }).base();
    return first < last ? std::string(first, last) : std::string{};
}

std::string CharsetFrom(const std::string& source) {
    const std::string lower = LowerAscii(source);
    const size_t marker = lower.find("charset");
    if (marker == std::string::npos) return {};
    size_t cursor = marker + 7;
    while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor]))) ++cursor;
    if (cursor >= source.size() || source[cursor] != '=') return {};
    ++cursor;
    while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor]))) ++cursor;
    const char quote = cursor < source.size() && (source[cursor] == '\'' || source[cursor] == '"')
        ? source[cursor++] : 0;
    const size_t end = source.find_first_of(quote ? std::string(1, quote) : " \t\r\n;>", cursor);
    return LowerAscii(Trim(source.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor)));
}

// ── portable UTF-16 → UTF-8 (no Windows API needed) ─────────────────────────

static void Utf8Encode(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

std::string DecodeUtf16(const std::string& bytes, bool littleEndian) {
    if (bytes.size() < 2) return {};
    std::string out;
    out.reserve(bytes.size());
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
        uint8_t a = (uint8_t)bytes[i], b = (uint8_t)bytes[i + 1];
        uint16_t unit = littleEndian ? (uint16_t)(a | (b << 8)) : (uint16_t)((a << 8) | b);
        // Surrogate pair
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < bytes.size()) {
            uint8_t c = (uint8_t)bytes[i + 2], d = (uint8_t)bytes[i + 3];
            uint16_t low = littleEndian ? (uint16_t)(c | (d << 8)) : (uint16_t)((c << 8) | d);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                uint32_t cp = 0x10000 + ((uint32_t)(unit - 0xD800) << 10) + (low - 0xDC00);
                Utf8Encode(out, cp);
                i += 2;
                continue;
            }
        }
        Utf8Encode(out, unit);
    }
    return out;
}

// ── Windows-1252 / Latin-1 → UTF-8 (portable lookup table) ──────────────────

static const uint16_t kCp1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

std::string DecodeLatin(const std::string& bytes, bool windows1252) {
    std::string out;
    out.reserve(bytes.size());
    for (unsigned char c : bytes) {
        if (c < 0x80) {
            out += (char)c;
        } else if (windows1252 && c >= 0x80 && c <= 0x9F) {
            Utf8Encode(out, kCp1252[c - 0x80]);
        } else {
            Utf8Encode(out, c);  // Latin-1: byte value = Unicode codepoint
        }
    }
    return out;
}

} // namespace

std::string DecodeTextToUtf8(const std::string& bytes,
                             const std::string& contentType,
                             bool sniffHtmlCharset) {
    // BOM detection
    if (bytes.size() >= 3
        && (uint8_t)bytes[0] == 0xEF
        && (uint8_t)bytes[1] == 0xBB
        && (uint8_t)bytes[2] == 0xBF)
        return bytes.substr(3);
    if (bytes.size() >= 2 && (uint8_t)bytes[0] == 0xFF && (uint8_t)bytes[1] == 0xFE)
        return DecodeUtf16(bytes.substr(2), true);
    if (bytes.size() >= 2 && (uint8_t)bytes[0] == 0xFE && (uint8_t)bytes[1] == 0xFF)
        return DecodeUtf16(bytes.substr(2), false);

    std::string charset = CharsetFrom(contentType);
    if (charset.empty() && sniffHtmlCharset)
        charset = CharsetFrom(bytes.substr(0, std::min<size_t>(bytes.size(), 8192)));
    if (charset.empty() || charset == "utf-8" || charset == "utf8")
        return bytes;
    if (charset == "utf-16" || charset == "utf-16le")
        return DecodeUtf16(bytes, true);
    if (charset == "utf-16be")
        return DecodeUtf16(bytes, false);
    if (charset == "windows-1252" || charset == "cp1252")
        return DecodeLatin(bytes, true);
    if (charset == "iso-8859-1" || charset == "latin1")
        return DecodeLatin(bytes, false);
    return bytes;  // Unknown encoding: preserve bytes rather than corrupt them.
}
