#pragma once
//
// http_client.h — hand-rolled HTTP/1.1 client (http:// and https://).
//
// Part of Vertex's zero-third-party-dependency push: replaces libcurl's
// role for HTTP(S), built on socket.h's raw TCP sockets (or tls_socket.h's
// TlsConnection for https://) and the already-shipped GzipInflate for
// Content-Encoding: gzip. This is a standalone, tested module for now;
// wiring it into the main FetchUrl() call sites is a separate follow-up
// decision (same pattern as PNG/JPEG not yet replacing stb_image's call
// sites).
//
#include "network/fetcher.h"
#include <string>

FetchResult FetchHttp(const std::string& url, size_t maxResponseBytes = 12 * 1024 * 1024);
