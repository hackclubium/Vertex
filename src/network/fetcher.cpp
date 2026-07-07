#include "network/fetcher.h"
#include "network/http_client.h"
#include <cctype>
#include <filesystem>
#include <fstream>

// ── helpers (data-URL decoding — platform-independent, kept as-is) ───────────

static int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static std::string PercentDecode(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            int hi = HexValue(input[i + 1]);
            int lo = HexValue(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += (input[i] == '+') ? ' ' : input[i];
    }
    return out;
}

static int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + c - 'a';
    if (c >= '0' && c <= '9') return 52 + c - '0';
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static std::string Base64Decode(const std::string& input) {
    std::string out;
    int val = 0;
    int bits = -8;
    for (char c : input) {
        if (std::isspace((unsigned char)c)) continue;
        if (c == '=') break;
        int b = Base64Value(c);
        if (b < 0) continue;
        val = (val << 6) | b;
        bits += 6;
        if (bits >= 0) {
            out += (char)((val >> bits) & 0xff);
            bits -= 8;
        }
    }
    return out;
}

static bool StartsWithNoCase(const std::string& value, const char* prefix) {
    for (size_t i = 0; prefix[i]; ++i) {
        if (i >= value.size()) return false;
        if (std::tolower((unsigned char)value[i]) != std::tolower((unsigned char)prefix[i]))
            return false;
    }
    return true;
}

static std::string PercentDecodeNoPlus(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            int hi = HexValue(input[i + 1]);
            int lo = HexValue(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += input[i];
    }
    return out;
}

static std::string FileContentTypeForPath(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".css") return "text/css";
    if (ext == ".js" || ext == ".mjs") return "text/javascript";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".txt" || ext == ".log") return "text/plain";
    return "application/octet-stream";
}

static std::filesystem::path FileUrlToPath(const std::string& url) {
    std::string path = PercentDecodeNoPlus(url.substr(7));
    if (path.rfind("localhost/", 0) == 0)
        path = path.substr(9);
    while (path.size() >= 2 && path[0] == '/' && path[1] == '/')
        path.erase(path.begin());
#ifdef _WIN32
    if (path.size() >= 3 && path[0] == '/' && std::isalpha((unsigned char)path[1]) && path[2] == ':')
        path.erase(path.begin());
#endif
    return std::filesystem::u8path(path);
}

// ── public API ───────────────────────────────────────────────────────────────

FetchResult FetchUrl(const std::string& url, size_t maxResponseBytes) {
    FetchResult r;

    // data: URLs are decoded locally (no network).
    if (StartsWithNoCase(url, "data:")) {
        size_t comma = url.find(',');
        if (comma == std::string::npos) {
            r.error = "Malformed data URL";
            return r;
        }
        std::string meta = url.substr(5, comma - 5);
        std::string payload = url.substr(comma + 1);
        bool base64 = false;
        r.contentType = "text/plain";

        size_t start = 0;
        while (start <= meta.size()) {
            size_t semi = meta.find(';', start);
            std::string part = meta.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
            if (!part.empty()) {
                std::string low;
                for (char c : part) low += (char)std::tolower((unsigned char)c);
                if (low == "base64") base64 = true;
                else if (part.find('/') != std::string::npos) r.contentType = part;
            }
            if (semi == std::string::npos) break;
            start = semi + 1;
        }

        std::string decoded = PercentDecode(payload);
        r.body = base64 ? Base64Decode(decoded) : decoded;
        r.finalUrl = url;
        r.success = true;
        return r;
    }

    if (StartsWithNoCase(url, "file://")) {
        try {
            std::filesystem::path path = FileUrlToPath(url);
            if (std::filesystem::is_directory(path)) {
                r.error = "File URL points to a directory";
                return r;
            }
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                r.error = "Could not open local file";
                return r;
            }
            r.body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (r.body.size() > maxResponseBytes) {
                r.body.clear();
                r.error = "Response exceeds size limit";
                return r;
            }
            r.contentType = FileContentTypeForPath(path);
            r.finalUrl = url;
            r.success = true;
            return r;
        } catch (const std::exception& e) {
            r.error = e.what();
            return r;
        } catch (...) {
            r.error = "Could not read local file";
            return r;
        }
    }

    // HTTP(S) via hand-rolled client (http_client.h — zero third-party deps).
    if (StartsWithNoCase(url, "http://") || StartsWithNoCase(url, "https://")) {
        return FetchHttp(url, maxResponseBytes);
    }

    r.error = "Unsupported URL scheme";
    return r;
}
