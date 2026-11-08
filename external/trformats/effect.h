// Compiled effects (.fxc in effects.glm): the technique table the engine chooses from.
//
// A .fxc is a chunk container holding a D3DX fx_2_0 effect (magic 0xFEFF0901; layout per Wine's d3dx9 effect.c). The engine reads each
// technique's annotations into a technique description (effDeviceEffect::BuildTechniqueDescriptions, retail FUN_005c1d40): RenderMode
// (0 forward / RM_NORMAL, 1 shadow map, 2 overdraw, 3 complexity, 4 water penetrate, 5 deferred), TransformMode (0 static, 1 skinned,
// 2 morph indexed, 3 morph non-indexed), TechniqueMode, ComplexityFactor; techniques without an annotation take 0.
// gfxDisplayList::AddPieceInstance (2007 build 0x00950b40, read 2026-11-04) asks GetTechniqueByRenderMode (0x009574b0) for the
// piece's list of (render mode, transform mode, technique mode) - no fallback to another mode: an empty list draws nothing. The mode is
// the view's, except that a phase whose PhasePriorityKey is at or above the display list's deferred-lighting cutoff (Opaque 1.0 is
// below it; Sky 1.99, Translucent 2.0, Emissive 2.015, Additive 2.02 ... above) uses mode 0 in a deferred view and is skipped in a
// shadow view.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tr {

struct EffectTechnique {
    std::string name;
    int32_t renderMode = 0;
    int32_t transformMode = 0;
    int32_t techniqueMode = 0;
    float complexity = 0.0f;
};

struct EffectInfo {
    bool valid = false;                      // an fx_2_0 effect was found and read
    std::vector<EffectTechnique> techniques;
    // The effect's own "Phase" parameter (a string: "Opaque", "Emissive", "Additive", "Water", ...) - the phase a piece takes when its
    // material does not set one (the material's parameters override the effect's defaults). "" when the effect has none.
    std::string defaultPhase;
    // Whether the engine finds a technique for (render mode, transform mode, technique mode).
    bool hasTechnique(int32_t renderMode, int32_t transformMode, int32_t techniqueMode = 0) const;
};

// Reads the technique table of a compiled effect (.fxc / .shc bytes). Returns valid = false when no fx_2_0 effect is in it.
EffectInfo readEffectInfo(const std::vector<uint8_t>& data);

// The compiled file of an effect named in a material ("TRDiffMapVertColor.fx"): the engine composes "<name><profile>.fxc" with its
// shader profile's suffix (ConvertToCompiledId, retail 0x005c39a0; GetProfileStringA 0x006b67d0: SP_1, SP_2, SP_2a, SP_2b, SP_3). The
// highest suffix the archives hold is taken (UNVERIFIED: how the engine falls back when its profile's file is missing is not traced;
// many effects ship only up to SP_2b). Empty when none exists.
std::string compiledEffectName(const std::string& effect, const std::function<bool(const std::string&)>& exists);

} // namespace tr
