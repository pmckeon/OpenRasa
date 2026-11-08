// UiGpu.cpp - see UiGpu.h.
#include "UiGpu.h"

#include "UiShaders.h"
#include "../scene/Renderer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace render {
namespace {

struct VertexUniforms {
    float rect[4];
    float screen[4];
};
struct FragmentUniforms {
    float colour[4];
    float region[4];
    float tiling[4];
    float rotation[4];
    float mouse[4];
    float flags[4];
};

SDL_GPUShader* shader(SDL_GPUDevice* device, const unsigned char* dxil, size_t dxilSize, const unsigned char* spirv, size_t spirvSize,
                      const char* entry, SDL_GPUShaderStage stage, uint32_t samplers) {
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
    SDL_GPUShaderCreateInfo info = {};
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        info.code = spirv;
        info.code_size = spirvSize;
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    } else if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
        info.code = dxil;
        info.code_size = dxilSize;
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
    } else {
        throw std::runtime_error("UI: the GPU takes neither SPIR-V nor DXIL shaders");
    }
    info.entrypoint = entry;
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_uniform_buffers = 1;
    SDL_GPUShader* s = SDL_CreateGPUShader(device, &info);
    if (!s) throw std::runtime_error(std::string("UI shader: ") + SDL_GetError());
    return s;
}

}  // namespace

UiGpu::UiGpu(SDL_GPUDevice* device, SDL_GPUTextureFormat targetFormat) : device_(device) {
    vertex_ = shader(device, uiVertexDxil, sizeof uiVertexDxil, uiVertexSpirv, sizeof uiVertexSpirv, "vsMain", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    fragment_ = shader(device, uiFragmentDxil, sizeof uiFragmentDxil, uiFragmentSpirv, sizeof uiFragmentSpirv, "psMain",
                       SDL_GPU_SHADERSTAGE_FRAGMENT, 3);
    for (int blend = 0; blend < 3; ++blend) {
        SDL_GPUColorTargetDescription target = {};
        target.format = targetFormat;
        target.blend_state.enable_blend = true;
        target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        target.blend_state.src_color_blendfactor = blend == 2 ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        target.blend_state.dst_color_blendfactor = blend == 0 ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
        target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = vertex_;
        info.fragment_shader = fragment_;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.num_color_targets = 1;
        info.target_info.color_target_descriptions = &target;
        pipelines_[blend] = SDL_CreateGPUGraphicsPipeline(device, &info);
        if (!pipelines_[blend]) throw std::runtime_error(std::string("UI pipeline: ") + SDL_GetError());
    }
    SDL_GPUSamplerCreateInfo s = {};
    s.min_filter = SDL_GPU_FILTER_LINEAR;
    s.mag_filter = SDL_GPU_FILTER_LINEAR;
    s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device, &s);
    // A 1 x 1 white texture: plain colours, unused mask slots.
    static UiCanvas::Image white;
    white.width = white.height = 1;
    white.rgba = {255, 255, 255, 255};
    white.serial = ~uint64_t(0);
    white_ = texture(&white);   // queued for the first prepare()
}

UiGpu::~UiGpu() {
    for (auto& [serial, e] : textures_) SDL_ReleaseGPUTexture(device_, e.texture);
    for (SDL_GPUGraphicsPipeline* p : pipelines_)
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    if (sampler_) SDL_ReleaseGPUSampler(device_, sampler_);
    if (vertex_) SDL_ReleaseGPUShader(device_, vertex_);
    if (fragment_) SDL_ReleaseGPUShader(device_, fragment_);
}

SDL_GPUTexture* UiGpu::texture(const UiCanvas::Image* image) {
    auto it = textures_.find(image->serial);
    if (it != textures_.end()) {
        it->second.lastUsed = frame_;
        return it->second.texture;
    }
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = uint32_t(std::max(image->width, 1));
    info.height = uint32_t(std::max(image->height, 1));
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* t = SDL_CreateGPUTexture(device_, &info);
    textures_[image->serial] = {t, frame_};
    pending_.push_back(image);
    return t;
}

void UiGpu::prepare(SDL_GPUCommandBuffer* commands, const std::vector<UiCanvas::Quad>& quads) {
    ++frame_;
    for (const UiCanvas::Quad& q : quads)
        for (const UiCanvas::Image* image : {q.image, q.mask, q.mouseMask})
            if (image) texture(image);
    // Textures no quad used for a while (text images of strings no longer shown).
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.texture != white_ && frame_ - it->second.lastUsed > 600) {
            SDL_ReleaseGPUTexture(device_, it->second.texture);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
    if (pending_.empty()) return;
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    for (const UiCanvas::Image* image : pending_) {
        auto it = textures_.find(image->serial);
        if (it == textures_.end() || image->rgba.size() < size_t(image->width) * size_t(image->height) * 4) continue;
        SDL_GPUTransferBufferCreateInfo tb = {};
        tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tb.size = uint32_t(image->rgba.size());
        SDL_GPUTransferBuffer* buffer = SDL_CreateGPUTransferBuffer(device_, &tb);
        void* mapped = SDL_MapGPUTransferBuffer(device_, buffer, false);
        std::memcpy(mapped, image->rgba.data(), image->rgba.size());
        SDL_UnmapGPUTransferBuffer(device_, buffer);
        SDL_GPUTextureTransferInfo from = {};
        from.transfer_buffer = buffer;
        SDL_GPUTextureRegion to = {};
        to.texture = it->second.texture;
        to.w = uint32_t(image->width);
        to.h = uint32_t(image->height);
        to.d = 1;
        SDL_UploadToGPUTexture(copy, &from, &to, false);
        SDL_ReleaseGPUTransferBuffer(device_, buffer);   // released once the upload is done
    }
    SDL_EndGPUCopyPass(copy);
    pending_.clear();
}

void UiGpu::draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const std::vector<UiCanvas::Quad>& quads, int width, int height) {
    int bound = -1;
    for (const UiCanvas::Quad& q : quads) {
        const int x0 = std::max(q.clip.x0, 0), y0 = std::max(q.clip.y0, 0);
        const int x1 = std::min(q.clip.x1, width), y1 = std::min(q.clip.y1, height);
        if (x1 <= x0 || y1 <= y0) continue;
        if (bound != q.blend) {
            SDL_BindGPUGraphicsPipeline(pass, pipelines_[q.blend]);
            bound = q.blend;
        }
        SDL_Rect scissor = {x0, y0, x1 - x0, y1 - y0};
        SDL_SetGPUScissor(pass, &scissor);
        SDL_GPUTextureSamplerBinding bindings[3] = {};
        if ((q.scene || q.movie) && !q.texture) continue;   // a SceneView whose view was not rendered, a Movie without a picture
        bindings[0].texture = q.texture ? static_cast<SDL_GPUTexture*>(q.texture) : q.image ? texture(q.image) : white_;
        bindings[1].texture = q.mask ? texture(q.mask) : white_;
        bindings[2].texture = q.mouseMask ? texture(q.mouseMask) : white_;
        for (auto& b : bindings) b.sampler = sampler_;
        countTextureBind();
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, 3);
        VertexUniforms vu = {{float(q.rect.x0), float(q.rect.y0), float(q.rect.x1), float(q.rect.y1)}, {float(width), float(height), 0, 0}};
        SDL_PushGPUVertexUniformData(commands, 0, &vu, sizeof vu);
        FragmentUniforms fu = {};
        std::copy(q.colour, q.colour + 4, fu.colour);
        std::copy(q.region, q.region + 4, fu.region);
        fu.tiling[0] = q.tileU;
        fu.tiling[1] = q.tileV;
        fu.tiling[2] = q.wrap ? 1.0f : 0.0f;
        std::copy(q.rotation, q.rotation + 4, fu.rotation);
        fu.mouse[0] = q.mouseX;
        fu.mouse[1] = q.mouseY;
        fu.mouse[2] = std::max(q.mouseSize, 1.0f);
        fu.flags[0] = q.mask ? 1.0f : 0.0f;
        fu.flags[1] = q.mouseMask ? (q.radarMask ? 2.0f : 1.0f) : 0.0f;
        fu.flags[2] = q.blend == 2 ? 1.0f : 0.0f;
        fu.flags[3] = q.rotate ? 1.0f : 0.0f;
        SDL_PushGPUFragmentUniformData(commands, 0, &fu, sizeof fu);
        countDraw(6);
        SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    }
}

}  // namespace render
