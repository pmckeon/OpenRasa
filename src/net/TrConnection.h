// TrConnection.h - tabula_rasa.exe's TRasa::TCPConnection (shared\tr\net\tcpconnection.cpp), the client side: the connection the
// login queue and the game server speak over.
//
// Frames: a little-endian u32 length (not counting itself), then the body (Rasa.NET LengthedSocket(SizeType.Dword, countSize false)).
// After the TCP connect the server opens a key negotiation:
//   1. server: u32 public key length, u32 prime length, u32 generator length, then the three byte strings
//      (_ReceiveServerPublicKey FUN_0070d4b0 checks 12 + the three lengths = the frame);
//   2. the client's key exchanger takes the prime and generator (vtable + 0x1c) and the server's public key (+ 0x20, giving the shared
//      key the encryptor starts from); the client sends its public key: u32 length + bytes (_BuildClientPublicKey FUN_0070e8d0);
//   3. server: "ENC OK", exactly 6 bytes (_ReceiveAckClientPublicKey FUN_0070daf0); the connection is then up (state 7).
// The login queue uses TRasa::NullKeyExchanger (public key the 8 bytes "PUBKEY12" at 0x00bc6f18, shared key "PRIVKEY!") with
// TRasa::PassThroughEncryptor: messages are plain. The game server uses TRasa::DHKeyExchanger with TRasa::BlowfishLiteEncryptor
// (GameCrypt.h): the client's public key goes out plain, "ENC OK" and every message after it are encrypted.
#pragma once

#include "GameCrypt.h"
#include "Socket.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace net {

class TrConnection {
public:
    enum class Event { Connected, ConnectionFailed, Disconnected };
    enum class Exchange { Null, DiffieHellman };

    bool connect(const std::string& host, int port, Exchange exchange = Exchange::Null);
    void close();
    // Up: negotiated ("ENC OK" received) and not yet reported down.
    bool connected() const { return up_; }
    void poll();
    void send(const std::vector<uint8_t>& message);
    bool popEvent(Event& out);
    bool popMessage(std::vector<uint8_t>& out);
    // The server's address as the TCP connection sees it (for AuthOnConnectedToQueue).
    const std::string& host() const { return host_; }
    int port() const { return port_; }

private:
    void handleFrame(std::vector<uint8_t>& body);

    enum class Stage { Idle, Connecting, AwaitServerKey, AwaitAck, Up };
    TcpConnection socket_;
    Stage stage_ = Stage::Idle;
    bool up_ = false, downQueued_ = false;
    Exchange exchange_ = Exchange::Null;
    bool encrypted_ = false;
    GameCipher cipher_;
    std::string host_;
    int port_ = 0;
    std::deque<Event> events_;
    std::deque<std::vector<uint8_t>> messages_;
};

}  // namespace net
