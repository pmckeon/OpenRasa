// GammaPass.h - the display gamma: the finished frame copied to the swapchain through the game's gamma ramp (see shaders/gamma.hlsl).
#pragma once

#include <SDL3/SDL.h>

namespace render {

class GammaPass {
public:
    GammaPass(SDL_GPUDevice* device, SDL_GPUTextureFormat targetFormat);
    ~GammaPass();
    GammaPass(const GammaPass&) = delete;
    GammaPass& operator=(const GammaPass&) = delete;

    // Draws source over target (its own render pass) with (c)^(1 / gamma) per channel.
    void draw(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* source, SDL_GPUTexture* target, float gamma);

private:
    SDL_GPUDevice* device_;
    SDL_GPUShader* vertex_ = nullptr;
    SDL_GPUShader* fragment_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
};

}  // namespace render
