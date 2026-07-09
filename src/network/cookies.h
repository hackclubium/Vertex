#pragma once
//
// cookies.h — in-memory cookie jar for Vertex.
//
// Stores cookies per-domain, sends matching cookies with each request,
// and captures Set-Cookie headers from responses. No persistence (cookies
// are lost when the browser closes). Supports:
// - Domain matching (cookie.example.com matches *.example.com)
// - Path matching (cookie /foo matches /foo/bar)
// - Secure flag (only sent over HTTPS)
// - HttpOnly flag (not accessible via document.cookie)
// - Expires/Max-Age (expired cookies are pruned)
//
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <ctime>
#include <sstream>
#include <algorithm>
#include <cctype>

struct Cookie {
    std::string name;
    std::string value;
    std::string domain;   // ".example.com" (leading dot = subdomain match)
    std::string path;     // "/" (default)
    time_t expires = 0;   // 0 = session cookie (never expires during session)
    bool secure = false;
    bool httpOnly = false;
};

class CookieJar {
public:
    static CookieJar& instance() {
        static CookieJar jar;
        return jar;
    }

    // Parse Set-Cookie header and store the cookie.
    void handleSetCookie(const std::string& header, const std::string& requestUrl) {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Parse: name=value; Path=/; Domain=.example.com; Secure; HttpOnly; Max-Age=3600
        Cookie c;

        // Extract domain from request URL for default.
        c.domain = domainFromUrl(requestUrl);
        c.path = defaultPathFromUrl(requestUrl);

        size_t eq = header.find('=');
        if (eq == std::string::npos) return;

        // Find the end of the value (first ; or end of string).
        size_t semi = header.find(';', eq);
        c.name = trim(header.substr(0, eq));
        c.value = trim(header.substr(eq + 1, semi == std::string::npos ? std::string::npos : semi - eq - 1));

        if (c.name.empty() || hasCtl(c.name) || hasCtl(c.value) || c.name.find(';') != std::string::npos) return;

        // Parse attributes after the first semicolon.
        if (semi != std::string::npos) {
            std::string rest = header.substr(semi + 1);
            std::istringstream ss(rest);
            std::string attr;
            while (std::getline(ss, attr, ';')) {
                attr = trim(attr);
                std::string low = toLower(attr);
                size_t aeq = attr.find('=');
                std::string aname = aeq != std::string::npos ? toLower(trim(attr.substr(0, aeq))) : low;
                std::string aval = aeq != std::string::npos ? trim(attr.substr(aeq + 1)) : "";

                if (aname == "domain") {
                    std::string requested = toLower(aval);
                    if (!requested.empty() && requested[0] != '.') requested = "." + requested;
                    if (!domainMatches(domainFromUrl(requestUrl), requested)) return;
                    c.domain = requested;
                } else if (aname == "path") {
                    c.path = aval.empty() ? "/" : aval;
                } else if (aname == "secure") {
                    c.secure = true;
                } else if (aname == "httponly") {
                    c.httpOnly = true;
                } else if (aname == "max-age") {
                    try {
                        int seconds = std::stoi(aval);
                        if (seconds <= 0) { c.expires = 1; } // expired
                        else c.expires = std::time(nullptr) + seconds;
                    } catch (...) {}
                } else if (aname == "expires") {
                    // Simple date parsing: try to detect if it's in the past.
                    // Full HTTP date parsing is complex; for now, store as session
                    // cookie unless we can parse it.
                    (void)aval; // Accept but don't parse expiry dates
                }
            }
        }

        // Delete expired cookies.
        if (c.expires == 1) {
            removeCookie(c.domain, c.path, c.name);
            return;
        }

        // Store or replace existing cookie with same name+domain+path.
        for (auto& existing : m_cookies) {
            if (existing.name == c.name && existing.domain == c.domain && existing.path == c.path) {
                existing = c;
                return;
            }
        }
        m_cookies.push_back(c);
    }

    // Build the Cookie header value for a request URL.
    std::string cookieHeader(const std::string& url) const {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::string domain = domainFromUrl(url);
        std::string path = pathFromUrl(url);
        bool isSecure = (url.rfind("https://", 0) == 0);
        time_t now = std::time(nullptr);

        std::string result;
        for (const auto& c : m_cookies) {
            // Check expiry.
            if (c.expires > 0 && c.expires < now) continue;
            // Check secure.
            if (c.secure && !isSecure) continue;
            // Check domain match.
            if (!domainMatches(domain, c.domain)) continue;
            // Check path match.
            if (!pathMatches(path, c.path)) continue;

            if (!result.empty()) result += "; ";
            result += c.name + "=" + c.value;
        }
        return result;
    }

    // Get all non-HttpOnly cookies for document.cookie (JS access).
    std::string documentCookies(const std::string& url) const {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::string domain = domainFromUrl(url);
        std::string path = pathFromUrl(url);
        bool isSecure = (url.rfind("https://", 0) == 0);
        time_t now = std::time(nullptr);

        std::string result;
        for (const auto& c : m_cookies) {
            if (c.httpOnly) continue;
            if (c.expires > 0 && c.expires < now) continue;
            if (c.secure && !isSecure) continue;
            if (!domainMatches(domain, c.domain)) continue;
            if (!pathMatches(path, c.path)) continue;

            if (!result.empty()) result += "; ";
            result += c.name + "=" + c.value;
        }
        return result;
    }

    // Set a cookie from document.cookie = "name=value; ..."
    void setFromJS(const std::string& cookieStr, const std::string& url) {
        std::string low = toLower(cookieStr);
        if (low.find("httponly") != std::string::npos) return;
        if (low.find("secure") != std::string::npos && url.rfind("https://", 0) != 0) return;
        handleSetCookie(cookieStr, url);
    }

private:
    mutable std::mutex m_mutex;
    std::vector<Cookie> m_cookies;

    void removeCookie(const std::string& domain, const std::string& path, const std::string& name) {
        m_cookies.erase(
            std::remove_if(m_cookies.begin(), m_cookies.end(),
                [&](const Cookie& c) { return c.name == name && c.domain == domain && c.path == path; }),
            m_cookies.end());
    }

    static std::string trim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        size_t end = s.find_last_not_of(" \t\r\n");
        return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
    }

    static std::string toLower(std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    static bool hasCtl(const std::string& s) {
        for (unsigned char c : s)
            if (c < 0x20 || c == 0x7f) return true;
        return false;
    }

    static std::string domainFromUrl(const std::string& url) {
        size_t scheme = url.find("://");
        if (scheme == std::string::npos) return "";
        size_t hostStart = scheme + 3;
        size_t hostEnd = url.find('/', hostStart);
        std::string host = url.substr(hostStart, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
        // Remove port.
        size_t colon = host.find(':');
        if (colon != std::string::npos) host = host.substr(0, colon);
        return toLower(host);
    }

    static std::string pathFromUrl(const std::string& url) {
        size_t scheme = url.find("://");
        if (scheme == std::string::npos) return "/";
        size_t pathStart = url.find('/', scheme + 3);
        if (pathStart == std::string::npos) return "/";
        size_t query = url.find('?', pathStart);
        return url.substr(pathStart, query == std::string::npos ? std::string::npos : query - pathStart);
    }

    static std::string defaultPathFromUrl(const std::string& url) {
        std::string path = pathFromUrl(url);
        if (path.empty() || path[0] != '/') return "/";
        size_t lastSlash = path.rfind('/');
        if (lastSlash == std::string::npos || lastSlash == 0) return "/";
        return path.substr(0, lastSlash);
    }

    static bool domainMatches(const std::string& requestDomain, const std::string& cookieDomain) {
        if (cookieDomain.empty()) return false;
        std::string cd = toLower(cookieDomain);
        std::string rd = toLower(requestDomain);
        if (rd == cd || rd == cd.substr(1)) return true; // exact match or without leading dot
        // Subdomain match: ".example.com" matches "sub.example.com"
        if (cd[0] == '.' && rd.size() > cd.size() && rd.substr(rd.size() - cd.size()) == cd) return true;
        return false;
    }

    static bool pathMatches(const std::string& requestPath, const std::string& cookiePath) {
        if (cookiePath.empty() || cookiePath == "/") return true;
        if (requestPath == cookiePath) return true;
        if (requestPath.rfind(cookiePath, 0) != 0) return false;
        return cookiePath.back() == '/'
            || (requestPath.size() > cookiePath.size() && requestPath[cookiePath.size()] == '/');
    }
};
