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
