#include "font/truetype.h"
#include <cstring>
#include <fstream>

namespace ttf {

namespace {

uint16_t ReadU16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
int16_t ReadS16(const uint8_t* p) { return (int16_t)ReadU16(p); }
uint32_t ReadU32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
float ReadF2Dot14(const uint8_t* p) { return (float)ReadS16(p) / 16384.f; }

}  // namespace

bool Font::LoadFromFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize sz = f.tellg();
    if (sz <= 12) return false;
    f.seekg(0, std::ios::beg);
    m_data.resize((size_t)sz);
    if (!f.read((char*)m_data.data(), sz)) return false;

    if (!ParseTableDirectory()) return false;
    if (m_glyfTable.length == 0 || m_locaTable.length == 0) return false;  // not a glyf-outline font (e.g. CFF-flavored OTF)
    if (!ParseHead()) return false;
    if (!ParseHhea()) return false;
    if (!ParseMaxp()) return false;
    if (!ResolveCmapSubtable()) return false;
    ParseName();  // best-effort; missing name strings aren't fatal

    m_loaded = true;
    return true;
}

bool Font::ParseTableDirectory() {
    if (m_data.size() < 12) return false;
    const uint8_t* d = m_data.data();
    uint32_t sfntVersion = ReadU32(d);
    // 0x00010000 = TrueType, 0x74727565 ("true") = old Mac TrueType. Reject
    // "OTTO" (CFF-flavored OpenType) and TrueType Collections ("ttcf") —
    // out of scope, see the header comment.
    if (sfntVersion != 0x00010000 && sfntVersion != 0x74727565) return false;
    uint16_t numTables = ReadU16(d + 4);
    if (12u + (uint32_t)numTables * 16u > m_data.size()) return false;

    for (uint16_t i = 0; i < numTables; i++) {
        const uint8_t* rec = d + 12 + i * 16;
        char tag[5] = {(char)rec[0], (char)rec[1], (char)rec[2], (char)rec[3], 0};
        uint32_t offset = ReadU32(rec + 8);
        uint32_t length = ReadU32(rec + 12);
        if ((uint64_t)offset + length > m_data.size()) continue;  // corrupt entry, skip
        TableRange range{offset, length};
        if (!strcmp(tag, "cmap")) m_cmapTable = range;
        else if (!strcmp(tag, "glyf")) m_glyfTable = range;
        else if (!strcmp(tag, "loca")) m_locaTable = range;
        else if (!strcmp(tag, "hmtx")) m_hmtxTable = range;
        else if (!strcmp(tag, "head")) m_headTable = range;
        else if (!strcmp(tag, "hhea")) m_hheaTable = range;
        else if (!strcmp(tag, "maxp")) m_maxpTable = range;
        else if (!strcmp(tag, "name")) m_nameTable = range;
    }
    return true;
}

bool Font::ParseHead() {
    if (m_headTable.length < 54) return false;
    const uint8_t* p = m_data.data() + m_headTable.offset;
    m_unitsPerEm = ReadU16(p + 18);
    if (m_unitsPerEm <= 0) m_unitsPerEm = 1000;
    m_indexToLocFormat = ReadS16(p + 50);
    return true;
}

bool Font::ParseHhea() {
    if (m_hheaTable.length < 36) return false;
    const uint8_t* p = m_data.data() + m_hheaTable.offset;
    m_ascender = (float)ReadS16(p + 4);
    m_descender = (float)ReadS16(p + 6);
    m_numberOfHMetrics = ReadU16(p + 34);
    return true;
}

bool Font::ParseMaxp() {
    if (m_maxpTable.length < 6) return false;
    const uint8_t* p = m_data.data() + m_maxpTable.offset;
    m_numGlyphs = ReadU16(p + 4);
    return true;
}

bool Font::ResolveCmapSubtable() {
    if (m_cmapTable.length < 4) return false;
    const uint8_t* base = m_data.data() + m_cmapTable.offset;
    uint16_t numTables = ReadU16(base + 2);
    if (4u + (uint32_t)numTables * 8u > m_cmapTable.length) return false;

    uint32_t bestOffset = 0;
    int bestScore = -1;
    for (uint16_t i = 0; i < numTables; i++) {
        const uint8_t* rec = base + 4 + i * 8;
        uint16_t platformID = ReadU16(rec);
        uint16_t encodingID = ReadU16(rec + 2);
        uint32_t offset = ReadU32(rec + 4);
        if (offset + 2 > m_cmapTable.length) continue;
        uint16_t format = ReadU16(base + offset);
        if (format != 4) continue;
        int score = (platformID == 3 && encodingID == 1) ? 2 : (platformID == 0 ? 1 : 0);
        if (score > bestScore) { bestScore = score; bestOffset = offset; }
    }
    if (bestScore < 0) return false;
    m_cmapSubtableOffset = m_cmapTable.offset + bestOffset;
    m_cmapSubtableLength = m_cmapTable.length - bestOffset;
    return true;
}

bool Font::ParseName() {
    if (m_nameTable.length < 6) return false;
    const uint8_t* base = m_data.data() + m_nameTable.offset;
    uint16_t count = ReadU16(base + 2);
    uint16_t stringOffset = ReadU16(base + 4);
    if (6u + (uint32_t)count * 12u > m_nameTable.length) return false;

    auto decodeRecord = [&](const uint8_t* rec, std::string& out) {
        uint16_t platformID = ReadU16(rec);
        uint16_t length = ReadU16(rec + 8);
        uint16_t offset = ReadU16(rec + 10);
        uint32_t strStart = (uint32_t)stringOffset + offset;
        if (strStart + length > m_nameTable.length) return;
        const uint8_t* s = base + strStart;
        std::string decoded;
        if (platformID == 3 || platformID == 0) {
            // UTF-16BE — decode assuming BMP ASCII-range family names (true
            // for virtually every real font's family/subfamily strings).
            for (uint16_t k = 0; k + 1 < length; k += 2) {
                uint16_t code = ReadU16(s + k);
                decoded += (code < 128) ? (char)code : '?';
            }
        } else {
            // Mac Roman (platform 1) or unknown — treat as near-enough ASCII.
            decoded.assign((const char*)s, length);
        }
        if (!decoded.empty()) out = decoded;
    };

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t* rec = base + 6 + i * 12;
        uint16_t nameID = ReadU16(rec + 6);
        if (nameID == 1 && m_family.empty()) decodeRecord(rec, m_family);
        else if (nameID == 2 && m_subfamily.empty()) decodeRecord(rec, m_subfamily);
    }
    return true;
}

uint16_t Font::GlyphIndexForCodepoint(uint32_t codepoint) const {
    if (!m_loaded || codepoint > 0xFFFF || m_cmapSubtableLength < 14) return 0;
    const uint8_t* sub = m_data.data() + m_cmapSubtableOffset;
    uint16_t segCountX2 = ReadU16(sub + 6);
    int segCount = segCountX2 / 2;
    if (segCount <= 0) return 0;
    const uint8_t* endCodes = sub + 14;
    const uint8_t* startCodes = endCodes + segCountX2 + 2;  // +2 for reservedPad
    const uint8_t* idDeltas = startCodes + segCountX2;
    const uint8_t* idRangeOffsets = idDeltas + segCountX2;
    if ((uint32_t)(idRangeOffsets - sub) + segCountX2 > m_cmapSubtableLength) return 0;

    uint16_t cp = (uint16_t)codepoint;
    int lo = 0, hi = segCount - 1, seg = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t endCode = ReadU16(endCodes + mid * 2);
        if (endCode >= cp) { seg = mid; hi = mid - 1; }
        else lo = mid + 1;
    }
    if (seg < 0) return 0;
    uint16_t startCode = ReadU16(startCodes + seg * 2);
    if (startCode > cp) return 0;
    int16_t idDelta = ReadS16(idDeltas + seg * 2);
    uint16_t idRangeOffset = ReadU16(idRangeOffsets + seg * 2);
    if (idRangeOffset == 0) {
        return (uint16_t)(cp + idDelta);
    }
    const uint8_t* glyphIndexAddr = idRangeOffsets + seg * 2 + idRangeOffset + 2 * (cp - startCode);
    if ((uint32_t)(glyphIndexAddr - sub) + 2 > m_cmapSubtableLength) return 0;
    uint16_t g = ReadU16(glyphIndexAddr);
    if (g == 0) return 0;
    return (uint16_t)(g + idDelta);
}

float Font::AdvanceWidthForGlyph(uint16_t glyphIndex) const {
    if (!m_loaded || m_numberOfHMetrics <= 0) return 0;
    int idx = (int)glyphIndex < m_numberOfHMetrics ? (int)glyphIndex : m_numberOfHMetrics - 1;
    uint32_t off = (uint32_t)idx * 4;
    if (off + 2 > m_hmtxTable.length) return 0;
    return (float)ReadU16(m_data.data() + m_hmtxTable.offset + off);
}

uint32_t Font::GlyfOffsetForIndex(uint16_t glyphIndex, uint32_t& outLength) const {
    outLength = 0;
    if ((int)glyphIndex >= m_numGlyphs) return 0;
    const uint8_t* loca = m_data.data() + m_locaTable.offset;
    uint32_t o1, o2;
    if (m_indexToLocFormat == 0) {
        uint32_t byteOff = (uint32_t)glyphIndex * 2;
        if (byteOff + 4 > m_locaTable.length) return 0;
        o1 = (uint32_t)ReadU16(loca + byteOff) * 2;
        o2 = (uint32_t)ReadU16(loca + byteOff + 2) * 2;
    } else {
        uint32_t byteOff = (uint32_t)glyphIndex * 4;
        if (byteOff + 8 > m_locaTable.length) return 0;
        o1 = ReadU32(loca + byteOff);
        o2 = ReadU32(loca + byteOff + 4);
    }
    if (o2 < o1 || o2 - o1 > m_glyfTable.length || o1 > m_glyfTable.length) return 0;
    outLength = o2 - o1;
    return o1;
}

GlyphOutline Font::ParseSimpleGlyph(const uint8_t* p, const uint8_t* end, int numberOfContours) const {
    GlyphOutline g;
    const uint8_t* cursor = p + 10;  // skip numberOfContours(2) + bbox(8)
    if (cursor + numberOfContours * 2 + 2 > end) return g;

    std::vector<uint16_t> endPts((size_t)numberOfContours);
    for (int i = 0; i < numberOfContours; i++) { endPts[(size_t)i] = ReadU16(cursor); cursor += 2; }
    int numPoints = numberOfContours > 0 ? endPts.back() + 1 : 0;
    if (numPoints <= 0 || numPoints > 20000) return g;  // sanity guard against corrupt data

    if (cursor + 2 > end) return g;
    uint16_t instructionLength = ReadU16(cursor);
    cursor += 2;
    cursor += instructionLength;
    if (cursor > end) return g;

    std::vector<uint8_t> flags((size_t)numPoints);
    for (int i = 0; i < numPoints;) {
        if (cursor >= end) return g;
        uint8_t f = *cursor++;
        flags[(size_t)i++] = f;
        if (f & 0x08) {  // REPEAT_FLAG
            if (cursor >= end) return g;
            uint8_t repeat = *cursor++;
            for (int r = 0; r < repeat && i < numPoints; r++) flags[(size_t)i++] = f;
        }
    }

    std::vector<int> xs((size_t)numPoints), ys((size_t)numPoints);
    int x = 0;
    for (int i = 0; i < numPoints; i++) {
        uint8_t f = flags[(size_t)i];
        if (f & 0x02) {
            if (cursor >= end) return g;
            uint8_t dx = *cursor++;
            x += (f & 0x10) ? dx : -dx;
        } else if (!(f & 0x10)) {
            if (cursor + 2 > end) return g;
            x += ReadS16(cursor);
            cursor += 2;
        }
        xs[(size_t)i] = x;
    }
    int y = 0;
    for (int i = 0; i < numPoints; i++) {
        uint8_t f = flags[(size_t)i];
        if (f & 0x04) {
            if (cursor >= end) return g;
            uint8_t dy = *cursor++;
            y += (f & 0x20) ? dy : -dy;
        } else if (!(f & 0x20)) {
            if (cursor + 2 > end) return g;
            y += ReadS16(cursor);
            cursor += 2;
        }
        ys[(size_t)i] = y;
    }

    int start = 0;
    for (int c = 0; c < numberOfContours; c++) {
        int endIdx = (int)endPts[(size_t)c];
        Contour contour;
        for (int i = start; i <= endIdx && i < numPoints; i++)
            contour.push_back({(float)xs[(size_t)i], (float)ys[(size_t)i], (flags[(size_t)i] & 0x01) != 0});
        if (!contour.empty()) g.contours.push_back(std::move(contour));
        start = endIdx + 1;
    }
    return g;
}

GlyphOutline Font::ParseCompositeGlyph(const uint8_t* p, const uint8_t* end, int depth) const {
    GlyphOutline result;
    if (depth > 8) return result;  // guard against malformed circular composite references
    const uint8_t* cursor = p + 10;  // skip numberOfContours(2, negative) + bbox(8)
    bool more = true;
    while (more && cursor + 4 <= end) {
        uint16_t flags = ReadU16(cursor); cursor += 2;
        uint16_t glyphIndex = ReadU16(cursor); cursor += 2;
        float dx = 0, dy = 0;
        if (flags & 0x0001) {  // ARGS_ARE_WORDS
            if (cursor + 4 > end) break;
            if (flags & 0x0002) { dx = (float)ReadS16(cursor); dy = (float)ReadS16(cursor + 2); }
            cursor += 4;
        } else {
            if (cursor + 2 > end) break;
            if (flags & 0x0002) { dx = (float)(int8_t)cursor[0]; dy = (float)(int8_t)cursor[1]; }
            cursor += 2;
        }
        float a = 1.f, b = 0.f, c = 0.f, d = 1.f;
        if (flags & 0x0008) {
            if (cursor + 2 > end) break;
            a = d = ReadF2Dot14(cursor);
            cursor += 2;
        } else if (flags & 0x0040) {
            if (cursor + 4 > end) break;
            a = ReadF2Dot14(cursor);
            d = ReadF2Dot14(cursor + 2);
            cursor += 4;
        } else if (flags & 0x0080) {
            if (cursor + 8 > end) break;
            a = ReadF2Dot14(cursor);
            b = ReadF2Dot14(cursor + 2);
            c = ReadF2Dot14(cursor + 4);
            d = ReadF2Dot14(cursor + 6);
            cursor += 8;
        }
        // Component offsets are only meaningful when ARGS_ARE_XY_VALUES is
        // set; the point-matching alternative (rare) isn't supported —
        // falls back to a (0,0) offset rather than misplacing the component.
        GlyphOutline sub = (flags & 0x0002) ? ParseGlyph(glyphIndex, depth + 1) : GlyphOutline{};
        for (auto& contour : sub.contours) {
            Contour transformed;
            transformed.reserve(contour.size());
            for (auto& pt : contour)
                transformed.push_back({a * pt.x + c * pt.y + dx, b * pt.x + d * pt.y + dy, pt.onCurve});
            result.contours.push_back(std::move(transformed));
        }
        more = (flags & 0x0020) != 0;  // MORE_COMPONENTS
    }
    return result;
}

GlyphOutline Font::ParseGlyph(uint16_t glyphIndex, int depth) const {
    uint32_t len = 0;
    uint32_t off = GlyfOffsetForIndex(glyphIndex, len);
    GlyphOutline g;
    g.advanceWidth = AdvanceWidthForGlyph(glyphIndex);
    if (len < 10) return g;  // no outline (e.g. space) or corrupt entry
    const uint8_t* p = m_data.data() + m_glyfTable.offset + off;
    const uint8_t* end = p + len;
    int16_t numberOfContours = ReadS16(p);
    if (numberOfContours >= 0) g.contours = ParseSimpleGlyph(p, end, numberOfContours).contours;
    else g.contours = ParseCompositeGlyph(p, end, depth).contours;
    return g;
}

GlyphOutline Font::OutlineForGlyph(uint16_t glyphIndex) const {
    if (!m_loaded) return {};
    return ParseGlyph(glyphIndex, 0);
}

}  // namespace ttf
