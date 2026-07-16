#include "network/arti_client.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <limits.h>
#else
#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>
#endif

namespace {

struct VertexArtiResponse {
    int success;
    int status;
    char* content_type;
    char* final_url;
    char* error;
    unsigned char* body;
    size_t body_len;
};

using FetchFn = VertexArtiResponse* (*)(const char*, size_t);
using FreeFn = void (*)(VertexArtiResponse*);

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool HostEndsWithOnion(const std::string& url) {
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) return false;
    size_t hostStart = scheme + 3;
    size_t hostEnd = url.find_first_of("/:?#", hostStart);
    std::string host = Lower(url.substr(hostStart, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart));
    return host.size() > 6 && host.compare(host.size() - 6, 6, ".onion") == 0;
}

struct ArtiLib {
    void* handle = nullptr;
    FetchFn fetch = nullptr;
    FreeFn freeResp = nullptr;
    std::string error;
};

bool DebugArti() {
    const char* env = std::getenv("VERTEX_ARTI_DEBUG");
    return env && *env && std::string(env) != "0";
}

void ArtiLog(const std::string& msg) {
    if (DebugArti()) std::fprintf(stderr, "[Arti] %s\n", msg.c_str());
}

std::string Dirname(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string ExecutableDir() {
#ifdef _WIN32
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    return n ? Dirname(std::string(path, n)) : std::string();
#elif defined(__APPLE__)
    char path[PATH_MAX] = {};
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return {};
    char resolved[PATH_MAX] = {};
    return realpath(path, resolved) ? Dirname(resolved) : Dirname(path);
#else
    char path[PATH_MAX] = {};
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    return n > 0 ? Dirname(std::string(path, (size_t)n)) : std::string();
#endif
}

std::string DefaultArtiPath() {
#ifdef _WIN32
    return ExecutableDir() + "vertex_arti.dll";
#elif defined(__APPLE__)
    return ExecutableDir() + "libvertex_arti.dylib";
#else
    return ExecutableDir() + "libvertex_arti.so";
#endif
}

ArtiLib& LoadArti() {
    static ArtiLib lib;
    static bool tried = false;
    if (tried) return lib;
    tried = true;
    const char* overridePath = std::getenv("VERTEX_ARTI_LIB");
#ifdef _WIN32
    HMODULE h = nullptr;
    if (overridePath && *overridePath) h = LoadLibraryExA(overridePath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        std::string path = DefaultArtiPath();
        ArtiLog("loading " + path);
        h = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    lib.handle = (void*)h;
    if (h) {
        lib.fetch = (FetchFn)GetProcAddress(h, "vertex_arti_fetch");
        lib.freeResp = (FreeFn)GetProcAddress(h, "vertex_arti_free_response");
    } else {
        lib.error = "LoadLibrary failed: " + std::to_string(GetLastError());
    }
#else
    void* h = nullptr;
    if (overridePath && *overridePath) h = dlopen(overridePath, RTLD_NOW);
    if (!h) {
        std::string path = DefaultArtiPath();
        ArtiLog("loading " + path);
        h = dlopen(path.c_str(), RTLD_NOW);
    }
    lib.handle = h;
    if (h) {
        lib.fetch = (FetchFn)dlsym(h, "vertex_arti_fetch");
        lib.freeResp = (FreeFn)dlsym(h, "vertex_arti_free_response");
    } else {
        const char* err = dlerror();
        lib.error = err ? err : "dlopen failed";
    }
#endif
    if (!lib.fetch || !lib.freeResp) {
        if (lib.error.empty()) lib.error = "vertex_arti exports missing";
        ArtiLog(lib.error);
    } else {
        ArtiLog("loaded bridge");
    }
    return lib;
}

} // namespace

bool IsOnionUrl(const std::string& url) {
    return HostEndsWithOnion(url);
}

bool FetchViaEmbeddedArti(const std::string& url, size_t maxResponseBytes, FetchResult& out) {
    if (!IsOnionUrl(url)) return false;
    ArtiLib& lib = LoadArti();
    if (!lib.fetch || !lib.freeResp) {
        out.error = lib.error;
        return false;
    }
    VertexArtiResponse* resp = lib.fetch(url.c_str(), maxResponseBytes);
    if (!resp) return false;
    out.status = resp->status;
    out.success = resp->success != 0;
    out.contentType = resp->content_type ? resp->content_type : "";
    out.finalUrl = resp->final_url ? resp->final_url : url;
    out.error = resp->error ? resp->error : "";
    if (resp->body && resp->body_len)
        out.body.assign((const char*)resp->body, resp->body_len);
    if (!out.success) ArtiLog("fetch failed: " + out.error);
    lib.freeResp(resp);
    return true;
}
