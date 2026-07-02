#pragma once

#include "network/fetcher.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

enum class ResourceKind {
    Document,
    Stylesheet,
    Script,
    Image,
    Font,
    Other
};

struct ResourceCacheStats {
    uint64_t requests = 0;
    uint64_t networkFetches = 0;
    uint64_t cacheHits = 0;
    uint64_t bytesFetched = 0;
    double fetchMs = 0.0;

    uint64_t documents = 0;
    uint64_t stylesheets = 0;
    uint64_t scripts = 0;
    uint64_t images = 0;
    uint64_t fonts = 0;
    uint64_t other = 0;
};

class ResourceCache {
public:
    static ResourceCache& instance();

    FetchResult fetch(const std::string& url,
                      size_t maxResponseBytes = 12 * 1024 * 1024,
                      ResourceKind kind = ResourceKind::Other);

    ResourceCacheStats stats() const;
    void clearForTests();

private:
    ResourceCache() = default;
};

using ResourceCallback = std::function<void(FetchResult)>;

FetchResult FetchResourceCached(const std::string& url,
                                size_t maxResponseBytes = 12 * 1024 * 1024,
                                ResourceKind kind = ResourceKind::Other);

void FetchResourceAsync(const std::string& url,
                        size_t maxResponseBytes,
                        ResourceKind kind,
                        ResourceCallback callback);

size_t DrainResourceCompletions(size_t maxCallbacks = 64);
bool HasPendingResourceCompletions();
