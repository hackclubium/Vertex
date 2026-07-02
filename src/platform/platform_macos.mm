#ifdef __APPLE__
//
// platform_macos.mm — macOS backend: Cocoa + Core Graphics + Core Text.
//
// This is an Objective-C++ file (.mm) so it can use both C++ and Cocoa APIs.
// Build with: clang++ -framework Cocoa -framework CoreGraphics -framework CoreText
//
#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#include "platform/platform.h"
#include <map>

// ── helpers ──────────────────────────────────────────────────────────────────

static NSString* ToNS(const std::string& s) {
    return [NSString stringWithUTF8String:s.c_str()];
}

static std::string FromNS(NSString* s) {
    return s ? std::string([s UTF8String]) : "";
}

// ── Core Graphics Renderer ───────────────────────────────────────────────────

class MacRenderer : public IPlatformRenderer {
public:
    ~MacRenderer() override { /* CGContext is managed by NSView, not us */ }

    bool Init(void* nativeWindow) override {
        m_view = (__bridge NSView*)nativeWindow;
        NSRect frame = [m_view bounds];
        m_width = (int)frame.size.width;
        m_height = (int)frame.size.height;
        return m_view != nil;
    }

    void Resize(int width, int height) override {
        m_width = width; m_height = height;
    }

    void BeginFrame() override {
        m_ctx = (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
        if (!m_ctx) return;
        // Core Graphics has origin at bottom-left; flip to top-left.
        CGContextSaveGState(m_ctx);
        CGContextTranslateCTM(m_ctx, 0, m_height);
        CGContextScaleCTM(m_ctx, 1, -1);
    }
    void EndFrame() override {
        if (m_ctx) CGContextRestoreGState(m_ctx);
        m_ctx = nullptr;
    }
    void Clear(PlatColor color) override {
        if (!m_ctx) return;
        CGContextSetRGBFillColor(m_ctx, color.r, color.g, color.b, color.a);
        CGContextFillRect(m_ctx, CGRectMake(0, 0, m_width, m_height));
    }

    void FillRect(float x, float y, float w, float h, PlatColor c) override {
        if (!m_ctx) return;
        CGContextSetRGBFillColor(m_ctx, c.r, c.g, c.b, c.a);
        CGContextFillRect(m_ctx, CGRectMake(x, y, w, h));
    }
    void DrawRect(float x, float y, float w, float h, PlatColor c, float sw) override {
        if (!m_ctx) return;
        CGContextSetRGBStrokeColor(m_ctx, c.r, c.g, c.b, c.a);
        CGContextSetLineWidth(m_ctx, sw);
        CGContextStrokeRect(m_ctx, CGRectMake(x, y, w, h));
    }
    void FillRoundedRect(float x, float y, float w, float h, float r, PlatColor c) override {
        if (!m_ctx) return;
        CGContextSetRGBFillColor(m_ctx, c.r, c.g, c.b, c.a);
        NSBezierPath* path = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(x, y, w, h)
                                                             xRadius:r yRadius:r];
        CGContextBeginPath(m_ctx);
        // Convert NSBezierPath to CGPath manually
        CGMutablePathRef cgPath = CGPathCreateMutable();
        for (NSInteger i = 0; i < [path elementCount]; i++) {
            NSPoint pts[3];
            switch ([path elementAtIndex:i associatedPoints:pts]) {
                case NSBezierPathElementMoveTo:    CGPathMoveToPoint(cgPath, NULL, pts[0].x, pts[0].y); break;
                case NSBezierPathElementLineTo:    CGPathAddLineToPoint(cgPath, NULL, pts[0].x, pts[0].y); break;
                case NSBezierPathElementCubicCurveTo: CGPathAddCurveToPoint(cgPath, NULL, pts[0].x, pts[0].y,
                                                       pts[1].x, pts[1].y, pts[2].x, pts[2].y); break;
                default: break;
                case NSBezierPathElementClosePath: CGPathCloseSubpath(cgPath); break;
            }
        }
        CGContextAddPath(m_ctx, cgPath);
        CGContextFillPath(m_ctx);
        CGPathRelease(cgPath);
    }
    void DrawLine(float x1, float y1, float x2, float y2, PlatColor c, float sw) override {
        if (!m_ctx) return;
        CGContextSetRGBStrokeColor(m_ctx, c.r, c.g, c.b, c.a);
        CGContextSetLineWidth(m_ctx, sw);
        CGContextMoveToPoint(m_ctx, x1, y1);
        CGContextAddLineToPoint(m_ctx, x2, y2);
        CGContextStrokePath(m_ctx);
    }

    void PushClip(float x, float y, float w, float h) override {
        if (!m_ctx) return;
        CGContextSaveGState(m_ctx);
        CGContextClipToRect(m_ctx, CGRectMake(x, y, w, h));
    }
    void PopClip() override { if (m_ctx) CGContextRestoreGState(m_ctx); }

    PlatFont CreateFont(float size, bool bold, bool italic, bool mono, const std::string& family) override {
        NSString* fam = family.empty()
            ? (mono ? @"Menlo" : @"Helvetica Neue")
            : ToNS(family);
        NSFontTraitMask traits = 0;
        if (bold) traits |= NSBoldFontMask;
        if (italic) traits |= NSItalicFontMask;
        NSFont* font = [[NSFontManager sharedFontManager] fontWithFamily:fam
                                                                  traits:traits
                                                                  weight:bold ? 9 : 5
                                                                    size:size];
        if (!font) font = [NSFont systemFontOfSize:size];
        CTFontRef ct = (__bridge CTFontRef)font;
        CFRetain(ct);
        return (PlatFont)ct;
    }
    void ReleaseFont(PlatFont font) override {
        if (font) CFRelease((CTFontRef)font);
    }
    float MeasureText(const std::wstring& text, PlatFont font) override {
        if (!font || text.empty()) return 0;
        CTFontRef ct = (CTFontRef)font;
        NSString* str = [[NSString alloc] initWithBytes:text.data()
                                                 length:text.size() * sizeof(wchar_t)
                                               encoding:NSUTF32LittleEndianStringEncoding];
        NSDictionary* attrs = @{ (id)kCTFontAttributeName: (__bridge id)ct };
        NSAttributedString* as = [[NSAttributedString alloc] initWithString:str attributes:attrs];
        CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)as);
        CGFloat width = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
        CFRelease(line);
        return (float)width;
    }
    float SpaceWidth(PlatFont font) override { return MeasureText(L" ", font); }
    float FontHeight(PlatFont font) override {
        if (!font) return 16.f;
        CTFontRef ct = (CTFontRef)font;
        return (float)(CTFontGetAscent(ct) + CTFontGetDescent(ct) + CTFontGetLeading(ct));
    }
    void DrawText(const std::wstring& text, float x, float y, float maxW, float maxH,
                  PlatFont font, PlatColor color, bool underline) override {
        if (!m_ctx || !font || text.empty()) return;
        CTFontRef ct = (CTFontRef)font;
        NSString* str = [[NSString alloc] initWithBytes:text.data()
                                                 length:text.size() * sizeof(wchar_t)
                                               encoding:NSUTF32LittleEndianStringEncoding];
        CGColorRef cgColor = CGColorCreateSRGB(color.r, color.g, color.b, color.a);
        NSMutableDictionary* attrs = [NSMutableDictionary dictionaryWithObjectsAndKeys:
            (__bridge id)ct, (id)kCTFontAttributeName,
            (__bridge id)cgColor, (id)kCTForegroundColorAttributeName,
            nil];
        if (underline)
            [attrs setObject:@(kCTUnderlineStyleSingle) forKey:(id)kCTUnderlineStyleAttributeName];
        NSAttributedString* as = [[NSAttributedString alloc] initWithString:str attributes:attrs];
        CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)as);
        // Core Text draws with CG's bottom-up coords; we've already flipped in BeginFrame,
        // but CTLineDraw expects the baseline, so adjust.
        CGFloat ascent = CTFontGetAscent(ct);
        CGContextSetTextPosition(m_ctx, x, y + ascent);
        CTLineDraw(line, m_ctx);
        CFRelease(line);
        CGColorRelease(cgColor);
    }

    PlatBitmap CreateBitmap(int width, int height, const uint8_t* rgbaPixels) override {
        if (!rgbaPixels || width <= 0 || height <= 0) return nullptr;
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef bc = CGBitmapContextCreate(
            (void*)rgbaPixels, width, height, 8, width * 4, cs,
            kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGImageRef img = CGBitmapContextCreateImage(bc);
        CGContextRelease(bc);
        CGColorSpaceRelease(cs);
        return (PlatBitmap)img;
    }
    void ReleaseBitmap(PlatBitmap bmp) override {
        if (bmp) CGImageRelease((CGImageRef)bmp);
    }
    void DrawBitmap(PlatBitmap bmp, float x, float y, float w, float h) override {
        if (!m_ctx || !bmp) return;
        CGContextDrawImage(m_ctx, CGRectMake(x, y, w, h), (CGImageRef)bmp);
    }
    void DrawBitmapTiled(PlatBitmap bmp, float destX, float destY, float destW, float destH,
                         float tileW, float tileH, float offsetX, float offsetY,
                         bool repeatX, bool repeatY) override {
        if (!m_ctx || !bmp) return;
        PushClip(destX, destY, destW, destH);
        float startX = destX + offsetX, startY = destY + offsetY;
        if (repeatX) while (startX > destX) startX -= tileW;
        if (repeatY) while (startY > destY) startY -= tileH;
        if (tileW < 1.f) tileW = 1.f;
        if (tileH < 1.f) tileH = 1.f;
        for (float ty = startY; ty < destY + destH; ty += tileH) {
            for (float tx = startX; tx < destX + destW; tx += tileW) {
                DrawBitmap(bmp, tx, ty, tileW, tileH);
                if (!repeatX) break;
            }
            if (!repeatY) break;
        }
        PopClip();
    }

    int Width() const override { return m_width; }
    int Height() const override { return m_height; }

private:
    NSView* m_view = nil;
    CGContextRef m_ctx = nullptr;
    int m_width = 800, m_height = 600;
};

std::unique_ptr<IPlatformRenderer> CreatePlatformRenderer() {
    return std::make_unique<MacRenderer>();
}

#endif // __APPLE__
