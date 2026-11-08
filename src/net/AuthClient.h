// AuthClient.h - the connection to the authentication server (tabula_rasa.exe's ncAuthClient).
//
// Frames: a little-endian u16 length that counts itself, then the body (Rasa.NET LengthedSocket, SizeType.Word). The server's first
// frame, ProtocolVersion, is plain; every other frame both ways is encrypted (AuthCrypt.h). Packets (opcode byte first; layouts as
// Rasa.NET's Rasa.Auth/Packets, which the real client talks to):
//   client: 0x00 Login (30-byte name / password block, first 24 bytes DES-encrypted; u32 game id; u16 CD key), 0x02 AboutToPlay (u32
//           session 1, u32 session 2, u8 server), 0x03 Logout (sessions), 0x05 ServerListExt (sessions, u8 list kind);
//   server: 0x00 ProtocolVersion (u32 one-time key, u32 version), 0x01 LoginFail (u8), 0x02 BlockedAccount (u32), 0x03 LoginOk (u32
//           session 1, session 2, update key 1, 2, pay status, remaining time, quota time, warn flag, login flag, u8), 0x04
//           SendServerListExt (u8 count, u8 last server, per server: u8 id, 4 address bytes, i32 port, u8 age limit, u8 PK flag, u16
//           players, u16 max players, u8 status), 0x05 SendServerListFail (u8), 0x06 PlayFail (u8), 0x07 PlayOk (u32 one-time key, u32
//           user id, u8 server), 0x08 AccountKicked (u8), 0x0C HandoffToQueue (u32 key, u32 account, u8 server).
// The engine reports them to the scripts' input state (ncAuthClient's listener, vtable 0x00be18d4: AuthOnConnected,
// AuthOnDisconnected, AuthOnConnectionFailed, AuthRecvLoginFail "(i)", AuthRecvBlockedAccount "(i)", AuthRecvLoginOk "(iiiii)" = pay
// status .. login flag (the session ids are kept for later requests), AuthRecvServerListEx "(Oi)" = [(id, "a.b.c.d", port, (age,
// pk, players, max, status))], last server, AuthRecvServerFail / AuthRecvPlayFail / AuthRecvAccountKicked "(i)", AuthRecvPlayOk
// "(iii)" = key, user id, server, AuthRecvHandoffToQueueMsg "(iii)").
#pragma once

#include "Socket.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace net {

struct ServerEntry {
    int id = 0;
    std::string address;   // "a.b.c.d"
    int port = 0;
    int ageLimit = 0, pkFlag = 0, players = 0, maxPlayers = 0, status = 0;
};

struct AuthEvent {
    enum class Kind {
        Connected, ConnectionFailed, Disconnected, LoginOk, LoginFail, BlockedAccount, ServerList, ServerFail, PlayOk, PlayFail,
        AccountKicked, HandoffToQueue
    };
    Kind kind;
    std::vector<int64_t> values;          // the callback's integer arguments, in order
    std::vector<ServerEntry> servers;     // ServerList
};

class AuthClient {
public:
    bool connect(const std::string& host, int port);
    void close();
    // ncAuthClient's connected state (FUN_00aa4240 == 2), as the scripts see it: from the Connected event until the Disconnected
    // event has been taken or close() was called. A server that answers and then closes (LoginFail) is still connected while the
    // scripts handle the answer - login.py's AuthRecvLoginFail calls RequestAuthDisconnect, which only closes (and marks the coming
    // AuthOnDisconnected as requested, so no "disconnected" dialog) while IsAuthConnected().
    bool connected() const { return reportedUp_; }
    bool login(const std::string& user, const std::string& password, uint32_t gameId, uint16_t cdKey);
    bool requestServerList(int kind);
    bool selectServer(int serverId);
    bool logout();
    // Advances the connection; events for the scripts are queued.
    void poll();
    bool popEvent(AuthEvent& out);

private:
    void sendPacket(const std::vector<uint8_t>& payload);
    void handlePacket(const uint8_t* data, size_t length);

    TcpConnection socket_;
    void reset();
    bool connecting_ = false, firstFrame_ = true;
    bool reportedUp_ = false, disconnectQueued_ = false;
    uint32_t sessionId1_ = 0, sessionId2_ = 0;
    std::deque<AuthEvent> events_;
};

}  // namespace net
