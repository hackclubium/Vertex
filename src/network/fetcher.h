#pragma once
#include <cstddef>
#include <string>

struct FetchResult {
    bool        success = false;
    int         status  = 0;       // HTTP status code (0 if not applicable)
    std::string finalUrl;
    std::string body;
    std::string contentType;
    std::string contentDisposition;
    std::string error;
};

// Fetch a URL over HTTP or HTTPS using WinINet.
// Follows redirects automatically.
FetchResult FetchUrl(const std::string& url,
                     size_t maxResponseBytes = 12 * 1024 * 1024);

// Thread-safe, idempotent libcurl global init (curl_global_init() itself is
// documented as unsafe to call concurrently the first time). Every
// translation unit that touches curl directly — fetcher.cpp and
// network/websocket.cpp — must call this (not roll its own guard) so they
// all share the one std::once_flag inside it.
void EnsureCurlInit();
