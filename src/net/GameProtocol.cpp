#include "GameProtocol.h"

#include <zlib.h>

#include <cstring>

namespace net {

namespace {

uint8_t xorCheck(size_t length) {
    const uint32_t v = uint32_t(length);
    return uint8_t((v & 0xFF) ^ ((v >> 8) & 0xFF) ^ ((v >> 16) & 0xFF) ^ ((v >> 24) & 0xFF));
}

constexpr uint8_t kDebug = 0x01, kDontFragment = 0x02;

}  // namespace

// ---- ProtocolReader -------------------------------------------------------------------------------------------------------------------

void ProtocolReader::debugByte(uint8_t expected) {
    if (!(flags_ & kDebug)) return;
    if (byte() != expected) ok_ = false;
}

uint8_t ProtocolReader::byte() {
    if (p_ >= end_) {
        ok_ = false;
        return 0;
    }
    return *p_++;
}

uint64_t ProtocolReader::sevenBit(int maxBytes) {
    uint64_t v = 0;
    for (int i = 0; i < maxBytes; ++i) {
        const uint8_t b = byte();
        v |= uint64_t(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) break;
    }
    return v;
}

uint16_t ProtocolReader::ushort() {
    debugByte(5);
    if (!(flags_ & kDontFragment)) return uint16_t(sevenBit(4));
    uint16_t v = byte();
    v |= uint16_t(byte()) << 8;
    return v;
}

uint32_t ProtocolReader::uint() {
    debugByte(7);
    if (!(flags_ & kDontFragment)) return uint32_t(sevenBit(6));
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(byte()) << (8 * i);
    return v;
}

int32_t ProtocolReader::int32() {
    debugByte(6);
    if (!(flags_ & kDontFragment)) {
        ok_ = false;   // the signed 7-bit form (Rasa.NET ReadIntBySevenBits) is not needed by the servers seen; not done
        return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(byte()) << (8 * i);
    return int32_t(v);
}

uint64_t ProtocolReader::ulong() {
    debugByte(9);
    if (!(flags_ & kDontFragment)) return sevenBit(10);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(byte()) << (8 * i);
    return v;
}

uint32_t ProtocolReader::count() {
    debugByte(203);
    if (p_ >= end_) {
        ok_ = false;
        return 0;
    }
    const int extra = *p_ >> 6;
    uint8_t b[4] = {};
    for (int i = 0; i <= extra; ++i) b[i] = byte();
    return uint32_t(b[0] & 0x3F) | uint32_t(b[1]) << 6 | uint32_t(b[2]) << 14 | uint32_t(b[3]) << 22;
}

std::string ProtocolReader::string() {
    debugByte(13);
    const uint32_t n = count();
    if (n > left()) {
        ok_ = false;
        return {};
    }
    std::string s(reinterpret_cast<const char*>(p_), n);
    p_ += n;
    return s;
}

std::vector<uint8_t> ProtocolReader::array() {
    debugByte(1);
    const uint32_t n = count();
    if (n > left()) {
        ok_ = false;
        return {};
    }
    std::vector<uint8_t> v(p_, p_ + n);
    p_ += n;
    return v;
}

// ---- ProtocolWriter -------------------------------------------------------------------------------------------------------------------

void ProtocolWriter::uint(uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}

void ProtocolWriter::count(uint32_t n) {
    uint8_t b[4] = {uint8_t(n & 0x3F), uint8_t(n >> 6), uint8_t(n >> 14), uint8_t(n >> 22)};
    const int extra = b[3] ? 3 : b[2] ? 2 : b[1] ? 1 : 0;
    b[0] |= uint8_t(extra << 6);
    out.insert(out.end(), b, b + extra + 1);
}

void ProtocolWriter::string(const std::string& s) {
    count(uint32_t(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

void ProtocolWriter::array(const std::vector<uint8_t>& bytes) {
    count(uint32_t(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

std::vector<uint8_t> encodeGameMessage(uint16_t type, int subtype, const std::vector<uint8_t>& fields) {
    std::vector<uint8_t> p = {0, 0, 0, 0};   // size, channel 0, padding
    // Header block: flags, type << 1 (not compressed), XOR check of the block's 3 bytes.
    p.push_back(kDontFragment);
    const uint16_t t = uint16_t(type << 1);
    p.push_back(uint8_t(t));
    p.push_back(uint8_t(t >> 8));
    p.push_back(xorCheck(3));
    // Body block.
    const size_t bodyStart = p.size();
    p.push_back(kDontFragment);
    if (subtype >= 0) p.push_back(uint8_t(subtype));
    p.insert(p.end(), fields.begin(), fields.end());
    p.push_back(xorCheck(p.size() - bodyStart));
    p[0] = uint8_t(p.size());
    p[1] = uint8_t(p.size() >> 8);
    return p;
}

// ---- GameStream -----------------------------------------------------------------------------------------------------------------------

bool GameStream::fail(const std::string& text) {
    error_ = true;
    errorText_ = text;
    return false;
}

bool GameStream::next(GameMessage& out) {
    if (error_ || buffer_.size() < 4) return false;
    const size_t size = size_t(buffer_[0]) | size_t(buffer_[1]) << 8;
    if (size < 4) return fail("protocol packet size below its header");
    if (buffer_.size() < size) return false;
    std::vector<uint8_t> packet(buffer_.begin(), buffer_.begin() + size);
    buffer_.erase(buffer_.begin(), buffer_.begin() + size);

    out = GameMessage{};
    out.channel = packet[2];
    size_t at = 4;
    if (out.channel == 0xFF) {
        out.type = 0;
        return true;   // the server's timeout check: nothing else in it
    }
    if (out.channel != 0) {
        if (size < 16) return fail("short sequenced packet");
        out.sequence = uint32_t(packet[4]) | uint32_t(packet[5]) << 8 | uint32_t(packet[6]) << 16 | uint32_t(packet[7]) << 24;
        at = 16;
    }
    // Header block.
    const size_t headerStart = at;
    if (at >= size) return fail("no header block");
    const uint8_t headerFlags = packet[at++];
    if (headerFlags >> 6) return fail("unsupported protocol flags");
    ProtocolReader header(packet.data() + at, size - at, headerFlags);
    header.debugByte(41);
    const uint16_t typeValue = header.ushort();
    header.debugByte(42);
    if (!header.ok()) return fail("bad header block");
    at = size_t(header.position() - packet.data());
    if (at >= size || packet[at] != xorCheck(at - headerStart)) return fail("header XOR check failed");
    ++at;
    out.type = uint16_t(typeValue >> 1);
    const bool compressed = typeValue & 1;

    std::vector<uint8_t> block;
    if (compressed) {
        if (at >= size) return fail("no compression byte");
        const uint8_t kind = packet[at++];
        if (kind >= 2) return fail("invalid compression type");
        if (kind == 1) {
            if (at + 4 > size) return fail("no inflated size");
            const uint32_t inflated = uint32_t(packet[at]) | uint32_t(packet[at + 1]) << 8 | uint32_t(packet[at + 2]) << 16 |
                                      uint32_t(packet[at + 3]) << 24;
            at += 4;
            if (inflated == 0 || inflated > 4 * 65535) return fail("bad inflated size");
            block.resize(inflated);
            z_stream z{};
            if (inflateInit2(&z, -15) != Z_OK) return fail("inflate init");
            z.next_in = packet.data() + at;
            z.avail_in = uInt(size - at);
            z.next_out = block.data();
            z.avail_out = uInt(block.size());
            const int r = inflate(&z, Z_FINISH);
            const size_t produced = block.size() - z.avail_out;
            inflateEnd(&z);
            if ((r != Z_STREAM_END && r != Z_OK) || produced != inflated) return fail("inflate failed");
        } else {
            block.assign(packet.begin() + at, packet.end());
        }
    } else {
        block.assign(packet.begin() + at, packet.end());
    }
    // Body block: flags, [debug 41], ..., [debug 42], XOR check of everything before it in the block.
    if (block.size() < 2) return fail("short body block");
    out.flags = block[0];
    if (out.flags >> 6) return fail("unsupported body flags");
    if (block.back() != xorCheck(block.size() - 1)) return fail("body XOR check failed");
    size_t from = 1, to = block.size() - 1;
    if (out.flags & kDebug) {
        if (to - from < 2 || block[from] != 41 || block[to - 1] != 42) return fail("body debug bytes");
        ++from;
        --to;
    }
    out.body.assign(block.begin() + from, block.begin() + to);
    return true;
}

}  // namespace net
