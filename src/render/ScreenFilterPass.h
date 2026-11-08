// ScreenFilterPass.h - the SCRF screen filters (shaders/screenfilter.hlsl): a copy of the finished world frame, drawn back over it through
// TRPostFX_ScreenFilter's pixel shader, blended SRCALPHA / INVSRCALPHA (technique 0) or SRCALPHA / ONE (technique 1).
#pragma once

#include <SDL3/SDL.h>

namespace render {

class ScreenFilterPass {
public:
    struct Filter {
        int base = 0;               // 0 Normal, 1 GreyScale, 2 White, 3 Red
        int variant = 0;            // 0 multiply, 1 add, 2 detail
        int technique = 0;          // 0 alpha blended, 1 additive
        float intensity = 0, alpha = 0, distortion = 0, overlayAlpha = 0;
        SDL_GPUTexture* overlay = nullptr;    // nullptr: white
        SDL_GPUTexture* distort = nullptr;    // nullptr: flat (0.5, 0.5)
    };

    ScreenFilterPass(SDL_GPUDevice* device, SDL_GPUTextureFormat targetFormat);
    ~ScreenFilterPass();
    ScreenFilterPass(const ScreenFilterPass&) = delete;
    ScreenFilterPass& operator=(const ScreenFilterPass&) = delete;

    // Draws the filter over target (width x height), reading a copy of it.
    void draw(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, int width, int height, const Filter& filter);

private:
    SDL_GPUDevice* device_;
    SDL_GPUTextureFormat format_;
    SDL_GPUShader* vertex_ = nullptr;
    SDL_GPUShader* fragment_ = nullptr;
    SDL_GPUGraphicsPipeline* blended_ = nullptr;
    SDL_GPUGraphicsPipeline* additive_ = nullptr;
    SDL_GPUSampler* clamp_ = nullptr;
    SDL_GPUSampler* wrap_ = nullptr;
    SDL_GPUTexture* copy_ = nullptr;
    SDL_GPUTexture* white_ = nullptr;
    SDL_GPUTexture* flat_ = nullptr;
    int copyWidth_ = 0, copyHeight_ = 0;
};

}  // namespace render
