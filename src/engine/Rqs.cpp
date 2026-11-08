// Real-time quality scaling - see Rqs.h. Traced 2026-11-01 in tabula_rasa.exe:
// - TRasa::RQSObject (base FUN_007e32a0): name (+ 4), sort key (+ 0x20, 0 for every item), enabled (+ 0x24, 1). Slot 5 (FUN_007e3310)
//   sets the flag and resets the item when cleared; slot 6 reads it.
// - TRasa::RQSFloatValue (FUN_00846850; the constructor's arguments: target, degraded, normal, seconds): rate = (normal - degraded) /
//   seconds; a step (slot 1, FUN_00846910) moves the value by rate x direction x seconds, clamped to the two; done (slot 2, FUN_00846960)
//   = at the degraded value going down, at the normal one going up; reset (slot 3) = normal; percent (slot 4) = (normal - value) /
//   (normal - degraded) x 100.
// - TRasa::RQSBoolValue (FUN_00846a30; target, normal value, seconds): a counter (+ 0x30) runs up by the seconds while degrading and
//   down while restoring, clamped to 0 .. seconds (FUN_00846ad0); at the top the flag becomes the opposite of normal, at 0 normal. Done
//   (FUN_00846bc0) = flag not normal and the counter full going down, flag normal and the counter 0 going up. Reset = normal, 0.
// - TRasa::RQSList, sequential (the root "RootRQS" is made with its parallel flag 0, FUN_007e3390): every item (+ 0x30, in the order
//   added), the items still in play (+ 0x3c) and the degraded ones (+ 0x48, a stack). Adding (FUN_007e35d0) appends to both lists and
//   sorts the in-play list (FUN_007e4140, a stable merge / insertion sort FUN_007e4850) by the node value - the item's ADDRESS. The
//   items are allocated one after another in the constructor, so the order taken here is the order added (UNVERIFIED: the heap's
//   placement decides; the in-game "Flora Density II wins the reset" behaviour - see the notes - fits this order).
//   A step (FUN_007e3650): degrading works on the last in-play item - stepped while it is enabled and not done, otherwise (done or
//   disabled) moved onto the degraded stack in the same call; restoring steps the last in-play item up while it is enabled and not
//   done, otherwise takes the top of the degraded stack back to the end of the in-play list. Reset (slot 3, FUN_007e3820): every
//   in-play item, then every degraded one (bottom first) reset and appended to the in-play list. Percent (FUN_007e38f0): degraded
//   count / all x 100 + the last in-play item's percent / all.
// - TRasa::TRRQS (vtable 0x00c089b4): threshold + 0x50 (20 from the constructor, then Client.Graphics.RealTimeQualityScaling, default
//   15). The update (FUN_00847790): render bit 12 clear - the first call starts a timer (+ 0x54 set), each call takes its elapsed whole
//   milliseconds x 0.001 (timTimer::GetElapsed, then restarted) and FUN_007e3b30: fps below the threshold -> a step down; fps at least
//   threshold + min(fps, 5) -> a step up (5.0, 0x00c2b7a8); between, nothing. Bit 12 set while running: reset (slot 3), stopped.
#include "Rqs.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace engine::rqs {
namespace {

Values vals;

struct Item {
    std::string name;
    bool isBool = false;
    float* value = nullptr;      // float items
    bool* flag = nullptr;        // bool items
    float degraded = 0.0f, normal = 0.0f, seconds = 1.0f;
    float low = 0.0f, high = 0.0f, rate = 0.0f;
    bool normalFlag = true;
    float counter = 0.0f;
    bool enabled = true;

    void reset() {
        if (isBool) {
            *flag = normalFlag;
            counter = 0.0f;
        } else {
            *value = normal;
        }
    }
    bool done(int direction) const {
        if (isBool) {
            if (direction == 1) return *flag == normalFlag && counter == 0.0f;
            if (direction == -1) return *flag != normalFlag && counter == seconds;
            return false;
        }
        if (direction == -1) return *value == degraded;
        if (direction == 1) return *value == normal;
        return true;
    }
    bool step(int direction, float dt) {
        if (isBool) {
            if (direction == 1) {
                counter = std::min(std::max(counter - dt, 0.0f), seconds);
                if (counter == 0.0f) *flag = normalFlag;
            } else if (direction == -1) {
                counter = std::min(std::max(counter + dt, 0.0f), seconds);
                if (counter == seconds) *flag = !normalFlag;
            }
            return done(direction);
        }
        *value = std::min(std::max(rate * float(direction) * dt + *value, low), high);
        return done(direction);
    }
    float percent() const {
        if (isBool) return seconds == 0.0f ? 0.0f : counter / seconds * 100.0f;
        return (normal - *value) / (normal - degraded) * 100.0f;
    }
};

struct Root {
    std::vector<Item> items;          // + 0x30, in the order added
    std::vector<size_t> inPlay;       // + 0x3c
    std::vector<size_t> degradedStack;  // + 0x48
    float threshold = 20.0f;          // + 0x50
    bool running = false;             // + 0x54
    uint64_t timerStart = 0;
    bool fullQuality = false;         // OpenRasa: the full-quality values are applied (see update)

    // OpenRasa deviation (user, 2026-11-01): several items share a target (Flora Density I / II, Sfx Relevance Threshhold I / II,
    // Animation Degradation I / II), and retail's reset runs the items in order, so the LATER item's normal value wins - flora 0.5,
    // SFX threshold 14, animation interval 0.005 - although the earlier item's normal is the full-quality one (1.0, 4, 0.0001). With
    // RQS off nothing ever steps, so retail stays at that half quality for good. Resetting the items last to first leaves each target
    // at its first item's normal value.
    void applyFullQuality() {
        for (size_t i = items.size(); i-- > 0;) items[i].reset();
    }

    void addFloat(const char* name, float* target, float degraded, float normal, float seconds) {
        Item it;
        it.name = name;
        it.value = target;
        it.degraded = degraded;
        it.normal = normal;
        it.seconds = seconds;
        it.rate = (normal - degraded) / seconds;
        it.high = std::max(degraded, normal);
        it.low = std::min(degraded, normal);
        items.push_back(it);
        inPlay.push_back(items.size() - 1);
    }
    void addBool(const char* name, bool* target, bool normal, float seconds) {
        Item it;
        it.name = name;
        it.isBool = true;
        it.flag = target;
        it.normalFlag = normal;
        it.seconds = seconds;
        items.push_back(it);
        inPlay.push_back(items.size() - 1);
    }
    void reset() {
        for (size_t i : inPlay) items[i].reset();
        for (size_t i : degradedStack) {
            items[i].reset();
            inPlay.push_back(i);
        }
        degradedStack.clear();
    }
    void step(int direction, float dt) {
        if (direction == -1) {
            if (inPlay.empty()) return;
            Item& last = items[inPlay.back()];
            if (last.enabled && !last.step(-1, dt)) return;
            degradedStack.push_back(inPlay.back());
            inPlay.pop_back();
        } else if (direction == 1) {
            if (!inPlay.empty()) {
                Item& last = items[inPlay.back()];
                if (last.enabled && !last.step(1, dt)) return;
            }
            if (degradedStack.empty()) return;
            inPlay.push_back(degradedStack.back());
            degradedStack.pop_back();
        }
    }
    float percent() const {
        if (items.empty()) return 0.0f;
        const float all = float(items.size());
        float p = float(degradedStack.size()) / all * 100.0f;
        if (!inPlay.empty()) p += items[inPlay.back()].percent() / all;
        return p;
    }
};

Root& root() {
    static Root r = [] {
        Root r;
        r.items.reserve(21);
        // FUN_00846c90, in the constructor's order (degraded, normal, seconds; bools: normal on, 2 s).
        r.addFloat("Animation Degradation I", &vals.animationDegradation, 0.005f, 0.0001f, 2.0f);
        r.addBool("Do AntiAliasing", &vals.antiAlias, true, 2.0f);
        r.addFloat("Flora Density I", &vals.flora, 0.5f, 1.0f, 1.0f);
        r.addFloat("Light ClipDistance", &vals.lightClipDistance, 0.0f, 1.0f, 1.0f);
        r.addFloat("Relevance Range", &vals.relevanceRange, 25.0f, 50.0f, 1.0f);
        r.addBool("Do Refractions", &vals.refractions, true, 2.0f);
        r.addFloat("DrawAttachedScalar", &vals.drawAttachedScalar, 2.0f, 1.0f, 1.0f);
        r.addFloat("Animation Degradation II", &vals.animationDegradation, 0.01f, 0.005f, 2.0f);
        r.addFloat("Sfx Relevance Threshhold I", &vals.sfxRelevanceThreshold, 14.0f, 4.0f, 1.0f);
        r.addFloat("Particle Degradation", &vals.particleDegradation, 0.01f, 0.005f, 2.0f);
        r.addFloat("Far CullLayer Scalar", &vals.farCullLayerScalar, 0.0f, 1.0f, 5.0f);
        r.addBool("Shadow Distance", &vals.shadowDistance, true, 2.0f);
        r.addBool("Dynamic Shadows", &vals.dynamicShadows, true, 2.0f);
        r.addBool("Static Shadows", &vals.staticShadows, true, 2.0f);
        r.addFloat("Flora Density II", &vals.flora, 0.0f, 0.5f, 1.0f);
        r.addFloat("Sfx Relevance Threshhold II", &vals.sfxRelevanceThreshold, 30.0f, 14.0f, 1.0f);
        r.addBool("Specular Lighting", &vals.specular, true, 2.0f);
        r.addFloat("Legacy Light Per Type", &vals.legacyLightPerType, 0.0f, 0.5f, 1.0f);
        r.addBool("Fake Shadows", &vals.fakeShadows, true, 2.0f);
        r.addFloat("View Distance Scalar", &vals.viewDistanceScalar, 0.0f, 1.0f, 5.0f);
        r.addFloat("Relevance Quality Scale", &vals.relevanceQualityScale, 0.5f, 1.0f, 2.0f);
        r.reset();   // FUN_007e3820 at the constructor's end
        return r;
    }();
    return r;
}

}  // namespace

const Values& values() {
    root();
    return vals;
}

void update(float fps, bool disabled) {
    Root& r = root();
    // RQS off - the threshold at 0 (it can never degrade: fps < 0 never holds) or render bit 12: retail resets once (bit 12) or
    // never steps (threshold 0) and keeps the reset's half-quality values; OpenRasa applies the full-quality ones instead (see
    // applyFullQuality). Turned back on, the retail reset puts the items as retail has them before stepping resumes.
    if (disabled || r.threshold <= 0.0f) {
        if (r.running || !r.fullQuality) {
            r.reset();
            r.applyFullQuality();
            r.fullQuality = true;
        }
        r.running = false;
        return;
    }
    if (r.fullQuality) {
        r.reset();
        r.fullQuality = false;
    }
    const uint64_t now = SDL_GetPerformanceCounter();
    if (!r.running) {
        r.timerStart = now;
        r.running = true;
    }
    const uint64_t ms = (now - r.timerStart) * 1000 / SDL_GetPerformanceFrequency();
    r.timerStart = now;
    const float dt = float(ms) * 0.001f;
    if (fps < r.threshold) {
        r.step(-1, dt);
        return;
    }
    const float margin = fps <= 5.0f ? fps : 5.0f;
    if (r.threshold + margin <= fps) r.step(1, dt);
}

void setThreshold(float fps) { root().threshold = fps; }

const std::vector<std::string>& itemNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const Item& i : root().items) out.push_back(i.name);
        return out;
    }();
    return names;
}

bool enabled(const std::string& name) {
    for (const Item& i : root().items)
        if (i.name == name) return i.enabled;
    return false;
}

void enable(const std::string& name, bool on) {
    Root& r = root();
    for (Item& i : r.items)
        if (i.name == name) {
            i.enabled = on;
            if (!on) i.reset();
            break;
        }
    if (on) r.reset();
    r.fullQuality = false;   // the next update applies the full-quality values again while RQS is off
}

float percent() { return root().percent(); }

}  // namespace engine::rqs
