// Socket.h - non-blocking TCP and UDP client connections, polled once a frame (Winsock on Windows, BSD sockets elsewhere).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace net {

class TcpConnection {
public:
    enum class State { Closed, Connecting, Connected, Failed };

    TcpConnection();
    ~TcpConnection();
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    // Starts connecting to host:port (a name or dotted address, IPv4); false when that cannot even start.
    bool connect(const std::string& host, int port);
    void close();
    // Advances a pending connect, sends what is queued and reads what has arrived; call once a frame.
    void poll();
    State state() const { return state_; }
    // Set once a connection that was up has been closed by the other side or broken (cleared by connect()).
    bool dropped() const { return dropped_; }
    void send(const uint8_t* data, size_t length);
    // Everything received so far (the caller takes what it uses from the front).
    std::vector<uint8_t>& received() { return in_; }

private:
    intptr_t socket_;
    State state_ = State::Closed;
    bool dropped_ = false;
    std::vector<uint8_t> in_, out_;
};

// A connected UDP socket (netUDPOutConnection's socket, netUdpOutConnection.cpp FUN_00a81da0: socket(AF_INET, SOCK_DGRAM), bind to
// any address, connect to the server - so only the server's datagrams arrive). Datagrams are sent and received whole.
class UdpConnection {
public:
    UdpConnection();
    ~UdpConnection();
    UdpConnection(const UdpConnection&) = delete;
    UdpConnection& operator=(const UdpConnection&) = delete;

    bool open(const std::string& host, int port);   // false when the name does not resolve or the socket cannot be made
    void close();
    bool isOpen() const;
    bool send(const uint8_t* data, size_t length);  // false when the datagram could not be handed to the system
    // The next datagram that has arrived, if any (false: none waiting; a refused / unreachable port also reads as none).
    bool receive(std::vector<uint8_t>& datagram);

private:
    intptr_t socket_;
};

}  // namespace net
