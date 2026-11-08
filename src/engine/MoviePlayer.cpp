// MoviePlayer.cpp - see MoviePlayer.h.
#include "MoviePlayer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine::ui {

MoviePlayback::~MoviePlayback() {
    if (audio_) SDL_DestroyAudioStream(audio_);
    if (texture_) SDL_ReleaseGPUTexture(device_, texture_);
}

bool MoviePlayback::open(const std::string& path, float volume) {
    std::string error;
    if (!movie_.open(path, &error)) {
        std::fprintf(stderr, "[movie] %s: %s\n", path.c_str(), error.c_str());
        return false;
    }
    if (movie_.audioChannels()) {
        SDL_AudioSpec spec = {SDL_AUDIO_F32, movie_.audioChannels(), movie_.audioRate()};
        audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (audio_) {
            SDL_SetAudioStreamGain(audio_, volume);
            SDL_ResumeAudioStreamDevice(audio_);
        } else {
            std::fprintf(stderr, "[movie] no audio output: %s\n", SDL_GetError());
        }
    }
    startNs_ = SDL_GetTicksNS();
    return update();
}

// Seconds into the movie: the audio played so far, else wall time.
double MoviePlayback::clock() const {
    if (audio_) {
        const int queued = SDL_GetAudioStreamQueued(audio_);
        const double frames = double(pushed_) - double(std::max(queued, 0)) / (4.0 * movie_.audioChannels());
        return std::max(frames, 0.0) / movie_.audioRate();
    }
    return double(SDL_GetTicksNS() - startNs_) / 1e9;
}

bool MoviePlayback::update() {
    const double fps = movie_.framesPerSecond();
    const int due = shown_ < 0 ? 0 : int(std::floor(clock() * fps));
    bool decoded = false;
    while (shown_ < due && (shown_ < 0 || movie_.nextFrame() + 1 < movie_.frameCount())) {
        std::string error;
        if (!movie_.decodeNext(&error)) {
            std::fprintf(stderr, "[movie] frame %u: %s\n", movie_.nextFrame(), error.c_str());
            return false;
        }
        ++shown_;
        decoded = true;
        const std::vector<float> samples = movie_.takeAudio();
        if (audio_ && !samples.empty()) {
            SDL_PutAudioStreamData(audio_, samples.data(), int(samples.size() * sizeof(float)));
            pushed_ += samples.size() / size_t(movie_.audioChannels());
        }
    }
    if (decoded) convert();
    return movie_.nextFrame() + 1 < movie_.frameCount();
}

void MoviePlayback::convert() {
    const int w = movie_.width(), h = movie_.height();
    rgba_.resize(size_t(w) * h * 4);
    const uint8_t *py = movie_.plane(0), *pu = movie_.plane(1), *pv = movie_.plane(2);
    const bool full = movie_.fullRange();
    // BT.601 in 16.16 fixed point: limited range (Y 16-235, chroma 16-240) or full range (the 'k' revision).
    const int ky = full ? 65536 : 76309, rv = full ? 91881 : 104597, gu = full ? 22554 : 25675, gv = full ? 46802 : 53279,
              bu = full ? 116130 : 132201, y0 = full ? 0 : 16;
    for (int y = 0; y < h; ++y) {
        const uint8_t* yr = py + y * movie_.stride(0);
        const uint8_t* ur = pu + (y >> 1) * movie_.stride(1);
        const uint8_t* vr = pv + (y >> 1) * movie_.stride(2);
        uint8_t* out = rgba_.data() + size_t(y) * w * 4;
        for (int x = 0; x < w; ++x, out += 4) {
            const int l = (yr[x] - y0) * ky, u = ur[x >> 1] - 128, v = vr[x >> 1] - 128;
            out[0] = uint8_t(std::clamp((l + rv * v + 32768) >> 16, 0, 255));
            out[1] = uint8_t(std::clamp((l - gu * u - gv * v + 32768) >> 16, 0, 255));
            out[2] = uint8_t(std::clamp((l + bu * u + 32768) >> 16, 0, 255));
            out[3] = 255;
        }
    }
    changed_ = true;
}

SDL_GPUTexture* MoviePlayback::texture(SDL_GPUDevice* device, SDL_GPUCommandBuffer* commands) {
    if (rgba_.empty()) return nullptr;
    if (!texture_) {
        device_ = device;
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = uint32_t(movie_.width());
        info.height = uint32_t(movie_.height());
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        texture_ = SDL_CreateGPUTexture(device, &info);
        changed_ = true;
    }
    if (changed_ && texture_) {
        SDL_GPUTransferBufferCreateInfo tb = {};
        tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tb.size = uint32_t(rgba_.size());
        SDL_GPUTransferBuffer* buffer = SDL_CreateGPUTransferBuffer(device, &tb);
        void* mapped = SDL_MapGPUTransferBuffer(device, buffer, false);
        std::memcpy(mapped, rgba_.data(), rgba_.size());
        SDL_UnmapGPUTransferBuffer(device, buffer);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureTransferInfo from = {};
        from.transfer_buffer = buffer;
        SDL_GPUTextureRegion to = {};
        to.texture = texture_;
        to.w = uint32_t(movie_.width());
        to.h = uint32_t(movie_.height());
        to.d = 1;
        SDL_UploadToGPUTexture(copy, &from, &to, false);
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(device, buffer);
        changed_ = false;
    }
    return texture_;
}

}  // namespace engine::ui
