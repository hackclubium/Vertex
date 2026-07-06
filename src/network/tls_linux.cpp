#if !defined(_WIN32) && !defined(__APPLE__)
#include "network/tls_socket.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <cstring>
#include <mutex>

namespace {

// There's no single OS API to query "the system trust store" on Linux the
// way Windows/macOS have one — probe the handful of well-known CA bundle
// paths real-world minimal TLS clients check.
const char* kCaBundlePaths[] = {
    "/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu
    "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora/RHEL
    "/etc/ssl/cert.pem",                  // Alpine and others
    "/etc/ssl/ca-bundle.pem",             // openSUSE
};

// Parsing the system bundle (100+ certs) is too expensive to redo per
// connection — load it once, share the (read-only after loading) result
// across every TlsConnection.
mbedtls_x509_crt* SharedCaChain() {
    static mbedtls_x509_crt chain;
    static bool loaded = false;
    static std::once_flag flag;
    std::call_once(flag, [] {
        mbedtls_x509_crt_init(&chain);
        for (const char* path : kCaBundlePaths) {
            if (mbedtls_x509_crt_parse_file(&chain, path) == 0) { loaded = true; break; }
        }
    });
    return loaded ? &chain : nullptr;
}

} // namespace

struct TlsConnection::Impl {
    mbedtls_net_context netCtx;
    mbedtls_ssl_context sslCtx;
    mbedtls_ssl_config sslConf;
    mbedtls_ctr_drbg_context ctrDrbg;
    mbedtls_entropy_context entropy;
    bool handshakeDone = false;

    Impl() {
        mbedtls_net_init(&netCtx);
        mbedtls_ssl_init(&sslCtx);
        mbedtls_ssl_config_init(&sslConf);
        mbedtls_ctr_drbg_init(&ctrDrbg);
        mbedtls_entropy_init(&entropy);
    }
    ~Impl() {
        mbedtls_net_free(&netCtx);
        mbedtls_ssl_free(&sslCtx);
        mbedtls_ssl_config_free(&sslConf);
        mbedtls_ctr_drbg_free(&ctrDrbg);
        mbedtls_entropy_free(&entropy);
    }
};

TlsConnection::TlsConnection() : impl_(new Impl()) {}
TlsConnection::~TlsConnection() { Close(); delete impl_; }

bool TlsConnection::Connect(const std::string& host, int port, int timeoutMs) {
    Impl& im = *impl_;

    // Reseeding a fresh DRBG per connection (rather than sharing one, mutex-
    // guarded, across all connections) is simpler to reason about
    // correctness-wise and cheap enough at browser connection frequency —
    // a deliberate simplicity-over-throughput choice, not an oversight.
    const char* pers = "vertex_tls";
    if (mbedtls_ctr_drbg_seed(&im.ctrDrbg, mbedtls_entropy_func, &im.entropy,
                              (const unsigned char*)pers, strlen(pers)) != 0)
        return false;

    mbedtls_x509_crt* caChain = SharedCaChain();
    if (!caChain) return false; // no usable system CA bundle found

    if (mbedtls_ssl_config_defaults(&im.sslConf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        return false;
    mbedtls_ssl_conf_authmode(&im.sslConf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&im.sslConf, caChain, nullptr);
    mbedtls_ssl_conf_rng(&im.sslConf, mbedtls_ctr_drbg_random, &im.ctrDrbg);
    mbedtls_ssl_conf_read_timeout(&im.sslConf, (uint32_t)timeoutMs);

    if (mbedtls_ssl_setup(&im.sslCtx, &im.sslConf) != 0) return false;
    if (mbedtls_ssl_set_hostname(&im.sslCtx, host.c_str()) != 0) return false; // SNI + hostname verification

    std::string portStr = std::to_string(port);
    if (mbedtls_net_connect(&im.netCtx, host.c_str(), portStr.c_str(), MBEDTLS_NET_PROTO_TCP) != 0)
        return false;

    // f_recv left null in favor of f_recv_timeout — the documented mbedTLS
    // pattern for giving reads a bounded timeout instead of blocking forever.
    mbedtls_ssl_set_bio(&im.sslCtx, &im.netCtx, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);

    int ret;
    while ((ret = mbedtls_ssl_handshake(&im.sslCtx)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            return false;
    }

    if (mbedtls_ssl_get_verify_result(&im.sslCtx) != 0) return false; // certificate validation failed

    im.handshakeDone = true;
    return true;
}

bool TlsConnection::SendAll(const char* data, size_t len) {
    if (!impl_->handshakeDone) return false;
    size_t sent = 0;
    while (sent < len) {
        int n = mbedtls_ssl_write(&impl_->sslCtx, (const unsigned char*)data + sent, len - sent);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

int TlsConnection::Recv(char* buffer, size_t maxLen, int timeoutMs) {
    if (!impl_->handshakeDone) return -1;
    mbedtls_ssl_conf_read_timeout(&impl_->sslConf, (uint32_t)timeoutMs);
    int n = mbedtls_ssl_read(&impl_->sslCtx, (unsigned char*)buffer, maxLen);
    if (n == MBEDTLS_ERR_SSL_TIMEOUT) return -1;
    if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
    if (n < 0) return -1;
    return n;
}

void TlsConnection::Close() {
    if (impl_ && impl_->handshakeDone) {
        mbedtls_ssl_close_notify(&impl_->sslCtx);
        impl_->handshakeDone = false;
    }
}

bool TlsConnection::IsValid() const { return impl_ && impl_->handshakeDone; }

#endif // !_WIN32 && !__APPLE__
