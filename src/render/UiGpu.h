// UiGpu.h - draws UiCanvas's quads (Options::quads) with SDL GPU: one draw per quad (shaders/ui.hlsl), scissored to its clip, with
// the pipeline of its blend (SRCALPHA / INVSRCALPHA, SRCALPHA / ONE, ONE / ONE). Textures are uploaded once per image (by its serial)
// and released when no quad has used them for a while.
#pragma once

#include "../engine/UiCanvas.h"

#include <SDL3/SDL.h>

#include <map>
#include <vector>

namespace render {

class UiGpu {
public:
    UiGpu(SDL_GPUDevice* device, SDL_GPUTextureFormat targetFormat);
    ~UiGpu();
    UiGpu(const UiGpu&) = delete;
    UiGpu& operator=(const UiGpu&) = delete;

    // Uploads the textures the quads need (a copy pass on commands; call before the render pass).
    void prepare(SDL_GPUCommandBuffer* commands, const std::vector<UiCanvas::Quad>& quads);
    // Draws the quads into an open render pass over a width x height target.
    void draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const std::vector<UiCanvas::Quad>& quads, int width, int height);

private:
    SDL_GPUTexture* texture(const UiCanvas::Image* image);
    SDL_GPUDevice* device_;
    SDL_GPUShader* vertex_ = nullptr;
    SDL_GPUShader* fragment_ = nullptr;
    SDL_GPUGraphicsPipeline* pipelines_[3] = {};
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUTexture* white_ = nullptr;
    struct Entry {
        SDL_GPUTexture* texture = nullptr;
        uint64_t lastUsed = 0;
    };
    std::map<uint64_t, Entry> textures_;        // by image serial
    std::vector<const UiCanvas::Image*> pending_;   // to upload in prepare()
    uint64_t frame_ = 0;
};

}  // namespace render
