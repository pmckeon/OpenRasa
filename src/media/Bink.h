// Bink.h - Bink 1 movies (data/movies/*.bik; the original client plays them through RAD's binkw32.dll): the container, the video
// decoder and the DCT audio decoder, ported from FFmpeg n7.1 (libavformat/bink.c, libavcodec/bink.c, binkdsp.c, binkaudio.c; LGPL-2.1-
// or-later, used here under the GPL-3.0-or-later its section 3 allows). Only what the game's files need: Bink 1 revisions 'f' - 'k'
// (all shipped movies are 'i'), YUV 4:2:0 with an optional alpha plane, DCT audio (every shipped track; RDFT audio is refused).
// Checked against ffmpeg's own decode of ncsoft.bik: video planes byte-identical, audio within float rounding.
#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace media {

class BinkMovie {
public:
    bool open(const std::string& path, std::string* error = nullptr);
    bool isOpen() const { return file_.is_open(); }

    int width() const { return width_; }
    int height() const { return height_; }
    uint32_t frameCount() const { return frameCount_; }
    double framesPerSecond() const { return fpsDen_ ? double(fpsNum_) / fpsDen_ : 0.0; }
    // The first audio track (the only one in the shipped movies); 0 channels when there is none.
    int audioRate() const { return audioRate_; }
    int audioChannels() const { return audioChannels_; }

    // Decodes the next frame (0, 1, ... frameCount() - 1): the picture and the audio stored with it. False at the end or on bad data.
    bool decodeNext(std::string* error = nullptr);
    uint32_t nextFrame() const { return next_; }

    // The last decoded picture: planes 0 Y, 1 U (Cb), 2 V (Cr), 3 alpha (when the movie has it); limited-range BT.601 values
    // (the 'k' revision is full range: fullRange()).
    const uint8_t* plane(int p) const { return frames_[current_][p].data(); }
    int stride(int p) const { return strides_[p]; }
    bool hasAlpha() const { return hasAlpha_; }
    bool fullRange() const { return version_ == 'k'; }

    // Audio decoded so far and not taken yet, interleaved floats (-1..1).
    std::vector<float> takeAudio();

private:
    struct Tree {
        int vlc = 0;
        uint8_t syms[16] = {};
    };
    struct Bundle {
        int len = 0;
        Tree tree;
        std::vector<uint8_t> data;
        uint8_t* curDec = nullptr;
        uint8_t* curPtr = nullptr;
    };
    class Bits;

    bool decodePlane(Bits& b, int planeIndex, bool chroma);
    void decodeAudioPacket(const uint8_t* data, size_t size);
    void audioBlock(Bits& b);
    void dct3(float* coeffs, float* out);

    std::ifstream file_;
    std::vector<uint32_t> index_;
    uint32_t fileSize_ = 0, frameCount_ = 0, fpsNum_ = 0, fpsDen_ = 0, next_ = 0;
    int width_ = 0, height_ = 0, version_ = 0;
    bool hasAlpha_ = false, swapPlanes_ = false;
    int audioTracks_ = 0, audioRate_ = 0, audioChannels_ = 0;
    bool audioDct_ = false;

    // Video: two pictures (the one being decoded and the previous one), four planes each.
    std::vector<uint8_t> frames_[2][4];
    int strides_[4] = {}, rows_[4] = {};
    int current_ = 0;
    bool havePrevious_ = false;
    Bundle bundles_[9];
    Tree colHigh_[16];
    int colLastVal_ = 0;

    // Audio.
    int frameLen_ = 0, overlapLen_ = 0, numBands_ = 0;
    float root_ = 0;
    float quantTable_[96] = {};
    unsigned bands_[26] = {};
    std::vector<float> previous_[2];
    bool firstBlock_ = true;
    std::vector<float> audioOut_;
    std::vector<float> twiddle_[2];      // the DCT's e^(i pi k / 2N) and the FFT's roots
    std::vector<float> fftRe_, fftIm_;
};

}  // namespace media
