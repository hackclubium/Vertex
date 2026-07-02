#include "network/resource_cache.h"

#include <chrono>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace {

struct CachedResource {
    FetchResult result;
};

struct PendingCompletion {
    ResourceCallback callback;
    FetchResult result;
};

std::mutex g_cacheMutex;
std::unordered_map<std::string, CachedResource> g_cache;
ResourceCacheStats g_stats;

std::mutex g_completionMutex;
std::deque<PendingCompletion> g_completions;
std::atomic<size_t> g_pendingAsync{0};

void CountKind(ResourceKind kind) {
    switch (kind) {
    case ResourceKind::Document:   ++g_stats.documents; break;
    case ResourceKind::Stylesheet: ++g_stats.stylesheets; break;
    case ResourceKind::Script:     ++g_stats.scripts; break;
    case ResourceKind::Image:      ++g_stats.images; break;
    case ResourceKind::Font:       ++g_stats.fonts; break;
    case ResourceKind::Other:      ++g_stats.other; break;
    }
}

FetchResult TooLargeFromCache(const std::string& url, size_t maxResponseBytes) {
    FetchResult result;
    result.finalUrl = url;
    result.error = "Cached resource exceeds size limit";
    result.status = 0;
    if (maxResponseBytes == 0)
        result.error = "Response exceeds size limit";
    return result;
}

} // namespace

ResourceCache& ResourceCache::instance() {
    static ResourceCache cache;
    return cache;
}

FetchResult ResourceCache::fetch(const std::string& url,
                                 size_t maxResponseBytes,
                                 ResourceKind kind) {
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        ++g_stats.requests;
        CountKind(kind);
        auto it = g_cache.find(url);
        if (it != g_cache.end()) {
            ++g_stats.cacheHits;
            if (it->second.result.body.size() > maxResponseBytes)
                return TooLargeFromCache(url, maxResponseBytes);
            return it->second.result;
        }
    }

    auto start = std::chrono::steady_clock::now();
    FetchResult result = FetchUrl(url, maxResponseBytes);
    auto end = std::chrono::steady_clock::now();
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(end - start).count();

    std::lock_guard<std::mutex> lock(g_cacheMutex);
    ++g_stats.networkFetches;
    g_stats.fetchMs += elapsedMs;
    if (result.success) {
        g_stats.bytesFetched += static_cast<uint64_t>(result.body.size());
        g_cache[url] = CachedResource{ result };
    }
    return result;
}

ResourceCacheStats ResourceCache::stats() const {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    return g_stats;
}

void ResourceCache::clearForTests() {
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_cache.clear();
        g_stats = ResourceCacheStats{};
    }
    {
        std::lock_guard<std::mutex> lock(g_completionMutex);
        g_completions.clear();
    }
    g_pendingAsync.store(0);
}

FetchResult FetchResourceCached(const std::string& url,
                                size_t maxResponseBytes,
                                ResourceKind kind) {
    return ResourceCache::instance().fetch(url, maxResponseBytes, kind);
}

void FetchResourceAsync(const std::string& url,
                        size_t maxResponseBytes,
                        ResourceKind kind,
                        ResourceCallback callback) {
    g_pendingAsync.fetch_add(1);
    std::thread([url, maxResponseBytes, kind, callback = std::move(callback)]() mutable {
        FetchResult result = FetchResourceCached(url, maxResponseBytes, kind);
        std::lock_guard<std::mutex> lock(g_completionMutex);
        g_completions.push_back(PendingCompletion{ std::move(callback), std::move(result) });
    }).detach();
}

size_t DrainResourceCompletions(size_t maxCallbacks) {
    size_t drained = 0;
    for (;;) {
        PendingCompletion next;
        {
            std::lock_guard<std::mutex> lock(g_completionMutex);
            if (g_completions.empty() || drained >= maxCallbacks) break;
            next = std::move(g_completions.front());
            g_completions.pop_front();
        }
        if (next.callback) next.callback(std::move(next.result));
        g_pendingAsync.fetch_sub(1);
        ++drained;
    }
    return drained;
}

bool HasPendingResourceCompletions() {
    return g_pendingAsync.load() > 0;
}
