#pragma once
//
// truetype.h — hand-rolled TrueType (.ttf) font parser.
//
// Part of Vertex's zero-third-party-dependency push (replaces Pango +
// fontconfig on Linux, phase 3 of the windowing rewrite). Scope: TrueType
// 'glyf' outlines only — CFF/PostScript-flavored OpenType (.otf) is
// deliberately not supported, the same "cover the common case first"
// tradeoff as JPEG's baseline-only scope. DejaVu Sans/Serif/Mono — what
// Vertex's generic sans-serif/serif/monospace families resolve to — are
// all TrueType, so this covers the overwhelming common case. cmap format 4
// only (Basic Multilingual Plane — covers Latin/Cyrillic/Greek/CJK-in-BMP,
// skips rare non-BMP codepoints like emoji). No kerning/ligatures/GPOS —
// Vertex's own layout engine already breaks text into positioned
// single-line runs before calling into any of this, so simple left-to-right
// advance-width placement is all that's needed.
//
#include <cstdint>
#include <string>
#include <vector>

namespace ttf {

// A point in font design units (y-up, unlike screen coordinates).
struct Point {
    float x = 0, y = 0;
    bool onCurve = true;
};
using Contour = std::vector<Point>;

struct GlyphOutline {
    std::vector<Contour> contours;  // on/off-curve points — quadratic Bezier control points where onCurve is false
    float advanceWidth = 0;         // font design units (divide by UnitsPerEm() for an em-relative value)
};

class Font {
public:
    bool LoadFromFile(const std::string& path);

    bool IsLoaded() const { return m_loaded; }
    int UnitsPerEm() const { return m_unitsPerEm; }
    float Ascender() const { return m_ascender; }    // font design units
    float Descender() const { return m_descender; }  // font design units, negative

    // Returns 0 (the standard "missing glyph" convention) if the font has
    // no mapping for this codepoint.
    uint16_t GlyphIndexForCodepoint(uint32_t codepoint) const;
    GlyphOutline OutlineForGlyph(uint16_t glyphIndex) const;
    float AdvanceWidthForGlyph(uint16_t glyphIndex) const;

    const std::string& FamilyName() const { return m_family; }
    const std::string& SubfamilyName() const { return m_subfamily; }

private:
    std::vector<uint8_t> m_data;
    bool m_loaded = false;
    int m_unitsPerEm = 1000;
    float m_ascender = 0, m_descender = 0;
    int m_indexToLocFormat = 0;
    int m_numGlyphs = 0;
    int m_numberOfHMetrics = 0;
    std::string m_family, m_subfamily;

    struct TableRange { uint32_t offset = 0, length = 0; };
    TableRange m_cmapTable, m_glyfTable, m_locaTable, m_hmtxTable, m_headTable, m_hheaTable, m_maxpTable, m_nameTable;
    // Resolved cmap subtable (format 4) location, within m_data directly (absolute offsets).
    uint32_t m_cmapSubtableOffset = 0, m_cmapSubtableLength = 0;

    bool ParseTableDirectory();
    bool ParseHead();
    bool ParseHhea();
    bool ParseMaxp();
    bool ParseName();
    bool ResolveCmapSubtable();

    uint32_t GlyfOffsetForIndex(uint16_t glyphIndex, uint32_t& outLength) const;
    GlyphOutline ParseSimpleGlyph(const uint8_t* p, const uint8_t* end, int numberOfContours) const;
    GlyphOutline ParseCompositeGlyph(const uint8_t* p, const uint8_t* end, int depth) const;
    GlyphOutline ParseGlyph(uint16_t glyphIndex, int depth) const;
};

}  // namespace ttf
