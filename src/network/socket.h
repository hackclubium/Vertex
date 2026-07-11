#pragma once
//
// socket.h — minimal cross-platform raw TCP socket.
//
// Part of Vertex's zero-third-party-dependency push: the transport layer
// http_client.h/.cpp builds its hand-rolled HTTP/1.1 client on top of,
// replacing libcurl's role for plain http:// (https:// still goes through
// fetcher.cpp/curl until the TLS phase of this initiative).
//
#include <cstddef>
#include <cstdint>
#include <string>

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    // Resolves `host` and connects to `port`. Returns false on DNS failure,
    // connection refused, or timeout.
    bool Connect(const std::string& host, int port, int timeoutMs = 15000);
    bool ConnectSocks5(const std::string& proxyHost, int proxyPort,
                       const std::string& targetHost, int targetPort,
                       int timeoutMs = 15000);

    // Sends all of `len` bytes, looping internally as needed. Returns false
    // on any send error.
    bool SendAll(const char* data, size_t len);

    // Reads up to `maxLen` bytes. Returns the byte count read (0 = peer
    // closed the connection cleanly), or -1 on error/timeout.
    int Recv(char* buffer, size_t maxLen, int timeoutMs = 30000);

    void Close();
    bool IsValid() const { return fd_ != -1; }
    intptr_t NativeHandle() const { return fd_; }
    intptr_t Detach();

private:
    intptr_t fd_ = -1; // SOCKET (Windows, unsigned pointer-sized) or int (POSIX), both fit in intptr_t
};
