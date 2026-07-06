#pragma once
//
// linux_font_registry.h — lets webfont.h hand a freshly-downloaded
// @font-face file straight to platform_linux.cpp's font index, now that
// fontconfig (which used to serve this exact purpose via
// FcConfigAppFontAddFile) is gone from the Linux build.
//
#ifdef __linux__
#include <string>

// Parses `path` and adds it to the in-process font index immediately, so
// the very next CreateFont() call for its family can find it — no
// rescanning of the system font directories needed.
void RegisterLinuxWebFont(const std::string& path);

#endif  // __linux__
