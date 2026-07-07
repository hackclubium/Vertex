#pragma once
//
// websocket.h — hand-rolled RFC 6455 WebSocket client.
//
// Vertex speaks the WebSocket protocol itself: the HTTP Upgrade handshake,
// Sec-WebSocket-Accept validation, frame masking/parsing, fragmentation,
// ping/pong, and the close handshake are all implemented here, not delegated
// to a library. Transport is TcpSocket (ws://) or TlsConnection (wss://) —
// neither curl nor any third-party WebSocket library is used.
//
#include <cstddef>
#include <functional>
#include <string>

enum class WsEventKind { Open, Message, Close, Error };

struct WsEvent {
    WsEventKind kind = WsEventKind::Error;
    std::string data;        // Message: the text/binary payload. Error: a message.
    bool isBinary = false;   // Message only: true for a binary frame, false for text.
    int code = 0;            // Close only: the WebSocket close code (RFC 6455 §7.4).
    std::string reason;      // Close only.
    bool wasClean = false;   // Close only: true if a close handshake actually completed.
};

using WsEventCallback = std::function<void(WsEvent)>;

// Opens a connection on a dedicated background thread and returns a handle.
// onEvent is never invoked from that thread directly — events are queued and
// only fire when drained on the main thread via DrainWebSocketEvents(),
// mirroring network/resource_cache.h's async fetch model so no JS/VM value
// is ever touched off the main thread.
int OpenWebSocket(const std::string& url, WsEventCallback onEvent);

// Queues a text frame to send. No-ops if the handle is unknown or closed.
void SendWebSocketText(int handle, const std::string& text);

// Requests a clean close (RFC 6455 close handshake). No-ops if unknown or
// already closing/closed.
void CloseWebSocket(int handle, int code, const std::string& reason);

size_t DrainWebSocketEvents(size_t maxEvents = 64);

// Computes the Sec-WebSocket-Accept value a conformant server must return for
// a given Sec-WebSocket-Key (RFC 6455 §1.3/§4.2.2): base64(sha1(key +
// "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")). Exposed for testing — the real
// handshake validation in websocket.cpp calls this same function, so a test
// against RFC 6455's own worked example exercises the exact code path used
// on every real connection.
std::string ComputeWebSocketAccept(const std::string& secWebSocketKey);

// True if any connection is still open/connecting, OR an event is queued but
// not yet drained. Platforms fold this into their "keep the JS timer alive"
// idle check (alongside HasPendingResourceCompletions()) — an open-but-quiet
// socket still needs the timer ticking so a message that arrives later
// actually gets delivered instead of sitting undrained indefinitely.
bool HasOpenWebSockets();
