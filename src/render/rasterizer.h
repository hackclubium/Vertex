#pragma once
//
// rasterizer.h — hand-rolled 2D software rasterizer.
//
// Part of Vertex's zero-third-party-dependency push: replaces Cairo's
// drawing engine on Linux (phase 2 of the windowing rewrite — phase 1 cut
// GTK3 but kept Cairo/Pango for actual drawing). Fill quality target:
// smooth anti-aliased edges comparable to Direct2D/Core Graphics, the bar
// the Windows/macOS backends already meet.
//
// Scope: solid-color fills only (no gradients/patterns — those aren't used
// by IPlatformRenderer's primitives). Curves are flattened to line segments
// with a fixed subdivision count rather than adaptive flatness testing,
// matching this project's established "baseline first" pattern (e.g. JPEG's
// nearest-neighbor chroma upsampling, PNG's no-Adam7).
//
#include "platform/platform.h"
#include <cstdint>
#include <vector>

namespace raster {

struct Vec2 {
    float x = 0, y = 0;
};

// A closed polygon contour (last point implicitly connects back to the
// first). FillPath accepts multiple contours so a single fill call can
// represent shapes with the "rounded rect" style single outer contour, or
// (in principle) multiple disjoint contours filled together.
using Contour = std::vector<Vec2>;

struct ClipRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // half-open [x0,x1) x [y0,y1)
    bool Empty() const { return x1 <= x0 || y1 <= y0; }
};

// Owns a BGRA8888 pixel buffer in the byte layout X11 TrueColor visuals use
// natively on a little-endian host (B,G,R,A per pixel — same convention
// already established by platform_linux.cpp's CreateBitmap for Cairo
// interop), so main_linux.cpp can hand the raw buffer straight to
// xcb_put_image with no channel-swizzle step.
struct Framebuffer {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;  // width*height*4 bytes

    void Resize(int w, int h);
    uint8_t* Row(int y) { return pixels.data() + (size_t)y * width * 4; }
    const uint8_t* Row(int y) const { return pixels.data() + (size_t)y * width * 4; }
};

// Fills one or more contours (nonzero winding rule) with a solid color,
// anti-aliased via 4x vertical supersampling combined with exact analytic
// horizontal coverage per subsample line. Clipped to `clip` (already
// intersected with the framebuffer bounds by the caller).
void FillPath(Framebuffer& fb, const std::vector<Contour>& contours, PlatColor color, const ClipRect& clip);

// Convenience shapes built on FillPath.
void FillRect(Framebuffer& fb, float x, float y, float w, float h, PlatColor color, const ClipRect& clip);
void FillRoundedRect(Framebuffer& fb, float x, float y, float w, float h, float radius, PlatColor color, const ClipRect& clip);
// Rectangle outline, built from 4 filled strips (not a hollow-polygon fill).
void StrokeRect(Framebuffer& fb, float x, float y, float w, float h, PlatColor color, float strokeWidth, const ClipRect& clip);
// A single line segment, built as a filled quad (rotated rectangle) around the segment.
void StrokeLine(Framebuffer& fb, float x0, float y0, float x1, float y1, PlatColor color, float strokeWidth, const ClipRect& clip);

// Nearest-neighbor scaled blit with alpha compositing, clipped to `clip`.
// `src`/`srcW`/`srcH` is a premultiplied-alpha BGRA8888 buffer (same layout
// as Framebuffer, and the same convention main_linux.cpp's image pipeline
// already premultiplies into before calling CreateBitmap, matching Cairo's
// ARGB32 format that convention was originally built for).
void BlitBitmap(Framebuffer& fb, const uint8_t* src, int srcW, int srcH,
                 float destX, float destY, float destW, float destH,
                 const ClipRect& clip, float globalAlpha = 1.f);

// Alpha-only coverage mask blit (used for glyph rendering): `mask` is an
// 8-bit coverage buffer (`maskStride` bytes per row), composited using
// `color` at full resolution (no scaling).
void BlitAlphaMask(Framebuffer& fb, const uint8_t* mask, int maskW, int maskH, int maskStride,
                    int destX, int destY, PlatColor color, const ClipRect& clip);

}  // namespace raster
