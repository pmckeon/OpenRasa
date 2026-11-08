#include "AuthClient.h"

#include "AuthCrypt.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace net {

namespace {

struct Reader {
    const uint8_t* p;
    size_t left;
    bool ok = true;
    uint32_t u(int bytes) {
        if (left < size_t(bytes)) {
            ok = false;
            return 0;
        }
        uint32_t v = 0;
        for (int i = 0; i < bytes; ++i) v |= uint32_t(p[i]) << (8 * i);
        p += bytes;
        left -= bytes;
        return v;
    }
};

void put(std::vector<uint8_t>& out, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(uint8_t(v >> (8 * i)));
}

bool logging() {
    static const bool on = [] {
        const char* v = std::getenv("OPENRASA_NETLOG");
        return v && *v == '1';
    }();
    return on;
}

}  // namespace

void AuthClient::reset() {
    socket_.close();
    connecting_ = reportedUp_ = disconnectQueued_ = false;
}

bool AuthClient::connect(const std::string& host, int port) {
    reset();
    firstFrame_ = true;
    if (!socket_.connect(host, port)) return false;
    connecting_ = true;
    if (logging()) std::printf("[net] auth: connecting to %s:%d\n", host.c_str(), port);
    return true;
}

// AuthClose: the connection ends and, if the scripts were told it was up, they hear AuthOnDisconnected (the scripts reset their
// "disconnect requested" flag there; UNVERIFIED that the exe reports a requested close this way).
void AuthClient::close() {
    const bool tell = reportedUp_ && !disconnectQueued_;
    socket_.close();
    connecting_ = reportedUp_ = false;
    if (tell) {
        disconnectQueued_ = true;
        events_.push_back({AuthEvent::Kind::Disconnected, {}, {}});
    }
}

void AuthClient::sendPacket(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> body = authEncrypt(payload.data(), payload.size());
    std::vector<uint8_t> frame;
    put(frame, uint32_t(body.size() + 2), 2);
    frame.insert(frame.end(), body.begin(), body.end());
    socket_.send(frame.data(), frame.size());
}

bool AuthClient::login(const std::string& user, const std::string& password, uint32_t gameId, uint16_t cdKey) {
    // FUN_00aa3670: refused (-1) for an empty name or password, a name of 15 characters or more, a password of 17 or more.
    if (!connected() || user.empty() || password.empty() || user.size() >= 15 || password.size() >= 17) return false;
    uint8_t block[30] = {};
    std::memcpy(block, user.data(), user.size());   // strncpy 14
    std::memcpy(block + 14, password.data(), password.size());
    static const uint8_t key[8] = {'T', 'E', 'S', 'T', 0, 0, 0, 0};
    desEncryptEcb(key, block, 24);
    std::vector<uint8_t> p{0x00};
    p.insert(p.end(), block, block + 30);
    put(p, gameId, 4);
    put(p, cdKey, 2);
    sendPacket(p);
    return true;
}

bool AuthClient::requestServerList(int kind) {
    if (!connected()) return false;
    std::vector<uint8_t> p{0x05};
    put(p, sessionId1_, 4);
    put(p, sessionId2_, 4);
    p.push_back(uint8_t(kind));
    sendPacket(p);
    return true;
}

bool AuthClient::selectServer(int serverId) {
    if (!connected()) return false;
    std::vector<uint8_t> p{0x02};
    put(p, sessionId1_, 4);
    put(p, sessionId2_, 4);
    p.push_back(uint8_t(serverId));
    sendPacket(p);
    return true;
}

bool AuthClient::logout() {
    if (!connected()) return false;
    std::vector<uint8_t> p{0x03};
    put(p, sessionId1_, 4);
    put(p, sessionId2_, 4);
    sendPacket(p);
    return true;
}

void AuthClient::poll() {
    socket_.poll();
    if (connecting_ && socket_.state() == TcpConnection::State::Connected) {
        connecting_ = false;
        reportedUp_ = true;
        disconnectQueued_ = false;
        events_.push_back({AuthEvent::Kind::Connected, {}, {}});
        if (logging()) std::printf("[net] auth: connected\n");
    } else if (connecting_ && socket_.state() == TcpConnection::State::Failed) {
        connecting_ = false;
        events_.push_back({AuthEvent::Kind::ConnectionFailed, {}, {}});
        if (logging()) std::printf("[net] auth: connection failed\n");
    }
    std::vector<uint8_t>& in = socket_.received();
    while (in.size() >= 2) {
        size_t length = size_t(in[0]) | size_t(in[1]) << 8;
        if (length <= 2) {
            std::fprintf(stderr, "[net] auth: bad frame length %zu\n", length);
            close();
            break;
        }
        if (in.size() < length) break;
        std::vector<uint8_t> body(in.begin() + 2, in.begin() + length);
        in.erase(in.begin(), in.begin() + length);
        if (firstFrame_) {
            firstFrame_ = false;   // ProtocolVersion, plain
            handlePacket(body.data(), body.size());
            continue;
        }
        if (!authDecrypt(body)) {
            std::fprintf(stderr, "[net] auth: a frame failed its checksum\n");
            continue;
        }
        handlePacket(body.data(), body.size() - 8);
    }
    if (reportedUp_ && !disconnectQueued_ && socket_.dropped()) {
        disconnectQueued_ = true;
        events_.push_back({AuthEvent::Kind::Disconnected, {}, {}});
        if (logging()) std::printf("[net] auth: disconnected\n");
    }
}

void AuthClient::handlePacket(const uint8_t* data, size_t length) {
    if (length == 0) return;
    Reader r{data + 1, length - 1};
    const uint8_t opcode = data[0];
    if (logging()) std::printf("[net] auth: packet 0x%02X, %zu bytes\n", opcode, length);
    AuthEvent e{};
    switch (opcode) {
    case 0x00:   // ProtocolVersion: the one-time key and version are not used by the client's scripts
        return;
    case 0x01: e = {AuthEvent::Kind::LoginFail, {int64_t(r.u(1))}, {}}; break;
    case 0x02: e = {AuthEvent::Kind::BlockedAccount, {int64_t(r.u(4))}, {}}; break;
    case 0x03: {
        sessionId1_ = r.u(4);
        sessionId2_ = r.u(4);
        r.u(4);
        r.u(4);   // update keys
        e.kind = AuthEvent::Kind::LoginOk;
        for (int i = 0; i < 5; ++i) e.values.push_back(int32_t(r.u(4)));
        break;
    }
    case 0x04: {
        e.kind = AuthEvent::Kind::ServerList;
        const int count = int(r.u(1));
        const int last = int(r.u(1));
        for (int i = 0; i < count && r.ok; ++i) {
            ServerEntry s;
            s.id = int(r.u(1));
            uint32_t a = r.u(4);
            char text[20];
            std::snprintf(text, sizeof text, "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
            s.address = text;
            s.port = int32_t(r.u(4));
            s.ageLimit = int(r.u(1));
            s.pkFlag = int(r.u(1));
            s.players = int(r.u(2));
            s.maxPlayers = int(r.u(2));
            s.status = int(r.u(1));
            e.servers.push_back(s);
        }
        e.values.push_back(last);
        break;
    }
    case 0x05: e = {AuthEvent::Kind::ServerFail, {int64_t(r.u(1))}, {}}; break;
    case 0x06: e = {AuthEvent::Kind::PlayFail, {int64_t(r.u(1))}, {}}; break;
    case 0x07: {
        const uint32_t key = r.u(4), user = r.u(4), server = r.u(1);
        e = {AuthEvent::Kind::PlayOk, {int32_t(key), int32_t(user), int64_t(server)}, {}};
        break;
    }
    case 0x08: e = {AuthEvent::Kind::AccountKicked, {int64_t(r.u(1))}, {}}; break;
    case 0x0C: {
        const uint32_t key = r.u(4), account = r.u(4), server = r.u(1);
        e = {AuthEvent::Kind::HandoffToQueue, {int32_t(key), int32_t(account), int64_t(server)}, {}};
        break;
    }
    default:
        std::fprintf(stderr, "[net] auth: packet 0x%02X not handled\n", opcode);
        return;
    }
    if (!r.ok) {
        std::fprintf(stderr, "[net] auth: packet 0x%02X too short\n", opcode);
        return;
    }
    events_.push_back(e);
}

bool AuthClient::popEvent(AuthEvent& out) {
    if (events_.empty()) return false;
    out = events_.front();
    events_.pop_front();
    if (out.kind == AuthEvent::Kind::Disconnected) reportedUp_ = false;
    return true;
}

}  // namespace net
