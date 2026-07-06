#pragma once
//
// tls_socket.h — TLS client connection.
//
// Part of Vertex's zero-third-party-dependency push. Each platform gets a
// real, hardened TLS implementation, never hand-rolled crypto:
//   - Windows: SChannel via SSPI (tls_windows.cpp) — OS-native, zero deps.
//   - macOS:   Secure Transport (tls_macos.cpp) — OS-native, zero deps.
//   - Linux:   mbedTLS (tls_linux.cpp) — Linux has no OS-provided TLS stack
//     at all, unlike Windows/macOS, so it gets one small bundled library
//     (fetched via CMake, like libcurl already is) as a deliberate, scoped
//     exception — the browser/protocol behavior above it is still entirely
//     Vertex's own code, matching how Windows/macOS rendering already uses
//     first-party OS APIs (Direct2D/Core Graphics) rather than being
//     "hand-rolled" pixel math.
//
#include <cstddef>
#include <string>

class TlsConnection {
public:
    TlsConnection();
    ~TlsConnection();
    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;

    // Connects a raw TCP socket to host:port, then performs the TLS
    // handshake — including validating the server's certificate against the
    // OS (or, on Linux, mbedTLS's bundled) trust store and checking the
    // hostname matches. Returns false on any failure at any stage.
    bool Connect(const std::string& host, int port, int timeoutMs = 15000);

    bool SendAll(const char* data, size_t len);

    // Reads up to `maxLen` plaintext bytes. Returns the byte count read
    // (0 = peer closed cleanly), or -1 on error/timeout.
    int Recv(char* buffer, size_t maxLen, int timeoutMs = 30000);

    void Close();
    bool IsValid() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
