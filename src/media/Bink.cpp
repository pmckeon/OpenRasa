// Bink.cpp - see Bink.h. The structure follows FFmpeg's decoder function for function (names in the comments) so the two can be
// compared; FFmpeg's bounds checks are kept as refusals.
#include "Bink.h"

#include "BinkData.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace media {

namespace {

uint32_t rl32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// The 16 Huffman trees (bink_init_vlcs): codes are read least significant bit first; a table indexed by the next maxbits bits gives
// symbol | length << 8.
struct Vlc {
    int bits = 0;
    std::vector<uint16_t> table;
};
const Vlc* trees() {
    static const std::vector<Vlc> t = [] {
        std::vector<Vlc> v(16);
        for (int i = 0; i < 16; ++i) {
            v[i].bits = bink::bink_tree_lens[i][15];
            v[i].table.assign(size_t(1) << v[i].bits, 0);
            for (int s = 0; s < 16; ++s) {
                const int len = bink::bink_tree_lens[i][s], code = bink::bink_tree_bits[i][s];
                for (size_t e = 0; e < v[i].table.size(); ++e)
                    if ((e & ((size_t(1) << len) - 1)) == size_t(code)) v[i].table[e] = uint16_t(s | len << 8);
            }
        }
        return v;
    }();
    return t.data();
}

enum Sources { BlockTypes, SubBlockTypes, Colors, Pattern, XOff, YOff, IntraDc, InterDc, Run, SourceCount };
enum Blocks { SkipBlock, ScaledBlock, MotionBlock, RunBlock, ResidueBlock, IntraBlock, FillBlock, InterBlock, PatternBlock, RawBlock };
constexpr uint8_t rleLens[4] = {4, 8, 12, 32};
constexpr uint16_t criticalFreqs[25] = {100,  200,  300,  400,  510,  630,  770,  920,  1080, 1270,  1480,  1720,  2000,
                                        2320, 2700, 3150, 3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500, 24500};
constexpr uint8_t audioRle[16] = {2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64};

int log2i(unsigned v) {
    int n = 0;
    while (v >>= 1) ++n;
    return n;
}

// binkdsp.c
constexpr int A1 = 2896, A2 = 2217, A3 = 3784, A4 = -5352;
inline int mul(int x, int y) { return int(unsigned(x) * unsigned(y)) >> 11; }
template <class Dest, class Munge>
inline void idctTransform(Dest dest, int d, const int* src, int s, Munge munge) {
    const int a0 = src[0] + src[s * 4], a1 = src[0] - src[s * 4];
    const int a2 = src[s * 2] + src[s * 6], a3 = mul(A1, src[s * 2] - src[s * 6]);
    const int a4 = src[s * 5] + src[s * 3], a5 = src[s * 5] - src[s * 3];
    const int a6 = src[s * 1] + src[s * 7], a7 = src[s * 1] - src[s * 7];
    const int b0 = a4 + a6, b1 = mul(A3, a5 + a7);
    const int b2 = mul(A4, a5) - b0 + b1, b3 = mul(A1, a6 - a4) - b2, b4 = mul(A2, a7) + b3 - b1;
    dest[0] = munge(a0 + a2 + b0);
    dest[d] = munge(a1 + a3 - a2 + b2);
    dest[d * 2] = munge(a1 - a3 + a2 + b3);
    dest[d * 3] = munge(a0 - a2 - b4);
    dest[d * 4] = munge(a0 - a2 + b4);
    dest[d * 5] = munge(a1 - a3 + a2 - b3);
    dest[d * 6] = munge(a1 + a3 - a2 - b2);
    dest[d * 7] = munge(a0 + a2 - b0);
}
inline int none(int x) { return x; }
inline int rowMunge(int x) { return (x + 0x7F) >> 8; }
void idctColumns(int* temp, const int32_t* block) {
    for (int i = 0; i < 8; ++i) {
        const int32_t* s = block + i;
        if ((s[8] | s[16] | s[24] | s[32] | s[40] | s[48] | s[56]) == 0)
            for (int r = 0; r < 8; ++r) temp[i + r * 8] = s[0];
        else
            idctTransform(temp + i, 8, s, 8, none);
    }
}
void idctPut(uint8_t* dest, int stride, int32_t* block) {
    int temp[64];
    idctColumns(temp, block);
    for (int i = 0; i < 8; ++i) idctTransform(dest + i * stride, 1, temp + 8 * i, 1, [](int x) { return uint8_t(rowMunge(x)); });
}
void idctAdd(uint8_t* dest, int stride, int32_t* block) {
    int temp[64];
    idctColumns(temp, block);
    for (int i = 0; i < 8; ++i) idctTransform(block + 8 * i, 1, temp + 8 * i, 1, rowMunge);
    for (int i = 0; i < 8; ++i, dest += stride, block += 8)
        for (int j = 0; j < 8; ++j) dest[j] = uint8_t(dest[j] + block[j]);
}
void scaleBlock(const uint8_t* src, uint8_t* dst, int stride) {
    for (int j = 0; j < 8; ++j, src += 8, dst += stride * 2)
        for (int i = 0; i < 8; ++i) dst[i * 2] = dst[i * 2 + 1] = dst[stride + i * 2] = dst[stride + i * 2 + 1] = src[i];
}
void copy8(uint8_t* dst, const uint8_t* src, int stride) {
    for (int i = 0; i < 8; ++i) std::memcpy(dst + i * stride, src + i * stride, 8);
}
void fill(uint8_t* dst, int v, int stride, int size) {
    for (int i = 0; i < size; ++i) std::memset(dst + i * stride, v, size_t(size));
}

}  // namespace

// The bit reader (BITSTREAM_READER_LE): bits from the least significant end of each byte. The data carries 8 bytes of padding.
class BinkMovie::Bits {
public:
    Bits(const uint8_t* data, size_t bytes) : data_(data), bits_(int64_t(bytes) * 8) {}
    uint32_t get(int n) {
        if (n == 0) return 0;
        const uint32_t v = peek(n);
        pos_ += n;
        return v;
    }
    uint32_t peek(int n) const {
        uint64_t word;
        std::memcpy(&word, data_ + (pos_ >> 3), 8);
        return uint32_t((word >> (pos_ & 7)) & ((uint64_t(1) << n) - 1));
    }
    void skip(int64_t n) { pos_ += n; }
    int64_t count() const { return pos_; }
    int64_t left() const { return bits_ - pos_; }
    void align32() {
        if (pos_ & 31) pos_ += 32 - (pos_ & 31);
    }
    int huff(const Tree& tree) {
        const Vlc& v = trees()[tree.vlc];
        const uint16_t e = v.table[peek(v.bits)];
        pos_ += e >> 8;
        return tree.syms[e & 0xFF];
    }

private:
    const uint8_t* data_;
    int64_t bits_, pos_ = 0;
};

bool BinkMovie::open(const std::string& path, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        file_.close();
        return false;
    };
    file_.open(path, std::ios::binary);
    if (!file_) return fail("cannot open the file");
    uint8_t h[44];
    if (!file_.read(reinterpret_cast<char*>(h), sizeof h)) return fail("short header");
    if (h[0] != 'B' || h[1] != 'I' || h[2] != 'K' || !(h[3] == 'f' || h[3] == 'g' || h[3] == 'h' || h[3] == 'i' || h[3] == 'k'))
        return fail("not a Bink 1 movie of a supported revision");
    version_ = h[3];
    fileSize_ = rl32(h + 4) + 8;
    frameCount_ = rl32(h + 8);
    width_ = int(rl32(h + 20));
    height_ = int(rl32(h + 24));
    fpsNum_ = rl32(h + 28);
    fpsDen_ = rl32(h + 32);
    const uint32_t flags = rl32(h + 36);
    audioTracks_ = int(rl32(h + 40));
    if (frameCount_ == 0 || frameCount_ > 1000000 || width_ <= 0 || height_ <= 0 || width_ > 7680 || height_ > 4800 || !fpsNum_ || !fpsDen_ ||
        audioTracks_ > 256)
        return fail("bad header");
    hasAlpha_ = flags & 0x00100000;
    swapPlanes_ = version_ >= 'h';
    if (version_ == 'k') file_.seekg(4, std::ios::cur);
    std::vector<uint8_t> t(size_t(audioTracks_) * 12);
    if (audioTracks_ && !file_.read(reinterpret_cast<char*>(t.data()), std::streamsize(t.size()))) return fail("short header");
    if (audioTracks_) {
        const uint8_t* a = t.data() + 4 * audioTracks_;          // after the tracks' maximum decoded sizes
        audioRate_ = a[0] | a[1] << 8;
        const int aflags = a[2] | a[3] << 8;
        audioChannels_ = aflags & 0x2000 ? 2 : 1;
        audioDct_ = aflags & 0x1000;
        if (!audioDct_ || audioRate_ <= 0) audioChannels_ = 0;   // RDFT audio is not decoded (no shipped movie has it)
    }
    std::vector<uint8_t> idx(size_t(frameCount_) * 4);
    if (!file_.read(reinterpret_cast<char*>(idx.data()), std::streamsize(idx.size()))) return fail("short frame index");
    index_.resize(frameCount_ + 1);
    for (uint32_t i = 0; i < frameCount_; ++i) index_[i] = rl32(idx.data() + 4 * i);
    index_[frameCount_] = fileSize_;
    for (uint32_t i = 0; i < frameCount_; ++i)
        if ((index_[i + 1] & ~1u) <= (index_[i] & ~1u)) return fail("bad frame index");

    // Pictures: whole 8 x 8 blocks (luma) / 16 x 16 (chroma) - the decoder writes every block, also the rows past the height.
    const int bw = (width_ + 7) >> 3, bh = (height_ + 7) >> 3;
    const int cbw = (width_ + 15) >> 4, cbh = (height_ + 15) >> 4;
    strides_[0] = strides_[3] = bw * 8;
    rows_[0] = rows_[3] = bh * 8;
    strides_[1] = strides_[2] = cbw * 8;
    rows_[1] = rows_[2] = cbh * 8;
    for (auto& f : frames_)
        for (int p = 0; p < 4; ++p)
            if (p < 3 || hasAlpha_) f[p].assign(size_t(strides_[p]) * rows_[p], p == 0 || p == 3 ? 0 : 128);
    for (Bundle& b : bundles_) b.data.assign(size_t(bw) * bh * 64, 0);
    current_ = 0;
    havePrevious_ = false;
    next_ = 0;

    if (audioChannels_) {
        const int bits = audioRate_ < 22050 ? 9 : audioRate_ < 44100 ? 10 : 11;
        frameLen_ = 1 << bits;
        overlapLen_ = frameLen_ / 16;
        const int half = int((int64_t(audioRate_) + 1) / 2);
        root_ = float(frameLen_ / (std::sqrt(double(frameLen_)) * 32768.0));
        for (int i = 0; i < 96; ++i) quantTable_[i] = std::exp(float(i) * 0.15289164787221953823f) * root_;
        for (numBands_ = 1; numBands_ < 25; ++numBands_)
            if (half <= criticalFreqs[numBands_ - 1]) break;
        bands_[0] = 2;
        for (int i = 1; i < numBands_; ++i) bands_[i] = (criticalFreqs[i - 1] * frameLen_ / half) & ~1u;
        bands_[numBands_] = unsigned(frameLen_);
        for (auto& p : previous_) p.assign(size_t(overlapLen_), 0.0f);
        firstBlock_ = true;
        twiddle_[0].resize(size_t(frameLen_) * 2);
        for (int k = 0; k < frameLen_; ++k) {
            const double a = 3.14159265358979323846 * k / (2.0 * frameLen_);
            twiddle_[0][2 * k] = float(std::cos(a));
            twiddle_[0][2 * k + 1] = float(std::sin(a));
        }
        twiddle_[1].resize(size_t(frameLen_));
        for (int k = 0; k < frameLen_ / 2; ++k) {
            const double a = 2.0 * 3.14159265358979323846 * k / frameLen_;
            twiddle_[1][2 * k] = float(std::cos(a));
            twiddle_[1][2 * k + 1] = float(std::sin(a));
        }
    }
    return true;
}

std::vector<float> BinkMovie::takeAudio() {
    std::vector<float> out;
    out.swap(audioOut_);
    return out;
}

bool BinkMovie::decodeNext(std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (!file_.is_open() || next_ >= frameCount_) return fail("no more frames");
    const uint32_t pos = index_[next_] & ~1u, end = index_[next_ + 1] & ~1u;
    std::vector<uint8_t> packet(size_t(end - pos) + 8, 0);
    file_.clear();
    file_.seekg(pos);
    if (!file_.read(reinterpret_cast<char*>(packet.data()), std::streamsize(end - pos))) return fail("short frame");
    ++next_;
    size_t at = 0, remain = end - pos;
    for (int t = 0; t < audioTracks_; ++t) {        // read_packet: per track, u32 size + that many bytes
        if (remain < 4) return fail("bad audio size");
        const uint32_t size = rl32(packet.data() + at);
        if (size > remain - 4) return fail("bad audio size");
        if (t == 0 && size >= 4 && audioChannels_) decodeAudioPacket(packet.data() + at + 4, size);
        at += 4 + size;
        remain -= 4 + size;
    }
    // decode_frame
    std::vector<uint8_t> video(packet.begin() + std::ptrdiff_t(at), packet.begin() + std::ptrdiff_t(at + remain));
    video.resize(video.size() + 8, 0);
    Bits b(video.data(), remain);
    const int work = havePrevious_ ? 1 - current_ : current_;
    const int saved = current_;
    current_ = work;   // decodePlane writes into frames_[current_]
    bool ok = true;
    if (hasAlpha_) {
        if (version_ >= 'i') b.skip(32);
        ok = decodePlane(b, 3, false);
    }
    if (ok) {
        if (version_ >= 'i') b.skip(32);
        for (int p = 0; p < 3 && ok; ++p) {
            const int planeIndex = (!p || !swapPlanes_) ? p : (p ^ 3);
            ok = decodePlane(b, planeIndex, p != 0);
            if (b.count() >= int64_t(remain) * 8) break;
        }
    }
    if (!ok) {
        current_ = saved;
        return fail("bad video data");
    }
    havePrevious_ = true;
    return true;
}

// bink_decode_plane and the bundle readers.
bool BinkMovie::decodePlane(Bits& gb, int planeIndex, bool chroma) {
    uint8_t* const frame = frames_[current_][planeIndex].data();
    const uint8_t* const last = havePrevious_ ? frames_[1 - current_][planeIndex].data() : frame;
    const int stride = strides_[planeIndex];
    const int bw = chroma ? (width_ + 15) >> 4 : (width_ + 7) >> 3;
    const int bh = chroma ? (height_ + 15) >> 4 : (height_ + 7) >> 3;
    const int width = width_ >> int(chroma), height = height_ >> int(chroma);

    if (version_ == 'k' && gb.get(1)) {
        const int v = int(gb.get(8));
        for (int i = 0; i < height; ++i) std::memset(frame + i * stride, v, size_t(width));
        gb.align32();
        return true;
    }

    // init_lengths
    const int w = (std::max(width, 8) + 7) & ~7;
    bundles_[BlockTypes].len = log2i(unsigned((w >> 3) + 511)) + 1;
    bundles_[SubBlockTypes].len = log2i(unsigned((w >> 4) + 511)) + 1;
    bundles_[Colors].len = log2i(unsigned(bw * 64 + 511)) + 1;
    bundles_[IntraDc].len = bundles_[InterDc].len = bundles_[XOff].len = bundles_[YOff].len = log2i(unsigned((w >> 3) + 511)) + 1;
    bundles_[Pattern].len = log2i(unsigned((bw << 3) + 511)) + 1;
    bundles_[Run].len = log2i(unsigned(bw * 48 + 511)) + 1;

    // read_tree / read_bundle
    auto readTree = [&](Tree& tree) {
        if (gb.left() < 4) return false;
        tree.vlc = int(gb.get(4));
        if (!tree.vlc) {
            for (int i = 0; i < 16; ++i) tree.syms[i] = uint8_t(i);
            return true;
        }
        if (gb.get(1)) {
            uint8_t used[16] = {};
            int len = int(gb.get(3));
            for (int i = 0; i <= len; ++i) {
                tree.syms[i] = uint8_t(gb.get(4));
                used[tree.syms[i]] = 1;
            }
            for (int i = 0; i < 16 && len < 15; ++i)
                if (!used[i]) tree.syms[++len] = uint8_t(i);
        } else {
            const int len = int(gb.get(2));
            uint8_t t1[16], t2[16], *in = t1, *out = t2;
            for (int i = 0; i < 16; ++i) in[i] = uint8_t(i);
            for (int i = 0; i <= len; ++i) {
                const int size = 1 << i;
                for (int t = 0; t < 16; t += size << 1) {   // merge
                    uint8_t *dst = out + t, *src = in + t, *src2 = src + size;
                    int s1 = size, s2 = size;
                    do {
                        if (!gb.get(1)) {
                            *dst++ = *src++;
                            --s1;
                        } else {
                            *dst++ = *src2++;
                            --s2;
                        }
                    } while (s1 && s2);
                    while (s1--) *dst++ = *src++;
                    while (s2--) *dst++ = *src2++;
                }
                std::swap(in, out);
            }
            std::memcpy(tree.syms, in, 16);
        }
        return true;
    };
    for (int i = 0; i < SourceCount; ++i) {
        if (i == Colors) {
            for (Tree& t : colHigh_)
                if (!readTree(t)) return false;
            colLastVal_ = 0;
        }
        if (i != IntraDc && i != InterDc && !readTree(bundles_[i].tree)) return false;
        bundles_[i].curDec = bundles_[i].curPtr = bundles_[i].data.data();
    }

    // CHECK_READ_VAL: a bundle reads its next run of values once the previous ones have been used.
    auto startRead = [&](Bundle& b, int& t) {
        if (!b.curDec || b.curDec > b.curPtr) return false;
        t = int(gb.get(b.len));
        if (!t) {
            b.curDec = nullptr;
            return false;
        }
        return true;
    };
    auto dataEnd = [](Bundle& b) { return b.data.data() + b.data.size(); };
    auto readRuns = [&](Bundle& b) {
        int t;
        if (!startRead(b, t)) return true;
        if (b.curDec + t > dataEnd(b) || gb.left() < 1) return false;
        if (gb.get(1)) {
            std::memset(b.curDec, int(gb.get(4)), size_t(t));
            b.curDec += t;
        } else {
            for (uint8_t* e = b.curDec + t; b.curDec < e;) *b.curDec++ = uint8_t(gb.huff(b.tree));
        }
        return true;
    };
    auto readMotion = [&](Bundle& b) {
        int t;
        if (!startRead(b, t)) return true;
        if (b.curDec + t > dataEnd(b) || gb.left() < 1) return false;
        auto signedValue = [&](int v) {
            if (v) {
                const int sign = -int(gb.get(1));
                v = (v ^ sign) - sign;
            }
            return v;
        };
        if (gb.get(1)) {
            const int v = signedValue(int(gb.get(4)));
            std::memset(b.curDec, v, size_t(t));
            b.curDec += t;
        } else {
            for (uint8_t* e = b.curDec + t; b.curDec < e;) *b.curDec++ = uint8_t(signedValue(gb.huff(b.tree)));
        }
        return true;
    };
    auto readBlockTypes = [&](Bundle& b) {
        int t;
        if (!startRead(b, t)) return true;
        if (version_ == 'k') {
            t ^= 0xBB;
            if (t == 0) {
                b.curDec = nullptr;
                return true;
            }
        }
        uint8_t* const e = b.curDec + t;
        if (e > dataEnd(b) || gb.left() < 1) return false;
        if (gb.get(1)) {
            std::memset(b.curDec, int(gb.get(4)), size_t(t));
            b.curDec += t;
        } else {
            int last = 0;
            while (b.curDec < e) {
                const int v = gb.huff(b.tree);
                if (v < 12) {
                    last = v;
                    *b.curDec++ = uint8_t(v);
                } else {
                    const int run = rleLens[v - 12];
                    if (e - b.curDec < run) return false;
                    std::memset(b.curDec, last, size_t(run));
                    b.curDec += run;
                }
            }
        }
        return true;
    };
    auto readPatterns = [&](Bundle& b) {
        int t;
        if (!startRead(b, t)) return true;
        uint8_t* const e = b.curDec + t;
        if (e > dataEnd(b)) return false;
        while (b.curDec < e) {
            if (gb.left() < 2) return false;
            int v = gb.huff(b.tree);
            v |= gb.huff(b.tree) << 4;
            *b.curDec++ = uint8_t(v);
        }
        return true;
    };
    auto colour = [&](Bundle& b) {
        colLastVal_ = gb.huff(colHigh_[colLastVal_]);
        int v = (colLastVal_ << 4) | gb.huff(b.tree);
        if (version_ < 'i') {
            const int sign = int(int8_t(v)) >> 7;
            v = ((v & 0x7F) ^ sign) - sign;
            v += 0x80;
        }
        return v;
    };
    auto readColors = [&](Bundle& b) {
        int t;
        if (!startRead(b, t)) return true;
        uint8_t* const e = b.curDec + t;
        if (e > dataEnd(b) || gb.left() < 1) return false;
        if (gb.get(1)) {
            std::memset(b.curDec, colour(b), size_t(t));
            b.curDec += t;
        } else {
            while (b.curDec < e) {
                if (gb.left() < 2) return false;
                *b.curDec++ = uint8_t(colour(b));
            }
        }
        return true;
    };
    auto readDcs = [&](Bundle& b, int startBits, bool hasSign) {
        int len;
        if (!startRead(b, len)) return true;
        int16_t* dst = reinterpret_cast<int16_t*>(b.curDec);
        int16_t* const dstEnd = reinterpret_cast<int16_t*>(dataEnd(b));
        if (gb.left() < startBits - int(hasSign)) return false;
        int v = int(gb.get(startBits - int(hasSign)));
        if (v && hasSign) {
            const int sign = -int(gb.get(1));
            v = (v ^ sign) - sign;
        }
        if (dstEnd - dst < 1) return false;
        *dst++ = int16_t(v);
        --len;
        for (int i = 0; i < len; i += 8) {
            const int len2 = std::min(len - i, 8);
            if (dstEnd - dst < len2) return false;
            const int bsize = int(gb.get(4));
            if (bsize) {
                for (int j = 0; j < len2; ++j) {
                    int v2 = int(gb.get(bsize));
                    if (v2) {
                        const int sign = -int(gb.get(1));
                        v2 = (v2 ^ sign) - sign;
                    }
                    v += v2;
                    *dst++ = int16_t(v);
                    if (v < -32768 || v > 32767) return false;
                }
            } else {
                for (int j = 0; j < len2; ++j) *dst++ = int16_t(v);
            }
        }
        b.curDec = reinterpret_cast<uint8_t*>(dst);
        return true;
    };
    auto value = [&](int bundle) -> int {   // get_value
        Bundle& b = bundles_[bundle];
        if (bundle < XOff || bundle == Run) return *b.curPtr++;
        if (bundle == XOff || bundle == YOff) return int8_t(*b.curPtr++);
        int16_t v;
        std::memcpy(&v, b.curPtr, 2);
        b.curPtr += 2;
        return v;
    };

    // read_dct_coeffs + unquantize_dct_coeffs
    auto dctCoeffs = [&](int32_t block[64], const int32_t (*quant)[64]) {
        int coefList[128], modeList[128], coefIdx[64];
        int listStart = 64, listEnd = 64, coefCount = 0;
        if (gb.left() < 4) return false;
        const int init[6][2] = {{4, 0}, {24, 0}, {44, 0}, {1, 3}, {2, 3}, {3, 3}};
        for (const auto& c : init) {
            coefList[listEnd] = c[0];
            modeList[listEnd++] = c[1];
        }
        auto coefficient = [&](int bits) {
            int t;
            if (!bits) {
                t = 1 - int(gb.get(1) << 1);
            } else {
                t = int(gb.get(bits)) | 1 << bits;
                const int sign = -int(gb.get(1));
                t = (t ^ sign) - sign;
            }
            return t;
        };
        for (int bits = int(gb.get(4)) - 1; bits >= 0; --bits) {
            int listPos = listStart;
            while (listPos < listEnd) {
                if (!(modeList[listPos] | coefList[listPos]) || !gb.get(1)) {
                    ++listPos;
                    continue;
                }
                int ccoef = coefList[listPos];
                const int mode = modeList[listPos];
                switch (mode) {
                case 0:
                case 2:
                    if (mode == 0) {
                        coefList[listPos] = ccoef + 4;
                        modeList[listPos] = 1;
                    } else {
                        coefList[listPos] = 0;
                        modeList[listPos++] = 0;
                    }
                    for (int i = 0; i < 4; ++i, ++ccoef) {
                        if (gb.get(1)) {
                            coefList[--listStart] = ccoef;
                            modeList[listStart] = 3;
                        } else {
                            block[bink::bink_scan[ccoef]] = coefficient(bits);
                            coefIdx[coefCount++] = ccoef;
                        }
                    }
                    break;
                case 1:
                    modeList[listPos] = 2;
                    for (int i = 0; i < 3; ++i) {
                        ccoef += 4;
                        coefList[listEnd] = ccoef;
                        modeList[listEnd++] = 2;
                    }
                    break;
                case 3:
                    block[bink::bink_scan[ccoef]] = coefficient(bits);
                    coefIdx[coefCount++] = ccoef;
                    coefList[listPos] = 0;
                    modeList[listPos++] = 0;
                    break;
                }
            }
        }
        const int q = int(gb.get(4));
        const uint32_t* qt = reinterpret_cast<const uint32_t*>(quant[q]);
        block[0] = int(uint32_t(block[0]) * qt[0]) >> 11;
        for (int i = 0; i < coefCount; ++i) {
            const int idx = coefIdx[i];
            int32_t& c = block[bink::bink_scan[idx]];
            c = int(uint32_t(c) * qt[idx]) >> 11;
        }
        return true;
    };
    // read_residue
    auto residue = [&](int16_t block[64], int masksCount) {
        int coefList[128], modeList[128], nzCoeff[64];
        int listStart = 64, listEnd = 64, nzCount = 0;
        const int init[4][2] = {{4, 0}, {24, 0}, {44, 0}, {0, 2}};
        for (const auto& c : init) {
            coefList[listEnd] = c[0];
            modeList[listEnd++] = c[1];
        }
        for (int mask = 1 << gb.get(3); mask; mask >>= 1) {
            for (int i = 0; i < nzCount; ++i) {
                if (!gb.get(1)) continue;
                if (block[nzCoeff[i]] < 0) block[nzCoeff[i]] = int16_t(block[nzCoeff[i]] - mask);
                else block[nzCoeff[i]] = int16_t(block[nzCoeff[i]] + mask);
                if (--masksCount < 0) return;
            }
            int listPos = listStart;
            while (listPos < listEnd) {
                if (!(coefList[listPos] | modeList[listPos]) || !gb.get(1)) {
                    ++listPos;
                    continue;
                }
                int ccoef = coefList[listPos];
                const int mode = modeList[listPos];
                switch (mode) {
                case 0:
                case 2:
                    if (mode == 0) {
                        coefList[listPos] = ccoef + 4;
                        modeList[listPos] = 1;
                    } else {
                        coefList[listPos] = 0;
                        modeList[listPos++] = 0;
                    }
                    for (int i = 0; i < 4; ++i, ++ccoef) {
                        if (gb.get(1)) {
                            coefList[--listStart] = ccoef;
                            modeList[listStart] = 3;
                        } else {
                            nzCoeff[nzCount++] = bink::bink_scan[ccoef];
                            const int sign = -int(gb.get(1));
                            block[bink::bink_scan[ccoef]] = int16_t((mask ^ sign) - sign);
                            if (--masksCount < 0) return;
                        }
                    }
                    break;
                case 1:
                    modeList[listPos] = 2;
                    for (int i = 0; i < 3; ++i) {
                        ccoef += 4;
                        coefList[listEnd] = ccoef;
                        modeList[listEnd++] = 2;
                    }
                    break;
                case 3: {
                    nzCoeff[nzCount++] = bink::bink_scan[ccoef];
                    const int sign = -int(gb.get(1));
                    block[bink::bink_scan[ccoef]] = int16_t((mask ^ sign) - sign);
                    coefList[listPos] = 0;
                    modeList[listPos++] = 0;
                    if (--masksCount < 0) return;
                    break;
                }
                }
            }
        }
    };

    const uint8_t* const refStart = last;
    const uint8_t* const refEnd = refStart + (bw - 1 + stride * (bh - 1)) * 8;
    int coordmap[64];
    for (int i = 0; i < 64; ++i) coordmap[i] = (i & 7) + (i >> 3) * stride;
    auto motion = [&](uint8_t* dst, const uint8_t* prev) {   // bink_put_pixels
        const int xoff = value(XOff), yoff = value(YOff);
        const uint8_t* ref = prev + xoff + yoff * stride;
        if (ref < refStart || ref > refEnd) return false;
        copy8(dst, ref, stride);
        return true;
    };
    auto runFill = [&](auto put) {
        if (gb.left() < 4) return false;
        const uint8_t* scan = bink::bink_patterns[gb.get(4)];
        int i = 0;
        do {
            const int run = value(Run) + 1;
            i += run;
            if (i > 64) return false;
            if (gb.get(1)) {
                const int v = value(Colors);
                for (int j = 0; j < run; ++j) put(*scan++, v);
            } else {
                for (int j = 0; j < run; ++j) put(*scan++, value(Colors));
            }
        } while (i < 63);
        if (i == 63) put(*scan++, value(Colors));
        return true;
    };

    int32_t dct[64];
    int16_t res[64];
    uint8_t ublock[64];
    for (int by = 0; by < bh; ++by) {
        if (!readBlockTypes(bundles_[BlockTypes]) || !readBlockTypes(bundles_[SubBlockTypes]) || !readColors(bundles_[Colors]) ||
            !readPatterns(bundles_[Pattern]) || !readMotion(bundles_[XOff]) || !readMotion(bundles_[YOff]) ||
            !readDcs(bundles_[IntraDc], 11, false) || !readDcs(bundles_[InterDc], 11, true) || !readRuns(bundles_[Run]))
            return false;
        uint8_t* dst = frame + 8 * by * stride;
        const uint8_t* prev = last + 8 * by * stride;
        for (int bx = 0; bx < bw; ++bx, dst += 8, prev += 8) {
            int blk = value(BlockTypes);
            if (((by & 1) || (bx & 1)) && blk == ScaledBlock) {   // the rest of an already decoded 16 x 16 block
                ++bx;
                dst += 8;
                prev += 8;
                continue;
            }
            switch (blk) {
            case SkipBlock:
                copy8(dst, prev, stride);
                break;
            case ScaledBlock:
                blk = value(SubBlockTypes);
                switch (blk) {
                case RunBlock:
                    if (!runFill([&](int at, int v) { ublock[at] = uint8_t(v); })) return false;
                    break;
                case IntraBlock:
                    std::memset(dct, 0, sizeof dct);
                    dct[0] = value(IntraDc);
                    if (!dctCoeffs(dct, bink::bink_intra_quant)) return false;
                    idctPut(ublock, 8, dct);
                    break;
                case FillBlock:
                    fill(dst, value(Colors), stride, 16);
                    break;
                case PatternBlock: {
                    int col[2];
                    for (int& c : col) c = value(Colors);
                    for (int j = 0; j < 8; ++j) {
                        int v = value(Pattern);
                        for (int i = 0; i < 8; ++i, v >>= 1) ublock[i + j * 8] = uint8_t(col[v & 1]);
                    }
                    break;
                }
                case RawBlock:
                    for (int j = 0; j < 64; ++j) ublock[j] = uint8_t(value(Colors));
                    break;
                default:
                    return false;
                }
                if (blk != FillBlock) scaleBlock(ublock, dst, stride);
                ++bx;
                dst += 8;
                prev += 8;
                break;
            case MotionBlock:
                if (!motion(dst, prev)) return false;
                break;
            case RunBlock:
                if (!runFill([&](int at, int v) { dst[coordmap[at]] = uint8_t(v); })) return false;
                break;
            case ResidueBlock:
                if (!motion(dst, prev)) return false;
                std::memset(res, 0, sizeof res);
                residue(res, int(gb.get(7)));
                for (int i = 0; i < 8; ++i)
                    for (int j = 0; j < 8; ++j) dst[i * stride + j] = uint8_t(dst[i * stride + j] + res[i * 8 + j]);
                break;
            case IntraBlock:
                std::memset(dct, 0, sizeof dct);
                dct[0] = value(IntraDc);
                if (!dctCoeffs(dct, bink::bink_intra_quant)) return false;
                idctPut(dst, stride, dct);
                break;
            case FillBlock:
                fill(dst, value(Colors), stride, 8);
                break;
            case InterBlock:
                if (!motion(dst, prev)) return false;
                std::memset(dct, 0, sizeof dct);
                dct[0] = value(InterDc);
                if (!dctCoeffs(dct, bink::bink_inter_quant)) return false;
                idctAdd(dst, stride, dct);
                break;
            case PatternBlock: {
                int col[2];
                for (int& c : col) c = value(Colors);
                for (int i = 0; i < 8; ++i) {
                    int v = value(Pattern);
                    for (int j = 0; j < 8; ++j, v >>= 1) dst[i * stride + j] = uint8_t(col[v & 1]);
                }
                break;
            }
            case RawBlock:
                for (int i = 0; i < 8; ++i) std::memcpy(dst + i * stride, bundles_[Colors].curPtr + i * 8, 8);
                bundles_[Colors].curPtr += 64;
                break;
            default:
                return false;
            }
        }
    }
    gb.align32();   // the next plane starts at a 32-bit boundary
    return true;
}

// binkaudio.c (the DCT variant): a packet = the decoded size (32 bits), then blocks, each aligned to 32 bits.
void BinkMovie::decodeAudioPacket(const uint8_t* data, size_t size) {
    std::vector<uint8_t> padded(data, data + size);
    padded.resize(size + 8, 0);
    Bits b(padded.data(), size);
    b.skip(32);
    while (b.left() > 0) {
        const size_t before = audioOut_.size();
        audioBlock(b);
        if (audioOut_.size() == before) return;   // incomplete block: the rest of the packet is dropped
        b.align32();
    }
}

void BinkMovie::audioBlock(Bits& gb) {
    const int channels = audioChannels_, n = frameLen_;
    std::vector<float> out(size_t(n) * channels);
    std::vector<float> coeffs(size_t(n) + 2);
    float quant[25];
    gb.skip(2);
    for (int ch = 0; ch < channels; ++ch) {
        if (gb.left() < 58) return;
        auto getFloat = [&] {
            const int power = int(gb.get(5));
            float f = std::ldexp(float(gb.get(23)), power - 23);
            if (gb.get(1)) f = -f;
            return f;
        };
        coeffs[0] = getFloat() * root_;
        coeffs[1] = getFloat() * root_;
        if (gb.left() < numBands_ * 8) return;
        for (int i = 0; i < numBands_; ++i) quant[i] = quantTable_[std::min(int(gb.get(8)), 95)];
        int k = 0;
        float q = quant[0];
        for (int i = 2; i < n;) {
            int j = gb.get(1) ? i + audioRle[gb.get(4)] * 8 : i + 8;
            j = std::min(j, n);
            const int width = int(gb.get(4));
            if (width == 0) {
                std::fill(coeffs.begin() + i, coeffs.begin() + j, 0.0f);
                i = j;
                while (bands_[k] < unsigned(i)) q = quant[k++];
            } else {
                for (; i < j; ++i) {
                    if (bands_[k] == unsigned(i)) q = quant[k++];
                    const int coeff = int(gb.get(width));
                    coeffs[i] = coeff ? (gb.get(1) ? -q * float(coeff) : q * float(coeff)) : 0.0f;
                }
            }
        }
        if (gb.left() < 0) return;
        std::vector<float> channel(static_cast<size_t>(n));
        dct3(coeffs.data(), channel.data());
        for (int i = 0; i < n; ++i) out[size_t(i) * channels + ch] = channel[size_t(i)];
    }
    // The block's start fades from the previous block's end (each channel's ramp offset by its index), its end is kept for the next.
    const int count = overlapLen_ * channels;
    for (int ch = 0; ch < channels; ++ch) {
        if (!firstBlock_)
            for (int i = 0, j = ch; i < overlapLen_; ++i, j += channels) {
                float& o = out[size_t(i) * channels + ch];
                o = (previous_[ch][size_t(i)] * float(count - j) + o * float(j)) / float(count);
            }
        for (int i = 0; i < overlapLen_; ++i) previous_[ch][size_t(i)] = out[size_t(n - overlapLen_ + i) * channels + ch];
    }
    firstBlock_ = false;
    audioOut_.insert(audioOut_.end(), out.begin(), out.begin() + std::ptrdiff_t(n - overlapLen_) * channels);
}

// The decoder's DCT (av_tx AV_TX_FLOAT_DCT, inverse, scale 1 / frame length, after coeffs[0] /= 0.5), fitted to ffmpeg's output:
// out[m] = 2 / N x (c[0] + sum over k >= 1 of c[k] cos(pi k (2m + 1) / 2N)). Computed as a complex FFT of c[k] e^(i pi k / 2N): the
// real part at n is out[2n], at N - 1 - n out[2n + 1].
void BinkMovie::dct3(float* coeffs, float* out) {
    const int n = frameLen_;
    fftRe_.resize(size_t(n));
    fftIm_.resize(size_t(n));
    for (int k = 0; k < n; ++k) {
        fftRe_[size_t(k)] = coeffs[k] * twiddle_[0][2 * size_t(k)];
        fftIm_[size_t(k)] = coeffs[k] * twiddle_[0][2 * size_t(k) + 1];
    }
    // In-place radix-2 FFT with e^(+2 pi i k n / N).
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(fftRe_[size_t(i)], fftRe_[size_t(j)]);
            std::swap(fftIm_[size_t(i)], fftIm_[size_t(j)]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int step = n / len;
        for (int i = 0; i < n; i += len)
            for (int j = 0; j < len / 2; ++j) {
                const float wr = twiddle_[1][2 * size_t(j * step)], wi = twiddle_[1][2 * size_t(j * step) + 1];
                const size_t a = size_t(i + j), b = size_t(i + j + len / 2);
                const float xr = fftRe_[b] * wr - fftIm_[b] * wi, xi = fftRe_[b] * wi + fftIm_[b] * wr;
                fftRe_[b] = fftRe_[a] - xr;
                fftIm_[b] = fftIm_[a] - xi;
                fftRe_[a] += xr;
                fftIm_[a] += xi;
            }
    }
    const float scale = 2.0f / float(n);
    for (int i = 0; i < n / 2; ++i) {
        out[2 * i] = fftRe_[size_t(i)] * scale;
        out[2 * i + 1] = fftRe_[size_t(n - 1 - i)] * scale;
    }
}

}  // namespace media
