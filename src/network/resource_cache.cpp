#include "network/resource_cache.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct CachedResource {
    FetchResult result;
};

struct PendingCompletion {
    ResourceCallback callback;
    FetchResult result;
};

struct AsyncRequest {
    std::string url;
    size_t maxResponseBytes = 0;
    ResourceKind kind = ResourceKind::Other;
    std::vector<ResourceCallback> waiters;
};

std::mutex g_cacheMutex;
std::unordered_map<std::string, CachedResource> g_cache;
ResourceCacheStats g_stats;

std::mutex g_asyncMutex;
std::condition_variable g_workerCv;
std::deque<std::string> g_asyncJobs;
std::unordered_map<std::string, AsyncRequest> g_inflight;
std::vector<std::thread> g_workerThreads;
bool g_stopWorkers = false;
bool g_workersStarted = false;

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

std::string InflightKey(const std::string& url, size_t maxResponseBytes) {
    return url + "\n" + std::to_string(maxResponseBytes);
}

void PushCompletion(ResourceCallback callback, FetchResult result) {
    std::lock_guard<std::mutex> lock(g_completionMutex);
    g_completions.push_back(PendingCompletion{ std::move(callback), std::move(result) });
}

void ResourceWorkerLoop() {
    for (;;) {
        std::string key;
        AsyncRequest request;
        {
            std::unique_lock<std::mutex> lock(g_asyncMutex);
            g_workerCv.wait(lock, [] {
                return g_stopWorkers || !g_asyncJobs.empty();
            });
            if (g_stopWorkers && g_asyncJobs.empty()) return;
            key = std::move(g_asyncJobs.front());
            g_asyncJobs.pop_front();
            auto it = g_inflight.find(key);
            if (it == g_inflight.end()) continue;
            request.url = it->second.url;
            request.maxResponseBytes = it->second.maxResponseBytes;
            request.kind = it->second.kind;
        }

        FetchResult result = FetchResourceCached(request.url, request.maxResponseBytes, request.kind);

        {
            std::lock_guard<std::mutex> lock(g_asyncMutex);
            auto it = g_inflight.find(key);
            if (it != g_inflight.end()) {
                request.waiters = std::move(it->second.waiters);
                g_inflight.erase(it);
            }
        }

        for (auto& waiter : request.waiters)
            PushCompletion(std::move(waiter), result);
    }
}

void StopResourceWorkers() {
    {
        std::lock_guard<std::mutex> lock(g_asyncMutex);
        g_stopWorkers = true;
    }
    g_workerCv.notify_all();
    for (auto& worker : g_workerThreads) {
        if (worker.joinable()) worker.join();
    }
}

void EnsureResourceWorkersLocked() {
    if (g_workersStarted) return;
    g_workersStarted = true;
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned count = std::max(2u, std::min(hw ? hw : 4u, 6u));
    g_workerThreads.reserve(count);
    for (unsigned i = 0; i < count; ++i)
        g_workerThreads.emplace_back(ResourceWorkerLoop);
    std::atexit(StopResourceWorkers);
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
        std::lock_guard<std::mutex> lock(g_asyncMutex);
        g_asyncJobs.clear();
        g_inflight.clear();
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
    std::lock_guard<std::mutex> lock(g_asyncMutex);
    EnsureResourceWorkersLocked();
    const std::string key = InflightKey(url, maxResponseBytes);
    auto it = g_inflight.find(key);
    if (it != g_inflight.end()) {
        it->second.waiters.push_back(std::move(callback));
        return;
    }
    AsyncRequest request;
    request.url = url;
    request.maxResponseBytes = maxResponseBytes;
    request.kind = kind;
    request.waiters.push_back(std::move(callback));
    g_inflight.emplace(key, std::move(request));
    g_asyncJobs.push_back(key);
    g_workerCv.notify_one();
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
