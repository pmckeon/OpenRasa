// AntiAliasPass.cpp - see AntiAliasPass.h.
#include "AntiAliasPass.h"

#include "UiShaders.h"
#include "../scene/Renderer.h"

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
    if (!s) throw std::runtime_error(std::string("anti-alias shader: ") + SDL_GetError());
    return s;
}

}  // namespace

AntiAliasPass::AntiAliasPass(SDL_GPUDevice* device, SDL_GPUTextureFormat targetFormat) : device_(device) {
    vertex_ = shader(device, antialiasVertexDxil, sizeof antialiasVertexDxil, antialiasVertexSpirv, sizeof antialiasVertexSpirv, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    fragment_ = shader(device, antialiasFragmentDxil, sizeof antialiasFragmentDxil, antialiasFragmentSpirv, sizeof antialiasFragmentSpirv,
                       SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    SDL_GPUColorTargetDescription target = {};
    target.format = targetFormat;
    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = vertex_;
    info.fragment_shader = fragment_;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.num_color_targets = 1;
    info.target_info.color_target_descriptions = &target;
    pipeline_ = SDL_CreateGPUGraphicsPipeline(device, &info);
    if (!pipeline_) throw std::runtime_error(std::string("anti-alias pipeline: ") + SDL_GetError());
    SDL_GPUSamplerCreateInfo s = {};   // point sampling: one source texel per screen pixel
    s.min_filter = SDL_GPU_FILTER_NEAREST;
    s.mag_filter = SDL_GPU_FILTER_NEAREST;
    s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device, &s);
}

AntiAliasPass::~AntiAliasPass() {
    if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
    if (sampler_) SDL_ReleaseGPUSampler(device_, sampler_);
    if (vertex_) SDL_ReleaseGPUShader(device_, vertex_);
    if (fragment_) SDL_ReleaseGPUShader(device_, fragment_);
}

void AntiAliasPass::draw(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* source, SDL_GPUTexture* target, int width, int height) {
    SDL_GPUColorTargetInfo t = {};
    t.texture = target;
    t.load_op = SDL_GPU_LOADOP_DONT_CARE;
    t.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &t, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipeline_);
    SDL_GPUTextureSamplerBinding binding = {source, sampler_};
    countTextureBind();
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    const float uniforms[4] = {1.0f / float(width > 0 ? width : 1), 1.0f / float(height > 0 ? height : 1), 0, 0};
    SDL_PushGPUFragmentUniformData(commands, 0, uniforms, sizeof uniforms);
    countDraw(3);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

}  // namespace render
