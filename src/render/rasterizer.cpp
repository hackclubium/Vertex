#include "render/rasterizer.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace raster {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kSubsamples = 4;

struct Edge {
    float x0, y0, x1, y1;
};

void BuildEdges(const std::vector<Contour>& contours, std::vector<Edge>& edges,
                 float& minX, float& minY, float& maxX, float& maxY) {
    minX = minY = 1e30f;
    maxX = maxY = -1e30f;
    for (const auto& c : contours) {
        size_t n = c.size();
        if (n < 2) continue;
        for (size_t i = 0; i < n; i++) {
            Vec2 a = c[i];
            Vec2 b = c[(i + 1) % n];
            minX = std::min({minX, a.x, b.x});
            maxX = std::max({maxX, a.x, b.x});
            minY = std::min({minY, a.y, b.y});
            maxY = std::max({maxY, a.y, b.y});
            if (a.y == b.y) continue;  // horizontal edges contribute no crossings
            edges.push_back({a.x, a.y, b.x, b.y});
        }
    }
}

// Standard Porter-Duff "over", tracking real destination alpha rather than
// assuming an opaque destination — needed for <canvas>'s transparent
// background (the main window framebuffer is always cleared to opaque
// first, and this reduces to exactly the old always-255 formula whenever
// dstA is already 1, so there's no behavior change for that caller).
inline void BlendPixel(uint8_t* p, PlatColor color, float alpha) {
    if (alpha <= 0.f) return;
    if (alpha > 1.f) alpha = 1.f;
    float dstA = p[3] / 255.f;
    float outA = alpha + dstA * (1.f - alpha);
    if (outA <= 0.0001f) { p[0] = p[1] = p[2] = p[3] = 0; return; }
    uint8_t srcB = (uint8_t)(color.b * 255.f + 0.5f);
    uint8_t srcG = (uint8_t)(color.g * 255.f + 0.5f);
    uint8_t srcR = (uint8_t)(color.r * 255.f + 0.5f);
    float ia = 1.f - alpha;
    p[0] = (uint8_t)((srcB * alpha + p[0] * dstA * ia) / outA + 0.5f);
    p[1] = (uint8_t)((srcG * alpha + p[1] * dstA * ia) / outA + 0.5f);
    p[2] = (uint8_t)((srcR * alpha + p[2] * dstA * ia) / outA + 0.5f);
    p[3] = (uint8_t)(outA * 255.f + 0.5f);
}

// Adds `weight` coverage to every pixel fully spanned by [x0,x1), and the
// exact fractional amount to the two (possibly identical) boundary pixels —
// exact analytic horizontal anti-aliasing for one subsample scanline.
void AddSpanCoverage(std::vector<float>& coverage, float x0, float x1, int ix0, int ix1, float weight) {
    x0 = std::max(x0, (float)ix0);
    x1 = std::min(x1, (float)ix1);
    if (x1 <= x0) return;
    int p0 = (int)std::floor(x0);
    int p1 = (int)std::floor(x1);
    if (p0 == p1) {
        if (p0 >= ix0 && p0 < ix1) coverage[p0 - ix0] += (x1 - x0) * weight;
        return;
    }
    if (p0 >= ix0 && p0 < ix1) coverage[p0 - ix0] += (p0 + 1 - x0) * weight;
    for (int x = p0 + 1; x < p1; x++) {
        if (x >= ix0 && x < ix1) coverage[x - ix0] += weight;
    }
    if (p1 >= ix0 && p1 < ix1) coverage[p1 - ix0] += (x1 - p1) * weight;
}

void AddArc(Contour& c, float cx, float cy, float r, float startAngle, float endAngle, int segments) {
    for (int i = 0; i <= segments; i++) {
        float t = startAngle + (endAngle - startAngle) * (float)i / (float)segments;
        c.push_back({cx + r * std::cos(t), cy + r * std::sin(t)});
    }
}

}  // namespace

void Framebuffer::Resize(int w, int h) {
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (w == width && h == height) return;
    width = w;
    height = h;
    pixels.assign((size_t)w * (size_t)h * 4, 0);
}

void FillPath(Framebuffer& fb, const std::vector<Contour>& contours, PlatColor color, const ClipRect& clip) {
    if (contours.empty() || color.a <= 0.f) return;
    std::vector<Edge> edges;
    float minX, minY, maxX, maxY;
    BuildEdges(contours, edges, minX, minY, maxX, maxY);
    if (edges.empty()) return;

    int ix0 = std::max(clip.x0, (int)std::floor(minX));
    int iy0 = std::max(clip.y0, (int)std::floor(minY));
    int ix1 = std::min(clip.x1, (int)std::ceil(maxX));
    int iy1 = std::min(clip.y1, (int)std::ceil(maxY));
    ix0 = std::max(ix0, 0);
    iy0 = std::max(iy0, 0);
    ix1 = std::min(ix1, fb.width);
    iy1 = std::min(iy1, fb.height);
    if (ix1 <= ix0 || iy1 <= iy0) return;

    std::vector<float> coverage((size_t)(ix1 - ix0));
    std::vector<std::pair<float, int>> crossings;

    for (int y = iy0; y < iy1; y++) {
        std::fill(coverage.begin(), coverage.end(), 0.f);
        for (int s = 0; s < kSubsamples; s++) {
            float sampleY = (float)y + ((float)s + 0.5f) / (float)kSubsamples;
            crossings.clear();
            for (const auto& e : edges) {
                float ylo = std::min(e.y0, e.y1), yhi = std::max(e.y0, e.y1);
                if (sampleY < ylo || sampleY >= yhi) continue;
                float t = (sampleY - e.y0) / (e.y1 - e.y0);
                float x = e.x0 + t * (e.x1 - e.x0);
                crossings.push_back({x, e.y1 > e.y0 ? 1 : -1});
            }
            if (crossings.empty()) continue;
            std::sort(crossings.begin(), crossings.end(),
                      [](const std::pair<float, int>& a, const std::pair<float, int>& b) { return a.first < b.first; });
            int winding = 0;
            float spanStart = 0.f;
            for (const auto& cr : crossings) {
                int prev = winding;
                winding += cr.second;
                if (prev == 0 && winding != 0) {
                    spanStart = cr.first;
                } else if (prev != 0 && winding == 0) {
                    AddSpanCoverage(coverage, spanStart, cr.first, ix0, ix1, 1.f / (float)kSubsamples);
                }
            }
        }
        uint8_t* row = fb.Row(y);
        for (int x = ix0; x < ix1; x++) {
            float cov = coverage[(size_t)(x - ix0)];
            if (cov <= 0.f) continue;
            BlendPixel(row + x * 4, color, cov * color.a);
        }
    }
}

void ClearRect(Framebuffer& fb, float x, float y, float w, float h, const ClipRect& clip) {
    if (w <= 0.f || h <= 0.f) return;
    int ix0 = std::max({clip.x0, 0, (int)std::floor(x)});
    int iy0 = std::max({clip.y0, 0, (int)std::floor(y)});
    int ix1 = std::min({clip.x1, fb.width, (int)std::ceil(x + w)});
    int iy1 = std::min({clip.y1, fb.height, (int)std::ceil(y + h)});
    if (ix1 <= ix0 || iy1 <= iy0) return;
    for (int py = iy0; py < iy1; py++) {
        std::memset(fb.Row(py) + (size_t)ix0 * 4, 0, (size_t)(ix1 - ix0) * 4);
    }
}

void FillRect(Framebuffer& fb, float x, float y, float w, float h, PlatColor color, const ClipRect& clip) {
    if (w <= 0.f || h <= 0.f) return;
    std::vector<Contour> contours(1);
    contours[0] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    FillPath(fb, contours, color, clip);
}

void FillRoundedRect(Framebuffer& fb, float x, float y, float w, float h, float radius, PlatColor color, const ClipRect& clip) {
    if (w <= 0.f || h <= 0.f) return;
    float r = std::min(radius, std::min(w, h) * 0.5f);
    if (r <= 0.01f) {
        FillRect(fb, x, y, w, h, color, clip);
        return;
    }
    const int segs = 8;
    Contour c;
    AddArc(c, x + r, y + r, r, kPi, kPi * 1.5f, segs);              // top-left
    AddArc(c, x + w - r, y + r, r, kPi * 1.5f, kPi * 2.f, segs);    // top-right
    AddArc(c, x + w - r, y + h - r, r, 0.f, kPi * 0.5f, segs);      // bottom-right
    AddArc(c, x + r, y + h - r, r, kPi * 0.5f, kPi, segs);          // bottom-left
    std::vector<Contour> contours{c};
    FillPath(fb, contours, color, clip);
}

void StrokeRect(Framebuffer& fb, float x, float y, float w, float h, PlatColor color, float strokeWidth, const ClipRect& clip) {
    if (strokeWidth <= 0.f || w <= 0.f || h <= 0.f) return;
    float half = strokeWidth * 0.5f;
    FillRect(fb, x, y - half, w, strokeWidth, color, clip);           // top
    FillRect(fb, x, y + h - half, w, strokeWidth, color, clip);       // bottom
    FillRect(fb, x - half, y, strokeWidth, h, color, clip);           // left
    FillRect(fb, x + w - half, y, strokeWidth, h, color, clip);       // right
}

void StrokeLine(Framebuffer& fb, float x0, float y0, float x1, float y1, PlatColor color, float strokeWidth, const ClipRect& clip) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6f || strokeWidth <= 0.f) return;
    float nx = -dy / len * strokeWidth * 0.5f;
    float ny = dx / len * strokeWidth * 0.5f;
    std::vector<Contour> contours(1);
    contours[0] = {{x0 + nx, y0 + ny}, {x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}, {x0 - nx, y0 - ny}};
    FillPath(fb, contours, color, clip);
}

void BlitBitmap(Framebuffer& fb, const uint8_t* src, int srcW, int srcH,
                 float destX, float destY, float destW, float destH,
                 const ClipRect& clip, float globalAlpha) {
    if (!src || srcW <= 0 || srcH <= 0 || destW <= 0.f || destH <= 0.f) return;
    int ix0 = std::max({clip.x0, 0, (int)std::floor(destX)});
    int iy0 = std::max({clip.y0, 0, (int)std::floor(destY)});
    int ix1 = std::min({clip.x1, fb.width, (int)std::ceil(destX + destW)});
    int iy1 = std::min({clip.y1, fb.height, (int)std::ceil(destY + destH)});
    if (ix1 <= ix0 || iy1 <= iy0) return;

    for (int y = iy0; y < iy1; y++) {
        int sy = (int)(((float)y + 0.5f - destY) / destH * (float)srcH);
        if (sy < 0) sy = 0;
        if (sy >= srcH) sy = srcH - 1;
        const uint8_t* srow = src + (size_t)sy * (size_t)srcW * 4;
        uint8_t* drow = fb.Row(y);
        for (int x = ix0; x < ix1; x++) {
            int sx = (int)(((float)x + 0.5f - destX) / destW * (float)srcW);
            if (sx < 0) sx = 0;
            if (sx >= srcW) sx = srcW - 1;
            const uint8_t* sp = srow + sx * 4;
            if (sp[3] == 0 && globalAlpha >= 1.f) continue;
            // sp is premultiplied, so only alpha (not the color channels) needs
            // the extra globalAlpha factor spread across both terms of the blend.
            float alpha = (sp[3] / 255.f) * globalAlpha;
            if (alpha > 1.f) alpha = 1.f;
            uint8_t* dp = drow + x * 4;
            float dstA = dp[3] / 255.f;
            float outA = alpha + dstA * (1.f - alpha);
            if (outA <= 0.0001f) { dp[0] = dp[1] = dp[2] = dp[3] = 0; continue; }
            float ia = 1.f - alpha;
            dp[0] = (uint8_t)((sp[0] * globalAlpha + dp[0] * dstA * ia) / outA + 0.5f);
            dp[1] = (uint8_t)((sp[1] * globalAlpha + dp[1] * dstA * ia) / outA + 0.5f);
            dp[2] = (uint8_t)((sp[2] * globalAlpha + dp[2] * dstA * ia) / outA + 0.5f);
            dp[3] = (uint8_t)(outA * 255.f + 0.5f);
        }
    }
}

void BlitAlphaMask(Framebuffer& fb, const uint8_t* mask, int maskW, int maskH, int maskStride,
                    int destX, int destY, PlatColor color, const ClipRect& clip) {
    if (!mask || maskW <= 0 || maskH <= 0) return;
    int ix0 = std::max({clip.x0, 0, destX});
    int iy0 = std::max({clip.y0, 0, destY});
    int ix1 = std::min({clip.x1, fb.width, destX + maskW});
    int iy1 = std::min({clip.y1, fb.height, destY + maskH});
    if (ix1 <= ix0 || iy1 <= iy0) return;

    uint8_t srcB = (uint8_t)(color.b * 255.f + 0.5f);
    uint8_t srcG = (uint8_t)(color.g * 255.f + 0.5f);
    uint8_t srcR = (uint8_t)(color.r * 255.f + 0.5f);
    for (int y = iy0; y < iy1; y++) {
        const uint8_t* mrow = mask + (size_t)(y - destY) * (size_t)maskStride;
        uint8_t* drow = fb.Row(y);
        for (int x = ix0; x < ix1; x++) {
            float a = (mrow[x - destX] / 255.f) * color.a;
            if (a <= 0.f) continue;
            if (a > 1.f) a = 1.f;
            uint8_t* dp = drow + x * 4;
            float dstA = dp[3] / 255.f;
            float outA = a + dstA * (1.f - a);
            if (outA <= 0.0001f) { dp[0] = dp[1] = dp[2] = dp[3] = 0; continue; }
            float ia = 1.f - a;
            dp[0] = (uint8_t)((srcB * a + dp[0] * dstA * ia) / outA + 0.5f);
            dp[1] = (uint8_t)((srcG * a + dp[1] * dstA * ia) / outA + 0.5f);
            dp[2] = (uint8_t)((srcR * a + dp[2] * dstA * ia) / outA + 0.5f);
            dp[3] = (uint8_t)(outA * 255.f + 0.5f);
        }
    }
}

}  // namespace raster
