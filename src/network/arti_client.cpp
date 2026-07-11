#include "network/arti_client.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
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
};

ArtiLib& LoadArti() {
    static ArtiLib lib;
    static bool tried = false;
    if (tried) return lib;
    tried = true;
    const char* overridePath = std::getenv("VERTEX_ARTI_LIB");
#ifdef _WIN32
    HMODULE h = nullptr;
    if (overridePath && *overridePath) h = LoadLibraryA(overridePath);
    if (!h) h = LoadLibraryA("vertex_arti.dll");
    lib.handle = (void*)h;
    if (h) {
        lib.fetch = (FetchFn)GetProcAddress(h, "vertex_arti_fetch");
        lib.freeResp = (FreeFn)GetProcAddress(h, "vertex_arti_free_response");
    }
#else
    void* h = nullptr;
    if (overridePath && *overridePath) h = dlopen(overridePath, RTLD_NOW);
#ifdef __APPLE__
    if (!h) h = dlopen("libvertex_arti.dylib", RTLD_NOW);
#else
    if (!h) h = dlopen("libvertex_arti.so", RTLD_NOW);
#endif
    lib.handle = h;
    if (h) {
        lib.fetch = (FetchFn)dlsym(h, "vertex_arti_fetch");
        lib.freeResp = (FreeFn)dlsym(h, "vertex_arti_free_response");
    }
#endif
    return lib;
}

} // namespace

bool IsOnionUrl(const std::string& url) {
    return HostEndsWithOnion(url);
}

bool FetchViaEmbeddedArti(const std::string& url, size_t maxResponseBytes, FetchResult& out) {
    if (!IsOnionUrl(url)) return false;
    ArtiLib& lib = LoadArti();
    if (!lib.fetch || !lib.freeResp) return false;
    VertexArtiResponse* resp = lib.fetch(url.c_str(), maxResponseBytes);
    if (!resp) return false;
    out.status = resp->status;
    out.success = resp->success != 0;
    out.contentType = resp->content_type ? resp->content_type : "";
    out.finalUrl = resp->final_url ? resp->final_url : url;
    out.error = resp->error ? resp->error : "";
    if (resp->body && resp->body_len)
        out.body.assign((const char*)resp->body, resp->body_len);
    lib.freeResp(resp);
    return true;
}
