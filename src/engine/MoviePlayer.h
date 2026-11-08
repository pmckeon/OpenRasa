// MoviePlayer.h - a Movie widget's playback (UIMovie, the original on RAD's binkw32.dll; traced 2026-10-25 in tabula_rasa.exe):
//   Play (FUN_008e0e10): BinkOpen(file, 0) of the name looked up in the game's paths ([Paths] Movies = "data/movies|.mov|.bik|.avi"),
//     an X8R8G8B8 texture of the movie's size, the widget shown (UIMovie slot 65 FUN_008e1370 tells its observers - a MovieComposite
//     shows itself with it), the volume BinkSetVolume(track 0, DAT_00d51a98 + 0xF8 x 32768) - the audio manager's float there is
//     taken to be Client.Audio.MasterVolume (+ 0xFC / + 0x100 are the effect and music volumes; UNVERIFIED). Returns whether it opened;
//     a file that does not open is closed at once.
//   Each render (FUN_008e13b0): BinkWait, and when a frame is due (or it is the first) BinkDoFrame + BinkCopyToBuffer + BinkNextFrame;
//     the picture keeps the movie's shape (UIMovie + 0xDC, 1 from the constructor FUN_008e0c80; FUN_008e14a0): scaled to the widget's
//     width and centred, or, when that overflows it, to its height (FUN_008e0bd0 centres).
//   Each update (FUN_008e1160): when Bink's next frame number reaches the frame count the movie is closed - so the last frame is never
//     shown; closing (FUN_008e10d0, also Stop) hides the widget and queues the movie's name, and the next update sends
//     eMessageMovieEnded {'Movie': name} (also through the observers: a MovieComposite sends it as itself, FUN_008f3890).
// Here: media::BinkMovie decodes (FFmpeg's decoder, checked byte for byte), the audio plays through an SDL audio stream and paces the
// pictures (the shipped movies store their audio ~0.72 s ahead, so it is always queued; without audio, wall time). Pictures are
// converted to RGBA as limited-range BT.601 with each chroma sample over 2 x 2 pixels - binkw32's own conversion is NOT traced.
#pragma once

#include "../media/Bink.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::ui {

class MoviePlayback {
public:
    ~MoviePlayback();
    bool open(const std::string& path, float volume);
    // Decodes the frames that are due (the first at once). False once Bink's next frame number would reach the frame count (the
    // original closes then).
    bool update();
    int width() const { return movie_.width(); }
    int height() const { return movie_.height(); }
    // The current picture on the GPU (uploaded on the command buffer when it changed; outside any pass).
    SDL_GPUTexture* texture(SDL_GPUDevice* device, SDL_GPUCommandBuffer* commands);

private:
    double clock() const;
    void convert();

    media::BinkMovie movie_;
    SDL_AudioStream* audio_ = nullptr;
    uint64_t startNs_ = 0, pushed_ = 0;   // audio sample frames given to the stream
    int shown_ = -1;                      // the frame on screen
    std::vector<uint8_t> rgba_;
    bool changed_ = false;
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTexture* texture_ = nullptr;
};

}  // namespace engine::ui
