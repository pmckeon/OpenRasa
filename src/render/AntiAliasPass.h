// AntiAliasPass.h - the game's software anti-aliasing (render settings bit 6 clear, see shaders/antialias.hlsl): the world's finished
// frame drawn into the UI frame through TRAntiAlias's edge blend.
#pragma once

#include <SDL3/SDL.h>

namespace render {

class AntiAliasPass {
public:
    AntiAliasPass(SDL_GPUDevice* device, SDL_GPUTextureFormat targetFormat);
    ~AntiAliasPass();
    AntiAliasPass(const AntiAliasPass&) = delete;
    AntiAliasPass& operator=(const AntiAliasPass&) = delete;

    // Draws source (width x height, the same size as target) over target (its own render pass) through the edge blend.
    void draw(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* source, SDL_GPUTexture* target, int width, int height);

private:
    SDL_GPUDevice* device_;
    SDL_GPUShader* vertex_ = nullptr;
    SDL_GPUShader* fragment_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
};

}  // namespace render
