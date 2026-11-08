// ScreenFilterPass.cpp - see ScreenFilterPass.h.
#include "ScreenFilterPass.h"

#include "UiShaders.h"
#include "../scene/Renderer.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace render {
namespace {

SDL_GPUShader* shader(SDL_GPUDevice* device, const unsigned char* dxil, size_t dxilSize, const unsigned char* spirv, size_t spirvSize,
                      SDL_GPUShaderStage stage, uint32_t samplers, uint32_t uniforms) {
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
    SDL_GPUShaderCreateInfo info = {};
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        info.code = spirv;
        info.code_size = spirvSize;
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    } else {
        info.code = dxil;
        info.code_size = dxilSize;
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
    }
    info.entrypoint = stage == SDL_GPU_SHADERSTAGE_VERTEX ? "vsMain" : "psMain";
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_uniform_buffers = uniforms;
    SDL_GPUShader* s = SDL_CreateGPUShader(device, &info);
    if (!s) throw std::runtime_error(std::string("screen filter shader: ") + SDL_GetError());
    return s;
}

// A 1 x 1 RGBA8 texture of one colour (the filter's stand-in when its overlay / distortion map is missing).
SDL_GPUTexture* pixel(SDL_GPUDevice* device, const uint8_t rgba[4]) {
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = info.height = 1;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);
    SDL_GPUTransferBufferCreateInfo t = {};
    t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    t.size = 4;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &t);
    if (!texture || !transfer) throw std::runtime_error(std::string("screen filter texture: ") + SDL_GetError());
    std::memcpy(SDL_MapGPUTransferBuffer(device, transfer, false), rgba, 4);
    SDL_UnmapGPUTransferBuffer(device, transfer);
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTextureTransferInfo source = {};
    source.transfer_buffer = transfer;
    SDL_GPUTextureRegion region = {};
    region.texture = texture;
    region.w = region.h = region.d = 1;
    SDL_UploadToGPUTexture(copy, &source, &region, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(commands);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    return texture;
}

}  // namespace

ScreenFilterPass::ScreenFilterPass(SDL_GPUDevice* device, SDL_GPUTextureFormat targetFormat) : device_(device), format_(targetFormat) {
    vertex_ = shader(device, screenFilterVertexDxil, sizeof screenFilterVertexDxil, screenFilterVertexSpirv, sizeof screenFilterVertexSpirv,
                     SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    fragment_ = shader(device, screenFilterFragmentDxil, sizeof screenFilterFragmentDxil, screenFilterFragmentSpirv,
                       sizeof screenFilterFragmentSpirv, SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 1);
    for (int additive = 0; additive < 2; ++additive) {
        SDL_GPUColorTargetDescription target = {};
        target.format = targetFormat;
        target.blend_state.enable_blend = true;
        target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        target.blend_state.dst_color_blendfactor = additive ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;   // the frame keeps its alpha
        target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = vertex_;
        info.fragment_shader = fragment_;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.num_color_targets = 1;
        info.target_info.color_target_descriptions = &target;
        (additive ? additive_ : blended_) = SDL_CreateGPUGraphicsPipeline(device, &info);
        if (!(additive ? additive_ : blended_)) throw std::runtime_error(std::string("screen filter pipeline: ") + SDL_GetError());
    }
    SDL_GPUSamplerCreateInfo s = {};
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_LINEAR;
    s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    clamp_ = SDL_CreateGPUSampler(device, &s);
    s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;   // sampler states NOT traced
    wrap_ = SDL_CreateGPUSampler(device, &s);
    const uint8_t white[4] = {255, 255, 255, 255}, flat[4] = {128, 128, 255, 255};
    white_ = pixel(device, white);
    flat_ = pixel(device, flat);
}

ScreenFilterPass::~ScreenFilterPass() {
    for (SDL_GPUGraphicsPipeline* p : {blended_, additive_})
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    for (SDL_GPUSampler* s : {clamp_, wrap_})
        if (s) SDL_ReleaseGPUSampler(device_, s);
    for (SDL_GPUTexture* t : {copy_, white_, flat_})
        if (t) SDL_ReleaseGPUTexture(device_, t);
    if (vertex_) SDL_ReleaseGPUShader(device_, vertex_);
    if (fragment_) SDL_ReleaseGPUShader(device_, fragment_);
}

void ScreenFilterPass::draw(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, int width, int height, const Filter& filter) {
    if (width <= 0 || height <= 0) return;
    if (!copy_ || copyWidth_ != width || copyHeight_ != height) {
        if (copy_) SDL_ReleaseGPUTexture(device_, copy_);
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = format_;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = Uint32(width);
        info.height = Uint32(height);
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        copy_ = SDL_CreateGPUTexture(device_, &info);
        copyWidth_ = width;
        copyHeight_ = height;
        if (!copy_) return;
    }
    // The game's RenderTarget: a copy of the frame so far (WaterPhase::InitRefractionScene's).
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTextureLocation from = {}, to = {};
    from.texture = target;
    to.texture = copy_;
    SDL_CopyGPUTextureToTexture(copy, &from, &to, Uint32(width), Uint32(height), 1, false);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUColorTargetInfo t = {};
    t.texture = target;
    t.load_op = SDL_GPU_LOADOP_LOAD;
    t.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &t, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, filter.technique == 1 ? additive_ : blended_);
    const SDL_GPUTextureSamplerBinding bindings[3] = {{copy_, clamp_}, {filter.overlay ? filter.overlay : white_, wrap_},
                                                      {filter.distort ? filter.distort : flat_, wrap_}};
    for (int k = 0; k < 3; ++k) countTextureBind();
    SDL_BindGPUFragmentSamplers(pass, 0, bindings, 3);
    const float uniforms[12] = {1.0f / float(width), 1.0f / float(height), 0, 0,
                                filter.intensity, filter.alpha, filter.distortion, filter.overlayAlpha,
                                float(filter.base), float(filter.variant), 0, 0};
    SDL_PushGPUFragmentUniformData(commands, 0, uniforms, sizeof uniforms);
    countDraw(3);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

}  // namespace render
