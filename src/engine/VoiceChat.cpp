// VoiceChat.cpp - see VoiceChat.h. Each part names the exe function it follows.
#include "VoiceChat.h"

#include "../net/Socket.h"

#include <SDL3/SDL.h>
#include <speex/speex.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

namespace engine::voice {
namespace {

ScriptCaller scriptCaller = nullptr;
bool logging() {
    static const bool on = std::getenv("OPENRASA_VOICELOG") != nullptr;
    return on;
}
void call(const char* method, PyObject* args, int delayMs) {
    if (logging()) std::printf("[voice] -> party.%s\n", method);
    if (scriptCaller) scriptCaller(method, args, delayMs);
    else Py_XDECREF(args);
}

// ---- The datagram layer (netBaseUDPConnection / netUDPOutConnection) ----------------------------------------------------------
// Datagram: u32 Adler-32 (zlib's, FUN_00a94cc0 -> FUN_00a94a80: adler32(adler32(0, 0, 0), ...)) of everything after it, u8 flags,
// u8 0, u16 0, then at most 1004 bytes (0x3ec, the send limit in FUN_00a80d80). Flag bits (0x00bc3c54 ..): 1, 2, 4, 8.
// cryNullKeyExchanger / cryPassThroughEncryptor: nothing is encrypted; the client's "public key" is the 8 bytes "PUBKEY12"
// (0x00bc5ae8, slot 4 FUN_00aa0bf0).
uint32_t adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

constexpr size_t kMaxPayload = 1004;   // 0x00bc3c60
constexpr uint32_t kResendMs = 500;    // 0x00bc3c6c
constexpr uint32_t kGiveUpMs = 15000;  // 0x00bc3c70

struct Connection {
    // netBaseUDPConnection + 0x28: 0 waiting for the key offer, 1 waiting for the key acknowledgement, 5 connected, 6 closing,
    // 7 closed by the server, 8 gone.
    int state = 8;
    net::UdpConnection socket;
    uint64_t opened = 0;    // the timer at + 0x38 (started when the connection opens and when the key is sent)
    uint64_t resent = 0;    // the timer at + 0x60 (the last hello / key)

    bool sendDatagram(uint8_t flags, const uint8_t* payload, size_t length) {
        std::vector<uint8_t> d(8 + length);
        d[4] = flags;
        if (length) std::memcpy(d.data() + 8, payload, length);
        const uint32_t check = adler32(d.data() + 4, d.size() - 4);
        std::memcpy(d.data(), &check, 4);
        return socket.send(d.data(), d.size());
    }
    // A datagram whose checksum holds (FUN_00a80a70 / FUN_00a808f0); false when none is waiting.
    bool receive(uint8_t& flags, std::vector<uint8_t>& payload, size_t& whole) {
        std::vector<uint8_t> d;
        while (socket.receive(d)) {
            if (d.size() < 8) continue;
            uint32_t check;
            std::memcpy(&check, d.data(), 4);
            if (check != adler32(d.data() + 4, d.size() - 4)) {
                if (logging()) std::printf("[voice] packet digest check failed\n");
                continue;
            }
            flags = d[4];
            whole = d.size();
            payload.assign(d.begin() + 8, d.end());
            return true;
        }
        return false;
    }
    // FUN_00a81c10: flag 1 and 1004 zero bytes.
    bool sendHello() {
        resent = SDL_GetTicks();
        const std::vector<uint8_t> zeros(kMaxPayload, 0);
        return sendDatagram(1, zeros.data(), zeros.size());
    }
    // FUN_00a81ac0: flag 4, u32 key length, the key, zero padding to 1004 bytes.
    void sendKey() {
        std::vector<uint8_t> p(kMaxPayload, 0);
        const uint32_t length = 8;
        std::memcpy(p.data(), &length, 4);
        std::memcpy(p.data() + 4, "PUBKEY12", 8);
        sendDatagram(4, p.data(), p.size());
        resent = SDL_GetTicks();
    }
    // FUN_00a810d0(param): flag 8 (| 1 when param is set) and 1004 zero bytes - closing, and its acknowledgement.
    void sendClose(bool acknowledge) {
        const std::vector<uint8_t> zeros(kMaxPayload, 0);
        sendDatagram(acknowledge ? 9 : 8, zeros.data(), zeros.size());
    }

    // FUN_00a81da0 + the first hello (state 0).
    bool open(const std::string& host, int port) {
        if (!socket.open(host, port)) return false;
        state = 0;
        opened = SDL_GetTicks();
        if (!sendHello()) {
            state = 8;
            return false;
        }
        return true;
    }

    // The negotiation step (FUN_00a82170), once a frame while connecting. False when the connection has failed.
    bool negotiate() {
        const uint64_t now = SDL_GetTicks();
        uint8_t flags = 0;
        std::vector<uint8_t> payload;
        size_t whole = 0;
        if (state == 0) {
            bool waiting = true;
            if (receive(flags, payload, whole)) {
                if (flags == 7 && payload.size() > 12) {
                    // The key offer: u32 generator, prime and public key lengths (1..16, 1..64, 1..128: 0x00bc3c48 / 44 / 4c), then
                    // the three. The null exchanger takes any; the client answers with its key.
                    uint32_t g, p, k;
                    std::memcpy(&g, payload.data(), 4);
                    std::memcpy(&p, payload.data() + 4, 4);
                    std::memcpy(&k, payload.data() + 8, 4);
                    if (payload.size() < size_t(g) + p + k + 12 || g == 0 || g > 16 || p == 0 || p > 64 || k == 0 || k > 128) {
                        if (logging()) std::printf("[voice] key offer too short for key / prime / generator\n");
                    } else {
                        sendKey();
                        state = 1;
                        opened = SDL_GetTicks();
                        waiting = false;
                    }
                } else if (flags & 8) {
                    state = 8;
                    return false;
                }
            }
            if (waiting) {
                if (now - resent > kResendMs) {
                    if (!sendHello()) {
                        state = 8;
                        return false;
                    }
                } else if (now - opened > kGiveUpMs) {
                    state = 8;
                    return false;
                }
            }
            return true;
        }
        if (state == 1) {
            if (receive(flags, payload, whole)) {
                // FUN_00a815e0 / FUN_00a82170: only a datagram longer than its 8-byte header is looked at here (CMP with 0x00bc3c5c,
                // JBE at 0x00a82764) - an acknowledgement without a payload is ignored and the key sent again.
                if (whole > 8) {
                    if (flags == 6) {
                        sendDatagram(2, nullptr, 0);   // FUN_00a81050: flag 2, nothing after the header
                        state = 5;
                    } else if (flags == 7) {
                        sendKey();
                    } else if (flags & 8) {
                        state = 8;
                        return false;
                    }
                } else if (logging()) {
                    std::printf("[voice] a datagram without a payload (flags %x) ignored while waiting for the key acknowledgement\n", flags);
                }
                return true;
            }
            if (now - resent > kResendMs) {
                sendKey();
            } else if (now - opened > kGiveUpMs) {
                sendClose(false);
                state = 8;
                return false;
            }
            return true;
        }
        return state == 5;
    }

    // The connected receive (FUN_00a813d0): a message (flag 0, non-empty), or false. A ping (6) is answered with 2 (FUN_00a81050); a
    // close (bit 8) ends the connection (state 7, the close sent back with FUN_00a810d0(0)).
    bool message(std::vector<uint8_t>& out) {
        uint8_t flags = 0;
        size_t whole = 0;
        while (state == 5 && receive(flags, out, whole)) {
            if (flags == 0) {
                if (!out.empty()) return true;
                if (logging()) std::printf("[voice] empty packet received\n");
            } else if (flags & 8) {
                state = 7;
                sendClose(false);
                return false;
            } else if (flags == 6) {
                sendDatagram(2, nullptr, 0);
            } else if (logging()) {
                std::printf("[voice] packet header unknown flags (%x) set\n", flags);
            }
        }
        return false;
    }
    bool send(const std::vector<uint8_t>& message) { return state == 5 && sendDatagram(0, message.data(), message.size()); }
    // Closing from this side (slot 0x14 -> FUN_00a811c0: state 6, a close datagram). The exe then waits for the acknowledgement
    // (FUN_00a81200 case 1); the connection is dropped here at once.
    void close() {
        if (state == 5) {
            state = 6;
            sendClose(false);
        }
        state = 8;
        socket.close();
    }
};

// ---- Messages (Talkback; encoders 0xa83410 .. 0xa83ed0) ----------------------------------------------------------------------
struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v)), u8(uint8_t(v >> 8)); }
    void u32(uint32_t v) { u16(uint16_t(v)), u16(uint16_t(v >> 16)); }
    void u64(uint64_t v) { u32(uint32_t(v)), u32(uint32_t(v >> 32)); }
    void f32(float v) {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        u32(u);
    }
    void string(const std::string& s) {
        u16(uint16_t(s.size()));
        b.insert(b.end(), s.begin(), s.end());
    }
};
struct Reader {
    const std::vector<uint8_t>& b;
    size_t at = 0;
    bool ok = true;
    bool take(size_t n) {
        if (!ok || b.size() - at < n) return ok = false;
        return true;
    }
    uint8_t u8() { return take(1) ? b[at++] : 0; }
    uint16_t u16() {
        if (!take(2)) return 0;
        const uint16_t v = uint16_t(b[at] | b[at + 1] << 8);
        at += 2;
        return v;
    }
    uint32_t u32() { return u16() | uint32_t(u16()) << 16; }
    uint64_t u64() { return u32() | uint64_t(u32()) << 32; }
    float f32() {
        const uint32_t u = u32();
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    std::string string() {
        const uint16_t n = u16();
        if (!take(n)) return {};
        std::string s(b.begin() + at, b.begin() + at + n);
        at += n;
        return s;
    }
};

// ---- The codec (Talkback::SpeexCodec, FUN_00a86300) ---------------------------------------------------------------------------
// Codecs 3 / 4 / 5 are Speex narrowband with VBR quality 1 / 5 / 10 (+ 0xdc); 8000 Hz, 16-bit mono, 160 samples a frame; at most
// 62 bytes a frame (+ 0x18, the receive buffer per frame), 5 frames a packet (+ 0x24, FUN_00a8a720). The encoder: sampling rate
// 8000, bitrate 6400, VBR on, VAD = voice activation, DTX on, then the VBR quality.
constexpr int kRate = 8000;
constexpr int kFrameSamples = 160;
constexpr int kFramesPerPacket = 5;
bool isSpeex(int codec) { return codec >= 3 && codec <= 5; }
float speexQuality(int codec) { return codec == 3 ? 1.0f : codec == 4 ? 5.0f : 10.0f; }

struct Encoder {
    void* state = nullptr;
    SpeexBits bits{};
    bool activation = false;
    // The activity detector (FUN_00a86820, voice activation only): per frame the sum of |s[i] - s[i - 1]| over the samples read as
    // UNSIGNED 16-bit values (MOVZX, so a sign change counts ~65536); a ring of the last 20 sums; active from inactive when more
    // than 1 of them exceed level x 2000, inactive again when none do. Starts active without voice activation (+ 0x15, FUN_00a847e0).
    int level = 100;
    int ring[20] = {};
    int ringAt = 0;
    bool active = true;

    Encoder(int codec, bool voiceActivation, int activationLevel) : activation(voiceActivation), level(activationLevel) {
        state = speex_encoder_init(&speex_nb_mode);
        speex_bits_init(&bits);
        spx_int32_t rate = kRate, bitrate = 0x1900, vbr = 1, vad = voiceActivation ? 1 : 0, dtx = 1, frame = 0;
        speex_encoder_ctl(state, SPEEX_SET_SAMPLING_RATE, &rate);
        speex_encoder_ctl(state, SPEEX_SET_BITRATE, &bitrate);
        speex_encoder_ctl(state, SPEEX_SET_VBR, &vbr);
        speex_encoder_ctl(state, SPEEX_SET_VAD, &vad);
        speex_encoder_ctl(state, SPEEX_SET_DTX, &dtx);
        speex_encoder_ctl(state, SPEEX_GET_FRAME_SIZE, &frame);
        float quality = speexQuality(codec);
        speex_encoder_ctl(state, SPEEX_SET_VBR_QUALITY, &quality);
        active = !voiceActivation;
    }
    ~Encoder() {
        speex_bits_destroy(&bits);
        speex_encoder_destroy(state);
    }
    std::vector<uint8_t> encode(int16_t* samples) {
        speex_encode_int(state, samples, &bits);
        std::vector<uint8_t> out(kFrameSamples * 2);
        out.resize(size_t(speex_bits_write(&bits, reinterpret_cast<char*>(out.data()), int(out.size()))));
        speex_bits_reset(&bits);
        if (activation) {
            int sum = 0;
            for (int i = 1; i != kFrameSamples; ++i)
                sum += std::abs(int(uint16_t(samples[i])) - int(uint16_t(samples[i - 1])));
            ring[ringAt] = sum;
            if (++ringAt > 19) ringAt = 0;
            int loud = 0;
            for (int r : ring)
                if (int(unsigned(level) * 2000) < r) ++loud;
            if (!active) {
                if (loud > 1) active = true;
            } else if (loud < 1) {
                active = false;
            }
        }
        return out;
    }
};

struct Decoder {
    void* state = nullptr;
    SpeexBits bits{};
    Decoder() {
        state = speex_decoder_init(&speex_nb_mode);
        speex_bits_init(&bits);
        spx_int32_t rate = kRate;
        speex_decoder_ctl(state, SPEEX_SET_SAMPLING_RATE, &rate);
    }
    ~Decoder() {
        speex_bits_destroy(&bits);
        speex_decoder_destroy(state);
    }
    bool decode(const uint8_t* data, int size, int16_t* out) {
        speex_bits_read_from(&bits, reinterpret_cast<const char*>(data), size);
        const int r = speex_decode_int(state, &bits, out);
        if (r != 0 && logging()) std::printf("[voice] Speex decode failure: %d\n", r);
        return r == 0;
    }
};

// ---- Playback (AudioOutThread / AudioOutStream) -------------------------------------------------------------------------------
// One stream per group member (maxMembers of them, FUN_00a884b0), each a DirectSound buffer of a second (2 x 8000 bytes). A speaker
// starts (FUN_00a86230, on BeginSpeaking or a member added while speaking): silence written ahead of the play cursor up to a third
// of a second (17 frames of 320 bytes = 2720 samples), playback held for 8 decoded frames and started on the 9th (+ 0x74); packets
// older than the last one's sequence number are dropped (+ 0x7c). A speaker stops (FUN_00a88e00 -> FUN_00a84ec0, EndSpeaking or a
// member removed): no more voice is taken, the buffer plays on for 64 mixer ticks of 50 ms (+ 0x78; the out thread runs every 50 ms,
// FUN_00a89e10) and is stopped. Voice for a member that is not speaking is dropped ("voice for non-speaking player").
// UNVERIFIED: the DirectSound cursor handling (FUN_00a85890's overrun / near-cursor refills) is not reproduced - SDL queues.
struct Speaker {
    SDL_AudioStream* stream = nullptr;
    Decoder decoder;
    bool speaking = false;
    int startCount = -1;   // + 0x74
    int lastSeq = -1;      // + 0x7c
    int tailMs = -1;       // + 0x78 x 50 ms
    int decoded = 0;       // frames decoded (OPENRASA_VOICELOG)
    ~Speaker() {
        if (stream) SDL_DestroyAudioStream(stream);
    }
};

struct Client {
    // ChatClientImpl state.
    bool connected = false;    // + 0x5c
    bool connecting = false;   // + 0x5d
    bool loggedIn = false;     // + 0x5e
    bool talking = false;      // + 0x188
    bool canTalk = false;      // + 0x2d
    uint8_t localId = 0;       // + 0x10
    uint32_t group = 0;        // + 0x14
    uint64_t player = 0;
    uint8_t maxMembers = 0, maxTalkers = 0;
    int codec = 0;             // + 0x24
    float maxTalkTime = 0, talkTimeRegen = 0;
    int mode = 0;              // + 0x1c8 (4 = peer-to-peer)
    uint16_t talkCounter = 0;  // + 0x204
    uint16_t voiceSeq = 0;     // + 0x54
    uint64_t heartbeat = 0;    // + 0x190
    bool voiceActivation = false;
    int activationLevel = 190, cutoffDelay = 300;
    float gain = 1.0f;
    Connection connection;

    // AudioIn (FUN_00a8e8e0 at login): the microphone, the encoder, the frames waiting to be sent.
    SDL_AudioStream* microphone = nullptr;
    std::unique_ptr<Encoder> encoder;
    bool recording = false;    // AudioIn + 0x964: frames are passed on
    std::vector<int16_t> pending;
    std::vector<std::vector<uint8_t>> frames;   // the packet being filled (FUN_00a8e0e0)
    std::deque<int> results;   // the in thread's results for the main thread: 2 started, 3 stopped (FUN_00a8e4e0)
    std::vector<std::unique_ptr<Speaker>> speakers;
    uint64_t lastMix = 0;

    ~Client() { stopAudio(); }

    void stopAudio() {
        if (microphone) SDL_DestroyAudioStream(microphone);
        microphone = nullptr;
        encoder.reset();
        speakers.clear();
        recording = false;
        frames.clear();
        pending.clear();
        results.clear();
    }

    void send(const Writer& w) { connection.send(w.b); }
    void resetHeartbeat() { heartbeat = SDL_GetTicks(); }

    // The in thread's commands (FUN_00a8ae40 = 1 start, FUN_00a8ae10 = 2 stop).
    // Without a microphone the results still come (the scripts and the server see the talking; no voice is sent) - UNVERIFIED:
    // what the exe's AudioIn does when DirectSoundCapture fails is not traced.
    void inStart() {
        if (microphone && !voiceActivation) {
            SDL_ClearAudioStream(microphone);
            SDL_ResumeAudioStreamDevice(microphone);
        }
        recording = true;
        results.push_back(2);
    }
    void inStop() {
        if (microphone && !voiceActivation) SDL_PauseAudioStreamDevice(microphone);
        recording = false;
        results.push_back(3);
    }

    // FUN_00a8e8e0, after a successful login: the audio in and out objects for the group's codec.
    void startAudio() {
        stopAudio();
        if (!isSpeex(codec)) {
            std::fprintf(stderr, "[voice] codec %d is not one of the Speex codecs (3 - 5); no audio\n", codec);
            return;
        }
        const SDL_AudioSpec spec{SDL_AUDIO_S16LE, 1, kRate};
        for (int i = 0; i < maxMembers; ++i) {
            auto s = std::make_unique<Speaker>();
            s->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
            if (s->stream) SDL_SetAudioStreamGain(s->stream, gain);
            speakers.push_back(std::move(s));
        }
        encoder = std::make_unique<Encoder>(codec, voiceActivation, activationLevel);
        microphone = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, nullptr, nullptr);
        if (!microphone) std::fprintf(stderr, "[voice] no microphone: %s\n", SDL_GetError());
        else if (voiceActivation) SDL_ResumeAudioStreamDevice(microphone);   // listening all the time for the activity detector
        lastMix = SDL_GetTicks();
    }

    Speaker* speaker(uint8_t id) { return id < speakers.size() ? speakers[id].get() : nullptr; }
    void speakerBegin(uint8_t id) {   // FUN_00a88e30 -> FUN_00a86230
        Speaker* s = speaker(id);
        if (!s || !s->stream) return;
        s->speaking = true;
        s->startCount = 8;
        s->lastSeq = -1;
        s->tailMs = -1;
        SDL_PauseAudioStreamDevice(s->stream);
        SDL_ClearAudioStream(s->stream);
        const std::vector<int16_t> silence(17 * kFrameSamples, 0);
        SDL_PutAudioStreamData(s->stream, silence.data(), int(silence.size() * 2));
    }
    void speakerEnd(uint8_t id) {     // FUN_00a88e00 -> FUN_00a84ec0
        Speaker* s = speaker(id);
        if (!s || !s->speaking) return;
        s->speaking = false;
        s->tailMs = 64 * 50;
    }
    // A relayed voice message (FUN_00a85100 fixed / FUN_00a85bc0 variable): u8 speaker, u8 frames, u16 seq, [sizes, u16 length], audio.
    void relay(const std::vector<uint8_t>& m, bool variable) {
        Reader r{m};
        r.u8();
        const uint8_t id = r.u8(), n = r.u8();
        const uint16_t seq = r.u16();
        Speaker* s = speaker(id);
        if (!s || !s->speaking || !s->stream) {
            if (logging()) std::printf("[voice] voice for non-speaking player %d\n", id);
            return;
        }
        if (s->lastSeq != -1 && s->lastSeq > int(seq)) return;
        s->lastSeq = seq;
        std::vector<uint8_t> sizes;
        if (variable) {
            for (int i = 0; i < n; ++i) sizes.push_back(r.u8());
            r.u16();
        } else {
            sizes.assign(n, 62);   // fixed-size frames: 0x00bc... (+ 0x18) bytes each; no fixed-size codec here
        }
        if (!r.ok) return;
        size_t at = r.at;
        for (int i = 0; i < n; ++i) {
            if (at + sizes[i] > m.size()) return;
            int16_t pcm[kFrameSamples];
            s->decoder.decode(m.data() + at, sizes[i], pcm);
            at += sizes[i];
            SDL_PutAudioStreamData(s->stream, pcm, int(sizeof pcm));
            if (logging() && ++s->decoded % 25 == 0)
                std::printf("[voice] speaker %d: %d frames decoded, %d bytes queued\n", id, s->decoded, SDL_GetAudioStreamQueued(s->stream));
            if (s->startCount < 1) {
                if (s->startCount == 0) {
                    s->startCount = -1;
                    SDL_ResumeAudioStreamDevice(s->stream);
                    if (logging()) std::printf("[voice] speaker %d playing (%d bytes queued)\n", id, SDL_GetAudioStreamQueued(s->stream));
                }
            } else {
                --s->startCount;
            }
        }
    }
    void mixTick(uint64_t now) {   // the out thread's 50 ms tick (FUN_00a89a80 -> FUN_00a85890)
        const int elapsed = int(now - lastMix);
        lastMix = now;
        for (auto& s : speakers) {
            if (s->tailMs < 0) continue;
            s->tailMs -= elapsed;
            if (s->tailMs <= 0) {
                s->tailMs = -1;
                if (logging()) std::printf("[voice] a speaker's buffer stopped\n");
                if (s->stream) {
                    SDL_PauseAudioStreamDevice(s->stream);
                    SDL_ClearAudioStream(s->stream);
                }
            }
        }
    }

    // The microphone: whole frames encoded; with voice activation the detector starts / stops talking (FUN_00a8ae70); frames passed on
    // while recording (command 4) and sent 5 to a packet.
    void capture() {
        if (!microphone || !encoder) return;
        int16_t buffer[1024];
        for (;;) {
            const int got = SDL_GetAudioStreamData(microphone, buffer, int(sizeof buffer));
            if (got <= 0) break;
            pending.insert(pending.end(), buffer, buffer + got / 2);
        }
        size_t at = 0;
        while (pending.size() - at >= size_t(kFrameSamples)) {
            int16_t* frame = pending.data() + at;
            at += kFrameSamples;
            const bool wasActive = encoder->active;
            std::vector<uint8_t> coded = encoder->encode(frame);
            if (voiceActivation && encoder->active != wasActive) {
                if (encoder->active) inStart();
                else inStop();
            }
            if (recording) queueFrame(std::move(coded));
        }
        pending.erase(pending.begin(), pending.begin() + std::ptrdiff_t(at));
    }
    void queueFrame(std::vector<uint8_t> coded) {   // FUN_00a8e0e0
        if (!talking) return;
        frames.push_back(std::move(coded));
        if (int(frames.size()) < kFramesPerPacket) return;
        // VoiceVariable (15, FUN_00a838b0): u8 frames, u16 seq, a size byte per frame, u16 length, the frames.
        Writer w;
        w.u8(15);
        w.u8(uint8_t(frames.size()));
        w.u16(voiceSeq);
        size_t length = 0;
        for (const auto& f : frames) {
            w.u8(uint8_t(f.size()));
            length += f.size();
        }
        w.u16(uint16_t(length));
        for (const auto& f : frames) w.b.insert(w.b.end(), f.begin(), f.end());
        send(w);
        ++voiceSeq;
        frames.clear();
    }
    // FUN_00a8e4e0: the in thread's results.
    void results_() {
        while (!results.empty()) {
            const int r = results.front();
            results.pop_front();
            if (r == 2) {   // started: StartTalking (13, FUN_00a83830) with the next counter
                talking = true;
                frames.clear();
                ++talkCounter;
                Writer w;
                w.u8(13);
                w.u8(localId);
                w.u16(talkCounter);
                send(w);
                resetHeartbeat();
                call("OnBeginSpeaking", Py_BuildValue(const_cast<char*>("(B)"), localId), 0);
            } else if (r == 3) {   // stopped: StopTalking (12, FUN_00a83880)
                Writer w;
                w.u8(12);
                w.u8(localId);
                send(w);
                resetHeartbeat();
                // The talk time passed is the global float at 0x00d029a8 (-0.0176 in the file; its writers not traced).
                call("OnEndSpeaking", Py_BuildValue(const_cast<char*>("(Bf)"), localId, double(-0.0176f)), 0);
            }
        }
    }

    // FUN_00a8ec20: a message from the server.
    void dispatch(const std::vector<uint8_t>& m) {
        Reader r{m};
        const uint8_t type = r.u8();
        if (logging()) std::printf("[voice] message %u (%zu bytes)\n", type, m.size());
        switch (type) {
        case 0x14: {   // LoginReply
            const uint8_t result = r.u8();
            group = r.u32();
            localId = r.u8();
            maxMembers = r.u8();
            maxTalkers = r.u8();
            codec = r.u8();
            maxTalkTime = r.f32();
            talkTimeRegen = r.f32();
            mode = r.u8();
            r.string();
            canTalk = r.u8() != 0;
            if (result == 0) {
                loggedIn = true;
                if (mode == 4) std::fprintf(stderr, "[voice] the server asks for peer-to-peer voice (mode 4), not supported\n");
                startAudio();
                call("OnVoiceServerLogin", Py_BuildValue(const_cast<char*>("(iLBBBff)"), int(group), (long long)player, localId,
                                                         maxMembers, maxTalkers, double(maxTalkTime), double(talkTimeRegen)), 0);
            } else {
                static const char* reasons[] = {"Success", "Incorrect version", "Unable to verify player information",
                                                "Bad message type", "Unable to join group"};
                std::fprintf(stderr, "[voice] Login failed: %s.\n", result < 5 ? reasons[result] : "?");
                connected = false;
                loggedIn = false;
                call("OnVoiceServerLoginFailed", Py_BuildValue(const_cast<char*>("()")), 0);
                close();
            }
            break;
        }
        case 0x15:   // RelayFixed
        case 0x1b:   // RelayVariable
            if (!speakers.empty()) relay(m, type == 0x1b);
            break;
        case 0x16: {   // PlayerAdded: u32 group, u64 player, u8 localId, u8 speaking, u32 address, u32 port, string, u8
            const uint32_t g = r.u32();
            const uint64_t p = r.u64();
            const uint8_t id = r.u8(), speaking = r.u8();
            r.u32();
            const uint32_t port = r.u32();
            r.string();
            r.u8();
            call("OnPlayerAddedToVoiceGroup", Py_BuildValue(const_cast<char*>("(iLBi)"), int(g), (long long)p, id, int(port)), 0);
            if (speaking) {
                speakerBegin(id);
                call("OnBeginSpeaking", Py_BuildValue(const_cast<char*>("(B)"), id), 0);
            }
            break;
        }
        case 0x17: {   // PlayerRemoved
            const uint32_t g = r.u32();
            const uint64_t p = r.u64();
            const uint8_t id = r.u8();
            speakerEnd(id);
            call("OnPlayerRemovedFromVoiceGroup", Py_BuildValue(const_cast<char*>("(iLB)"), int(g), (long long)p, id), 0);
            call("OnEndSpeaking", Py_BuildValue(const_cast<char*>("(Bf)"), id, 0.0), 0);
            break;
        }
        case 0x18: {   // EndSpeaking: u8 localId, f32 talked, u16 seq - the server ends this client's talking when the counters match
            const uint8_t id = r.u8();
            const float talked = r.f32();
            const uint16_t seq = r.u16();
            speakerEnd(id);
            if (id == localId && seq == talkCounter) stopTalkingNow();
            call("OnEndSpeaking", Py_BuildValue(const_cast<char*>("(Bf)"), id, double(talked)), 0);
            break;
        }
        case 0x19: {   // BeginSpeaking
            const uint8_t id = r.u8();
            if (id != localId) speakerBegin(id);
            call("OnBeginSpeaking", Py_BuildValue(const_cast<char*>("(B)"), id), 0);
            break;
        }
        case 0x1a:
        case 0x1c:     // a group list (callback slot 12, which VoiceChatClient leaves empty)
            break;
        default:
            if (logging()) std::printf("[voice] Unknown packet type: %d\n", type);
        }
    }

    void stopTalkingNow() {   // FUN_00a8ca50
        talking = false;
        inStop();
    }

    void close() {   // FUN_00a8d790
        connection.close();
        connected = connecting = loggedIn = talking = false;
        stopAudio();
        call("OnVoiceServerDisconnected", Py_BuildValue(const_cast<char*>("()")), -1);
    }

    void update() {
        const uint64_t now = SDL_GetTicks();
        if (connecting) {
            if (!connection.negotiate()) {
                // The connect-failed callback (VoiceChatClient slot 4) is empty: the scripts are not told.
                if (logging()) std::printf("[voice] connecting failed\n");
                connecting = false;
                connection.socket.close();
            } else if (connection.state == 5) {
                connecting = false;
                connected = true;
                resetHeartbeat();
                call("OnVoiceServerConnected", Py_BuildValue(const_cast<char*>("()")), 0);
            }
        }
        if (!connected) return;
        if (connection.state != 5) {   // FUN_00a8fce0: the pipe is gone
            close();
            return;
        }
        std::vector<uint8_t> m;
        while (connected && connection.message(m)) dispatch(m);
        if (!connected) return;
        if (connection.state != 5) {
            close();
            return;
        }
        if (now - heartbeat > 10000) {   // Heartbeat (14, FUN_00a83800) after 10 s without a talk message
            resetHeartbeat();
            Writer w;
            w.u8(14);
            w.u8(localId);
            send(w);
        }
        capture();
        results_();
        mixTick(now);
    }
};

std::unique_ptr<Client> client;

}  // namespace

void setScriptCaller(ScriptCaller caller) { scriptCaller = caller; }

bool init(bool voiceActivation, int activationLevel, int cutoffDelay, int volume) {
    if (client) return true;
    if (!SDL_WasInit(SDL_INIT_AUDIO)) return false;
    client = std::make_unique<Client>();
    client->voiceActivation = voiceActivation;
    client->activationLevel = activationLevel;
    client->cutoffDelay = cutoffDelay;   // stored at + 0x2a (FUN_00a86a20); what reads it is not traced
    setVolume(volume);
    return true;
}
bool exists() { return client != nullptr; }
bool connected() { return client && client->connected; }
bool loggedIn() { return client && client->loggedIn; }
bool talking() { return client && client->talking; }

bool connect(const std::string& hostAndPort) {
    if (!client || client->connected) return false;
    const size_t colon = hostAndPort.rfind(':');
    if (colon == std::string::npos) return false;
    const int port = std::atoi(hostAndPort.c_str() + colon + 1);
    client->loggedIn = false;
    if (!client->connection.open(hostAndPort.substr(0, colon), port)) return false;
    client->connecting = true;
    if (logging()) std::printf("[voice] connecting to %s\n", hostAndPort.c_str());
    return true;
}

bool login(uint32_t group, uint64_t player, uint32_t token) {
    if (!client || !client->connected || client->loggedIn) return false;
    // Login (10, FUN_00a83ed0): f32 1.3 (0x00bc4d8c), u32 group, u64 player, u32 token, string "", u8 codec 5, u8 0, "", "".
    client->player = player;
    Writer w;
    w.u8(10);
    w.f32(1.3f);
    w.u32(group);
    w.u64(player);
    w.u32(token);
    w.string("");
    w.u8(5);
    w.u8(0);
    w.string("");
    w.string("");
    return client->connection.send(w.b);
}

void disconnect() {
    if (client && client->connected) client->close();
}

void shutdown() {
    if (!client) return;
    if (client->connected) client->close();
    client.reset();
}

void startTalking() {   // FUN_00a8db90
    if (!client || !client->connected || !client->loggedIn || !client->canTalk) return;
    client->talking = true;
    client->inStart();
}
void stopTalking() {    // FUN_00a8ca50
    if (client) client->stopTalkingNow();
}

void setVolume(int percent) {
    if (!client) return;
    // FUN_004d5af0: 0 -> -10000, else (int)(log10(v / 100) x 5000) hundredths of a decibel (FUN_00a8d340 takes 0 .. -10000), which
    // DirectSound applies as a gain of 10^(mB / 2000).
    const int millibels = percent <= 0 ? -10000 : int(std::log10(float(percent) / 100.0f) * 5000.0f);
    if (millibels > 0 || millibels < -10000) return;
    client->gain = std::pow(10.0f, float(millibels) / 2000.0f);
    for (auto& s : client->speakers)
        if (s->stream) SDL_SetAudioStreamGain(s->stream, client->gain);
}

void update() {
    if (client) client->update();
}

}  // namespace engine::voice
