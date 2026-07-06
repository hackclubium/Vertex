#pragma once
//
// image.h — the common decoded-image result shared by every from-scratch
// image codec (png.h, jpeg.h, ...), so callers don't care which decoder
// actually produced a given bitmap.
//
#include <cstdint>
#include <vector>

struct DecodedImage {
    bool success = false;
    int width = 0;
    int height = 0;
    // width*height*4 bytes, row-major top-to-bottom, straight (not
    // premultiplied) RGBA — same convention stb_image's callers already
    // expect, so this is a drop-in replacement at the call site.
    std::vector<uint8_t> rgba;
};
