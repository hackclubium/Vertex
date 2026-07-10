#include "network/url.h"

#include <cctype>
#include <utility>
#include <vector>

namespace {
std::string LowerAscii(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string PercentDecode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = HexValue(value[i + 1]);
            const int lo = HexValue(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += value[i];
    }
    return out;
}

std::string Base64UrlDecode(std::string encoded) {
    for (char& c : encoded) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (encoded.size() % 4 != 0) encoded += '=';

    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((encoded.size() / 4) * 3);
    for (size_t i = 0; i + 3 < encoded.size(); i += 4) {
        int values[4] = {};
        for (int j = 0; j < 4; ++j) {
            if (encoded[i + j] == '=') values[j] = -2;
            else {
                const size_t found = alphabet.find(encoded[i + j]);
                if (found == std::string::npos) return {};
                values[j] = static_cast<int>(found);
            }
        }
        if (values[0] < 0 || values[1] < 0) return {};
        out += static_cast<char>((values[0] << 2) | (values[1] >> 4));
        if (values[2] >= 0) {
            out += static_cast<char>(((values[1] & 0x0f) << 4) | (values[2] >> 2));
            if (values[3] >= 0)
                out += static_cast<char>(((values[2] & 0x03) << 6) | values[3]);
        }
    }
    return out;
}

std::string StripFragment(const std::string& url) {
    const size_t hash = url.find('#');
    return hash == std::string::npos ? url : url.substr(0, hash);
}

std::string StripQueryAndFragment(const std::string& url) {
    const size_t cut = url.find_first_of("?#");
    return cut == std::string::npos ? url : url.substr(0, cut);
}

std::string NormalizePath(std::string path) {
    std::string suffix;
    const size_t suffixStart = path.find_first_of("?#");
    if (suffixStart != std::string::npos) {
        suffix = path.substr(suffixStart);
        path.erase(suffixStart);
    }

    const bool absolute = !path.empty() && path.front() == '/';
    const bool trailingSlash = !path.empty() && path.back() == '/';
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= path.size()) {
        const size_t slash = path.find('/', pos);
        std::string part = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(std::move(part));
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }

    std::string out = absolute ? "/" : "";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += '/';
        out += parts[i];
    }
    if (out.empty() && absolute) out = "/";
    if (trailingSlash && !out.empty() && out.back() != '/') out += '/';
    return out + suffix;
}

std::string QueryParam(const std::string& url, const std::string& name) {
    std::string normalized = url;
    size_t htmlAmp = 0;
    while ((htmlAmp = normalized.find("&amp;", htmlAmp)) != std::string::npos)
        normalized.replace(htmlAmp, 5, "&");

    const size_t query = normalized.find('?');
    if (query == std::string::npos) return {};
    size_t pos = query + 1;
    while (pos < normalized.size()) {
        const size_t end = normalized.find('&', pos);
        const std::string parameter = normalized.substr(pos,
            end == std::string::npos ? std::string::npos : end - pos);
        const size_t equals = parameter.find('=');
        if (equals != std::string::npos && parameter.substr(0, equals) == name)
            return PercentDecode(parameter.substr(equals + 1));
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return {};
}
} // namespace

bool HasUrlScheme(const std::string& url) {
    size_t colon = url.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    size_t stop = url.find_first_of("/?#");
    if (stop != std::string::npos && stop < colon) return false;
    for (size_t i = 0; i < colon; ++i) {
        char c = url[i];
        if (!std::isalnum((unsigned char)c) && c != '+' && c != '-' && c != '.')
            return false;
    }
    return true;
}

std::string ResolveUrlAgainstBase(const std::string& href, const std::string& base) {
    if (href.empty()) return {};
    if (HasUrlScheme(href)) return href;
    if (href[0] == '#') return StripFragment(base) + href;
    if (href[0] == '?') return StripQueryAndFragment(base) + href;

    size_t p = base.find("://");
    if (href.size() >= 2 && href[0] == '/' && href[1] == '/') {
        // Protocol-relative URL: use base URL's scheme
        if (p != std::string::npos) {
            return base.substr(0, p + 1) + href;
        }
        return "https:" + href; // Fallback if base has no scheme
    }

    if (p == std::string::npos) {
        if (href[0] == '/') return NormalizePath(href);
        const size_t last = StripQueryAndFragment(base).rfind('/');
        return NormalizePath((last == std::string::npos ? base + "/" : base.substr(0, last + 1)) + href);
    }

    const size_t authorityEnd = base.find_first_of("/?#", p + 3);
    const std::string origin = authorityEnd == std::string::npos ? base : base.substr(0, authorityEnd);
    if (href[0] == '/') {
        return origin + NormalizePath(href);
    }

    std::string basePath = authorityEnd == std::string::npos ? "/" : base.substr(authorityEnd);
    basePath = StripQueryAndFragment(basePath);
    const size_t last = basePath.rfind('/');
    const std::string dir = last == std::string::npos ? "/" : basePath.substr(0, last + 1);
    return origin + NormalizePath(dir + href);
}

std::string UnwrapBingRedirect(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return url;
    const size_t hostEnd = url.find_first_of("/?#", scheme + 3);
    const std::string host = LowerAscii(url.substr(scheme + 3,
        hostEnd == std::string::npos ? std::string::npos : hostEnd - scheme - 3));
    if (host != "bing.com" && host != "www.bing.com") return url;

    const std::string value = QueryParam(url, "u");
    if (value.rfind("a1", 0) == 0) {
        const std::string destination = Base64UrlDecode(value.substr(2));
        const std::string lowerDestination = LowerAscii(destination);
        if (lowerDestination.rfind("https://", 0) == 0
            || lowerDestination.rfind("http://", 0) == 0)
            return destination;
    }
    return url;
}

std::string UnwrapDuckDuckGoRedirect(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return url;
    const size_t hostEnd = url.find_first_of("/?#", scheme + 3);
    const std::string host = LowerAscii(url.substr(scheme + 3,
        hostEnd == std::string::npos ? std::string::npos : hostEnd - scheme - 3));
    if (host != "duckduckgo.com" && host != "www.duckduckgo.com") return url;

    const std::string destination = QueryParam(url, "uddg");
    const std::string lowerDestination = LowerAscii(destination);
    if (lowerDestination.rfind("https://", 0) == 0
        || lowerDestination.rfind("http://", 0) == 0)
        return destination;
    return url;
}
