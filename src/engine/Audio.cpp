// Audio.cpp - see Audio.h.
#include "Audio.h"

#include "Assets.h"
#include "EntityBody.h"

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace engine::audio {
namespace {

struct Sound {
    std::string file;
    float volume = 1;                  // AddSound's float (the sample volume factor, FUN_007b0d50)
};
struct Category {
    std::string name;
    int priority = 0;                  // AddAudioCategory's priority (category record + 0x20): the requests' first sort key
    int maxSimultaneous = 0;           // its max simultaneous (+ 0x21, a byte): the category's voices (the manager's map + 0xcc)
    bool music = false;
    bool is3D = false;                 // AddAudioCategory's Is3D (category record + 0x23)
    int fadeInMs = 0, fadeOutMs = 0;
};
// TRasa::AudioSetMember (0x18 bytes, AudioManager::AddAudioSetMember FUN_007a1360): + 0 set, + 4 sound, + 8 / + 0xc the member data's
// first two values (an int and a float - their readers not traced), + 0x10 the third, a byte: the member's occurrence weight.
struct Member {
    int sound = 0;
    int value1 = 0;
    float value2 = 0;
    uint8_t weight = 0;
};
struct AudioSet {
    std::string name;
    int category = 0;
    bool looping = false;
    int selection = 1;                 // set record + 0x25 (audiosetdata.selectionType): 1 / 3 a weighted pick, anything else member 0
    float silenceSeconds = 0;          // BetweenTrackSilenceTimeSecs
    float minDistance = 0, maxDistance = 0;   // AddAudioSet's 3D distances
    std::vector<Member> members;       // set + 0x44, in the order added
    unsigned totalWeight = 0;          // set + 0x38: AddMember adds each weight; the detail level re-sums it (FUN_007b2120)
    bool mufflable = false;            // AddAudioSet's Mufflable (set record + 0x34): its samples take the muffling occlusion
    // SetAudioSetMuffling(set, multiplier, fade in ms, fade out ms) (FUN_007a11e0: set record + 0x3c): playing this set muffles the rest.
    bool muffles = false;
    float muffleLevel = 0;
    int muffleInMs = 0, muffleOutMs = 0;
};

std::map<int, Sound> sounds;
std::map<int, Category> categories;
std::map<int, AudioSet> sets;

bool enabled = true;
float masterVolume = 1, musicVolume = 1, effectsVolume = 1;
unsigned outputRate = 44100;           // Client.Audio.PlaybackFrequency (default 44100 at 0x00bc5744): Miles' driver rate
unsigned outputBits = 16;              // Client.Audio.PlaybackBits (default 16 at 0x00bc5748): the driver's sample size
unsigned maxVoices = 64;               // Client.Audio.MaxHandles (FUN_004e3840, default 0x40; manager + 0x18)
unsigned updatePeriodMs = 500;         // Client.Audio.UpdatePeriodMilliseconds (default 500 at 0x00bc5750; manager + 0x24)
bool started = false;
MIX_Mixer* mixer = nullptr;

struct Clip {
    std::vector<uint8_t> bytes;
    MIX_Audio* audio = nullptr;
};
std::map<std::string, std::unique_ptr<Clip>> clips;   // by lower-case file name; audio null = could not load

MIX_Audio* clip(const std::string& file) {
    std::string key = file;
    std::transform(key.begin(), key.end(), key.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
    auto it = clips.find(key);
    if (it != clips.end()) return it->second->audio;
    auto c = std::make_unique<Clip>();
    tr::Bytes bytes;
    if (assets().tryRead(file, bytes)) {
        c->bytes.assign(bytes.begin(), bytes.end());
        SDL_IOStream* io = SDL_IOFromConstMem(c->bytes.data(), c->bytes.size());
        c->audio = io ? MIX_LoadAudio_IO(mixer, io, false, true) : nullptr;
        if (!c->audio) std::fprintf(stderr, "[audio] Problem loading audio %s: %s\n", file.c_str(), SDL_GetError());
    } else {
        std::fprintf(stderr, "[audio] Problem loading audio %s: not in the archives\n", file.c_str());
    }
    MIX_Audio* a = c->audio;
    clips[key] = std::move(c);
    return a;
}

bool start() {
    if (started) return mixer != nullptr;
    started = true;
    if (!enabled) return false;
    if (!MIX_Init()) {
        std::fprintf(stderr, "[audio] SDL_mixer: %s\n", SDL_GetError());
        return false;
    }
    // AudioManager::Initialize (FUN_007a0550): AIL_open_digital_driver(PlaybackFrequency, PlaybackBits, MSS_MC_USE_SYSTEM_CONFIG, 0) -
    // the rate and sample size from the options, the channels from the system's speaker set-up (PlaybackChannels is read into the
    // settings, + 0x10, but never passed on). 8 bits -> unsigned 8-bit, anything else 16-bit (what Miles does with other sizes is not
    // traced).
    SDL_AudioSpec spec{};
    if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr)) spec.channels = 2;
    spec.freq = int(outputRate);
    spec.format = outputBits == 8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16;
    mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!mixer) {
        std::fprintf(stderr, "[audio] no audio device: %s\n", SDL_GetError());
        return false;
    }
    MIX_SetMixerGain(mixer, masterVolume);
    return true;
}

// ---- Requests -----------------------------------------------------------------------------------------------------------------
struct Request {
    PyObject_HEAD
    int set;                           // the audio set, or 0 for a single file (GetAudio2D / GetAudioStream)
    std::string* file;
    float volume;
    int music;                         // -1: the category's
    std::vector<MIX_Track*>* tracks;   // playing
    std::vector<std::unique_ptr<struct LowPass>>* filters;   // each track's Miles low-pass (occlusion)
    std::vector<float>* gains;         // each track's sound volume
    bool active;                       // played and not stopped (a looping set between tracks is active)
    double silenceLeft;                // ms until the next track of a looping set
    float seed;                        // + 0x68: SetAudioParams' randomSeed / SetRandomSeed, -1.0 from the constructor (random picks)
    int loop;                          // SetAudioParams(loop=): 1 loops, 0 does not, -1 not given (the set's own)
    // 3D (GetAudio3D, SetPosition, AttachAudio, emitters): placed at `position` - or on `entity`'s body - heard from the camera.
    bool is3D;
    float minDistance, maxDistance;
    float position[3];
    uint64_t entity;
    float gain3D, left, right;         // this frame's falloff and balance
    int fadeInMs;                      // an emitter's FadeInTime (-1: the category's)
    int priority;                      // + 0xd: the category's (-1), or SetAudioParams' priority x 20 (FUN_007b0ca0)
    // The manager's bookkeeping (TRasa::AudioBase, see update()).
    int state;                         // + 8: 0 idle / between a looping set's tracks, 1 waiting to start, 2 playing, 3 done
    float audibility;                  // + 0x4c (slot 4), refreshed every UpdatePeriodMilliseconds
    float distanceSq;                  // + 0x60: squared distance to the listener at that refresh (3D)
    unsigned serial;                   // + 0x74: the manager's request counter (+ 0x28) at creation
    double rampLeft;                   // + 0x58: ms left of a fade (in, or out while fadingOut)
    bool fadingOut;                    // + 0x67: Stop with a fade under way
    double firstQueuedMs;              // + 0x7c: the manager clock when first queued to start (0: not yet)
    bool hasVoice;                     // a Miles sample handle is held (from _Play until slot 13 / done)
    float occlusion;                   // + 0x84: the muffling level last given to it
};
PyTypeObject requestType = {PyObject_HEAD_INIT(nullptr) 0};
std::set<Request*> activeRequests;     // the manager's container (+ 0xf4); each holds a reference to itself while in it
unsigned requestCounter = 0;           // manager + 0x28
double managerClock = 0, nextRefresh = 0;   // manager + 0x1c / + 0x20 (ms)
std::vector<Request*> requestOrder;    // the container in walk order, sorted again only when a key may have changed
bool orderChanged = true;
std::mt19937 rng(12345);

// Music or effect volume (TRasa::AudioBase::SetVolume, FUN_007b0d50): a set's request takes the music volume (manager + 0x100) when its
// category's name starts "Music" (category record + 0x24) - not the category's music flag (+ 0x25, which only picks the AudioStream
// class): the "Ambient" category is flagged music yet plays at the effect volume (+ 0xfc). A request without a set (a file) uses its
// SetIsMusic flag (+ 0x65).
bool isMusic(const Request* r) {
    auto s = sets.find(r->set);
    if (r->set == 0 || s == sets.end()) return r->music > 0;
    auto c = categories.find(s->second.category);
    return c != categories.end() && c->second.name.compare(0, 5, "Music") == 0;
}

void applyGains(Request* r) {
    const float category = isMusic(r) ? musicVolume : effectsVolume;
    const float g3 = r->is3D ? r->gain3D : 1.0f;
    for (size_t i = 0; i < r->tracks->size(); ++i) {
        MIX_SetTrackGain((*r->tracks)[i], (*r->gains)[i] * r->volume * category * g3);
        if (r->is3D) {
            const MIX_StereoGains pan{r->left, r->right};
            MIX_SetTrackStereo((*r->tracks)[i], &pan);
        }
    }
}

// ---- 3D --------------------------------------------------------------------------------------------------------------------
std::array<float, 3> listenerPosition{0, 0, 0}, listenerForward{0, 0, 1}, listenerUp{0, 1, 0};
bool listenerSet = false;

float dot3(const std::array<float, 3>& a, const std::array<float, 3>& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
std::array<float, 3> unit3(const std::array<float, 3>& a) {
    const float l = std::sqrt(dot3(a, a));
    return l >= 1e-4f ? std::array<float, 3>{a[0] / l, a[1] / l, a[2] / l} : a;
}

// Miles' 3D levels for a stereo driver (mss32.dll, traced 2026-10-26). The game opens the driver with MSS_MC_USE_SYSTEM_CONFIG (0x10,
// FUN_007a0550), which reads DirectSound's speaker configuration (FUN_2112ee80): stereo speakers -> MSS_MC_STEREO, speakers at
// (-0.7, 0, 0.7) and (0.7, 0, 0.7) in the listener's frame (x right = up x face, y up, z face; AIL_set_speaker_configuration with
// the driver's power 3.0, 0x21159578; FUN_21128540 adds a virtual speaker at -face because no real speaker faces backwards).
// Headphones would give (-1, 0, 0) / (1, 0, 0) instead - the Windows setting is not known here; stereo is taken.
// Distance (FUN_211288c0): d = |source - listener|; beyond the max distance the sample is silent (the driver's + 0x1c, 1 from
// FUN_21130070, nothing the game calls changes it); gain 1 up to max(min, 1e-4) + 1e-4, else the registered falloff (the game's,
// LAB_007b37e0: ((max - d) / (max - min))^2). No cones and no Doppler (the game sets no velocities or cones).
// Pan (FUN_21128ad0): per speaker w = pi - acos(direction . speaker) (pi above 0.9999, 0 below -0.9999), S = the sum over the three;
// a speaker's level = sqrt(w^3 / S), the virtual speaker's spread to the real ones by 1 / (2 x 3) each; each channel x 1 / sqrt(3),
// clamped to 0 .. 1, x the distance gain. A source within 1e-4 m: sqrt(1/2) on both.
void stereoLevels(const std::array<float, 3>& towards, float& left, float& right) {
    const std::array<float, 3> r = unit3({listenerUp[1] * listenerForward[2] - listenerUp[2] * listenerForward[1],
                                          listenerUp[2] * listenerForward[0] - listenerUp[0] * listenerForward[2],
                                          listenerUp[0] * listenerForward[1] - listenerUp[1] * listenerForward[0]});
    const std::array<float, 3>& f = listenerForward;
    const std::array<float, 3> speakerL = unit3({-0.7f * r[0] + 0.7f * f[0], -0.7f * r[1] + 0.7f * f[1], -0.7f * r[2] + 0.7f * f[2]});
    const std::array<float, 3> speakerR = unit3({0.7f * r[0] + 0.7f * f[0], 0.7f * r[1] + 0.7f * f[1], 0.7f * r[2] + 0.7f * f[2]});
    const std::array<float, 3> back{-f[0], -f[1], -f[2]};
    auto weight = [&](const std::array<float, 3>& s) {
        const float c = dot3(towards, s);
        if (c > 0.9999f) return 3.14159265f;
        if (c < -0.9999f) return 0.0f;
        return 3.14159265f - std::acos(c);
    };
    const float wl = weight(speakerL), wr = weight(speakerR), wb = weight(back);
    const float sum = wl + wr + wb;
    const float spread = std::sqrt(wb * wb * wb / sum) / 6.0f;
    const float scale = 1.0f / std::sqrt(3.0f);
    left = std::clamp((std::sqrt(wl * wl * wl / sum) + spread) * scale, 0.0f, 1.0f);
    right = std::clamp((std::sqrt(wr * wr * wr / sum) + spread) * scale, 0.0f, 1.0f);
}

void place3D(Request* r) {
    if (!r->is3D) return;
    if (r->entity) {
        std::array<float, 3> p;
        float yaw = 0;
        if (entityBodyPlace(r->entity, p, yaw)) {
            r->position[0] = p[0];
            r->position[1] = p[1];
            r->position[2] = p[2];
        }
    }
    const float dx = r->position[0] - listenerPosition[0], dy = r->position[1] - listenerPosition[1], dz = r->position[2] - listenerPosition[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float mx = std::max(r->minDistance, r->maxDistance);           // AIL_set_sample_3D_distances keeps the larger as the max
    const float mn = std::max(std::min(r->minDistance, r->maxDistance), 1e-4f);
    if (!listenerSet || d > mx) r->gain3D = 0;
    else if (d <= mn + 1e-4f) r->gain3D = 1;
    else {
        const float f = (mx - d) / (mx - mn);
        r->gain3D = f * f;
    }
    if (d <= 1e-4f) r->left = r->right = std::sqrt(0.5f);
    else stereoLevels({dx / d, dy / d, dz / d}, r->left, r->right);
}

// ---- Occlusion: Miles' low-pass filter (mss32.dll, traced 2026-10-26) ------------------------------------------------------------
// AIL_set_sample_occlusion (0x2111d0f0) keeps the value (sample + 0x98, read by nothing else) and, unless the driver's 3D provider
// does occlusion itself (provider + 0x54 bit 0x20; none here), sets the sample's low-pass cut-off to 1 - occlusion + 0.01 (0x211594ec).
// The cut-off (FUN_2112c100): off unless 0 <= cut-off < 0.999 (0x2115947c); f = cut-off x the sample's rate / the driver's rate, at most
// 0.999; below 1e-4 (0x21159450) every coefficient is 0 (silence); else c = 1 / tan(f x pi/2) (0x21159478) and the Q12 (4096,
// 0x2115946c) Butterworth coefficients A = 4096 / (c^2 + sqrt(2) c + 1), a0 = round(A), b1 = round(2 (1 - c^2) A), b2 = round((1 - sqrt(2)
// c + c^2) A). Recomputed only when f changes; the filter state is zeroed when it was off, else its scaled inputs are set to the
// previous outputs x A (X1 = round(y1 A), X2 = round(y2 A), per channel). The mixer (FUN_2112cfd0) filters the decoded 16-bit data at
// the sample's own rate, before resampling: X0 = x a0, y = (2 X1 + X2 + X0 - b1 y1 - b2 y2) >> 12, kept unclamped as y1, written
// clamped to 16 bits; mono or stereo (two states). A sample re-initialised (AIL_init_sample, the next track of a looping set) starts
// with the filter off. Here: SDL_mixer's raw track callback (the decoded data before gain, fades and resampling; floats x 32768).
struct LowPass {
    std::mutex lock;
    float cutoff = 1.0f;               // + 0x234
    bool on = false;                   // + 0x238
    double lastF = -1.0;               // + 0x240
    int rate = 0;                      // the sample rate the coefficients were made for
    int a0 = 0, b1 = 0, b2 = 0;        // + 0x228 / + 0x22c / + 0x230
    int state[8] = {};                 // + 0x208: X1, X2, y1, y2 (channel 0), then channel 1
};

void lowPassCoefficients(LowPass& p, int sampleRate) {
    p.rate = sampleRate;
    const double c = p.cutoff;
    if (!(c >= 0.0 && c < 0.999)) {
        p.on = false;
        p.lastF = -1.0;
        return;
    }
    double f = double(sampleRate) * c / double(outputRate);
    if (f > 0.999) f = 0.999;
    if (p.lastF != f) {
        p.lastF = f;
        double scale = 0.0;
        if (f >= 1e-4) {
            const double cc = 1.0 / std::tan(f * double(1.5707963705062866f));
            const double root2 = std::sqrt(2.0);
            scale = 4096.0 / (cc * cc + root2 * cc + 1.0);
            p.a0 = int(std::nearbyint(scale));
            p.b1 = int(std::nearbyint(2.0 * (1.0 - cc * cc) * scale));
            p.b2 = int(std::nearbyint((1.0 - root2 * cc + cc * cc) * scale));
        } else {
            p.a0 = p.b1 = p.b2 = 0;
        }
        if (!p.on) {
            for (int& s : p.state) s = 0;
        } else {
            p.state[0] = int(std::nearbyint(p.state[2] * scale));
            p.state[1] = int(std::nearbyint(p.state[3] * scale));
            p.state[4] = int(std::nearbyint(p.state[6] * scale));
            p.state[5] = int(std::nearbyint(p.state[7] * scale));
        }
    }
    p.on = true;
}

void SDLCALL lowPassCallback(void* userdata, MIX_Track*, const SDL_AudioSpec* spec, float* pcm, int samples) {
    LowPass& p = *static_cast<LowPass*>(userdata);
    std::lock_guard<std::mutex> hold(p.lock);
    if (p.rate != spec->freq) lowPassCoefficients(p, spec->freq);
    if (!p.on || spec->channels < 1) return;
    const int channels = spec->channels;
    const int filtered = std::min(channels, 2);
    for (int k = 0; k + channels <= samples; k += channels)
        for (int ch = 0; ch < filtered; ++ch) {
            int* s = p.state + 4 * ch;
            const int x = std::clamp(int(std::lrint(pcm[k + ch] * 32768.0f)), -32768, 32767);
            const int x0 = x * p.a0;
            const int y = (s[0] * 2 + s[1] + x0 - p.b1 * s[2] - p.b2 * s[3]) >> 12;
            s[1] = s[0];
            s[0] = x0;
            s[3] = s[2];
            s[2] = y;
            pcm[k + ch] = float(std::clamp(y, -32768, 32767)) / 32768.0f;
        }
}

void setOcclusion(Request* r, float occlusion) {
    const float cutoff = 1.0f - occlusion + 0.01f;
    for (auto& f : *r->filters) {
        std::lock_guard<std::mutex> hold(f->lock);
        f->cutoff = cutoff;
        if (f->rate > 0) lowPassCoefficients(*f, f->rate);
        else f->on = cutoff >= 0.0f && cutoff < 0.999f;   // coefficients made at the first callback (the rate is known there)
    }
}

void playFile(Request* r, const std::string& file, float soundVolume, bool loop, int fadeInMs) {
    MIX_Audio* a = clip(file);
    if (!a) return;
    MIX_Track* t = MIX_CreateTrack(mixer);
    if (!t) return;
    MIX_SetTrackAudio(t, a);
    r->filters->push_back(std::make_unique<LowPass>());
    MIX_SetTrackRawCallback(t, lowPassCallback, r->filters->back().get());
    r->tracks->push_back(t);
    r->gains->push_back(soundVolume);
    applyGains(r);
    SDL_PropertiesID props = SDL_CreateProperties();
    if (loop) SDL_SetNumberProperty(props, MIX_PROP_PLAY_LOOPS_NUMBER, -1);
    if (fadeInMs > 0) SDL_SetNumberProperty(props, MIX_PROP_PLAY_FADE_IN_MILLISECONDS_NUMBER, fadeInMs);
    if (!MIX_PlayTrack(t, props)) std::fprintf(stderr, "[audio] %s: %s\n", file.c_str(), SDL_GetError());
    static const bool log = std::getenv("OPENRASA_AUDIOLOG") != nullptr;   // test runs: every sound started
    if (log) std::fprintf(stderr, "[audio] play %s (set %d, gain %.2f%s)\n", file.c_str(), r->set, MIX_GetTrackGain(t), loop ? ", loop" : "");
    if (log && r->is3D) {
        const float dx = r->position[0] - listenerPosition[0], dy = r->position[1] - listenerPosition[1], dz = r->position[2] - listenerPosition[2];
        std::fprintf(stderr, "[audio]   3D levels left %.2f right %.2f distance gain %.2f (distance %.1f, %.1f .. %.1f, entity %llu)\n", r->left,
                     r->right, r->gain3D, std::sqrt(dx * dx + dy * dy + dz * dz), r->minDistance, r->maxDistance,
                     static_cast<unsigned long long>(r->entity));
    }
    SDL_DestroyProperties(props);
}

// ---- Muffling (TRasa::AudioManager + 0x10c .. + 0x11c, traced 2026-10-26) ---------------------------------------------------------
// A fader: ms left (+ 0x110), level (+ 0x114), target (+ 0x118), slope (+ 0x11c). Each manager frame (FUN_007a31d0, with the frame's ms)
// while ms left > 0: ms left = max(ms left - ms, 0), level = clamp(slope x ms left + target, 0, 1). A fade (FUN_007a2f20 / inside
// FUN_007a3250): target, ms left = the fade's ms, slope = (level - target) / ms (0 for 0 ms), no fade at all when the slope or the
// difference is 0; then every request of the container takes the level (+ 0x84) and those holding a sample whose set is mufflable (or
// that have no set) get AIL_set_sample_occlusion(sqrt(level)) (FUN_0043ccf0). Started when a sample handle is allocated for a set with
// muffling data (FUN_007a1440 / 1510 / 1620 -> FUN_007a3250) and its fade-in is longer than the ms left or its multiplier above the
// level - that request becomes the owner (+ 0x10c = its serial); when the owner's handle is released (FUN_007a34d0) the level fades to 0
// over its fade-out (forced). In the frame walk, while a fade was running at the frame's start (+ 0x110 > 0), every request takes the
// level and its occlusion again. In the shipped data: the ear ringing loop (multiplier 1, 1 s / 1 s), the radio mission alert (0.95,
// 0.5 s / 1 s), the 10 s ear ring and the Lightbender quills (0.96, no fade-in - so no muffle), two sets with zeros.
int muffleLeftMs = 0;
float muffleLevel = 0, muffleTarget = 0, muffleSlope = 0;
unsigned muffleOwner = 0;

void stepMuffle(int ms) {
    if (muffleLeftMs <= 0) return;
    muffleLeftMs = std::max(muffleLeftMs - ms, 0);
    muffleLevel = std::clamp(muffleSlope * float(muffleLeftMs) + muffleTarget, 0.0f, 1.0f);
}
void applyMuffle(Request* r) {
    r->occlusion = muffleLevel;
    if (r->tracks->empty()) return;
    auto s = sets.find(r->set);
    if (r->set != 0 && (s == sets.end() || !s->second.mufflable)) return;
    setOcclusion(r, std::sqrt(r->occlusion));
}
void startMuffleFade(int ms, float target) {
    const float difference = muffleLevel - target;
    muffleTarget = target;
    muffleLeftMs = ms;
    muffleSlope = ms > 0 ? difference / float(ms) : 0.0f;
    if (muffleSlope == 0.0f || difference == 0.0f) muffleLeftMs = 0;
    stepMuffle(0);
    static const bool log = std::getenv("OPENRASA_AUDIOLOG") != nullptr;
    if (log) std::fprintf(stderr, "[audio] muffle fade to %.2f over %d ms (level %.2f)\n", target, muffleLeftMs, muffleLevel);
    for (Request* r : activeRequests) applyMuffle(r);
}
void muffleStarted(Request* r) {
    auto s = sets.find(r->set);
    if (s == sets.end() || !s->second.muffles) return;
    if (!(muffleLeftMs < s->second.muffleInMs || muffleLevel < s->second.muffleLevel)) return;
    startMuffleFade(s->second.muffleInMs, s->second.muffleLevel);
    muffleOwner = r->serial;
}
void muffleReleased(Request* r) {
    if (muffleOwner != r->serial) return;
    auto s = sets.find(r->set);
    if (s == sets.end() || !s->second.muffles) return;
    startMuffleFade(s->second.muffleOutMs, 0.0f);
}

// The tracks of the current play - the request keeps its voice (Play again on a playing request, the next track of a looping set).
void dropTracks(Request* r) {
    for (MIX_Track* t : *r->tracks) MIX_DestroyTrack(t);
    r->tracks->clear();
    r->gains->clear();
    r->filters->clear();
}
// The voice released (slot 13 / a one-shot done / stopped): the muffle it started fades out (FUN_007a34d0).
void destroyTracks(Request* r) {
    dropTracks(r);
    if (r->hasVoice) {
        r->hasVoice = false;
        muffleReleased(r);
    }
}

// The audio detail level (DAT_00d04218, 1.0 in the file), clamped 0 .. 1 and written only by FUN_007b2120.
float audioDetail = 1.0f;

// FUN_007b1e40: the members a pick may take - max(min(n, 1), round(n x the detail level)), rounded to nearest even (the x87's FISTP).
size_t usableMembers(const AudioSet& set) {
    const size_t n = set.members.size();
    const auto scaled = size_t(std::max(0.0, std::nearbyint(double(float(n) * audioDetail))));
    return std::max(std::min<size_t>(n, 1), scaled);
}

// AudioSet::ChooseSound (FUN_007b21a0): selection 1 or 3 - with a seed below 0 (the default -1.0, 0x00c2b50c) r = the random 32-bit
// value x the set's total weight >> 32 (matRandom::Generate; the game's generator and its seed not reproduced), else r = round(total x
// seed); the first of the usable members whose running weight exceeds r. Past them ("Total occurence weight for set less than 1.0"
// assert) the member at that index, at most the last. Any other selection: members[index % n], and every play passes index 0.
const Member* chooseSound(const AudioSet& set, float seed) {
    const size_t n = set.members.size();
    if (set.selection != 1 && set.selection != 3) return &set.members[0];
    uint32_t r;
    if (seed < 0.0f) r = uint32_t((uint64_t(rng()) * uint64_t(set.totalWeight)) >> 32);
    else r = uint32_t(int64_t(std::nearbyint(double(float(set.totalWeight) * seed))));
    const size_t usable = usableMembers(set);
    unsigned running = 0;
    size_t i = 0;
    for (; i < usable; ++i) {
        running += set.members[i].weight;
        if (r < running) break;
    }
    if (i >= n) std::fprintf(stderr, "[audio] AudioSet::ChooseSound() - Total occurence weight for set less than 1.0 (AudioSet=%s)\n",
                             set.name.c_str());
    return &set.members[std::min(i, n - 1)];
}

// FUN_007a07c0 -> FUN_007b2120 for every set: the level (clamped 0 .. 1) becomes the global and each set's total the weights of its
// usable members. GameClient::Initialize calls it once with Client.Audio.DetailLevel, before the scripts add any set - so in the game
// the option changes nothing (no other reader of the option). The value the call passes is not traced (FUN_004e3620; 100 clamps to 1).
void applyDetailLevel(float level) {
    for (auto& [id, set] : sets) {
        audioDetail = std::clamp(level, 0.0f, 1.0f);
        set.totalWeight = 0;
        for (size_t i = 0, k = usableMembers(set); i < k; ++i) set.totalWeight += set.members[i].weight;
    }
}

// One play of the set: a member by the set's selection (all of them for simultaneous). A looping set with one track and no silence
// loops that track; otherwise update() starts the next play when the tracks end.
void playOnce(Request* r) {
    if (r->set == 0) {
        playFile(r, *r->file, 1.0f, r->loop == 1, 0);
        return;
    }
    auto s = sets.find(r->set);
    if (s == sets.end() || s->second.members.empty()) return;
    AudioSet& set = s->second;
    const auto c = categories.find(set.category);
    const int fadeIn = r->fadeInMs >= 0 ? r->fadeInMs : c != categories.end() ? c->second.fadeInMs : 0;
    const bool looping = r->loop >= 0 ? r->loop == 1 : set.looping;
    const bool loopTrack = looping && set.members.size() == 1 && set.silenceSeconds <= 0;
    // Each play (FUN_007b0bd0, + 0x78 counted) takes one member: AudioSet::ChooseSound(the request's seed, index 0).
    const Member* m = chooseSound(set, r->seed);
    auto snd = sounds.find(m->sound);
    if (snd != sounds.end()) playFile(r, snd->second.file, snd->second.volume, loopTrack, fadeIn);
}

void activate(Request* r) {
    if (!r->active) {
        r->active = true;
        Py_INCREF(reinterpret_cast<PyObject*>(r));
        activeRequests.insert(r);
    }
    orderChanged = true;
}
void deactivate(Request* r) {
    if (r->active) {
        r->active = false;
        activeRequests.erase(r);
        orderChanged = true;
        Py_DECREF(reinterpret_cast<PyObject*>(r));
    }
}

PyObject* none() {
    Py_INCREF(Py_None);
    return Py_None;
}

// A request's audibility (vtable slot 4): Audio2D / AudioStream (FUN_007b0e20) the ramp factor x the request's volume (+ 0x48);
// Audio3D (FUN_007b36b0) 0 without a position (+ 0xb0), else d^2 to the listener is kept (+ 0x60) and beyond the max distance (+ 0xa0)
// it is 0, within the min (+ 0x9c) 1, between ((max - d) / (max - min))^2 - x the ramp factor x the volume. The ramp factor
// (FUN_007b0e50) is Miles' Volume Ramp Filter "Ramp At" while the sample plays and a manager flag (+ 0x10a) is set, else 1 - taken
// as 1 here (SDL_mixer's fades expose no level; it only reorders fading requests, which are never parked).
float audibility(Request* r) {
    if (!r->is3D) return r->volume;
    if (r->entity) {
        std::array<float, 3> p;
        float yaw = 0;
        if (entityBodyPlace(r->entity, p, yaw)) {
            r->position[0] = p[0];
            r->position[1] = p[1];
            r->position[2] = p[2];
        }
    }
    const float dx = r->position[0] - listenerPosition[0], dy = r->position[1] - listenerPosition[1], dz = r->position[2] - listenerPosition[2];
    r->distanceSq = dx * dx + dy * dy + dz * dz;
    if (r->distanceSq > r->maxDistance * r->maxDistance) return 0.0f;
    float f = 1.0f;
    if (r->minDistance * r->minDistance < r->distanceSq) {
        f = (r->maxDistance - std::sqrt(r->distanceSq)) / (r->maxDistance - r->minDistance);
        f = f * f;
    }
    return f * r->volume;
}

// Play (slot 8, FUN_007b0b60 -> FUN_007a0d60): nothing starts here - the request becomes "waiting to start" (state 1) with its
// audibility worked out now, joins the manager's container, and update() starts it when it is among the voices given out.
PyObject* rPlay(PyObject* o, PyObject* args) {
    Request* r = reinterpret_cast<Request*>(o);
    PyObject* ignored = nullptr;
    if (!PyArg_ParseTuple(args, const_cast<char*>("|O:Play"), &ignored)) return nullptr;
    if (!start()) return none();
    dropTracks(r);
    r->state = 1;
    r->fadingOut = false;
    r->rampLeft = 0;
    r->firstQueuedMs = 0;
    r->audibility = audibility(r);
    activate(r);
    return none();
}
// Stop: at once (slot 0x24), or (FUN_007b0fe0) with the category's fade-out when a sample plays - without one it is done at once.
void stopRequest(Request* r, int fadeOutMs) {
    if (fadeOutMs > 0 && !r->tracks->empty()) {
        if (!r->fadingOut) {
            for (MIX_Track* t : *r->tracks) MIX_StopTrack(t, MIX_TrackMSToFrames(t, fadeOutMs));
            r->rampLeft = fadeOutMs;
            r->fadingOut = true;
            orderChanged = true;
        }
        return;
    }
    for (MIX_Track* t : *r->tracks) MIX_StopTrack(t, 0);
    destroyTracks(r);
    if (r->fadingOut) orderChanged = true;
    r->fadingOut = false;
    r->state = 3;
}
PyObject* rStop(PyObject* o, PyObject* args) {
    Request* r = reinterpret_cast<Request*>(o);
    int immediate = 0;
    if (!PyArg_ParseTuple(args, const_cast<char*>("|i:Stop"), &immediate)) return nullptr;
    auto s = sets.find(r->set);
    int fadeOut = 0;
    if (!immediate && s != sets.end()) {
        auto c = categories.find(s->second.category);
        if (c != categories.end()) fadeOut = c->second.fadeOutMs;
    }
    stopRequest(r, fadeOut);
    return none();
}
PyObject* rIsDone(PyObject* o, PyObject*) {
    const Request* r = reinterpret_cast<Request*>(o);
    return PyBool_FromLong(!r->active || r->state == 3 ? 1 : 0);
}
PyObject* rSetVolume(PyObject* o, PyObject* args) {
    Request* r = reinterpret_cast<Request*>(o);
    if (!PyArg_ParseTuple(args, const_cast<char*>("f:SetVolume"), &r->volume)) return nullptr;
    applyGains(r);
    return none();
}
PyObject* rSetIsMusic(PyObject* o, PyObject* args) {
    Request* r = reinterpret_cast<Request*>(o);
    int v = 0;
    if (!PyArg_ParseTuple(args, const_cast<char*>("i:SetIsMusic"), &v)) return nullptr;
    r->music = v ? 1 : 0;
    applyGains(r);
    return none();
}
PyObject* rIgnore(PyObject*, PyObject*) { return none(); }   // pan: not applied yet
// SetRandomSeed(f) (0x00575ef0): the request's + 0x68 (ChooseSound's seed).
PyObject* rSetRandomSeed(PyObject* o, PyObject* args) {
    float seed = 0;
    if (!PyArg_ParseTuple(args, const_cast<char*>("f:SetRandomSeed"), &seed)) return nullptr;
    reinterpret_cast<Request*>(o)->seed = seed;
    return none();
}
// SetAudioParams(loop=0, callbackInfo=None, priority=2, randomSeed=-1.0) (0x00575CD0, keywords at 0x00d2513c): the loop flag is kept
// (FUN_007b0ca0), the priority x 20 replaces the category's as the request's sort key (+ 0xd); priority 0 .. 4 checked; the callback is not
// used here; the seed is kept (+ 0x68).
PyObject* rSetAudioParams(PyObject* o, PyObject* args, PyObject* kwargs) {
    Request* r = reinterpret_cast<Request*>(o);
    static char* names[] = {const_cast<char*>("loop"), const_cast<char*>("callbackInfo"), const_cast<char*>("priority"),
                            const_cast<char*>("randomSeed"), nullptr};
    int loop = 0, priority = 2;
    PyObject* callback = nullptr;
    float seed = -1;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, const_cast<char*>("|iOif:SetAudioParams"), names, &loop, &callback, &priority, &seed))
        return nullptr;
    if (priority < 0 || priority > 4) {
        PyErr_SetString(PyExc_RuntimeError, "invalid priority value, must be 0 - 4 (lowest to highest)");
        return nullptr;
    }
    r->loop = loop != 0 ? 1 : 0;
    r->priority = priority * 20;
    r->seed = seed;   // + 0x68 (SetAudioParams FUN_007b0ca0)
    orderChanged = true;
    return none();
}
// SetPosition((x, y, z)) (0x00576120): a 3D position.
PyObject* rSetPosition(PyObject* o, PyObject* args) {
    Request* r = reinterpret_cast<Request*>(o);
    if (!PyArg_ParseTuple(args, const_cast<char*>("(fff):SetPosition"), &r->position[0], &r->position[1], &r->position[2])) return nullptr;
    place3D(r);
    applyGains(r);
    return none();
}

PyMethodDef requestMethods[] = {
    {const_cast<char*>("Play"), rPlay, METH_VARARGS, nullptr},
    {const_cast<char*>("Stop"), rStop, METH_VARARGS, nullptr},
    {const_cast<char*>("IsDone"), rIsDone, METH_NOARGS, nullptr},
    {const_cast<char*>("SetVolume"), rSetVolume, METH_VARARGS, nullptr},
    {const_cast<char*>("SetIsMusic"), rSetIsMusic, METH_VARARGS, nullptr},
    {const_cast<char*>("SetAudioParams"), reinterpret_cast<PyCFunction>(rSetAudioParams), METH_VARARGS | METH_KEYWORDS, nullptr},
    {const_cast<char*>("SetVolumePan"), rIgnore, METH_VARARGS, nullptr},
    {const_cast<char*>("SetPosition"), rSetPosition, METH_VARARGS, nullptr},
    {const_cast<char*>("SetRandomSeed"), rSetRandomSeed, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr}};

PyObject* requestGetAttr(PyObject* o, char* name) { return Py_FindMethod(requestMethods, o, name); }
void requestDealloc(PyObject* o) {
    Request* r = reinterpret_cast<Request*>(o);
    destroyTracks(r);
    delete r->file;
    delete r->filters;
    delete r->tracks;
    delete r->gains;
    PyObject_Del(o);
}

PyObject* newRequest(int set, const std::string& file) {
    if (!requestType.tp_name) {
        requestType.ob_type = &PyType_Type;
        requestType.tp_name = const_cast<char*>("AudioRequest");
        requestType.tp_basicsize = sizeof(Request);
        requestType.tp_dealloc = requestDealloc;
        requestType.tp_getattr = requestGetAttr;
        requestType.tp_flags = Py_TPFLAGS_DEFAULT;
        PyType_Ready(&requestType);
    }
    Request* r = PyObject_New(Request, &requestType);
    r->set = set;
    r->file = new std::string(file);
    r->volume = 1;
    r->music = set == 0 ? 0 : -1;
    r->tracks = new std::vector<MIX_Track*>();
    r->filters = new std::vector<std::unique_ptr<LowPass>>();
    r->gains = new std::vector<float>();
    r->active = false;
    r->silenceLeft = -1;
    r->seed = -1.0f;
    r->loop = -1;
    r->is3D = false;
    r->minDistance = 8;
    r->maxDistance = 30;
    r->position[0] = r->position[1] = r->position[2] = 0;
    r->entity = 0;
    r->gain3D = 1;
    r->left = r->right = 1;
    r->fadeInMs = -1;
    r->priority = -1;
    r->state = 0;
    r->audibility = 0;
    r->distanceSq = 0;
    r->serial = ++requestCounter;
    r->rampLeft = 0;
    r->fadingOut = false;
    r->firstQueuedMs = 0;
    r->hasVoice = false;
    r->occlusion = 0;
    return reinterpret_cast<PyObject*>(r);
}

// CreateAudioRequest(set) (FUN_007a0940) picks the request's class from the set's category: a music category (record + 0x25) an
// AudioStream, a 3D one (+ 0x23) an Audio3D with the set's distances (the smaller as the min), anything else an Audio2D. Audio2D and
// AudioStream's SetPosition / SetMin / SetMax do nothing, so a position or a body never places them.
void makeSetRequest(Request* r) {
    auto s = sets.find(r->set);
    if (s == sets.end()) return;
    auto c = categories.find(s->second.category);
    if (c == categories.end() || c->second.music || !c->second.is3D) return;
    r->is3D = true;
    r->minDistance = std::min(s->second.minDistance, s->second.maxDistance);
    r->maxDistance = std::max(s->second.minDistance, s->second.maxDistance);
}

// ---- The manager's methods (table 0x00d07ce8) -------------------------------------------------------------------------------------
PyObject* mCreateAudioRequest(PyObject*, PyObject* args) {
    int set = 0;
    if (!PyArg_ParseTuple(args, const_cast<char*>("|i:CreateAudioRequest"), &set)) return nullptr;
    if (!sets.count(set)) {
        std::fprintf(stderr, "[audio] AudioSet %d not found.\n", set);
        return none();
    }
    PyObject* o = newRequest(set, "");
    makeSetRequest(reinterpret_cast<Request*>(o));
    return o;
}
// GetAudio3D(file, maxDistance, minDistance) (0x005750C0): a 3D sound of one file (backgroundaudiomgr.py: the distances from
// audiodata.audio3DParameter, 8 / 30 m by default). TRasa::Audio3D::_Play refuses distances below a minimum (0x00c2be78).
PyObject* mGetAudio3D(PyObject*, PyObject* args) {
    char* file;
    float maxDistance = 30, minDistance = 8;
    if (!PyArg_ParseTuple(args, const_cast<char*>("sff:GetAudio3D"), &file, &maxDistance, &minDistance)) return nullptr;
    PyObject* o = newRequest(0, file);
    Request* r = reinterpret_cast<Request*>(o);
    r->is3D = true;
    r->minDistance = minDistance;
    r->maxDistance = maxDistance;
    return o;
}
PyObject* mGetAudio2D(PyObject*, PyObject* args) {
    char* file;
    float a = 0, b = 0;
    if (!PyArg_ParseTuple(args, const_cast<char*>("s|ff"), &file, &a, &b)) return nullptr;
    return newRequest(0, file);
}
PyObject* mAddSound(PyObject*, PyObject* args) {
    int id;
    char *file, *desc;
    float volume;
    if (!PyArg_ParseTuple(args, const_cast<char*>("issf:AddSound"), &id, &file, &desc, &volume)) return nullptr;
    sounds[id] = {file, volume};
    return none();
}
PyObject* mAddAudioCategory(PyObject*, PyObject* args) {
    int id, priority, maxSimultaneous, eax, is3d, music, curve;
    char* name;
    float fadeIn, fadeOut;
    if (!PyArg_ParseTuple(args, const_cast<char*>("isiiiiiffi:AddAudioCategory"), &id, &name, &priority, &maxSimultaneous, &eax, &is3d,
                          &music, &fadeIn, &fadeOut, &curve))
        return nullptr;
    categories[id] = {name, priority & 0xff, maxSimultaneous & 0xff, music != 0, is3d != 0, int(fadeIn), int(fadeOut)};
    return none();
}
PyObject* mAddAudioSet(PyObject*, PyObject* args) {
    int id, category, looping, selection, mufflable = 0;
    char* name;
    float minDistance, maxDistance, silence;
    if (!PyArg_ParseTuple(args, const_cast<char*>("isiiifffi:AddAudioSet"), &id, &name, &category, &looping, &selection, &minDistance,
                          &maxDistance, &silence, &mufflable))
        return nullptr;
    AudioSet& s = sets[id];
    s.name = name;
    s.category = category;
    s.looping = looping != 0;
    s.selection = selection;
    s.silenceSeconds = silence;
    s.minDistance = minDistance;
    s.maxDistance = maxDistance;
    s.mufflable = mufflable != 0;
    return none();
}
// SetAudioSetMuffling(set, multiplier, fadeInMs, fadeOutMs) (0x00575A90 -> FUN_007a11e0).
PyObject* mSetAudioSetMuffling(PyObject*, PyObject* args) {
    int set, fadeIn, fadeOut;
    float multiplier;
    if (!PyArg_ParseTuple(args, const_cast<char*>("ifii:SetAudioSetMuffling"), &set, &multiplier, &fadeIn, &fadeOut)) return nullptr;
    auto s = sets.find(set);
    if (s == sets.end()) {
        std::fprintf(stderr, "[audio] Trying to add Attenuation to nonexistent AudioSet [%d].\n", set);
        return none();
    }
    s->second.muffles = true;
    s->second.muffleLevel = multiplier;
    s->second.muffleInMs = fadeIn;
    s->second.muffleOutMs = fadeOut;
    return none();
}
PyObject* mAddAudioSetMember(PyObject*, PyObject* args) {
    int set, sound, a, c;
    float b;
    if (!PyArg_ParseTuple(args, const_cast<char*>("iiifi:AddAudioSetMember"), &set, &sound, &a, &b, &c)) return nullptr;
    if (sounds.find(sound) == sounds.end()) {   // FUN_007a1360 looks the sound up first
        std::fprintf(stderr, "[audio] Trying to add member with nonexistent soundId [%d].\n", sound);
        return none();
    }
    auto s = sets.find(set);
    if (s == sets.end()) {
        std::fprintf(stderr, "[audio] Trying to add member to nonexistent AudioSet [%d].\n", set);
        return none();
    }
    // AudioSet::AddMember (FUN_007b2070): a member of the same sound is taken out (its weight off the total), the new one appended.
    AudioSet& target = s->second;
    for (auto it = target.members.begin(); it != target.members.end(); ++it)
        if (it->sound == sound) {
            target.totalWeight -= it->weight;
            target.members.erase(it);
            break;
        }
    target.members.push_back({sound, a, b, uint8_t(c)});
    target.totalWeight += uint8_t(c);
    return none();
}
PyObject* getVolume(float v) { return PyFloat_FromDouble(v); }
PyObject* mGetMasterVolume(PyObject*, PyObject*) { return getVolume(masterVolume); }
PyObject* mGetMusicVolume(PyObject*, PyObject*) { return getVolume(musicVolume); }
PyObject* mGetEffectsVolume(PyObject*, PyObject*) { return getVolume(effectsVolume); }
void regain() {
    if (mixer) MIX_SetMixerGain(mixer, masterVolume);
    for (Request* r : activeRequests) applyGains(r);
    static const bool log = std::getenv("OPENRASA_AUDIOLOG") != nullptr;
    if (log) std::fprintf(stderr, "[audio] volumes master %.2f music %.2f effects %.2f (%zu playing)\n", masterVolume, musicVolume,
                          effectsVolume, activeRequests.size());
}
PyObject* setVolume(PyObject* args, float& v) {
    if (!PyArg_ParseTuple(args, const_cast<char*>("f"), &v)) return nullptr;
    regain();
    return none();
}
PyObject* mSetMasterVolume(PyObject*, PyObject* args) { return setVolume(args, masterVolume); }
PyObject* mSetMusicVolume(PyObject*, PyObject* args) { return setVolume(args, musicVolume); }
PyObject* mSetEffectsVolume(PyObject*, PyObject* args) { return setVolume(args, effectsVolume); }
PyObject* mIgnore(PyObject*, PyObject*) { return none(); }

}  // namespace

void configure(bool on, float master, float music, float effects, unsigned maxHandles, unsigned updatePeriod, unsigned playbackFrequency,
               unsigned playbackBits, float detailLevel) {
    static bool initialised = false;
    if (!initialised) {   // GameClient::Initialize's part: the driver's format and the detail level, once
        initialised = true;
        outputRate = playbackFrequency > 0 ? playbackFrequency : 44100;
        outputBits = playbackBits;
        applyDetailLevel(detailLevel);
    }
    maxVoices = maxHandles;
    updatePeriodMs = updatePeriod;
    enabled = on;
    masterVolume = master;
    musicVolume = music;
    effectsVolume = effects;
    regain();   // the mixer's master gain and every playing sound's gain at once (the options' sliders change them while dragged)
}

float masterGain() { return enabled ? masterVolume : 0.0f; }
bool running() { return start(); }

PyObject* managerObject() {
    static PyObject* manager = nullptr;
    if (!manager) {
        static PyMethodDef defs[] = {
            {const_cast<char*>("CreateAudioRequest"), mCreateAudioRequest, METH_VARARGS, nullptr},
            {const_cast<char*>("GetAudio2D"), mGetAudio2D, METH_VARARGS, nullptr},
            {const_cast<char*>("GetAudio3D"), mGetAudio3D, METH_VARARGS, nullptr},
            {const_cast<char*>("GetAudioStream"), mGetAudio2D, METH_VARARGS, nullptr},
            {const_cast<char*>("GetMasterVolume"), mGetMasterVolume, METH_VARARGS, nullptr},
            {const_cast<char*>("SetMasterVolume"), mSetMasterVolume, METH_VARARGS, nullptr},
            {const_cast<char*>("GetMusicVolume"), mGetMusicVolume, METH_VARARGS, nullptr},
            {const_cast<char*>("SetMusicVolume"), mSetMusicVolume, METH_VARARGS, nullptr},
            {const_cast<char*>("GetEffectsVolume"), mGetEffectsVolume, METH_VARARGS, nullptr},
            {const_cast<char*>("SetEffectsVolume"), mSetEffectsVolume, METH_VARARGS, nullptr},
            {const_cast<char*>("AddSound"), mAddSound, METH_VARARGS, nullptr},
            {const_cast<char*>("AddSelectionType"), mIgnore, METH_VARARGS, nullptr},
            {const_cast<char*>("AddAudioCategory"), mAddAudioCategory, METH_VARARGS, nullptr},
            {const_cast<char*>("AddAudioSet"), mAddAudioSet, METH_VARARGS, nullptr},
            {const_cast<char*>("AddAudioSetMember"), mAddAudioSetMember, METH_VARARGS, nullptr},
            {const_cast<char*>("SetAudioSetMuffling"), mSetAudioSetMuffling, METH_VARARGS, nullptr},
            {nullptr, nullptr, 0, nullptr}};
        manager = Py_InitModule(const_cast<char*>("openrasa_audiomanager"), defs);
        Py_INCREF(manager);
    }
    Py_INCREF(manager);
    return manager;
}

bool playAudioSet(int audioSetId) {
    if (!sets.count(audioSetId)) return false;
    PyObject* r = newRequest(audioSetId, "");
    PyObject* empty = PyTuple_New(0);
    PyObject* result = rPlay(r, empty);
    Py_XDECREF(result);
    Py_DECREF(empty);
    Py_DECREF(r);
    return true;
}

// ---- Audio emitters ----------------------------------------------------------------------------------------------------------------
// TRasa::AudioEmitter (audioemitter.cpp; 0x78 bytes: UID + 0x28, AudioSetID + 0x30, the playing request + 0x34, MinDistance + 0x38,
// MaxDistance + 0x3c, Volume + 0x40, FadeInTime + 0x44, FadeOutTime + 0x48, Position + 0x4c, loop + 0x58, MinDelay + 0x5c, MaxDelay
// + 0x60, the delay left + 0x64), updated each frame with the frame's milliseconds (FUN_007b8230):
// - its request done: released;
// - not looping and its request no longer playing: stopped (FadeOutTime x 1000 ms, FUN_007b81a0) and a new delay drawn;
// - otherwise the delay counts down (seconds); at 0 a new delay is drawn (FUN_007b7fc0: looping when MinDelay and MaxDelay are both
//   below 1, delay 0; else MinDelay + (MaxDelay - MinDelay) x a uniform random number) and, when nothing plays and the listener is
//   within MaxDistance (FUN_007b7eb0), the set starts (FUN_007b8040): a request of the set with the loop flag, at the position, the
//   distances (the smaller as min), FadeInTime x 1000 ms, the volume.
struct Emitter {
    tr::AudioEmitter props;
    Request* request = nullptr;
    bool loop = false;
    float delay = 0;
};
std::vector<Emitter> emitters;
std::uniform_real_distribution<float> unit(0.0f, 1.0f);

void drawDelay(Emitter& e) {
    e.loop = e.props.minDelay < 1.0f && e.props.maxDelay < 1.0f;
    e.delay = e.loop ? 0.0f : e.props.minDelay + (e.props.maxDelay - e.props.minDelay) * unit(rng);
}
void releaseEmitter(Emitter& e) {
    if (!e.request) return;
    Py_DECREF(reinterpret_cast<PyObject*>(e.request));
    e.request = nullptr;
}
void stopEmitter(Emitter& e) {
    if (!e.request) return;
    stopRequest(e.request, int(e.props.fadeOutTime * 1000.0f));
    releaseEmitter(e);
}
void startEmitter(Emitter& e) {
    if (e.request || e.props.audioSetId == 0 || !sets.count(int(e.props.audioSetId)) || !listenerSet) return;
    const float dx = e.props.position.x - listenerPosition[0], dy = e.props.position.y - listenerPosition[1],
                dz = e.props.position.z - listenerPosition[2];
    if (dx * dx + dy * dy + dz * dz > e.props.maxDistance * e.props.maxDistance) return;
    // FUN_007b8040: CreateAudioRequest(set) (the category's class), the loop flag, SetPosition / SetMin / SetMax with the emitter's
    // values (no-ops for a 2D or music request), volume = Volume, pan 0.5, then Play(-1) - the category's fade-in; FadeInTime is not read.
    Request* r = reinterpret_cast<Request*>(newRequest(int(e.props.audioSetId), ""));
    makeSetRequest(r);
    r->loop = e.loop ? 1 : 0;
    r->position[0] = e.props.position.x;
    r->position[1] = e.props.position.y;
    r->position[2] = e.props.position.z;
    if (r->is3D) {
        r->minDistance = std::min(e.props.minDistance, e.props.maxDistance);
        r->maxDistance = std::max(e.props.minDistance, e.props.maxDistance);
    }
    r->volume = e.props.volume;
    e.request = r;
    PyObject* empty = PyTuple_New(0);
    PyObject* result = rPlay(reinterpret_cast<PyObject*>(r), empty);
    Py_XDECREF(result);
    Py_DECREF(empty);
}
void updateEmitters(int msElapsed) {
    for (Emitter& e : emitters) {
        if (e.request && (!e.request->active || e.request->state == 3)) releaseEmitter(e);
        if (!e.loop && e.request && (!e.request->active || e.request->state == 3)) {
            stopEmitter(e);
            drawDelay(e);
            continue;
        }
        e.delay -= float(msElapsed) * 0.001f;
        if (e.delay <= 0.0f) {
            drawDelay(e);
            startEmitter(e);
        }
    }
}

bool isLooping(const Request* r) {
    if (r->loop >= 0) return r->loop == 1;
    auto s = sets.find(r->set);
    return s != sets.end() && s->second.looping;
}
const Category* categoryOf(const Request* r, int& id) {
    id = 0;
    auto s = sets.find(r->set);
    if (s == sets.end()) return nullptr;
    id = s->second.category;
    auto c = categories.find(id);
    return c == categories.end() ? nullptr : &c->second;
}
bool tracksPlaying(const Request* r) {
    for (MIX_Track* t : *r->tracks)
        if (MIX_TrackPlaying(t)) return true;
    return false;
}

// One request's tick in the manager's walk (FUN_007b0a10, the frame's ms): a fade-out that has finished (or a sample no longer
// playing) stops it; between a looping set's tracks (state 0) the silence counts down and then it waits to start again (state 1); a
// playing sample's fade counts down, and when its sample has ended a one-shot is done (FUN_0079ec40) and a looping set waits
// BetweenTrackSilenceTimeSecs (the set's + 0x30) in state 0.
void tick(Request* r, int msElapsed) {
    if (r->fadingOut && (!tracksPlaying(r) || r->rampLeft == 0)) stopRequest(r, 0);
    if (r->state == 0 && r->silenceLeft >= 0) {
        r->silenceLeft -= msElapsed;
        if (r->silenceLeft <= 0) {
            r->state = 1;
            r->silenceLeft = -1;
        }
    }
    if (!r->tracks->empty() && r->state == 2) {
        if (r->rampLeft > 0) r->rampLeft = std::max(0.0, r->rampLeft - msElapsed);
        if (!tracksPlaying(r)) {
            if (!isLooping(r)) {
                destroyTracks(r);
                r->state = 3;
                return;
            }
            dropTracks(r);
            auto s = sets.find(r->set);
            r->silenceLeft = s != sets.end() ? s->second.silenceSeconds * 1000.0 : 0.0;
            r->state = 0;
        }
    }
}

// The audio manager's frame (FUN_007a2230, from PreRender with the frame's ms). Every UpdatePeriodMilliseconds (FUN_007a2960) each
// request's audibility is worked out again and the container re-sorted. Then the requests are walked in order - category priority
// (+ 0xd, the category's + 0x20), audibility, nearer (+ 0x60), fading out (+ 0x67), older (+ 0x74) first (the container's second
// index, comparator FUN_0079ec80, walked from its end) - each ticked, then:
// - parked when it is done, or MaxHandles voices are already given out this frame, or its category's max simultaneous is reached,
//   or (no fade in or out under way and) its audibility is at most 1/64 (0x00c2bde0 into manager + 0x104): a looping one that was
//   playing goes back to waiting (state 1) and a one-shot waiting or playing is done (state 3); either way its sample is released
//   (slot 13);
// - otherwise it takes a voice of the frame's count and of its category's, and if it was waiting it is started (slot 15 _Play; the
//   first attempt's time kept at + 0x7c - a failed start is retried, a one-shot dropped after 2000 ms).
// Done requests then leave the container (FUN_007a2770). A category the scripts never registered has no voices (map default 0).
// The refresh (FUN_007a2960) walks the container's first index - ordered by the request's address (FUN_007abb60) - and stops at the
// first done request, so between a Stop and the next purge the requests at higher addresses keep their old audibility for a period;
// heap addresses cannot be reproduced, so every request is refreshed here.
void updateMusic(int msElapsed);
void update(int msElapsed) {
    updateEmitters(msElapsed);
    managerClock += msElapsed;
    const bool muffleFading = muffleLeftMs > 0;
    stepMuffle(msElapsed);
    if (managerClock > nextRefresh) {
        nextRefresh = managerClock + updatePeriodMs;
        for (Request* r : activeRequests)
            if (r->state != 3) r->audibility = audibility(r);
        orderChanged = true;
    }
    if (orderChanged) {
        requestOrder.assign(activeRequests.begin(), activeRequests.end());
        std::sort(requestOrder.begin(), requestOrder.end(), [](const Request* a, const Request* b) {
            int ia, ib;
            const Category* ca = categoryOf(a, ia);
            const Category* cb = categoryOf(b, ib);
            const int pa = a->priority >= 0 ? a->priority : ca ? ca->priority : 0, pb = b->priority >= 0 ? b->priority : cb ? cb->priority : 0;
            if (pa != pb) return pa > pb;
            if (a->audibility != b->audibility) return a->audibility > b->audibility;
            if (a->distanceSq != b->distanceSq) return a->distanceSq < b->distanceSq;
            if (a->fadingOut != b->fadingOut) return a->fadingOut;
            return a->serial < b->serial;
        });
        orderChanged = false;
    }
    const std::vector<Request*> list = requestOrder;   // the walk may stop requests (deactivate)
    static const bool log = std::getenv("OPENRASA_AUDIOLOG") != nullptr;
    unsigned voices = 0;
    std::map<int, unsigned> categoryVoices;
    std::vector<Request*> toStart;
    for (Request* r : list) {
        if (muffleFading) applyMuffle(r);
        tick(r, msElapsed);
        int categoryId = 0;
        const Category* c = categoryOf(r, categoryId);
        const unsigned limit = r->set == 0 ? maxVoices : c ? unsigned(c->maxSimultaneous) : 0u;   // category 0: MaxHandles
        unsigned& inCategory = categoryVoices[categoryId];
        const bool park = r->state == 3 || voices >= maxVoices || inCategory >= limit ||
                          (r->rampLeft <= 0 && !r->fadingOut && r->audibility <= 1.0f / 64.0f);
        if (park) {
            if (isLooping(r)) {
                if (r->state == 2) r->state = 1;
            } else if (r->state == 1 || r->state == 2) {
                r->state = 3;
            }
            destroyTracks(r);
            continue;
        }
        ++voices;
        ++inCategory;
        if (r->state == 1) {
            if (r->firstQueuedMs == 0) r->firstQueuedMs = managerClock;
            toStart.push_back(r);
        }
        if (r->is3D && !r->tracks->empty()) {
            const float before = r->gain3D;
            place3D(r);
            applyGains(r);
            if (log && before <= 0.0f && r->gain3D > 0.0f)
                std::fprintf(stderr, "[audio] audible: set %d (entity %llu) left %.2f right %.2f distance gain %.2f\n", r->set,
                             static_cast<unsigned long long>(r->entity), r->left, r->right, r->gain3D);
        }
    }
    for (Request* r : list)
        if (r->state == 3) deactivate(r);
    for (Request* r : toStart) {
        place3D(r);
        const bool newVoice = !r->hasVoice;
        r->hasVoice = true;
        playOnce(r);
        if (newVoice) muffleStarted(r);
        r->state = 2;
        int categoryId = 0;
        const Category* c = categoryOf(r, categoryId);
        const int fadeIn = r->fadeInMs >= 0 ? r->fadeInMs : c ? c->fadeInMs : 0;
        r->rampLeft = r->set != 0 && fadeIn > 0 ? fadeIn : 0;
    }
    updateMusic(msElapsed);   // the exe: right after the audio manager's update in PreRender's timed block (FUN_00539520)
}

void setListener(const std::array<float, 3>& position, const std::array<float, 3>& forward, const std::array<float, 3>& up) {
    listenerPosition = position;
    listenerForward = unit3(forward);
    listenerUp = unit3(up);
    listenerSet = true;
}

// body.DetachAudio(request) (0x004FE780 -> FUN_00502f70): the request leaves the body's list - it keeps playing where it was.
PyObject* detachFromEntity(PyObject* request, uint64_t entity) {
    if (!request || request->ob_type != &requestType) {
        PyErr_SetString(PyExc_TypeError, "DetachAudio: not an audio request");
        return nullptr;
    }
    Request* r = reinterpret_cast<Request*>(request);
    if (r->entity == entity) r->entity = 0;
    Py_INCREF(Py_None);
    return Py_None;
}
// body.Destroy() (0x00500FB0) stops each attached sound at once (slot 0x24) and empties the list.
void stopEntitySounds(uint64_t entity) {
    std::vector<Request*> attached;
    for (Request* r : activeRequests)
        if (r->entity == entity) attached.push_back(r);
    for (Request* r : attached) {
        stopRequest(r, 0);
        r->entity = 0;
        deactivate(r);
    }
}

PyObject* attachToEntity(PyObject* request, uint64_t entity) {
    if (!request || request->ob_type != &requestType) {
        PyErr_SetString(PyExc_TypeError, "AttachAudio: not an audio request");
        return nullptr;
    }
    Request* r = reinterpret_cast<Request*>(request);
    r->entity = entity;
    PyObject* empty = PyTuple_New(0);
    PyObject* result = rPlay(request, empty);
    Py_DECREF(empty);
    return result;
}

// ---- Special-FX package sounds (TRasa::SfxAudioEvent / SfxAudioProduct) -----------------------------------------------------------
std::map<uint64_t, PyObject*> packageSounds;   // the products' references to their requests
uint64_t packageSoundCounter = 0;

uint64_t playPackageSound(int audioSet, const std::array<float, 3>& position, bool lowRelevance) {
    auto s = sets.find(audioSet);
    if (s == sets.end()) return 0;
    auto c = categories.find(s->second.category);
    if (c == categories.end()) return 0;
    // A body below the SFX relevance threshold plays only categories whose name starts "Music" (category + 0x24).
    if (lowRelevance && c->second.name.compare(0, 5, "Music") != 0) {
        if (std::getenv("OPENRASA_AUDIOLOG"))
            std::fprintf(stderr, "[audio] package sound set %d (%s) at %.2f %.2f %.2f not played: low relevance\n", audioSet, s->second.name.c_str(),
                         position[0], position[1], position[2]);
        return 0;
    }
    PyObject* o = newRequest(audioSet, "");
    Request* r = reinterpret_cast<Request*>(o);
    makeSetRequest(r);
    r->position[0] = position[0];
    r->position[1] = position[1];
    r->position[2] = position[2];
    place3D(r);
    applyGains(r);
    PyObject* empty = PyTuple_New(0);
    PyObject* result = rPlay(o, empty);
    Py_DECREF(empty);
    Py_XDECREF(result);
    const uint64_t handle = ++packageSoundCounter;
    packageSounds[handle] = o;
    if (std::getenv("OPENRASA_AUDIOLOG"))
        std::fprintf(stderr, "[audio] package sound %llu: set %d (%s) at %.2f %.2f %.2f%s\n", static_cast<unsigned long long>(handle), audioSet,
                     s->second.name.c_str(), position[0], position[1], position[2], lowRelevance ? " (low relevance)" : "");
    return handle;
}
bool movePackageSound(uint64_t handle, const std::array<float, 3>& position) {
    auto it = packageSounds.find(handle);
    if (it == packageSounds.end()) return false;
    Request* r = reinterpret_cast<Request*>(it->second);
    if (!r->active || r->state == 3) return false;
    r->position[0] = position[0];
    r->position[1] = position[1];
    r->position[2] = position[2];
    return true;
}
void releasePackageSound(uint64_t handle, bool fade, int fadeMs) {
    auto it = packageSounds.find(handle);
    if (it == packageSounds.end()) return;
    Request* r = reinterpret_cast<Request*>(it->second);
    if (std::getenv("OPENRASA_AUDIOLOG"))
        std::fprintf(stderr, "[audio] package sound %llu released (set %d, state %d%s)\n", static_cast<unsigned long long>(handle), r->set,
                     r->state, fade ? ", fade" : "");
    if (fade && r->active && r->state != 3) {
        stopRequest(r, fadeMs);
    }
    Py_DECREF(it->second);
    packageSounds.erase(it);
}

void loadEmitters(const std::vector<tr::AudioEmitter>& list) {
    clearEmitters();
    if (std::getenv("OPENRASA_AUDIOLOG")) std::fprintf(stderr, "[audio] %zu emitters\n", list.size());
    for (const tr::AudioEmitter& p : list) {
        Emitter e;
        e.props = p;
        drawDelay(e);
        e.delay = 0;                  // the constructor draws with MinDelay / MaxDelay still 0 (delay 0) and nothing redraws before the first update
        emitters.push_back(e);
    }
}
void clearEmitters() {
    for (Emitter& e : emitters) stopEmitter(e);
    emitters.clear();
}

// ---- The region's ambient sound (RegionManagerClient slot 6, TransitionAmbientSound 0x0052c920) ------------------------------------
// The request is kept at the region manager's + 0x174. No ambient for the new region (no region with enabledFlags bit 2, or its ambient
// is 0): the kept request stops - over 5000 ms (0x00bc78f0) unless `immediate`, else at once (slot 0x24) - and is let go. A different
// ambient from the previous region's: the old one stops the same way, then CreateAudioRequest(set), SetAudioParams(loop 1, no callback,
// priority 2 -> 40, seed -1.0 (0x00c2b50c)) and Play with a 5000 ms fade in. The same ambient: nothing.
Request* regionAmbient = nullptr;
int regionAmbientSet = 0;

void setRegionAmbient(int audioSet, bool immediate) {
    static const bool log = std::getenv("OPENRASA_AUDIOLOG") != nullptr;
    if (audioSet != 0 && audioSet == regionAmbientSet) return;
    if (regionAmbient) {
        if (log) std::fprintf(stderr, "[audio] region ambient: set %d stopped%s\n", regionAmbientSet, immediate ? " (immediate)" : "");
        stopRequest(regionAmbient, immediate ? 0 : 5000);
        Py_DECREF(reinterpret_cast<PyObject*>(regionAmbient));
        regionAmbient = nullptr;
    }
    regionAmbientSet = audioSet;
    if (audioSet == 0) return;
    if (!sets.count(audioSet)) {
        std::fprintf(stderr, "[audio] Couldn't get the Ambient Sound for this Region (set %d)\n", audioSet);
        return;
    }
    Request* r = reinterpret_cast<Request*>(newRequest(audioSet, ""));
    makeSetRequest(r);
    r->loop = 1;
    r->priority = 40;
    r->fadeInMs = 5000;
    regionAmbient = r;
    PyObject* empty = PyTuple_New(0);
    PyObject* result = rPlay(reinterpret_cast<PyObject*>(r), empty);
    Py_XDECREF(result);
    Py_DECREF(empty);
    if (log) std::fprintf(stderr, "[audio] region ambient: set %d (%s)%s\n", audioSet, sets[audioSet].name.c_str(), immediate ? " (immediate)" : "");
}

// The region music (TRasa::MusicManager, GameClient + 0x284; constructor FUN_00539420: state + 4, stream + 8, timer + 0xc, track + 0x10).
// RegionManagerClient slot 7 TransitionMusicTrack (0x0052cb80): when the new settings carry enabledFlags bit 3 (+ 0xb8) and the old ones
// did too (+ 0x24) with the same set (+ 0x64 == + 0xf8), nothing; else SetMusicTracks(the new set, or 0 without bit 3, immediate).
// SetMusicTracks (FUN_00539750; EAX = set): the track = set; state 1 / 3 / 5: nothing more (a fade-out under way starts the new track when
// it ends); playing (2): StopMusic - at once when immediate, then on, else a 5000 ms fade (0x00bc78f0) and return; waiting between plays
// (4): timer and state 0; then a set -> PlayMusic: state 2, else (or no set) 0. PlayMusic (FUN_005398a0): only without a stream:
// CreateAudioRequest(set), Play(-1.0) (the category's fade-in). StopMusic (FUN_00539960): with a stream - fade > 0: FadeOutAndStop(fade),
// timer 5000, state 5; else Stop (slot 0x24) and the stream let go; then state 0 unless 1. Update (FUN_00539520, the frame's ms): playing
// and the request done (+ 8 == 3): StopMusic(0), then with a track wait rand() % 120000 + 60000 ms (state 4) - the CRT's rand is 15 bits,
// so 60 - 93 s - else state 0; waiting / fading (4 / 5): no timer -> 0; the timer counts down and at <= 0 a fade's stream is let go, state
// 0, PlayTrack (FUN_00539870: the track's PlayMusic -> 2, else 0), timer 0. Client.Audio.MusicOff is the scripts' (backgroundaudiomgr.py),
// not read here (UNVERIFIED that the engine reads it anywhere).
struct MusicManager {
    int state = 0;
    Request* stream = nullptr;
    int timer = 0;
    int track = 0;
} music;
int previousMusicSet = 0;
bool previousMusicFlag = false;
uint32_t musicRandom = 1;   // the CRT's rand(): 214013 x + 2531011, bits 16 - 30 (seed not traced)

int crtRand() {
    musicRandom = musicRandom * 214013u + 2531011u;
    return int((musicRandom >> 16) & 0x7fff);
}
void releaseMusicStream() {
    if (music.stream) Py_DECREF(reinterpret_cast<PyObject*>(music.stream));
    music.stream = nullptr;
}
void stopMusic(int fadeMs) {
    if (music.stream) {
        if (fadeMs > 0) {
            stopRequest(music.stream, fadeMs);
            music.timer = 5000;
            music.state = 5;
            return;
        }
        stopRequest(music.stream, 0);
        releaseMusicStream();
    }
    if (music.state != 1) music.state = 0;
}
bool playMusic(int set) {
    static const bool log = std::getenv("OPENRASA_AUDIOLOG") != nullptr;
    if (music.stream) return false;
    if (!sets.count(set)) return false;   // CreateAudioRequest of an unknown set gives no request (UNVERIFIED: the exe's message)
    Request* r = reinterpret_cast<Request*>(newRequest(set, ""));
    makeSetRequest(r);
    music.stream = r;
    PyObject* empty = PyTuple_New(0);
    Py_XDECREF(rPlay(reinterpret_cast<PyObject*>(r), empty));
    Py_DECREF(empty);
    if (log) std::fprintf(stderr, "[audio] region music: set %d (%s)\n", set, sets[set].name.c_str());
    return true;
}
void playMusicTrack() { music.state = music.track != 0 && playMusic(music.track) ? 2 : 0; }
void setMusicTracks(int set, bool immediate) {
    music.track = set;
    switch (music.state) {
    case 1:
    case 3:
    case 5:
        return;
    case 2:
        if (!immediate) {
            stopMusic(5000);
            return;
        }
        stopMusic(0);
        break;
    case 4:
        music.timer = 0;
        music.state = 0;
        break;
    }
    playMusicTrack();
}
void updateMusic(int msElapsed) {
    switch (music.state) {
    case 2:
    case 3:
        if (!music.stream) {   // the exe asserts "No valid m_pStream ptr when state said we should be playing something"
            music.state = 0;
            return;
        }
        if (music.stream->state == 3) {
            stopMusic(0);
            if (music.track != 0) {
                music.state = 4;
                music.timer = crtRand() % 120000 + 60000;
            } else {
                music.state = 0;
            }
        }
        return;
    case 4:
    case 5:
        if (music.timer == 0) {
            music.state = 0;
            return;
        }
        music.timer -= msElapsed;
        if (music.timer <= 0) {
            if (music.state == 5) releaseMusicStream();
            music.state = 0;
            playMusicTrack();
            music.timer = 0;
        }
        return;
    }
}
void setRegionMusic(bool flag, int audioSet, bool immediate) {
    if (!(flag && previousMusicFlag && previousMusicSet == audioSet)) setMusicTracks(flag ? audioSet : 0, immediate);
    previousMusicFlag = flag;
    previousMusicSet = audioSet;
}

void shutdown() {
    if (regionAmbient) Py_DECREF(reinterpret_cast<PyObject*>(regionAmbient));
    regionAmbient = nullptr;
    regionAmbientSet = 0;
    releaseMusicStream();
    music = MusicManager{};
    clearEmitters();
    std::vector<Request*> active(activeRequests.begin(), activeRequests.end());
    for (Request* r : active) {
        destroyTracks(r);
        deactivate(r);
    }
    for (auto& [name, c] : clips)
        if (c->audio) MIX_DestroyAudio(c->audio);
    clips.clear();
    if (mixer) MIX_DestroyMixer(mixer);
    mixer = nullptr;
    if (started && enabled) MIX_Quit();
    started = false;
}

}  // namespace engine::audio
