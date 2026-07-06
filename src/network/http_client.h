#pragma once
//
// http_client.h — hand-rolled HTTP/1.1 client (http:// only, no TLS).
//
// Part of Vertex's zero-third-party-dependency push: replaces libcurl's
// role for plain HTTP, built on socket.h's raw TCP sockets and the
// already-shipped GzipInflate for Content-Encoding: gzip. https:// is NOT
// handled here — callers should keep using FetchUrl() (fetcher.cpp/curl)
// for that until the TLS phase of this initiative. This is a standalone,
// tested module for now; wiring it into the main FetchUrl() call sites is a
// separate follow-up decision (same pattern as PNG/JPEG not yet replacing
// stb_image's call sites).
//
#include "network/fetcher.h"
#include <string>

FetchResult FetchHttp(const std::string& url, size_t maxResponseBytes = 12 * 1024 * 1024);
