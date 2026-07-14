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

struct FetchRequest {
    std::string url;
    std::string method = "GET";
    std::string body;
    std::string contentType;
};

// Fetch a URL: data:/file: are handled locally; http:/https: use the
// hand-rolled HTTP client (http_client.h). Redirects are followed
// automatically.
FetchResult FetchUrl(const std::string& url,
                     size_t maxResponseBytes = 12 * 1024 * 1024);
FetchResult FetchUrl(const FetchRequest& request,
                     size_t maxResponseBytes = 12 * 1024 * 1024);
