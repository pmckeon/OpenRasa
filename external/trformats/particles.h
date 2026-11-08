// particles.h - particle systems (.pfx, 2,097 files in the archives): the engine's "palantir" pfx library.
//
// Chunk file (CHNKBLXX, chunks as in .geo). Layouts from tabula_rasa.exe's unserializers (pfx*::Unserialize, names from
// the RTTI and Property_pfx* strings) and verified 2026-10-10: every byte of all 2,097 files is consumed.
//   PSYS v4 { u8 enabled, u32 n, n x (group, operator), BBOX, f32 warmupInterval, u32 warmupCount }  (v2: no enabled byte; v1: no
//             BBOX / tail)
//   group: PGRP (pfxParticleGroup) or PGRS { PGRP, u8 sortOrder } (pfxParticleGroupSorted)
//   PGRP v8 { u8 enabled, cstr name, u32 zBias, u32 maxParticles, u8 primitive, u8 referenceFrame,
//             u32 D3D SrcBlend, u32 DestBlend, u32 BlendOp, u8 fogEnabled, [primitive 2..4: ISTR texture, u32 billboard] }
//         v7 without the fogEnabled byte, v6 without enabled, v5 without name, v4 { u32 maxParticles, u8 primitive,
//         u8 referenceFrame, 3 x u32 blend, [ISTR, u32] }.
//   Operators (pfxParticleOperator) and generators (pfxFloat/Point/ColorGenerator) are chunks; see the kinds below.
//   Operators of chunk version >= 2 (VORT >= 3) start with u8 enabled.
// Semantics (decompiled; the exe function is named where it matters):
//   - Per frame each group's operator is applied with the time step dt (seconds).
//   - Particle: position, previous position, velocity, colour rgba, age (0..1), 1 / lifetime, size, growth, rotation
//     (turns), spin, stretch. MOVE integrates position += velocity dt, age += dt / lifetime, size += growth dt,
//     rotation += spin dt, and kills particles whose age reaches 1 (no MOVE: particles never age).
//   - EMIT (pfxEmit::Apply, FUN_009e2a50): emitter time t += dt; n = rate(t) * dt, the fraction rounded by
//     roundingMode (0 randomly, 1 accumulated), capped at maxParticles; each new particle takes point(t), velocity(t),
//     colour(t), size(t), growth(t), rotation(t), spin(t), stretch(t), 1 / lifetime(t), and is advanced by a random
//     fraction of dt. Reference frame World / Hybrid: position (and for Hybrid the velocity) taken to world space.
//   - SET* operators evaluate their generator at the particle's age, so keyframe times are fractions of a life.
//   - Generators keep state between calls: SEQx cycles its children per call, HLDx holds a value for round(count)
//     calls, LBDx moves linearly from the previous base value to the next over round(count) calls.
//   - SEQ (pfxSequence): stage k runs from its time until stage k + 1's (the last one forever); each frame its
//     operator gets the part of [t, t + dt] inside its window.
#pragma once

#include "trformats.h"

#include <string>
#include <vector>

namespace tr {

struct ParticleValue { float x = 0, y = 0, z = 0, w = 0; };   // float: x; point: xyz; colour: rgba (0..1)

enum class ParticleGeneratorKind : uint8_t {
    Constant,       // CONF / CONP / CONC: value
    Uniform,        // UNIF: minimum + (maximum - minimum) * random
    UnitRandom,     // UUNI: random 0..1
    Gaussian,       // GAUS: mean, deviation (normal distribution - UNVERIFIED shape)
    Sine,           // SINE: amplitude * sin(2 pi frequency (t - offset))
    Sequence,       // SEQF / SEQP / SEQC: children in turn, one per call
    Random,         // RNDF / RNDP / RNDC: a random child per call (UNVERIFIED: not decompiled)
    Hold,           // HLDF / HLDP / HLDC: children count, base; base value held for round(count) calls
    Blend,          // LBDF / LBDP: children count, base; linear walk from the last base value to the next
    Add,            // ADDF / ADDP / ADDC: children first + second
    Keys,           // KEYF / KEYP / KEYC: keys (time, value), linear, clamped at the ends
    Sphere,         // SPEP: centre + r (sin phi cos theta, cos phi, sin phi sin theta); children radius, theta (turns),
                    //       phi (half turns)
    Box,            // BOXP: children x, y, z
    Cylinder,       // CYLP: centre + distance * axis + radius (cos a axisU + sin a axisV); children distance, angle
                    //       (turns), radius
    Range,          // RNGP: centre + child(t) * axis;  RNGC: lerp(colour1, colour2, child(t))
    PointColour,    // PTCL: children point (rgb), alpha
    Geo,            // GEOG: points on a model (geo); not evaluated
};

struct ParticleKey {
    float time = 0;
    ParticleValue value;
};

struct ParticleGenerator {
    ParticleGeneratorKind kind = ParticleGeneratorKind::Constant;
    uint32_t tag = 0;                       // chunk tag (fourcc), e.g. CONF
    ParticleValue value;                    // Constant
    float minimum = 0, maximum = 1;         // Uniform
    float mean = 0, deviation = 0;          // Gaussian
    float amplitude = 0, frequency = 0, offset = 0;   // Sine
    Vec3 centre;                            // Sphere, Cylinder, Range (point)
    Vec3 axis;                              // Cylinder, Range (point): direction
    Vec3 axisU, axisV;                      // Cylinder
    ParticleValue colour1, colour2;         // Range (colour)
    std::vector<ParticleKey> keys;          // Keys
    std::vector<ParticleGenerator> children;    // roles per kind, see above
    std::string geo;                        // Geo
    uint32_t stateSlot = 0;                 // index of this generator's run-time state (ParticleSystem::stateCount)
};

// The i-th child, or a constant 1 if there is none (the engine's default for a missing generator).
const ParticleGenerator& generatorChild(const ParticleGenerator& generator, size_t index);

// pfxEmit (EMIT) / pfxEmitFromParticle (PEMT, no point).
struct ParticleEmitter {
    ParticleGenerator rate;                 // particles per second
    ParticleGenerator point;
    ParticleGenerator velocity;
    ParticleGenerator colour;
    ParticleGenerator size;
    ParticleGenerator growth;               // size per second
    ParticleGenerator rotation;             // turns
    ParticleGenerator spin;                 // turns per second
    ParticleGenerator stretch;              // the sprite is 2^stretch wider and 2^-stretch taller
    ParticleGenerator lifetime;             // seconds
    uint32_t roundingMode = 0;              // 0 Randomize, 1 Accumulate
};

// Domain of TOUT / BONC (pfxBox / pfxHalfSpace / pfxSphere, traced 2026-10-20): BOXS box (invert, centre, dimension, rotation in
// degrees - x pitch, y yaw, z roll, D3DXQuaternionRotationYawPitchRoll), HALF half space (normal, distance: inside where
// n . p + distance >= 0), SPHE sphere (centre, radius - stored squared -, v2: invert). Inside tests (TOUT) ignore invert; the
// bounce hit test uses it (see ParticleSim).
struct ParticleDomain {
    enum class Kind : uint8_t { Box, HalfSpace, Sphere };
    Kind kind = Kind::Box;
    Vec3 centre;
    Vec3 rotation;
    Vec3 dimension;
    Vec3 normal;
    float distance = 0;
    float radius = 0;
    bool invert = false;
};

enum class ParticleOperatorKind : uint8_t {
    Composite,          // COMP: children in order
    Sequence,           // "SEQ ": stages (time, optional operator in children)
    ApplyAfter,         // APAF: the child, once more than time seconds have passed
    Emit,               // EMIT
    EmitFromParticle,   // PEMT: emits at the particles of the PREVIOUS group of the system (ParticleSim)
    Move,               // MOVE
    Drag,               // DRAG: velocity += |velocity| * coefficient * dt * velocity
    Accelerate,         // ACCL: velocity += vector * dt
    AccelerateSpin,     // ACCS: spin += coefficient * dt
    Gravity,            // GRAV: velocity += (vector - position) * coefficient / (|vector - position|^2 + 0.001) * dt
    Vortex,             // VORT: vector (centre), axis, coefficient (magnitude) - approximated
    TargetColour,       // TCOL: colour moves towards target, rate time per second
    TargetAlpha,        // TALP: alpha += time * dt * (target.w - alpha)
    KeyColour,          // KCLR: colour from keys at the particle's age
    SetColour, SetGrowth, SetPosition, SetRotation, SetSize, SetVelocity, SetStretch, SetSpin,   // SETC SETG SETP SETR SETS SETV SEST SESP
    AlignToVelocity,    // ALTV: previous position = position + velocity (aligned sprites point along the velocity)
    TerminateOutside,   // TOUT: kills particles outside the domain
    Bounce,             // BONC (ParticleSim)
    Dummy,              // DUMM
};

struct ParticleStage {
    float time = 0;
    int32_t child = -1;                     // index into ParticleOperator::children, -1 = no operator
};

struct ParticleOperator {
    ParticleOperatorKind kind = ParticleOperatorKind::Dummy;
    uint32_t tag = 0;
    bool enabled = true;
    ParticleEmitter emitter;                // Emit, EmitFromParticle
    ParticleGenerator generator;            // Set*
    std::vector<ParticleOperator> children; // Composite, Sequence (stage operators), ApplyAfter (one)
    std::vector<ParticleStage> stages;      // Sequence
    float coefficient = 0;                  // Drag, AccelerateSpin, Gravity, Vortex
    Vec3 vector;                            // Accelerate, Gravity / Vortex centre
    Vec3 axis;                              // Vortex
    float time = 0;                         // ApplyAfter, TargetColour, TargetAlpha
    float unknown = 0;                      // ApplyAfter: second float (UNKNOWN)
    ParticleValue target;                   // TargetColour; TargetAlpha in w
    std::vector<ParticleKey> keys;          // KeyColour
    float friction = 0, resilience = 0, cutoff = 0;   // Bounce, as stored: tangential factor, normal factor, squared-speed cutoff
    ParticleDomain domain;                  // TerminateOutside, Bounce
    uint32_t stateSlot = 0;
};

struct ParticleGroup {
    uint32_t version = 0;
    bool enabled = true;                    // as stored (Enabled, group + 0x154); the system's reset sets it at start
    std::string name;
    uint32_t zBias = 0;                     // ZBias (group + 0x13c): draw sort key = depth x (1 - zBias x 1e-5), FUN_0066f650
    uint32_t maxParticles = 0;
    uint8_t primitive = 2;                  // 0 Point, 1 Line, 2 Sprite, 3 Aligned Sprite, 4 Oriented Sprite
    uint8_t referenceFrame = 0;             // 0 Object, 1 World, 2 Hybrid
    uint32_t sourceBlend = 5;               // D3DBLEND (5 SRCALPHA)
    uint32_t destinationBlend = 2;          // D3DBLEND (2 ONE, 6 INVSRCALPHA)
    uint32_t blendOperation = 1;            // D3DBLENDOP (1 ADD, 3 REVSUBTRACT)
    // v8 only. pfxParticleGroup::Serialize (2007 PDB 0x0095fdc0) writes GetFogEnabled here; SetFogEnabled stores it into the
    // effect's FogIsEnabled parameter - which no shader, expression or pass state of any PalParticleSystem*.fxc reads (checked
    // 2026-11-04: sprite / aligned / oriented effects always fog in the vertex shader from gFogDensity / gFogStartDist, FOGENABLE 0;
    // point / line effects never fog). So it has no effect in the game. 1 in 1,582 of 4,076 v8 groups.
    uint8_t fogEnabled = 0;
    std::string texture;
    uint32_t billboard = 3;                 // X 0, Y 1, Z 2, Camera 3: the sprite's plane is across that axis
    bool sorted = false;                    // PGRS
    uint8_t sortOrder = 0;                  // PGRS: particles kept in emission order; 0 newest drawn first, 1 oldest first
    ParticleOperator op;
};

// BlendType as the engine derives it from the D3D states (pfxParticleGroup, FUN_0066f600):
// 0 Additive (SRCALPHA, ONE, ADD), 3 Subtractive (SRCALPHA, ONE, other op), 1 Multiply (SrcBlend ZERO), 2 Diffuse.
uint32_t particleBlendType(const ParticleGroup& group);

struct ParticleSystem {
    uint32_t version = 0;
    bool enabled = true;
    std::vector<ParticleGroup> groups;
    // PSYS tail = pfxParticleSystem m_warmupInterval / m_warmupCount (2007 PDB Unserialize 0x00972c80; 0 in 1,973 files).
    // pfxParticleSystem::Warmup (0x009718c0) would run warmupCount x Update(warmupInterval) but nothing calls it in the 2007
    // build, and retail's system update has one caller, the per-frame SceneParticle step: the game never warms systems up.
    float warmupInterval = 0;
    uint32_t warmupCount = 0;
    uint32_t stateCount = 0;                // generators + operators (their stateSlot values are 0 .. stateCount - 1)
};

ParticleSystem readParticleSystem(const Bytes& data);

// Special-FX package (.pkg, specialfx.glm, 2,637 files): a chunk file `SFXP` v1 whose children are events, one chunk
// each, created by tag (TRasa::SfxManager FUN_00751260: ACCY, ACBM, ACTR, SND, BEAM, BONE, CAM!, OPAC, AOPC, SLIT, MOTB,
// PFXA, PFX, PFXP, PFXS, PFXT, PROJ, STOP, TCKL, TRNS, TRN2, HUEB, TIME, TODE, APKG, SCRF, SCRO, CAMO). Only the accessory
// event is decoded: `ACCY` v1-8 (SfxAccessoryEvent unserializer FUN_0075fdd0; all 859 in the shipped packages parse
// exactly): u8, asset mesh, asset animation, u8, [v2+: asset, asset, u8], [v3+: u8 bool, asset (a nested package)],
// [v4+: asset x3], [v5+: u8], [v7+: u8 bool], [v8+: f32]; an asset is an ISTR (name), INON (none) or ITNI (id) chunk.
// Traced 2026-10-28 (trigger FUN_0075f1c0, product TRasa::SfxAccessoryProduct FUN_007745b0): the u8 after the animation (+ 0x1c) is the
// connection point the accessory hangs on (type 1, usage 1, FUN_00924880; 0 = the body's root), the v2+ u8 (+ 0xc) the target mode the
// trigger walks (FUN_007552e0, as SfxParticleAttach's target), the v5+ u8 (+ 0x20) the ComputeGfxBody selector (FUN_007554c0: the
// accessory of that body it hangs on). The accessory is connected with a gfxConnection (FUN_00626610 / FUN_005e2b80).
// Its life (traced 2026-10-31, TRasa::SfxAccessoryProduct, the 2007 build's names; event offsets from the unserializer FUN_0075fdd0 and
// the trigger's pushes at 0x0075f4be): three phases, each an animation (event + 0x28 / + 0x40 / + 0x58 -> product + 0x30 / + 0x3c /
// + 0x48) with a second asset (+ 0x34 / + 0x4c / + 0x64 -> + 0x54 / + 0x60 / + 0x6c, BlendAnimationIn's last argument; its role is NOT
// traced). It starts in the first phase that has an animation (0, else 1, else 2; FUN_00589a50 = the asset is set). PlayAnimForPhase
// (FUN_00774bd0) blends the phase's animation in (blend DAT_00bc79b8, weight 1), looping only in phase 1; the time left (+ 0x78) =
// the animation's length, 1 for phase 1, 0 for a phase without an animation. Update (FUN_00774d30), once the animation is loaded:
// outside phase 1 the time left drops by the frame's ms; at < 1 the next phases are played until one has time left - phase 1 then
// stays (looping) until the package stops -, and past phase 2 the accessory is removed (Cleanup FUN_007750c0). Stop (FUN_00774d90):
// phase 2 is played (outside it), and with no fade (third argument 0) the accessory is removed at once. So an accessory without any
// animation goes at its first update. The v3+ bool (+ 0x24, product + 0x24) is `follow`: UpdateBodies (FUN_00774fd0) runs the
// connection's update (phyCPConnection::Update, FUN_005e2b80) at the product's first update (argument 1) and afterwards only while it
// is set - without it the accessory stays where it was first placed, its own animation playing from there (the flare of
// vfx_flaregun.pkg: left in the air at the muzzle, not swung with the hand). The v7 bool and v8 f32: bodyScale / minRelevance.
// `PFXA` and `SLIT` are decoded too (below).
struct SfxAccessory {
    uint32_t marker = 0;         // the first u8 (+ 8): the marker that fires it (see SfxPackage)
    std::string mesh;            // e.g. arch_hum_outpost_waypoint_fx.geo
    std::string animation;       // + 0x28: phase 0, played once (e.g. arch_hum_outpost_waypoint.anm)
    uint32_t contactPoint = 0;   // + 0x1c
    uint32_t target = 0;         // + 0xc (v2+)
    uint32_t accessory = 0;      // + 0x20 (v5+)
    std::string loopAnimation;   // + 0x40 (v2+): phase 1, looped until the package stops (often the phase-0 animation)
    std::string endAnimation;    // + 0x58 (v2+): phase 2, played once, then the accessory is removed
    std::string phaseAsset0, phaseAsset1, phaseAsset2;   // + 0x34 / + 0x4c / + 0x64 (v4+): each phase's second asset (role not traced)
    bool follow = false;         // + 0x24 (v3+): follows its connection point every frame (else placed once)
    // + 0x25 (v7+): when the connection is updated (the first update, then every frame with follow), the accessory takes its body's
    // uniform scale (UpdateBodies FUN_00774fd0: matTransform::SetScale from the body node's + 0xb8); otherwise it keeps scale 1 (the
    // connection's own, phyCPConnection::Update).
    bool bodyScale = false;
    // + 0x7c (v8+; constructor 0x0075f110: 5.0 from 0x00bc5998): the trigger (FUN_0075f1c0) makes the accessory only while the body's
    // relevance (SceneBody + 0xe4) is at least min(the SFX threshold settings + 0x140, this), or when the phase-1 asset (+ 0x40) is set.
    float minRelevance = 5.0f;
    std::string package;         // v3+ nested package (e.g. a light), "" if none
    uint32_t version = 0;
};
// `PFXA` particle attach v1-2 (SfxParticleAttachEvent, unserializer FUN_0076b140, trigger FUN_0076ab00; all 3,583 shipped
// events parse exactly): u8, asset (.pfx), u8 target, u8 contact point, u8, [v2: u8 bool]. The trigger iterates the
// bodies chosen by `target` (FUN_007552e0 / FUN_00755a40 with that selector; which body each value means is NOT decoded,
// 0 is by far the most common) and attaches the system to the contact point whose connection point (type 1, usage 1,
// id `contactPoint`; see tr::ConnectionPointName) matches (FUN_00924880; 0 = the body itself, a missing one skips it).
// Targets (FUN_007552e0, traced 2026-10-28): 0 the package's own body; 1 every body of the package's target list, 2 its hits, 3 its
// misses (the list AttachFXPackage / ChangeTargets fill: hits flagged 1, misses 0). `accessory` (+ 0x20) is TRasa::SfxPackage::
// ComputeGfxBody's selector (FUN_007554c0): 0 the body itself, else the accessory attached to the body's connection point with that id
// (type 1, usage 1; e.g. 11 HPS_Right_Hand = the weapon in the hand) - the contact point is then looked up on that accessory; a missing
// socket logs "object=<%s> is missing hardpoint=%s" and uses the body.
struct SfxParticleAttach {
    uint32_t version = 0;
    uint32_t marker = 0;                         // + 8: the marker that fires it (see SfxPackage)
    std::string file;
    uint32_t target = 0;
    uint32_t contactPoint = 0;
    uint32_t accessory = 0;                      // + 0x20 (see above)
    bool flag = false;                           // v2 (+ 0x25): hue - the source body's hue onto the system, as PFXT's + 0x29 (none shipped)
};
// One keyed track of an `SLIT` light event (FUN_00759c80): u8 loop, f32 uiMax, u32 n, n x (f32 seconds, f32 value).
// Evaluated by FUN_00848c40: linear between keys, the time clamped to the last key; it never reads uiMax (an editor
// slider limit - e.g. range 50, intensity 3 / 10, angles 150), so key values are absolute. With `loop` the product's
// clock for that track wraps at the last key (FUN_00771740).
struct SfxCurve {
    bool loop = false;
    float uiMax = 1;
    std::vector<std::pair<float, float>> keys;   // (time s, value)
};
float sfxCurveValue(const SfxCurve& curve, float seconds);   // loop applied, then FUN_00848c40
// `SLIT` light v1-5 (SfxLightEvent, unserializer FUN_0075ad70, trigger FUN_0075a510, product FUN_007711f0 / FUN_007717f0 /
// FUN_00771410; all 1,112 shipped events parse exactly): u8 x4 (+ 8 ?, + 0xc target, + 0x10 contact point, + 0x14 ?),
// u32 durationMs (0 = no end), f32 Priority, u8 CastsShadows, u32 ShadowQuality, f32 ShadowBiasPercent, f32 x3 Position,
// f32 x3 Offset, [v < 4: cstr (converted), else asset GlobeMapFile], [v < 3: quaternion x y z w, else f32 x3 rotation in
// degrees: D3DXQuaternionRotationYawPitchRoll(rotation[1], rotation[0], rotation[2])], 7 curves, [v2+: 2 curves, u8 spot
// (1 = 'LISP' spot, else 'LIPT' point)], [v5+: u8 RenderMode]. The trigger sets those light properties by name; the
// product sets Color = curves 0-3, Range = 4 (the light is switched off while it is below a small constant), FalloffPercent
// = 5, Intensity = 6, PenumbraAngle = 7, ConeAngle = 8 every frame from the elapsed milliseconds.
struct SfxLight {
    uint32_t version = 0;
    uint32_t marker = 0, target = 0, contactPoint = 0;   // marker: + 8 (see SfxPackage); target + 0xc as SfxParticleAttach's
    uint32_t accessory = 0;                      // + 0x14: the ComputeGfxBody selector (FUN_007711f0 -> FUN_007554c0), as SfxParticleAttach's
    uint32_t durationMs = 0;
    float priority = 1;
    bool castsShadows = false;
    uint32_t shadowQuality = 0;
    float shadowBiasPercent = 0;
    Vec3 position;
    Vec3 offset;
    std::string globeMap;
    Quat rotation;                               // from the degrees as the trigger converts them (or v < 3 as stored)
    bool spot = false;
    uint32_t renderMode = 0;
    SfxCurve colourR, colourG, colourB, colourA, range, falloffPercent, intensity, penumbraAngle, coneAngle;
};
const SfxCurve& sfxLightCurve(const SfxLight& light, int index);   // 0..8 in file order
// `ACBM` accessory beam v1-5 (SfxAccessoryBeamEvent, unserializer FUN_0076df30, serializer FUN_0076d990, trigger FUN_0076d3c0; all
// 131 shipped events are v5 and parse exactly): u8 marker (+ 8), asset texture (+ 0x10), u32 durationMs (+ 0x1c; 0 = no end),
// u8 target1 / contactPoint1 / target2 / contactPoint2 (+ 0x20 .. + 0x2c), [v2+: f32 TileRate, f32 AnimRate (+ 0x30 / + 0x34)],
// f32 noise speed, noise amplitude, width (+ 0x38 / + 0x3c / + 0x40), [v3+: u8 (+ 0xc, use NOT traced)], [v4+: u32 (+ 0x44: 0 =
// texture tiles per metre of the segment, else once per segment), u32 maxSubdivisions (+ 0x48), u8 branchChance / 100 (+ 0x4c)],
// [v5: u8 x3 / 100: branchLength, branchDecay, branchPull (+ 0x50 / + 0x54 / + 0x58)]. Defaults for older versions are the editor's
// (the constructor's are NOT traced; no shipped event is older than v5). What the trigger and the beam system do with them: see Beams.h.
struct SfxBeam {
    uint32_t version = 0;
    uint32_t marker = 0;
    std::string texture;
    uint32_t durationMs = 0;
    uint32_t target1 = 0, contactPoint1 = 0, target2 = 0, contactPoint2 = 0;
    float tileRate = 1, animRate = 0;
    float noiseSpeed = 0, noiseAmplitude = 0, width = 1;
    uint32_t unknownC = 0;
    uint32_t tileOncePerSegment = 0;
    uint32_t maxSubdivisions = 6;
    float branchChance = 0, branchLength = 0, branchDecay = 0, branchPull = 0;
};
// `BEAM` beam v1-7 (TRasa::SfxBeamEvent, unserializer FUN_0076ffd0, trigger FUN_0076f100, product TRasa::SfxBeamProduct; traced
// 2026-10-28): u8 marker (+ 8), asset texture (+ 0xc), u32 durationMs (+ 0x18), [v2+: u8 accessory (+ 0x1c, the source end's
// ComputeGfxBody selector)], u8 startContactPoint (+ 0x20), u8 endContactPoint (+ 0x24), [v4+: f32 TileRate, AnimRate (+ 0x28 /
// + 0x2c)], f32 noise speed, noise amplitude, width (+ 0x30 / + 0x34 / + 0x38), [v3+: u8 branchChance / 100 (+ 0x3c)], [v5+: u32
// tileOncePerSegment (+ 0x40), u32 maxSubdivisions (+ 0x44)], [v6+: u8 x3 / 100 branchLength, branchDecay, branchPull (+ 0x48 ..
// + 0x50)], [v7: u8 chain (+ 0x54)]. The trigger walks the package's target list with sub-mode 2 (FUN_007552e0 case 0: the body
// paired with every target, hits and misses): a bolt from the source end (the body or its accessory, at startContactPoint) to the
// target's endContactPoint - for a miss (flag 0) a random point within the target's bounds radius r around it ((2u - 1) r, u r,
// (2u - 1) r with FUN_007880d0 randoms) -, built by FUN_0076ea10 as the accessory beam's segments; with `chain` each later bolt starts
// at the previous target's point (chain lightning). All bolts of one firing become one beam (FUN_007f0f20 with the texture, duration /
// 1000, TileRate, AnimRate, noise speed, amplitude, width, tileOncePerSegment). The fields mean what SfxBeam's do (see Beams.h).
struct SfxTargetBeam {
    SfxBeam beam;                                // contactPoint1 = startContactPoint, contactPoint2 = endContactPoint
    uint32_t accessory = 0;
    bool chain = false;
};
// `AOPC` animate opacity v1-3 (TRasa::SfxAnimateOpacityEvent, factory FUN_00753920, constructor FUN_00766df0, unserializer
// FUN_00767550, trigger FUN_00766ec0; all 253 shipped events parse exactly): u8 (+ 8), u8 target (+ 0xc), u8 loop (+ 0x2c, a
// TRasa::ModifierTrack at + 0x14), u32 n, n x (f32 seconds, f32 opacity), [v2+: u32 durationMs (+ 0x30; v1: the last key x 1000)],
// [v3: u8 (+ 0x10: when set the trigger looks up something on the body first - NOT traced)]. The trigger makes a product
// (FUN_007779b0, TRasa::SfxAnimateOpacityProduct) per selected body that multiplies the body's opacity by the curve (see
// SceneView::bodyOpacity). Object uses are state TRANSITIONS (usabledata.specialFX with a previous state other than 97: dropships,
// deaths); the character select's vfx_tierselect_recruit.pkg fades each new mannequin in with it.
// `CAM!` camera shake v1 (TRasa::SfxCameraShakeEvent, unserializer FUN_007585e0 / 2007 0x0081c8a0, trigger FUN_00758360; constructor
// defaults target 2, near 1 m / 0.5, far 8 m / 0): u8 marker (+ 8), u8 target (+ 0xc, the package's target mode), f32 nearDist (+ 0x10),
// nearMag, farDist, farMag. The trigger hands each target body's position (scene node + 0xa0) to the callback the game client installs
// (GameClient::Initialize: FUN_004d7e70): d = |the current manifestation's body - that point|; nothing at d >= farDist; magnitude =
// nearMag at d <= nearDist, else nearMag + (d - nearDist) (farMag - nearMag) / (farDist - nearDist); above 0 -> UserController::ShakeCamera
// (FUN_004f5d60; Normal mode applies it only with User.Camera.CameraShake on).
struct SfxCameraShake {
    uint32_t marker = 0, target = 2;
    float nearDist = 1.0f, nearMag = 0.5f, farDist = 8.0f, farMag = 0.0f;
};
// `HUEB` hue body v1 (TRasa::SfxHueBodyEvent, factory 0x00753cd0, unserializer FUN_00769080, trigger FUN_00768cf0 -> product
// TRasa::SfxHueBodyProduct FUN_00778010, vtable 0x00bfd89c; traced 2026-11-04): u8 marker (+ 8), u8 target (+ 0xc), u32 durationMs (+ 0x10;
// 0 = until the package stops), u32 r, g, b (+ 0x14 / 0x18 / 0x1c, x 1/255; alpha 1). The product keeps the HueColor of the body's pieces
// in the groups "flesh", "hair" and "facedetails_hair", collects the swap ids of its other pieces and HueSwapsets each of those with the
// colour (flesh keeps its kept colour); Update does nothing; Stop (FUN_00778670) puts every collected swap set back. Shipped: 2 events,
// vfx_enemy_halo_blue / _red.pkg (blue 0 0 255 / red 255 0 0, marker 0, target 0, duration 0) - the families
// EFFECT_CREATURE_LT_VISUAL_BLUE / RED.
struct SfxHueBody {
    uint32_t marker = 0, target = 0;
    uint32_t durationMs = 0;
    uint32_t r = 0, g = 0, b = 0;
};
// `SCRF` screen filter v1 (TRasa::SfxScreenFilterEvent, factory 0x00753de0 -> constructor FUN_0076bb30, unserializer FUN_0076c470,
// trigger FUN_0076bf00; traced 2026-11-04): u8 marker (+ 8), u32 type (+ 0xc: the filter, below), u32 (+ 0x10),
// u32 technique (+ 0x14; constructor 0), u32 variant (+ 0x18; constructor 2), u32 durationMs (+ 0x1c; constructor 500, 0 = until the
// package stops), f32 (+ 0x20; constructor 1, not read by the product), asset overlay texture (+ 0x24; detail_standin_02.dds), asset
// distortion texture (+ 0x30; normal_standin.dds), curves (loop, min, max, keys - as TRN2's) Intensity, Alpha, Distortion, OverlayAlpha.
// The trigger walks the package's hit targets (target mode 2) and makes a product (TRasa::SfxScreenFilterProduct FUN_0077b1f0, update
// FUN_0077b330) only for the current manifestation's body (callback DAT_00d451a8; the event's + 0xbc flag, never read from the file,
// lifts that). The product adds a PostEffect-phase effect by type: 0 'PXNM' PostEffect_NormalScreenFilter, 1 'PXGS' GreyScale, 2 'PXWH'
// White, 3 'PXRD' Red (FUN_0087ca10 registers the four; base modes 0 1 2 3, matched to each pixel shader's arithmetic) through
// FUN_0084d590: TRPostFX_ScreenFilter.fx, technique = + 0x14 (0 SRCALPHA / INVSRCALPHA, 1 SRCALPHA / ONE), int Mode = base + 4 x + 0x18
// (psShader[Mode]: + 0x18 = 0 the overlay multiplies the frame, 1 adds to it, 2 adds as a detail map, (o - 0.5) x 2), OverlayTexture /
// DistortTexture; each frame the curves at the product's elapsed seconds into Intensity / Alpha / Distortion / OverlayAlpha. Shipped:
// 4 events - the flashbang / magnesium flash / lightbender quills: White, add, additive, 2.5 s; vfx_game_effect_stun.pkg: GreyScale,
// detail, blended, endless.
struct SfxScreenFilter {
    uint32_t marker = 0, type = 0, value10 = 0, technique = 0, variant = 2, durationMs = 500;
    float value20 = 1.0f;
    std::string overlay, distort;
    SfxCurve intensity, alpha, distortion, overlayAlpha;
};
struct SfxOpacity {
    uint32_t version = 0;
    uint32_t marker = 0, target = 0;
    bool loop = false;
    std::vector<std::pair<float, float>> keys;   // (seconds, opacity)
    uint32_t durationMs = 0;
    uint32_t unknown10 = 0;
};
// `SND ` audio v2-12 (TRasa::SfxAudioEvent, factory 0x007537d0 -> constructor FUN_00760180, unserializer FUN_007609c0, trigger
// FUN_00760210; all 2,775 shipped events parse exactly): u8 marker (+ 8), [v < 12: u8 n, n x asset (the sounds, before audio sets)],
// u8 target (+ 0xc: the package's target mode, FUN_007552e0), [v < 12: f32], [v3+: u8 bool (+ 0x10)], [v4+: u8 (+ 0x14; constructor 3)],
// [v5+: u8 flag, when set u32 x2], [v6+: u32 audioSet (+ 0x20; the trigger does nothing when it is 0)], [v7+: u8 connectionPoint
// (+ 0x24), u8 selector (+ 0x28: ComputeGfxBody's accessory selector)], [v8+: u8 fadeOnStop (+ 0x2c; constructor 1), f32 fadeMs
// (+ 0x30; v < 11 forced to 1000 ms, constructor 100)], [v10+: u8 playerOnly (+ 0x34), f32 x3 offset (+ 0x38)].
// Trigger, per body the target mode gives: with playerOnly only the current manifestation's body (callback DAT_00d451a8 = LAB_004d7fd0);
// the body's relevance below the SFX threshold (settings + 0x140) marks it low; the accessory the selector names (none: nothing); the
// connection point (type 1 usage 1) on it - none: the body's position (VfxPoint); the audio set looked up (none, or no category:
// nothing); low relevance plays only categories whose name starts "Music"; CreateAudioRequest(set), SetPosition(point + offset),
// Play(-1); an SfxAudioProduct follows the point (FUN_007753f0) until the request is done, and on Stop / the body's removal fades the
// request out over fadeMs when fadeOnStop (FUN_00775580), otherwise lets it play on.
struct SfxSound {
    uint32_t version = 0;
    uint32_t marker = 0;
    std::vector<std::string> files;              // v < 12: the event's own sounds (not played by the trigger)
    uint32_t target = 0;
    bool flag10 = false;
    uint32_t unknown14 = 3;
    bool hasPair = false;
    uint32_t pair0 = 0, pair1 = 0;
    uint32_t audioSet = 0;
    uint32_t connectionPoint = 0, selector = 0;
    bool fadeOnStop = true;
    float fadeMs = 100;
    bool playerOnly = false;
    Vec3 offset;
};
// `TIME` timer v1 (TRasa::SfxTimerEvent, factory 0x00753d10, unserializer FUN_0076cce0, trigger FUN_0076ca30, product
// TRasa::SfxTimerProduct FUN_0077b780 / update FUN_0077b930; 183 shipped): u8 marker (+ 8), f32 duration (+ 0xc, s; 0 = until the
// package stops), f32 minDelay, maxDelay (+ 0x10 / + 0x14, s), u8 fireAtOnce (+ 0x18), u8 fires (+ 0x1c, the marker it fires; the
// constructor's default 53 = Timer1). When fired, the trigger makes one product per selected body: its countdown starts at 0 with
// fireAtOnce, else uniform random in [minDelay, maxDelay] (FUN_00788250); each update takes the elapsed seconds off and, at or below
// 0, fires `fires` into the package (SfxPackage vtable slot 3) and draws a new delay; with a duration > 0 it ends once that has passed.
// `ACTR` accessory tracer v1-2 (TRasa::SfxAccessoryTracerEvent, unserializer FUN_00767f40, trigger FUN_00767940; constructor defaults at
// factory 0x00753780: connection points 28 / 29, duration 1500): u8 marker (+ 8), asset (+ 0xc), u8 target (+ 0x18), u32 durationMs
// (+ 0x24), u32 (+ 0x28, ms), [v2: u8 contactPoint1 (+ 0x1c), u8 contactPoint2 (+ 0x20)]. The trigger finds both connection points
// (type 1, usage 1) on the body `target` selects and makes a tracer between them with the VFX manager (FUN_007edac0: the asset, the
// two points' frames, durationMs x 0.001, the + 0x28 value x 0.001), kept as an SfxAccessoryTracerProduct.
struct SfxTracer {
    uint32_t version = 0;
    uint32_t marker = 0;
    std::string asset;
    uint32_t target = 0;
    uint32_t durationMs = 1500;
    uint32_t value28 = 0;                        // + 0x28, ms (use NOT traced)
    uint32_t contactPoint1 = 28, contactPoint2 = 29;
};
// `PFXT` particles towards v1-2 (TRasa::SfxParticleTowardsEvent, unserializer FUN_00768ab0, trigger FUN_00768190, read in the
// assembly 2026-11-01): u8 marker (+ 8), asset .pfx (+ 0xc), u8 location (+ 0x18: the target mode, walked with sub-mode 2 - one
// instance per target of the mode, the package's body paired with it; none without targets), u8 startContactPoint (+ 0x1c, on the
// source), u8 sourceAccessory (+ 0x20, ComputeGfxBody's selector on the package's body), u8 endContactPoint (+ 0x24, on the target),
// [v2: u8 hue (+ 0x29): the source body's hue onto the system - traced 2026-11-01: 0x008003e0 takes the first piece of the source gfx body outside the "flesh" group with a HueColor parameter (its colour as set, GetColor adds the 0.5 back; none: (0, 0, 0, 1)), FUN_00867900 multiplies its alpha by the SceneBody's four opacity factors and FUN_005849c0 stores it in every group (+ 0x1a0), which the three vertex fills multiply into each particle's colour; NO shipped event sets it (0 of 199; PFXA's + 0x25 the same, 0 of 3,583) - OpenRasa SceneView::hueTint]. Per pair the system is added STATIC
// (ParticleSystemManager::AddParticleStatic, world space, not following) at the start point, turned by StoreLookDir towards the end
// point (0: the body's position); a missing connection point or scene skips it; below the SFX relevance threshold it plays only when
// the event's + 0x28 is set - the last system it started was endless (flag 0x40000; slot 5 0x007687e0 returns it).
struct SfxParticleTowards {
    uint32_t version = 0;
    uint32_t marker = 0;
    std::string file;
    uint32_t location = 0, startContactPoint = 0, sourceAccessory = 0, endContactPoint = 0;
    bool hue = false;
};
// `PFX ` particle event v1-2 (TRasa::SfxParticleEvent; unserializer FUN_00759450, trigger FUN_00758870; members named by the 2007
// build's PDB, read 2026-11-02): u8 marker (+ 8), asset .pfx (+ 0xc), u8 location (+ 0x18, m_location: the target mode the trigger
// walks), u8 startCp (+ 0x1c), u8 accessoryCp (+ 0x20, ComputeGfxBody's selector), u8 endCp (+ 0x24), u8 orientation (+ 0x28,
// eOrientation), [v2: u8 hue (+ 0x2d, as PFXA's)]. Per instance, by orientation: 0 eDefault - attached to startCp on the package's
// body or the accessory accessoryCp picks (AddParticleToCP, not frozen; a missing point skips it), as PFXA; 1 eSurface - on the
// instance body's ground (ParticleSystemManager::AddParticleToSurface, see SfxParticleAtBody); 2 eTowardsOther - static at startCp,
// turned towards endCp on the paired target (as PFXT; a missing point skips it); other values make nothing. No shipped package has one;
// the engine itself makes them (EntityBody.AttachParticleAtCPs).
struct SfxParticle {
    uint32_t version = 0;
    uint32_t marker = 0;
    std::string file;
    uint32_t location = 0, startContactPoint = 0, accessory = 0, endContactPoint = 0, orientation = 0;
    bool hue = false;
};
// `PFXP` particle at a position v1-2 (TRasa::SfxParticlePositionEvent; unserializer FUN_0075c1c0, trigger FUN_0075bbc0) and `PFXS`
// particle on a surface v1-2 (TRasa::SfxParticleSurfaceEvent; FUN_0076b9b0, FUN_0076b350); read 2026-11-02: u8 marker (+ 8), asset
// .pfx (+ 0xc), u8 location (+ 0x18, m_location: the target mode), [v2: u8 hue (+ 0x1d)]. Per instance body (the package's body for
// mode 0, else each target of the mode, FUN_007552e0): PFXP adds the system static at the body node's position and rotation
// (AddParticleStatic; it does not follow); PFXS calls AddParticleToSurface (FUN_007d90c0, 2007 0x008730b0): a pick (FUN_008035b0: the
// terrain segment, then the scene's bodies, type 5) from the body's position + 0.5 m up (double 0x00c2b4c0) straight down (0x00c2b50c),
// 100 m long (0x00bc79e4); the system at the BODY's position, rotated by GraphicsUtil::RotateFromTo (FUN_00801570, the shortest arc)
// from (0, 1, 0) to the pick's normal ((0, 1, 0) without a hit), scaled by the body node's uniform scale (+ 0xb8), static. Both play
// only while the body's relevance is above 4 (0x00bc5434) or the system is endless (flag 0x40000), as AddParticleToCP. Shipped: PFXP
// 2, PFXS 11.
struct SfxParticleAtBody {
    uint32_t version = 0;
    uint32_t marker = 0;
    std::string file;
    uint32_t location = 0;
    bool surface = false;                        // PFXS (else PFXP)
    bool hue = false;
};
// `PROJ` projectile v2-9 (TRasa::SfxProjectileEvent, unserializer FUN_00762700, trigger FUN_00760f70; the members named by the 2007
// build's PDB, TRasa::SfxProjectileEvent 0x7c bytes): u8 marker (+ 8), asset projectile (+ 0x10, m_projectileId: the projectile's mesh),
// [v3+: asset particle (+ 0x1c, m_particleId: a .pfx on the projectile)], f32 velocity (+ 0x58, m/s), u8 launchCp (+ 0x68), u8 targetCp
// (+ 0x6c), [v3+: u8 location (+ 0xc, eLocation: which bodies, the package's target mode)], [v4+: i32 zombieDurationMsec (+ 0x60: how
// long the projectile stays after arriving)], [v5+: asset animation (+ 0x28)], [v6+: asset loopAnimation (+ 0x40)], [v7+: u8 accessoryCp
// (+ 0x64, the launch body's accessory: SfxPackage::ComputeGfxBody), asset animationPackage (+ 0x34), asset loopAnimationPackage
// (+ 0x4c)], [v8+: f32 gravity (+ 0x5c)], [v9: asset arrivalPackage (+ 0x70)]. The trigger (per target of `location`): launch point =
// launchCp on the source body; aim = targetCp on a hit target, the target's position plus a random scatter on a miss; duration = |aim -
// launch| / velocity x 1000 ms; ProjectileManager::CreateProjectile(mesh, particle, animation, loopAnimation, duration, gravity, the
// segment); the arrival package listens to the projectile's markers (14 PROJECTILE_ARRIVED). See ProjectileManager in the notes.
struct SfxProjectile {
    uint32_t version = 0;
    uint32_t marker = 0;
    std::string projectile;                      // m_projectileId: the mesh
    std::string particle;                        // m_particleId
    float velocity = 0;                          // m_velocity
    uint32_t launchCp = 0, targetCp = 0;         // m_launchCp / m_targetCp (connection points, usage 1)
    uint32_t location = 0;                       // m_location
    int32_t zombieDurationMs = 0;                // m_zombieDurationMsec
    std::string animation, loopAnimation;        // m_animationId / m_loopAnimationId
    uint32_t accessoryCp = 0;                    // m_accessoryCp
    std::string animationPackage, loopAnimationPackage;
    float gravity = 0;                           // m_gravity
    std::string arrivalPackage;                  // m_arrivalPackageId
};
struct SfxTimer {
    uint32_t marker = 0;
    float duration = 0, minDelay = 1, maxDelay = 1;
    bool fireAtOnce = true;
    uint32_t fires = 53;
};
// Markers (traced 2026-10-20 in tabula_rasa.exe): every event's first byte (+ 8 on the event object, read by each unserializer)
// is an animation marker type (tr::animMarkerTypeName: 0 Start, 1 Strike, 6-13 Windup / Resolve, 15 Tick, 24 Stop, 30-36 Footstep,
// 37-44 Audio, 53-60 Timer, ...). TRasa::SfxPackage's vtable slot 3 (FUN_00755740) fires a marker: every event whose marker equals
// it triggers (vtable + 8), and the live products are told too; while the package still loads, fired markers are queued and replayed
// once it has (FUN_00755db0 / FUN_00754c40). Starting a package (FUN_00755230; body.AttachFXPackage, not AttachFXPackageNoStart)
// fires 0; `TIME` events fire Timer markers; `STOP` (SfxStopEvent, trigger FUN_00764a10 -> FUN_00755250) stops every live product
// and fires 24. The other markers come from outside the package (the body's animation markers, ability scripts - NOT traced).
// Shipped: 2,201 of 3,583 particle attaches, 819 of 1,112 lights and 1,703 of 2,775 sounds wait for 0.
// `TRN2` transporter overlay v1 (TRasa::SfxTransporter2VFXEvent, unserializer FUN_00766180, product SfxTransporter2VFXProduct:
// constructor FUN_00776a60, update FUN_00777090, apply FUN_00777380; read 2026-10-25): u8 marker (+ 8), u8 location (+ 0xc),
// u32 durationMs (+ 0x14; the product ends at it, 0 = never), u32 blend mode (+ 0x18; non-zero passes a phase to FUN_007ffe50 - NOT
// traced), the diffuse layer: asset texture, u32 projection (+ 0x34), f32 rotation (+ 0x3c, radians), f32 x2 scale (+ 0x88), curves u /
// v scroll; the detail layer the same; four colour curves (TransporterColor r, g, b, a); u8 connection point (+ 0x10; 0 = the body,
// else the accessory on that connection point - the shipped values 66 Pistol_Holster, 11 Right_Hand, 62). A curve here is u8 loop,
// f32 min, f32 max (editor limits), u32 n, n x (f32 seconds, f32 value). The product makes TRTransporter2.fx an extra material of the
// body (FUN_00817920) and per frame sets DiffMapUVTransform / DetMapUVTransform = Scaling(s) x translation(u, v curves) x RotationZ
// (FUN_00776ea0; for projection <= 2 the scale is max(scale, 1e-4) / the body's bounds extent along the projection's axes: 0 x, y,
// 1 z, y, 2 x, z), TransporterColor = the colour curves, Anchor = the body's position. Each curve runs on its own clock (ms of frame
// time x 0.001), wrapped at its last key whatever its loop byte; the colour curves share one. All 172 shipped events are v1 and
// parse exactly (viewport_test RASA_PKGSCAN; =transporters lists them).
// `TRNS` transporter overlay v1-4 (TRasa::SfxTransporterVFXEvent, unserializer FUN_0075dde0 -> v1 FUN_0075dec0, v2 FUN_0075e560, v3
// FUN_0075eb40, v4 FUN_0075ebb0; product SfxTransporterVFXProduct: constructor FUN_007737e0, update FUN_00773e40, apply FUN_007741a0;
// read 2026-10-25): u8 marker, u8 location, u32 durationMs, then three layers - diffuse (+ 0x18), detail (+ 0x24), decal (+ 0x30) - each
// v4: asset texture, u32 projection, f32 rotation, f32 x2 scale, curves u / v scroll; v2 / v3: asset texture, u32 projection, f32 u
// offset, f32 v offset, f32 rotation, f32 x2 scale, two curves that are read and dropped (the scroll curves become the constant offsets:
// FUN_00848dd0 clears, FUN_00848b90 adds the key (0, offset)); v3 / v4 then u8 connection point (+ 0x10). No blend mode, no colour.
// TRTransporter.fx (its Phase parameter "Additive": SRCALPHA / ONE): rgb = lerp(diffuse + detail - 0.5, decal.rgb, decal.a), alpha 1;
// ProjectionType = diffuse x 9 + detail x 3 + decal (each projection mod 3; the vertex shader table's elements, disassembled). Shipped:
// 145 v4 and 1 v2 (creature_pet_shell_bot_resolve.pkg), all parse exactly. v1 (three layers of scale / offset pairs and one projection
// type for all) is not read - none ships.
// Both effects' vertex shader tables (TRTransporterSP_2.fxc / TRTransporter2SP_2.fxc vsTableSimple / vsTableSkinned, disassembled
// 2026-10-25) project layer n from the world position minus Anchor: projection 0 (x, y), 1 (z, y), 2 (x, z).
struct SfxTransporterLayer {
    std::string texture;
    uint32_t projection = 0;
    float rotation = 0;
    float scaleU = 1, scaleV = 1;
    SfxCurve scrollU, scrollV;
};
struct SfxTransporter {
    bool two = true;                             // TRN2 (TRTransporter2.fx); false: TRNS (TRTransporter.fx, with the decal layer)
    uint32_t version = 0;
    uint32_t marker = 0, location = 0;
    uint32_t durationMs = 0, blendMode = 0;
    SfxTransporterLayer diffuse, detail, decal;
    SfxCurve colourR, colourG, colourB, colourA;
    uint32_t connectionPoint = 0;
};
struct SfxPackage {
    std::vector<uint32_t> events;                // every event's tag, in order
    std::vector<uint32_t> eventMarkers;          // each event's first byte (+ 8 on the event object: the marker that fires it)
    std::vector<SfxAccessory> accessories;
    std::vector<SfxParticleAttach> particles;
    std::vector<SfxLight> lights;
    std::vector<SfxBeam> beams;
    std::vector<SfxTargetBeam> targetBeams;      // `BEAM`
    std::vector<SfxOpacity> opacities;
    std::vector<SfxCameraShake> cameraShakes;   // `CAM!`
    std::vector<SfxHueBody> hueBodies;           // `HUEB`
    std::vector<SfxScreenFilter> screenFilters;  // `SCRF`
    std::vector<SfxTransporter> transporters;
    std::vector<SfxSound> sounds;
    std::vector<SfxTimer> timers;
    std::vector<uint32_t> stops;                 // `STOP` events' markers
    std::vector<SfxTracer> tracers;
    std::vector<SfxParticleTowards> towards;
    std::vector<SfxParticle> plainParticles;     // `PFX `
    std::vector<SfxParticleAtBody> bodyParticles; // `PFXP`, `PFXS`
    std::vector<SfxProjectile> projectiles;
};
SfxPackage readSfxPackage(const Bytes& data);

// Particle systems a model carries: "psys" values "<contact point id>,<file>.pfx" from its contact points, its GBOD
// property table and its pieces' user data (the same value often appears in several), without duplicates.
// 2,147 of 2,203 references in the shipped models name an existing contact point.
struct ParticleAttachment {
    uint32_t contactPoint = 0;
    std::string file;
};
std::vector<ParticleAttachment> particleAttachments(const GeoModel& model);
const GeoContactPoint* findContactPoint(const GeoModel& model, uint32_t id);

}  // namespace tr
