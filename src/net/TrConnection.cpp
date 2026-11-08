#include "TrConnection.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace net {

namespace {

bool logging() {
    static const bool on = [] {
        const char* v = std::getenv("OPENRASA_NETLOG");
        return v && *v == '1';
    }();
    return on;
}

uint32_t readU32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}

const char kNullPublicKey[] = "PUBKEY12";   // TRasa::NullKeyExchanger (0x00bc6f18), 8 bytes

}  // namespace

bool TrConnection::connect(const std::string& host, int port, Exchange exchange) {
    socket_.close();
    exchange_ = exchange;
    encrypted_ = false;
    events_.clear();
    messages_.clear();
    up_ = downQueued_ = false;
    host_ = host;
    port_ = port;
    if (!socket_.connect(host, port)) {
        stage_ = Stage::Idle;
        return false;
    }
    stage_ = Stage::Connecting;
    if (logging()) std::printf("[net] tcp: connecting to %s:%d\n", host.c_str(), port);
    return true;
}

void TrConnection::close() {
    const bool tell = up_ && !downQueued_;
    socket_.close();
    stage_ = Stage::Idle;
    up_ = false;
    if (tell) {
        downQueued_ = true;
        events_.push_back(Event::Disconnected);
    }
}

void TrConnection::send(const std::vector<uint8_t>& message) {
    std::vector<uint8_t> frame;
    const std::vector<uint8_t> body = encrypted_ ? cipher_.encrypt(message.data(), message.size()) : message;
    putU32(frame, uint32_t(body.size()));
    frame.insert(frame.end(), body.begin(), body.end());
    socket_.send(frame.data(), frame.size());
}

void TrConnection::poll() {
    if (stage_ == Stage::Idle) return;
    socket_.poll();
    if (stage_ == Stage::Connecting) {
        if (socket_.state() == TcpConnection::State::Connected) {
            stage_ = Stage::AwaitServerKey;
        } else if (socket_.state() == TcpConnection::State::Failed) {
            stage_ = Stage::Idle;
            events_.push_back(Event::ConnectionFailed);
            if (logging()) std::printf("[net] tcp: connection failed\n");
            return;
        }
    }
    std::vector<uint8_t>& in = socket_.received();
    while (in.size() >= 4 && stage_ != Stage::Idle) {
        const size_t length = readU32(in.data());
        if (in.size() < 4 + length) break;
        std::vector<uint8_t> body(in.begin() + 4, in.begin() + 4 + length);
        in.erase(in.begin(), in.begin() + 4 + length);
        handleFrame(body);
    }
    if (socket_.dropped() && stage_ != Stage::Idle) {
        const bool wasUp = up_ && !downQueued_;
        stage_ = Stage::Idle;
        if (wasUp) {
            downQueued_ = true;
            events_.push_back(Event::Disconnected);
        } else if (!up_) {
            events_.push_back(Event::ConnectionFailed);   // closed during the negotiation
        }
        if (logging()) std::printf("[net] tcp: closed by the server\n");
    }
}

void TrConnection::handleFrame(std::vector<uint8_t>& body) {
    switch (stage_) {
    case Stage::AwaitServerKey: {
        if (body.size() < 12) {
            std::fprintf(stderr, "[net] tcp: invalid packet received during negotiation\n");
            close();
            return;
        }
        const uint32_t keyLength = readU32(&body[0]), primeLength = readU32(&body[4]), generatorLength = readU32(&body[8]);
        if (uint64_t(keyLength) + primeLength + generatorLength + 12 != body.size()) {
            std::fprintf(stderr, "[net] tcp: bad negotiation message\n");
            close();
            return;
        }
        if (logging())
            std::printf("[net] tcp: server key %u bytes, prime %u, generator %u\n", keyLength, primeLength, generatorLength);
        std::vector<uint8_t> reply;
        if (exchange_ == Exchange::Null) {
            // NullKeyExchanger: the prime and generator are stored, its own public key is fixed.
            putU32(reply, 8);
            reply.insert(reply.end(), kNullPublicKey, kNullPublicKey + 8);
            send(reply);
        } else {
            const std::vector<uint8_t> serverKey(body.begin() + 12, body.begin() + 12 + keyLength);
            const std::vector<uint8_t> prime(body.begin() + 12 + keyLength, body.begin() + 12 + keyLength + primeLength);
            const std::vector<uint8_t> generator(body.begin() + 12 + keyLength + primeLength, body.end());
            DhExchange dh;
            dh.start(prime, generator);
            const std::vector<uint8_t> mine = dh.publicKey();
            const std::vector<uint8_t> shared = dh.sharedKey(serverKey);
            if (mine.empty() || shared.empty()) {
                std::fprintf(stderr, "[net] tcp: set prime and generator in key exchanger failed - bad prime?\n");
                close();
                return;
            }
            if (const char* dbg = std::getenv("OPENRASA_NETDEBUG"); dbg && *dbg == '1') {
                auto hex = [](const char* name, const std::vector<uint8_t>& v) {
                    std::printf("[net] dh %s ", name);
                    for (uint8_t c : v) std::printf("%02x", c);
                    std::printf("\n");
                };
                hex("prime", prime);
                hex("generator", generator);
                hex("server", serverKey);
                hex("secret", dh.secret);
                hex("public", mine);
                hex("shared", shared);
            }
            putU32(reply, uint32_t(mine.size()));
            reply.insert(reply.end(), mine.begin(), mine.end());
            send(reply);   // plain: the encryptor starts after it
            cipher_.init(shared);
            encrypted_ = true;
        }
        stage_ = Stage::AwaitAck;
        return;
    }
    case Stage::AwaitAck:
        if (encrypted_) {
            if (const char* dbg = std::getenv("OPENRASA_NETDEBUG"); dbg && *dbg == '1') {
                std::printf("[net] dh ack ");
                for (uint8_t c : body) std::printf("%02x", c);
                std::printf("\n");
            }
            std::vector<uint8_t> plain;
            if (!cipher_.decrypt(body, plain)) plain.clear();
            if (const char* dbg = std::getenv("OPENRASA_NETDEBUG"); dbg && *dbg == '1') {
                std::printf("[net] dh plain ");
                for (uint8_t c : plain) std::printf("%02x", c);
                std::printf("\n");
            }
            body.swap(plain);
        }
        if (body.size() != 6 || std::memcmp(body.data(), "ENC OK", 6) != 0) {
            std::fprintf(stderr, "[net] tcp: bad OK packet received during negotiation\n");
            close();
            return;
        }
        stage_ = Stage::Up;
        up_ = true;
        events_.push_back(Event::Connected);
        if (logging()) std::printf("[net] tcp: negotiated\n");
        return;
    case Stage::Up:
        if (encrypted_) {
            std::vector<uint8_t> plain;
            if (!cipher_.decrypt(body, plain)) {
                std::fprintf(stderr, "[net] tcp: a message failed to decrypt\n");
                return;
            }
            body.swap(plain);
        }
        messages_.push_back(std::move(body));
        return;
    default:
        return;
    }
}

bool TrConnection::popEvent(Event& out) {
    if (events_.empty()) return false;
    out = events_.front();
    events_.pop_front();
    if (out == Event::Disconnected) up_ = false;
    return true;
}

bool TrConnection::popMessage(std::vector<uint8_t>& out) {
    if (messages_.empty()) return false;
    out = std::move(messages_.front());
    messages_.pop_front();
    return true;
}

}  // namespace net
