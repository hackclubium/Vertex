#include "network/socket.h"
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket close
#endif

namespace {

void EnsureSocketsInit() {
#ifdef _WIN32
    // Thread-safe, idempotent, independent of curl's own WSAStartup (via
    // EnsureCurlInit()) — this component never touches curl, so it needs
    // its own initialization.
    static std::once_flag flag;
    std::call_once(flag, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
#endif
}

bool SetNonBlocking(intptr_t fd, bool nonBlocking) {
#ifdef _WIN32
    u_long mode = nonBlocking ? 1 : 0;
    return ioctlsocket((SOCKET)fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl((int)fd, F_GETFL, 0);
    if (flags < 0) return false;
    flags = nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl((int)fd, F_SETFL, flags) == 0;
#endif
}

} // namespace

TcpSocket::~TcpSocket() { Close(); }

bool TcpSocket::Connect(const std::string& host, int port, int timeoutMs) {
    EnsureSocketsInit();
    Close();

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* addrList = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &addrList) != 0 || !addrList)
        return false;

    bool connected = false;
    for (struct addrinfo* rp = addrList; rp != nullptr; rp = rp->ai_next) {
#ifdef _WIN32
        SOCKET s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s == INVALID_SOCKET) continue;
#else
        int s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s < 0) continue;
#endif
        // Connect non-blocking so a slow/unreachable host is bounded by
        // timeoutMs instead of the OS's own (often much longer) default.
        SetNonBlocking((intptr_t)s, true);
        int rc = connect(s, rp->ai_addr, (int)rp->ai_addrlen);
        bool ok = false;
        bool inProgress =
#ifdef _WIN32
            (rc != 0 && WSAGetLastError() == WSAEWOULDBLOCK);
#else
            (rc != 0 && errno == EINPROGRESS);
#endif
        if (rc == 0) {
            ok = true;
        } else if (inProgress) {
            fd_set wfds; FD_ZERO(&wfds); FD_SET(s, &wfds);
            timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
            if (select((int)s + 1, nullptr, &wfds, nullptr, &tv) > 0) {
                int err = 0;
#ifdef _WIN32
                int len = sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
#else
                socklen_t len = sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
                ok = (err == 0);
            }
        }
        if (ok) {
            SetNonBlocking((intptr_t)s, false); // back to blocking for simple SendAll/Recv semantics
            fd_ = (intptr_t)s;
            connected = true;
            break;
        }
        closesocket(s);
    }
    freeaddrinfo(addrList);
    return connected;
}

bool TcpSocket::SendAll(const char* data, size_t len) {
    if (fd_ == -1) return false;
    size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int n = send((SOCKET)fd_, data + sent, (int)(len - sent), 0);
#else
        ssize_t n = send((int)fd_, data + sent, len - sent, 0);
#endif
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

int TcpSocket::Recv(char* buffer, size_t maxLen, int timeoutMs) {
    if (fd_ == -1) return -1;
#ifdef _WIN32
    SOCKET s = (SOCKET)fd_;
#else
    int s = (int)fd_;
#endif
    fd_set rfds; FD_ZERO(&rfds); FD_SET(s, &rfds);
    timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    int sel = select((int)s + 1, &rfds, nullptr, nullptr, &tv);
    if (sel <= 0) return -1; // timeout or select error
#ifdef _WIN32
    int n = recv(s, buffer, (int)maxLen, 0);
#else
    ssize_t n = recv(s, buffer, maxLen, 0);
#endif
    if (n < 0) return -1;
    return (int)n;
}

void TcpSocket::Close() {
    if (fd_ != -1) {
#ifdef _WIN32
        closesocket((SOCKET)fd_);
#else
        closesocket((int)fd_);
#endif
        fd_ = -1;
    }
}
