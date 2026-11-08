// GameProtocol.h - the game server's message layer over TrConnection (tabula_rasa.exe TRasa::GameClientNetManager and the
// TRasa::ClientMsg_* classes; layouts as Rasa.NET Rasa.Game/Packets/Protocol/ProtocolPacket.cs and Memory/ProtocolBuffer*, which the
// real client talks to).
//
// The decrypted TCPConnection messages form one byte stream of packets:
//   u16 size (the whole packet), u8 channel, u8 0; channel != 0: u32 sequence, u32 0xDEADBEEF, u32 0 (channel 0xFF: timeout check, no
//   body); then a header block - flags byte (bits 0..5: 1 debug bytes, 2 "don't fragment" = fixed-size integers, else 7-bit varints;
//   bits 6..7 must be 0), the u16 type << 1 | compressed, an XOR check of the block's length - and a body block: [compressed: u8 1, u32
//   inflated size, raw deflate] flags byte, [debug 41], subtype byte (for typed messages), the message's fields, [debug 42], XOR check.
// The XOR check is the four bytes of the length XORed together.
// Message types (ClientMessageOpcode / the exe's ClientMsg_* vtables and OnReceiveMessage FUN_00529870): 1 Login, 2 LoginResponse,
// 3 Move, 4 MoveObject, 5 NegotiateMoveChannel, 6 CallServerMethod, 7 CallMethod, 8 Ping.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace net {

// Reads a message body's fields with its flags (ProtocolBufferReader).
class ProtocolReader {
public:
    ProtocolReader(const uint8_t* data, size_t length, uint8_t flags) : p_(data), end_(data + length), flags_(flags) {}
    bool ok() const { return ok_; }
    size_t left() const { return size_t(end_ - p_); }
    const uint8_t* position() const { return p_; }
    void debugByte(uint8_t expected);
    uint8_t byte();
    uint16_t ushort();
    uint32_t uint();
    int32_t int32();
    uint64_t ulong();
    uint32_t count();
    std::string string();
    std::vector<uint8_t> array();
    uint8_t flags() const { return flags_; }

private:
    uint64_t sevenBit(int maxBytes);
    const uint8_t* p_;
    const uint8_t* end_;
    uint8_t flags_;
    bool ok_ = true;
};

// Builds a message body (ProtocolBufferWriter with "don't fragment": fixed-size little-endian integers).
class ProtocolWriter {
public:
    void byte(uint8_t v) { out.push_back(v); }
    void uint(uint32_t v);
    void count(uint32_t n);
    void string(const std::string& s);
    void array(const std::vector<uint8_t>& bytes);   // count, then the bytes
    std::vector<uint8_t> out;
};

struct GameMessage {
    uint16_t type = 0;
    uint8_t channel = 0;
    uint8_t flags = 0;
    uint32_t sequence = 0;
    std::vector<uint8_t> body;   // after the body block's flags byte (the subtype first, for typed messages), before its XOR check
};

// A whole packet for a message: fields written after the subtype (subtype < 0: none).
std::vector<uint8_t> encodeGameMessage(uint16_t type, int subtype, const std::vector<uint8_t>& fields);

class GameStream {
public:
    void append(const std::vector<uint8_t>& data) { buffer_.insert(buffer_.end(), data.begin(), data.end()); }
    // The next complete packet; false when none is complete yet. A malformed one sets error() (the stream cannot be resynchronised).
    bool next(GameMessage& out);
    bool error() const { return error_; }
    const std::string& errorText() const { return errorText_; }

private:
    bool fail(const std::string& text);
    std::vector<uint8_t> buffer_;
    bool error_ = false;
    std::string errorText_;
};

}  // namespace net
