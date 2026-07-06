#pragma once
//
// raster_bitmap.h — the PlatBitmap backing store shared by platform_linux.cpp
// (IPlatformRenderer's CreateBitmap/DrawBitmap, for decoded <img> content)
// and canvas_raster.cpp (<canvas> drawImage()'s image lookup) — both need to
// agree on the same type so a decoded image can be drawn by either.
//
#include <cstdint>
#include <vector>

// Premultiplied-alpha BGRA8888, straight copy of what main_linux.cpp's image
// pipeline already prepares before calling CreateBitmap (see ProcessImage's
// swizzle step) — the same layout rasterizer.h's BlitBitmap expects.
struct RasterBitmap {
    int width = 0, height = 0;
    std::vector<uint8_t> bgra;
};
