#pragma once
// Real-time quality scaling (TRasa::TRRQS, render settings + 0xE0; constructor FUN_00846c90, update FUN_00847790 from
// GameClient::PostRender, traced 2026-11-01). When the frame rate falls below Client.Graphics.RealTimeQualityScaling the items are
// degraded one at a time - each ramping its render setting from the normal to the degraded value - and restored in reverse when it
// recovers. The values below are the render settings the rest of the client reads.
#include <string>
#include <vector>

namespace engine::rqs {

// The scaled render settings (TRRQS + 0x58 .. + 0x8c = render settings + 0x138 .. + 0x16c), at full quality after the start-up reset.
struct Values {
    float viewDistanceScalar = 1.0f;       // + 0x58 "View Distance Scalar": the main view's far plane (FUN_007fd9e0)
    float farCullLayerScalar = 1.0f;       // + 0x5c "Far CullLayer Scalar": culling layers 2 / 8 / 9 / 10 (FUN_00856c00)
    float sfxRelevanceThreshold = 14.0f;   // + 0x60 "Sfx Relevance Threshhold I / II": package sounds and accessories, the particle step
    float drawAttachedScalar = 1.0f;       // + 0x64 "DrawAttachedScalar": LODHandler_TR::Update (FUN_0081bd70)
    float relevanceRange = 50.0f;          // + 0x68 "Relevance Range": SceneBody relevance (FUN_00817e70)
    float particleDegradation = 0.005f;    // + 0x70 "Particle Degradation": the particle step interval
    float lightClipDistance = 1.0f;        // + 0x74 "Light ClipDistance": no reader found
    float legacyLightPerType = 0.5f;       // + 0x78 "Legacy Light Per Type": the point-light cap of a LightTriage (FUN_008476a0)
    float relevanceQualityScale = 1.0f;    // + 0x7c "Relevance Quality Scale"
    float animationDegradation = 0.005f;   // + 0x80 "Animation Degradation I / II": the animation interval
    bool shadowDistance = true;            // + 0x84 "Shadow Distance": 25 + 50 x ShadowDistance, else 25 m (FUN_008475b0)
    bool staticShadows = true;             // + 0x85 "Static Shadows": static lights' shadow maps (FUN_00806f00)
    bool dynamicShadows = true;            // + 0x86 "Dynamic Shadows": the other lights' shadow maps, the sun's included
    bool fakeShadows = true;               // + 0x87 "Fake Shadows": the blob shadows (FUN_007f0120)
    bool specular = true;                  // + 0x88 "Specular Lighting": the specular buffer (FUN_00807640)
    bool refractions = true;               // + 0x89 "Do Refractions": RefractionPhase / water (FUN_007caa60, FUN_007c95b0)
    bool antiAlias = true;                 // + 0x8a "Do AntiAliasing": the software anti-aliasing pass (FUN_007be550)
    float flora = 0.5f;                    // + 0x8c "Flora Density I / II": the clutter's drawn share (FUN_0073b930)
};
const Values& values();

// GameClient::PostRender (0x004d2580): the update with GameClient + 0x32c (GetFramerate). While render settings bit 12 ("Disable
// Real-time Scaling") is clear it steps the items by the seconds since its last call; when the bit is set it resets them once.
void update(float framesPerSecond, bool disabled);
// The threshold (TRRQS + 0x50): Client.Graphics.RealTimeQualityScaling at start-up and on a change (FUN_004cd8c0, FUN_00539b50).
void setThreshold(float framesPerSecond);
// The items' names in the order the constructor adds them (GetRQSValuesList walks that list, + 0x30).
const std::vector<std::string>& itemNames();
// GetRQSEnabled / EnableRQSValue (FUN_007e3a10 / FUN_007e3940): an item's enabled flag; disabling resets that item, and enabling
// one resets them all (0x004d3b40, TRRQS slot 3). False / nothing for an unknown name.
bool enabled(const std::string& name);
void enable(const std::string& name, bool on);
// TRRQS slot 4 (FUN_007e38f0): how far the scaling has gone, 0 .. 100 (GetPerformanceData's third value).
float percent();

}  // namespace engine::rqs
