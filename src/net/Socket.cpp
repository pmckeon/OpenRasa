#include "Socket.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
static const intptr_t kNoSocket = intptr_t(INVALID_SOCKET);
static int lastError() { return WSAGetLastError(); }
static bool wouldBlock(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY; }
static void closeSocket(intptr_t s) { closesocket(SocketHandle(s)); }
static void setNonBlocking(intptr_t s) {
    u_long on = 1;
    ioctlsocket(SocketHandle(s), FIONBIO, &on);
}
static void startup() {
    static bool started = false;
    if (!started) {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        started = true;
    }
}
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
static const intptr_t kNoSocket = -1;
static int lastError() { return errno; }
static bool wouldBlock(int e) { return e == EWOULDBLOCK || e == EAGAIN || e == EINPROGRESS || e == EALREADY; }
static void closeSocket(intptr_t s) { ::close(int(s)); }
static void setNonBlocking(intptr_t s) { fcntl(int(s), F_SETFL, fcntl(int(s), F_GETFL, 0) | O_NONBLOCK); }
static void startup() {}
#endif

namespace net {

TcpConnection::TcpConnection() : socket_(kNoSocket) {}
TcpConnection::~TcpConnection() { close(); }

bool TcpConnection::connect(const std::string& host, int port) {
    close();
    startup();
    dropped_ = false;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) {
        std::fprintf(stderr, "[net] cannot resolve %s\n", host.c_str());
        state_ = State::Failed;
        return false;
    }
    socket_ = intptr_t(::socket(found->ai_family, found->ai_socktype, found->ai_protocol));
    if (socket_ == kNoSocket) {
        freeaddrinfo(found);
        state_ = State::Failed;
        return false;
    }
    setNonBlocking(socket_);
    int noDelay = 1;
    setsockopt(SocketHandle(socket_), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof noDelay);
    int r = ::connect(SocketHandle(socket_), found->ai_addr, int(found->ai_addrlen));
    freeaddrinfo(found);
    if (r != 0 && !wouldBlock(lastError())) {
        close();
        state_ = State::Failed;
        return false;
    }
    state_ = r == 0 ? State::Connected : State::Connecting;
    return true;
}

void TcpConnection::close() {
    if (socket_ != kNoSocket) closeSocket(socket_);
    socket_ = kNoSocket;
    if (state_ != State::Failed) state_ = State::Closed;
    in_.clear();
    out_.clear();
}

void TcpConnection::send(const uint8_t* data, size_t length) { out_.insert(out_.end(), data, data + length); }

void TcpConnection::poll() {
    if (socket_ == kNoSocket) return;
    if (state_ == State::Connecting) {
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(SocketHandle(socket_), &writable);
        FD_SET(SocketHandle(socket_), &failed);
        timeval none{};
        if (select(int(socket_) + 1, nullptr, &writable, &failed, &none) <= 0) return;
        int error = 0;
        socklen_t size = sizeof error;
        getsockopt(SocketHandle(socket_), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size);
        if (FD_ISSET(SocketHandle(socket_), &failed) || error != 0) {
            closeSocket(socket_);
            socket_ = kNoSocket;
            state_ = State::Failed;
            return;
        }
        state_ = State::Connected;
    }
    while (!out_.empty()) {
        int sent = ::send(SocketHandle(socket_), reinterpret_cast<const char*>(out_.data()), int(out_.size()), 0);
        if (sent > 0) {
            out_.erase(out_.begin(), out_.begin() + sent);
            continue;
        }
        if (sent < 0 && wouldBlock(lastError())) break;
        dropped_ = true;
        close();
        return;
    }
    uint8_t buffer[16384];
    for (;;) {
        int got = ::recv(SocketHandle(socket_), reinterpret_cast<char*>(buffer), int(sizeof buffer), 0);
        if (got > 0) {
            in_.insert(in_.end(), buffer, buffer + got);
            continue;
        }
        if (got < 0 && wouldBlock(lastError())) break;
        dropped_ = true;   // 0: closed by the other side; < 0: broken
        closeSocket(socket_);
        socket_ = kNoSocket;
        state_ = State::Closed;
        out_.clear();
        return;   // what arrived before the close stays in in_
    }
}

UdpConnection::UdpConnection() : socket_(kNoSocket) {}
UdpConnection::~UdpConnection() { close(); }
bool UdpConnection::isOpen() const { return socket_ != kNoSocket; }

bool UdpConnection::open(const std::string& host, int port) {
    close();
    startup();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) {
        std::fprintf(stderr, "[net] cannot resolve %s\n", host.c_str());
        return false;
    }
    socket_ = intptr_t(::socket(AF_INET, SOCK_DGRAM, 0));
    if (socket_ == kNoSocket) {
        freeaddrinfo(found);
        return false;
    }
    sockaddr_in any{};
    any.sin_family = AF_INET;
    const bool ok = ::bind(SocketHandle(socket_), reinterpret_cast<const sockaddr*>(&any), sizeof any) == 0 &&
                    ::connect(SocketHandle(socket_), found->ai_addr, int(found->ai_addrlen)) == 0;
    freeaddrinfo(found);
    if (!ok) {
        close();
        return false;
    }
    setNonBlocking(socket_);
    return true;
}

void UdpConnection::close() {
    if (socket_ != kNoSocket) closeSocket(socket_);
    socket_ = kNoSocket;
}

bool UdpConnection::send(const uint8_t* data, size_t length) {
    if (socket_ == kNoSocket) return false;
    return ::send(SocketHandle(socket_), reinterpret_cast<const char*>(data), int(length), 0) == int(length);
}

bool UdpConnection::receive(std::vector<uint8_t>& datagram) {
    if (socket_ == kNoSocket) return false;
    uint8_t buffer[2048];
    for (;;) {
        int got = ::recv(SocketHandle(socket_), reinterpret_cast<char*>(buffer), int(sizeof buffer), 0);
        if (got >= 0) {
            datagram.assign(buffer, buffer + got);
            return true;
        }
        if (wouldBlock(lastError())) return false;
        // An ICMP "port unreachable" from an earlier datagram (WSAECONNRESET on Windows, ECONNREFUSED elsewhere): skip it.
#ifdef _WIN32
        if (lastError() == WSAECONNRESET) continue;
#else
        if (lastError() == ECONNREFUSED) continue;
#endif
        return false;
    }
}

}  // namespace net
