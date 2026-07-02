// test_url.cpp — unit tests for LooksLikeUrl and URL helpers.
#include "platform/browser_core.h"
#include <cstdio>
#include <cstring>

static int g_pass = 0, g_fail = 0;

static void check(bool cond, const char* expr, const char* file, int line) {
    if (cond) { ++g_pass; }
    else { ++g_fail; fprintf(stderr, "FAIL: %s (%s:%d)\n", expr, file, line); }
}
#define CHECK(x) check((x), #x, __FILE__, __LINE__)

int main() {
    // URLs that should be recognized
    CHECK(LooksLikeUrl("https://example.com"));
    CHECK(LooksLikeUrl("http://example.com"));
    CHECK(LooksLikeUrl("ftp://files.example.com"));
    CHECK(LooksLikeUrl("example.com"));
    CHECK(LooksLikeUrl("en.wikipedia.org"));
    CHECK(LooksLikeUrl("sub.domain.example.com"));
    CHECK(LooksLikeUrl("vertex://home"));
    CHECK(LooksLikeUrl("vertex://bookmarks"));
    CHECK(LooksLikeUrl("about:blank"));
    CHECK(LooksLikeUrl("about:vertex"));

    // localhost
    CHECK(LooksLikeUrl("localhost"));
    CHECK(LooksLikeUrl("localhost:3000"));
    CHECK(LooksLikeUrl("localhost:8080"));

    // IP addresses
    CHECK(LooksLikeUrl("127.0.0.1"));
    CHECK(LooksLikeUrl("127.0.0.1:8080"));
    CHECK(LooksLikeUrl("192.168.1.1"));
    CHECK(LooksLikeUrl("10.0.0.1:3000"));

    // IPv6
    CHECK(LooksLikeUrl("[::1]"));
    CHECK(LooksLikeUrl("[::1]:8080"));

    // Protocol-relative
    CHECK(LooksLikeUrl("//cdn.example.com/file.js"));

    // Search queries (should NOT be URLs)
    CHECK(!LooksLikeUrl("hello world"));
    CHECK(!LooksLikeUrl("what is CSS"));
    CHECK(!LooksLikeUrl("how to build a browser"));
    CHECK(!LooksLikeUrl(""));
    CHECK(!LooksLikeUrl("singleword"));
    CHECK(!LooksLikeUrl(".hidden"));
    CHECK(!LooksLikeUrl("trailing."));

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
