// UiRender.cpp - see UiRender.h.
#include "UiRender.h"

#include "../scene/Renderer.h"
#include "EntityBody.h"

#include "UiCanvas.h"
#include "UiManager.h"

#include "Assets.h"
#include "../scene/SceneWidget.h"
#include "MoviePlayer.h"

#include "assetlibrary.h"

#include <SDL3/SDL_timer.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <memory>
#include <string>

namespace engine::ui {
namespace {

std::string utf8(const std::u16string& t) {
    std::string out;
    for (size_t i = 0; i < t.size(); ++i) {
        uint32_t c = t[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < t.size() && t[i + 1] >= 0xDC00 && t[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (t[++i] - 0xDC00);
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | c >> 6), out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | c >> 12), out += char(0x80 | (c >> 6 & 0x3F)), out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | c >> 18), out += char(0x80 | (c >> 12 & 0x3F)), out += char(0x80 | (c >> 6 & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

void setAttribute(tr::XmlNode& n, const char* name, const std::string& value) {
    for (auto& [k, v] : n.attributes)
        if (k == name) {
            v = value;
            return;
        }
    n.attributes.emplace_back(name, value);
}

// The widget as an export element: its own element's attributes with the live state written over them.
void snapshot(const Widget* w, tr::XmlNode& n, bool recurse = true) {
    if (w->source) {
        n.tag = w->source->tag;
        n.attributes = w->source->attributes;
    } else {
        n.tag = w->tag;
    }
    auto num = [](int v) { return std::to_string(v); };
    if (!w->uiid.empty()) setAttribute(n, "uiid", w->uiid);
    setAttribute(n, "visible", w->visible ? "true" : "false");
    // A composite's SetOffset (+0xb4 / +0xb8) moves its children's drawing: the render (FUN_008aee70) adds trunc(offset x the screen
    // scale) to the render context's translation before drawing them (its layout rectangles stay put). Folded into the child's place.
    const int scrollX = w->parent ? w->parent->scrollX : 0, scrollY = w->parent ? w->parent->scrollY : 0;
    if (w->layoutParent && w->parent) {
        // SetParent: drawn in its old parent's list (the composite render walks its children) at the place its layout gives it - by the
        // new parent (FUN_008b6fb0 reads + 0x48) -, i.e. at that rectangle relative to the old parent's top left.
        const Rect r = manager().screenRect(w), p = manager().screenRect(w->parent);
        setAttribute(n, "anchorOffsetX", num(r.left - p.left + scrollX));
        setAttribute(n, "anchorOffsetY", num(r.top - p.top + scrollY));
        setAttribute(n, "sizeW", num(r.right - r.left));
        setAttribute(n, "sizeH", num(r.bottom - r.top));
        setAttribute(n, "childAnchor", "0");
        setAttribute(n, "parentAnchor", "0");
        setAttribute(n, "resizeToParent", "false");
    } else {
        setAttribute(n, "anchorOffsetX", num(w->offsetX + scrollX));
        setAttribute(n, "anchorOffsetY", num(w->offsetY + scrollY));
        setAttribute(n, "sizeW", num(w->width));
        setAttribute(n, "sizeH", num(w->height));
        setAttribute(n, "childAnchor", num(w->childAnchor));
        setAttribute(n, "parentAnchor", num(w->parentAnchor));
        setAttribute(n, "resizeToParent", w->resizeToParent ? "true" : "false");
    }
    setAttribute(n, "enabled", w->enabled ? "true" : "false");
    setAttribute(n, "colorR", num(w->r));
    setAttribute(n, "colorG", num(w->g));
    setAttribute(n, "colorB", num(w->b));
    setAttribute(n, "colorA", num(w->a));
    if (!w->textureDef.empty()) setAttribute(n, "textureDef", w->textureDef);
    if (w->kind == Kind::Bitmap) {
        setAttribute(n, "textureFile", w->textureFile);
        if (!w->textureFile.empty()) {
            setAttribute(n, "textureRegionLeft", num(w->regionLeft));
            setAttribute(n, "textureRegionTop", num(w->regionTop));
            setAttribute(n, "textureRegionRight", num(w->regionRight));
            setAttribute(n, "textureRegionBottom", num(w->regionBottom));
            setAttribute(n, "textureTile", num(w->tileFlags));
        }
    }
    if (w->radarMasked) {
        setAttribute(n, "radarMask", radarMaskTexture());
        setAttribute(n, "radarMaskX", num(w->radarMaskX));
        setAttribute(n, "radarMaskY", num(w->radarMaskY));
    }
    if (!w->fontId.empty()) setAttribute(n, "fontId", w->fontId);
    if (!w->effect.empty()) setAttribute(n, "effect", w->effect);
    if (w->playerTime >= 0) setAttribute(n, w->playerBitmap ? "bitmapAnimName" : "widgetAnimName", w->playerAnim);
    if (w->kind == Kind::Text || w->kind == Kind::RichText) setAttribute(n, "wrapText", w->wrapText ? "true" : "false");
    if (w->kind == Kind::ProgressBar) setAttribute(n, "percentComplete", std::to_string(w->percentComplete));
    if (w->kind == Kind::ScrollBar || w->kind == Kind::Slider || w->kind == Kind::Spinner) {
        setAttribute(n, "minRange", num(w->rangeMin));
        setAttribute(n, "maxRange", num(w->rangeMax));
        setAttribute(n, "pageSize", num(w->pageSize));
        setAttribute(n, "currentValue", num(w->value));
    }
    if (!recurse) return;
    n.children.clear();
    if (!w->visible) return;
    n.children.resize(w->children.size());
    for (size_t i = 0; i < w->children.size(); ++i) snapshot(w->children[i].get(), n.children[i]);
}

// The live state UiCanvas draws besides the attributes, by element (collected after the tree is complete: its nodes no longer
// move): texts, edit boxes' typed text, the button the mouse is over / pushed, toggled buttons, the focused edit box and its caret.
struct LiveState {
    std::map<const tr::XmlNode*, std::string> texts, editTexts;
    const tr::XmlNode* hovered = nullptr;
    const tr::XmlNode* pushed = nullptr;
    std::set<const tr::XmlNode*> toggled;
    const tr::XmlNode* focused = nullptr;
    size_t caret = 0, selStart = 0, selEnd = 0;
    std::map<const Widget*, const tr::XmlNode*> editNodes;   // each edit box's element in the snapshot (editIndexAt)
    std::map<const tr::XmlNode*, float> widgetAnimTimes, bitmapAnimTimes;
    std::map<const tr::XmlNode*, Widget*> sceneViews;   // SceneView elements and their widgets (renderScenes)
    std::map<const tr::XmlNode*, Widget*> movies;       // Movie elements and their widgets (renderScenes)
    std::vector<std::pair<const tr::XmlNode*, const Widget*>> animated;   // elements with an animation clock (refreshAnimTimes)
    std::string signature;               // the same as text, for the redraw test
    std::map<const tr::XmlNode*, std::vector<const tr::XmlNode*>> window;   // each element's cached ancestors (depths 2 .. maxDepth)
    // The animated elements' last sampled animation frames: the default widget animation's, the default bitmap animation's and a
    // scripted player's (-1 none).
    std::map<const tr::XmlNode*, std::array<long, 3>> sampled;
};

// A fingerprint of everything snapshot() and collect() read from the widgets, other than the animations' clocks: when it is the same
// as the last snapshot's, that snapshot is still right and only its animation times need refreshing. Keep it in step with them.
struct Fingerprint {
    uint64_t h = 1469598103934665603ull;
    void add(uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    }
    // Strings eight bytes a step (each step is a bijection of the hash for a given word, so a change in any one word always shows).
    void addBytes(const void* data, size_t n) {
        add(n);
        const char* p = static_cast<const char*>(data);
        for (; n >= 8; p += 8, n -= 8) {
            uint64_t v;
            std::memcpy(&v, p, 8);
            add(v);
        }
        if (n) {
            uint64_t v = 0;
            std::memcpy(&v, p, n);
            add(v);
        }
    }
    void add(const std::string& s) { addBytes(s.data(), s.size()); }
    void add(const std::u16string& s) { addBytes(s.data(), s.size() * sizeof(char16_t)); }
};

// The fingerprint walk also keeps each widget's own hash (everything but its children), in the order snapshot() visits them, and a
// hash of the tree's shape (widgets, visibility, child counts): with the shape unchanged, a changed snapshot is updated node by node.
struct FingerprintWalk {
    Fingerprint structure;
    std::vector<uint64_t> owns;
};

void ownFingerprint(const Widget* w, Fingerprint& f) {
    f.add(uint64_t(reinterpret_cast<uintptr_t>(w)));
    f.add(uint64_t(reinterpret_cast<uintptr_t>(w->source)));
    f.add(w->tag);
    f.add(w->uiid);
    f.add(uint64_t(w->kind));
    f.add(w->visible);
    const int scrollX = w->parent ? w->parent->scrollX : 0, scrollY = w->parent ? w->parent->scrollY : 0;
    f.add(uint64_t(int64_t(scrollX)));
    f.add(uint64_t(int64_t(scrollY)));
    if (w->layoutParent && w->parent) {
        const Rect r = manager().screenRect(w), p = manager().screenRect(w->parent);
        for (const int v : {r.left, r.top, r.right, r.bottom, p.left, p.top}) f.add(uint64_t(int64_t(v)));
    } else {
        for (const int v : {w->offsetX, w->offsetY, w->width, w->height, w->childAnchor, w->parentAnchor}) f.add(uint64_t(int64_t(v)));
        f.add(w->resizeToParent);
    }
    f.add(w->enabled);
    for (const int v : {int(w->r), int(w->g), int(w->b), int(w->a)}) f.add(uint64_t(int64_t(v)));
    f.add(w->textureDef);
    if (w->kind == Kind::Bitmap) {
        f.add(w->textureFile);
        for (const int v : {w->regionLeft, w->regionTop, w->regionRight, w->regionBottom, int(w->tileFlags)}) f.add(uint64_t(int64_t(v)));
    }
    f.add(w->radarMasked);
    if (w->radarMasked) {
        f.add(radarMaskTexture());
        f.add(uint64_t(int64_t(w->radarMaskX)));
        f.add(uint64_t(int64_t(w->radarMaskY)));
    }
    f.add(w->fontId);
    f.add(w->effect);
    const bool player = w->playerTime >= 0;
    f.add(player);
    if (player) {
        f.add(w->playerAnim);
        f.add(w->playerBitmap);
    }
    f.add(w->wrapText);
    uint32_t percentBits = 0;
    std::memcpy(&percentBits, &w->percentComplete, sizeof percentBits);
    f.add(percentBits);
    for (const int v : {w->rangeMin, w->rangeMax, w->pageSize, w->value}) f.add(uint64_t(int64_t(v)));
    // collect()
    if (w->kind == Kind::Text || w->kind == Kind::RichText) f.add(displayText(const_cast<Widget*>(w)));
    if (w->kind == Kind::EditBox || w->kind == Kind::PasswordBox || w->kind == Kind::MultiLineEditBox) {
        f.add(w->text);
        const bool focused = manager().focus() == w;
        f.add(focused);
        if (focused) {
            f.add(w->caret);
            f.add(w->selAnchor);
            f.add(w->selEnd);
        }
    }
    f.add(w->widgetAnimName);
    f.add(w->bitmapAnimName);
    f.add(w->highlighted);
    f.add(w->pushed);
    f.add(w->toggled);
    f.add(w->children.size());
}

void fingerprint(const Widget* w, Fingerprint& f, FingerprintWalk& walk) {
    Fingerprint own;
    ownFingerprint(w, own);
    f.add(own.h);
    walk.owns.push_back(own.h);
    walk.structure.add(uint64_t(reinterpret_cast<uintptr_t>(w)));
    walk.structure.add(w->visible);
    walk.structure.add(w->children.size());
    if (!w->visible) return;   // snapshot() leaves a hidden widget's children out
    for (const auto& c : w->children) fingerprint(c.get(), f, walk);
}

// The snapshot's (widget, element) pairs in the fingerprint walk's order.
void pairUp(const Widget* w, tr::XmlNode& n, std::vector<std::pair<const Widget*, tr::XmlNode*>>& out) {
    out.emplace_back(w, &n);
    if (!w->visible) return;
    for (size_t i = 0; i < n.children.size() && i < w->children.size(); ++i) pairUp(w->children[i].get(), n.children[i], out);
}

bool isAnimated(const Widget* w) { return !w->widgetAnimName.empty() || !w->bitmapAnimName.empty() || w->playerTime >= 0; }

// The animation times of the last snapshot's elements, read again from their widgets; returns what of them the drawing depends on.
// changed: the elements whose drawn animation frame moved since the last call - default animations (widgetAnimName / bitmapAnimName)
// as well as scripted players; their cached subtrees are drawn again (a widget animation's scale, translation and colour reach every
// element under it, so the caller invalidates the whole subtree).
std::string refreshAnimTimes(LiveState& live, const UiCanvas& canvas, std::vector<const tr::XmlNode*>* changed = nullptr) {
    std::string out;
    for (const auto& [n, w] : live.animated) {
        std::array<long, 3> frame{-1, -1, -1};
        if (!w->widgetAnimName.empty()) {
            const float t = float(manager().clockMs - w->widgetAnimStart);
            live.widgetAnimTimes[n] = t;
            if (w->visible) frame[0] = canvas.sampledAnimationTime(w->widgetAnimName, false, t);
        }
        if (!w->bitmapAnimName.empty()) {
            const float t = float(manager().clockMs - w->bitmapAnimStart);
            live.bitmapAnimTimes[n] = t;
            if (w->visible) frame[1] = canvas.sampledAnimationTime(w->bitmapAnimName, true, t);
        }
        if (w->playerTime >= 0 && w->visible) {
            (w->playerBitmap ? live.bitmapAnimTimes : live.widgetAnimTimes)[n] = float(w->playerTime);
            frame[2] = canvas.sampledAnimationTime(w->playerAnim, w->playerBitmap, w->playerTime);
            out += std::to_string(frame[2]);
            out += ' ';
        }
        if (changed) {
            const auto last = live.sampled.find(n);
            if (last == live.sampled.end() || last->second != frame) changed->push_back(n);
            live.sampled[n] = frame;
        }
    }
    return out;
}

// An element changed in a way that reaches every element under it: its cached ancestors and every cached subtree inside it are drawn
// again.
void invalidateCached(const LiveState& live, UiCanvas::Options::QuadCache& cache, const tr::XmlNode& n) {
    if (const auto chain = live.window.find(&n); chain != live.window.end())
        for (const tr::XmlNode* a : chain->second) cache.entries[a].valid = false;
    std::vector<const tr::XmlNode*> stack{&n};
    while (!stack.empty()) {
        const tr::XmlNode* d = stack.back();
        stack.pop_back();
        if (const auto e = cache.entries.find(d); e != cache.entries.end()) e->second.valid = false;
        for (const tr::XmlNode& c : d->children) stack.push_back(&c);
    }
}

// The depth-2 ancestors (the root windows' children) of every element: the subtrees UiCanvas caches.
void mapWindows(const tr::XmlNode& n, std::vector<const tr::XmlNode*>& chain, size_t depth, size_t minDepth, size_t maxDepth,
                std::map<const tr::XmlNode*, std::vector<const tr::XmlNode*>>& out) {
    const bool cached = depth >= minDepth && depth <= maxDepth;
    if (cached) chain.push_back(&n);
    if (!chain.empty()) out[&n] = chain;
    for (const tr::XmlNode& c : n.children) mapWindows(c, chain, depth + 1, minDepth, maxDepth, out);
    if (cached) chain.pop_back();
}

void collect(const Widget* w, const tr::XmlNode& n, LiveState& live, const UiCanvas& canvas) {
    if (w->kind == Kind::Text || w->kind == Kind::RichText) live.texts[&n] = utf8(displayText(const_cast<Widget*>(w)));
    if (w->kind == Kind::EditBox || w->kind == Kind::PasswordBox || w->kind == Kind::MultiLineEditBox) {
        live.editTexts[&n] = utf8(w->text);
        live.editNodes[w] = &n;
        if (manager().focus() == w) {
            live.focused = &n;
            live.caret = std::min(w->caret, w->text.size());
            live.selStart = std::min(w->selAnchor, w->text.size());
            live.selEnd = std::min(w->selEnd, w->text.size());
            live.signature += "focus " + w->uiid + " " + std::to_string(live.caret) + " " + std::to_string(live.selStart) + "-" +
                              std::to_string(live.selEnd) + " " + live.editTexts[&n] + "\n";
        }
    }
    if (w->kind == Kind::SceneView) live.sceneViews[&n] = const_cast<Widget*>(w);
    if (w->kind == Kind::Movie) live.movies[&n] = const_cast<Widget*>(w);
    if (!w->widgetAnimName.empty() || !w->bitmapAnimName.empty() || w->playerTime >= 0) live.animated.emplace_back(&n, w);
    if (!w->widgetAnimName.empty()) live.widgetAnimTimes[&n] = float(manager().clockMs - w->widgetAnimStart);
    if (!w->bitmapAnimName.empty()) live.bitmapAnimTimes[&n] = float(manager().clockMs - w->bitmapAnimStart);
    if (w->playerTime >= 0 && w->visible) {   // a hidden widget's player changes nothing drawn
        (w->playerBitmap ? live.bitmapAnimTimes : live.widgetAnimTimes)[&n] = float(w->playerTime);
        // A finished stall animation's time keeps growing while its frame stays the same: sign the time it is sampled at.
        live.signature += "player " + w->uiid + " " + w->playerAnim + " " +
                          std::to_string(canvas.sampledAnimationTime(w->playerAnim, w->playerBitmap, w->playerTime)) + "\n";
    }
    if (w->highlighted) live.hovered = &n, live.signature += "hover " + w->uiid + "\n";
    if (w->pushed) live.hovered = live.pushed = &n, live.signature += "push " + w->uiid + "\n";
    if (w->toggled) live.toggled.insert(&n), live.signature += "on " + w->uiid + "\n";
    for (size_t i = 0; i < n.children.size() && i < w->children.size(); ++i) collect(w->children[i].get(), n.children[i], live, canvas);
}

// Everything the drawing depends on, as text: tags, attributes and texts in tree order.
void sign(const tr::XmlNode& n, const std::map<const tr::XmlNode*, std::string>& texts, std::string& out) {
    out += n.tag;
    out += '\x02';
    for (const auto& [k, v] : n.attributes) out += k, out += '=', out += v, out += '\x03';
    if (const auto t = texts.find(&n); t != texts.end()) out += t->second;
    out += '\x04';
    for (const tr::XmlNode& c : n.children) sign(c, texts, out);
    out += '\x05';
}

// OPENRASA_UITIMING=1: per frame, the snapshot's and the drawing's milliseconds.
size_t cacheHits = 0, cacheMisses = 0;   // the quad cache's last render (timing)
void timing(uint64_t start, uint64_t built, uint64_t end, int drawn) {
    static const bool on = std::getenv("OPENRASA_UITIMING") != nullptr;
    if (!on) return;
    const double f = 1000.0 / double(SDL_GetPerformanceFrequency());
    std::fprintf(stderr, "[ui timing] snapshot %.2f ms, %s %.2f ms (cache hits %zu misses %zu)\n", (built - start) * f,
                 drawn ? "drawn" : "unchanged", (end - built) * f, cacheHits, cacheMisses);
}

struct State {
    UiCanvas::Options::QuadCache quadCache;   // the windows' quads of the last snapshot (see Options::quadCache)
    tr::UiExport snapshot;              // the definitions (copied once) and the last snapshot's widget tree
    LiveState live;                     // its live state (pointers into snapshot.widgets)
    UiCanvas canvas;
    bool ready = false;
    std::string signature;              // the last drawn snapshot (sign)
    uint64_t fingerprint = 0;           // the widgets' fingerprint when the snapshot was taken (0 none)
    uint64_t structure = 0;             // its shape (FingerprintWalk::structure)
    std::vector<uint64_t> owns;         // each element's own hash at the snapshot (FingerprintWalk::owns)
    std::vector<std::pair<const Widget*, tr::XmlNode*>> pairs;   // the snapshot's elements in that order
    std::vector<bool> animatedPairs;    // whether each had an animation clock (collect: LiveState::animated)
    std::string animSig;                // the last drawn frame's animation state (refreshAnimTimes, the canvas clock)
    int width = 0, height = 0;          // its size
};

State& state() {
    static State s;
    return s;
}

UiCanvas::Options optionsFor(State& s, int width, int height) {
    UiCanvas::Options options;
    options.screenWidth = width;
    options.screenHeight = height;
    options.mouseHighlight = true;
    options.mouseX = float(manager().mouseX);
    options.mouseY = float(manager().mouseY);
    options.texts = &s.live.texts;
    options.interactiveButtons = true;
    options.hoveredButton = s.live.hovered;
    options.pushedButton = s.live.pushed;
    options.toggledButtons = &s.live.toggled;
    options.editTexts = &s.live.editTexts;
    options.focusedEdit = s.live.focused;
    options.caretIndex = s.live.caret;
    options.selectionStart = s.live.selStart;
    options.selectionEnd = s.live.selEnd;
    options.widgetAnimTimes = &s.live.widgetAnimTimes;
    options.bitmapAnimTimes = &s.live.bitmapAnimTimes;
    options.scriptedWidening = true;
    return options;
}

}  // namespace

RichLayout richTextLayout(Widget* w, RichMeasure* measureOut) {
    State& s = state();
    if (!s.ready) {
        s.ready = true;
        s.snapshot = manager().definitions();
        s.snapshot.widgets.children.clear();
        s.canvas.setSources(&s.snapshot, &assets(), nullptr, ".");
    }
    // The screen scale (UIManager + 0x84 / + 0x88, FUN_00897190): the drawn size over the root's logical size.
    float sx = 1, sy = 1;
    if (const Widget* root = manager().root(); root && s.width > 0 && s.height > 0 && root->width > 0 && root->height > 0) {
        sx = float(s.width) / float(root->width);
        sy = float(s.height) / float(root->height);
    }
    RichMeasure measure = s.canvas.richMeasure(sy);
    const uint32_t own = uint32_t(w->a) << 24 | uint32_t(w->r) << 16 | uint32_t(w->g) << 8 | uint32_t(w->b);
    RichLayout layout = layoutRichText(displayText(w), w->fontId, own, w->width, sx, measure);
    if (measureOut) *measureOut = std::move(measure);
    return layout;
}

bool editHighlightRect(const Widget* w, int& left, int& top, int& right, int& bottom) {
    State& s = state();
    const auto it = s.live.editNodes.find(w);
    int32_t x = 0, y = 0, width = 0, height = 0;
    if (it == s.live.editNodes.end() || !s.canvas.editHighlight(it->second, x, y, width, height)) return false;
    const Rect box = manager().screenRect(w);
    left = box.left + x;
    top = box.top + y;
    right = left + width;
    bottom = top + height;
    return true;
}

bool editIndexAt(const Widget* w, int logicalX, size_t& index) {
    State& s = state();
    const auto it = s.live.editNodes.find(w);
    if (it == s.live.editNodes.end()) return false;
    index = std::min(s.canvas.editIndexAt(it->second, logicalX), w->text.size());
    return true;
}

std::vector<std::pair<int, int>> wrapTextLines(const Widget* w, const std::u16string& text) {
    State& s = state();
    if (!s.ready) {
        s.ready = true;
        s.snapshot = manager().definitions();
        s.snapshot.widgets.children.clear();
        s.canvas.setSources(&s.snapshot, &assets(), nullptr, ".");
    }
    float sx = 1, sy = 1;
    if (const Widget* root = manager().root(); root && s.width > 0 && s.height > 0 && root->width > 0 && root->height > 0) {
        sx = float(s.width) / float(root->width);
        sy = float(s.height) / float(root->height);
    }
    const Rect r = manager().screenRect(w);
    // FUN_008f9860: the width in screen pixels = ROUND(width x UIManager + 0x84) (x87 round to nearest).
    return s.canvas.wrapLines(w->fontId, text, int(std::lround(float(r.right - r.left) * sx)), sy);
}

// UIText::GetTextHeight / ResizeToTextHeight (2007 0x0088d240): the laid-out lines (SetText's: FUN_008f9860 when wrapping, else
// FUN_008f9c90 - split at line ends) x FontUtil::GetLineHeight = the multi-font's tallest tmHeight over ALL its ranges (gfxMultiFont::
// AddFont) x logical / screen. Fonts such as "Chat" carry MS UI Gothic ranges taller than their Conduit range: the NPC bark bubble
// (OverheadChat, sized to this height) has a margin above and below the text in retail.
int textLayoutHeight(const Widget* w, const std::u16string& text) {
    if (text.empty()) return 0;
    State& s = state();
    if (!s.ready) {
        s.ready = true;
        s.snapshot = manager().definitions();
        s.snapshot.widgets.children.clear();
        s.canvas.setSources(&s.snapshot, &assets(), nullptr, ".");
    }
    float sy = 1;
    if (const Widget* root = manager().root(); root && s.height > 0 && root->height > 0) sy = float(s.height) / float(root->height);
    size_t lines = 0;
    if (w->wrapText) {
        lines = wrapTextLines(w, text).size();
    } else {
        lines = 1;
        for (size_t i = 0; i < text.size(); ++i)
            if (text[i] == 10 || (text[i] == 13 && !(i + 1 < text.size() && text[i + 1] == 10))) ++lines;   // LF; CR not followed by LF
    }
    return int(lines) * s.canvas.richMeasure(sy).lineHeight(w->fontId);
}

float glyphAdvanceLogical(const std::string& fontId, char16_t c) {
    State& s = state();
    if (!s.ready) {
        s.ready = true;
        s.snapshot = manager().definitions();
        s.snapshot.widgets.children.clear();
        s.canvas.setSources(&s.snapshot, &assets(), nullptr, ".");
    }
    float sx = 1, sy = 1;
    if (const Widget* root = manager().root(); root && s.width > 0 && s.height > 0 && root->width > 0 && root->height > 0) {
        sx = float(s.width) / float(root->width);
        sy = float(s.height) / float(root->height);
    }
    return float(s.canvas.glyphAdvance(fontId, c, sy)) / sx;
}

float uiScaleX() {
    State& s = state();
    const Widget* root = manager().root();
    return root && s.width > 0 && root->width > 0 ? float(s.width) / float(root->width) : 1.0f;
}

// OPENRASA_NOQUADCACHE=1 (diagnostics): every frame draws every window's quads.
static bool quadCacheOn() {
    static const bool off = std::getenv("OPENRASA_NOQUADCACHE") != nullptr;
    return !off;
}

bool renderUi(int pixelWidth, int pixelHeight, std::vector<UiCanvas::Quad>& quads) {
    State& s = state();
    Widget* root = manager().root();
    if (!root) {
        quads.clear();
        return true;
    }
    if (!s.ready) {
        s.ready = true;
        s.snapshot = manager().definitions();
        s.snapshot.widgets.children.clear();
        s.canvas.setSources(&s.snapshot, &assets(), nullptr, ".");
    }
    // While an animation is moving every frame is drawn; otherwise only changed snapshots.
    if (s.canvas.animating()) manager().dirty = true;
    if (!manager().dirty && pixelWidth == s.width && pixelHeight == s.height) return false;
    manager().dirty = false;
    const uint64_t start = SDL_GetPerformanceCounter();
    // The widgets as they were at the last snapshot (only the animations' clocks moved): that snapshot is kept, its animation times
    // read again, and the frame drawn only when they draw something else.
    Fingerprint print;
    print.add(uint64_t(int64_t(manager().mouseX)));
    print.add(uint64_t(int64_t(manager().mouseY)));
    print.add(uint64_t(int64_t(root->width)));
    print.add(uint64_t(int64_t(root->height)));
    FingerprintWalk walk;
    walk.owns.reserve(s.owns.size());
    walk.structure.add(root->children.size());
    for (const auto& c : root->children) fingerprint(c.get(), print, walk);
    const auto animState = [&] {
        std::string a = refreshAnimTimes(s.live, s.canvas);
        if (s.canvas.animating()) a += "animating " + std::to_string(manager().clockMs);
        return a;
    };
    if (!manager().displayStats && s.fingerprint != 0 && print.h == s.fingerprint && pixelWidth == s.width && pixelHeight == s.height) {
        std::string anim = animState();
        const uint64_t built = SDL_GetPerformanceCounter();
        if (anim == s.animSig) {
            timing(start, built, SDL_GetPerformanceCounter(), 0);
            return false;
        }
        s.animSig = std::move(anim);
        std::vector<const tr::XmlNode*> changed;
        refreshAnimTimes(s.live, s.canvas, &changed);
        for (const tr::XmlNode* n : changed) invalidateCached(s.live, s.quadCache, *n);
        UiCanvas::Options options = optionsFor(s, pixelWidth, pixelHeight);
        options.quads = &quads;
        options.quadCache = quadCacheOn() ? &s.quadCache : nullptr;
        std::vector<uint8_t> none;
        s.canvas.render({}, options, none);
        cacheHits = s.quadCache.hits;
        cacheMisses = s.quadCache.misses;
        timing(start, built, SDL_GetPerformanceCounter(), 1);
        return true;
    }
    // The same widgets in the same shape (the usual change: something moved, a text or a colour changed): only the elements whose own
    // state changed are rewritten, and only the cached subtrees holding them are drawn again.
    static const bool noIncremental = std::getenv("OPENRASA_NOINCREMENTALUI") != nullptr;   // diagnostics: always the full rebuild
    if (!noIncremental && !manager().displayStats && s.fingerprint != 0 && walk.structure.h == s.structure && walk.owns.size() == s.owns.size() &&
        walk.owns.size() == s.pairs.size() && pixelWidth == s.width && pixelHeight == s.height) {
        std::vector<size_t> changed;
        bool simple = true;
        for (size_t i = 0; i < walk.owns.size() && simple; ++i) {
            if (walk.owns[i] == s.owns[i]) continue;
            const Widget* w = s.pairs[i].first;
            // Edit boxes, scene views, movies and animation clocks keep more live state than a node's attributes: rebuilt in full.
            if (w->kind == Kind::EditBox || w->kind == Kind::PasswordBox || w->kind == Kind::MultiLineEditBox || w->kind == Kind::SceneView ||
                w->kind == Kind::Movie || isAnimated(w) != s.animatedPairs[i])
                simple = false;
            changed.push_back(i);
        }
        if (simple) {
            LiveState& live = s.live;
            for (const size_t i : changed) {
                const Widget* w = s.pairs[i].first;
                tr::XmlNode& n = *s.pairs[i].second;
                snapshot(w, n, false);
                s.canvas.invalidateNodeCache(&n);
                if (w->kind == Kind::Text || w->kind == Kind::RichText) live.texts[&n] = utf8(displayText(const_cast<Widget*>(w)));
                if (live.hovered == &n) live.hovered = nullptr;
                if (live.pushed == &n) live.pushed = nullptr;
                if (w->highlighted) live.hovered = &n;
                if (w->pushed) live.hovered = live.pushed = &n;
                if (w->toggled) live.toggled.insert(&n);
                else live.toggled.erase(&n);
                // Its cached ancestors and every cached subtree under it (their quads are placed through it).
                invalidateCached(live, s.quadCache, n);
            }
            s.owns = std::move(walk.owns);
            s.fingerprint = print.h;
            s.signature.clear();             // the full path's text signature is no longer the drawn one
            std::vector<const tr::XmlNode*> moved;
            refreshAnimTimes(s.live, s.canvas, &moved);
            for (const tr::XmlNode* n : moved) invalidateCached(s.live, s.quadCache, *n);
            s.animSig = animState();
            const uint64_t built = SDL_GetPerformanceCounter();
            if (changed.empty() && moved.empty()) {
                timing(start, built, SDL_GetPerformanceCounter(), 0);
                return false;
            }
            UiCanvas::Options options = optionsFor(s, pixelWidth, pixelHeight);
            options.quads = &quads;
            options.quadCache = quadCacheOn() ? &s.quadCache : nullptr;
            std::vector<uint8_t> none;
            s.canvas.render({}, options, none);
            cacheHits = s.quadCache.hits;
            cacheMisses = s.quadCache.misses;
            timing(start, built, SDL_GetPerformanceCounter(), 1);
            return true;
        }
    }
    s.fingerprint = manager().displayStats ? 0 : print.h;
    s.structure = walk.structure.h;
    s.owns = std::move(walk.owns);
    s.canvas.invalidateNodeCache();   // the snapshot's nodes are rewritten below
    // <Widgets> holds the root window's children (the root itself is the engine's).
    tr::XmlNode& widgets = s.snapshot.widgets;
    widgets.children.resize(root->children.size() + (manager().displayStats ? 4 : 0));
    for (size_t i = 0; i < root->children.size(); ++i) snapshot(root->children[i].get(), widgets.children[i]);
    s.live = LiveState();
    for (size_t i = 0; i < root->children.size(); ++i) collect(root->children[i].get(), widgets.children[i], s.live, s.canvas);
    s.pairs.clear();
    s.pairs.reserve(s.owns.size());
    for (size_t i = 0; i < root->children.size(); ++i) pairUp(root->children[i].get(), widgets.children[i], s.pairs);
    s.animatedPairs.resize(s.pairs.size());
    for (size_t i = 0; i < s.pairs.size(); ++i) s.animatedPairs[i] = isAnimated(s.pairs[i].first);
    s.quadCache.entries.clear();
    {
        std::vector<const tr::XmlNode*> chain;
        for (const tr::XmlNode& c : widgets.children) mapWindows(c, chain, 1, s.quadCache.minDepth, s.quadCache.maxDepth, s.live.window);
    }
    refreshAnimTimes(s.live, s.canvas, nullptr);
    {   // the sampled times the fast path compares against
        std::vector<const tr::XmlNode*> changed;
        refreshAnimTimes(s.live, s.canvas, &changed);
    }
    if (manager().displayStats) {
        // UIManager.ToggleDisplayStats: the root window's post-render (FUN_008dcc90, while + 0xC5 is set) draws four lines in the font
        // "Default", colour 0xFF00FF00, at (2, 2), (2, 18), (2, 34), (2, 50) - the renderer's triangles, draw calls, texture changes and
        // texture working set (OpenRasa's own counts, FrameStats). UNVERIFIED: whether those coordinates are logical units (here) or
        // pixels.
        const FrameStats& f = lastFrameStats();
        const std::string lines[4] = {"Triangles " + std::to_string(f.triangles), "Draw Calls " + std::to_string(f.drawCalls),
                                      "Texture Changes " + std::to_string(f.textureChanges),
                                      "Texture Working Set [MB] " + std::to_string(f.textureBytes >> 20)};
        for (int k = 0; k < 4; ++k) {
            tr::XmlNode& t = widgets.children[root->children.size() + size_t(k)];
            t = tr::XmlNode();
            t.tag = "Text";
            t.attributes = {{"anchorOffsetX", "2"}, {"anchorOffsetY", std::to_string(2 + 16 * k)}, {"sizeW", "600"}, {"sizeH", "16"},
                            {"fontId", "Default"}, {"colorR", "0"}, {"colorG", "255"}, {"colorB", "0"}, {"colorA", "255"}};
            s.live.texts[&t] = lines[k];
        }
    }
    s.animSig = animState();
    const uint64_t built = SDL_GetPerformanceCounter();
    // Scripts call setters every frame with unchanged values (the target reticle's SetAnchorOffset): draw only when the snapshot,
    // its texts or the mouse differ from the last drawn ones, or something animates.
    std::string signature = std::to_string(manager().mouseX) + "," + std::to_string(manager().mouseY);
    for (const tr::XmlNode& n : widgets.children) sign(n, s.live.texts, signature);
    signature += s.live.signature;
    if (s.canvas.animating()) signature += "animating " + std::to_string(manager().clockMs);
    if (signature == s.signature && pixelWidth == s.width && pixelHeight == s.height) {
        timing(start, built, SDL_GetPerformanceCounter(), 0);
        return false;
    }
    if (static const bool diffLog = std::getenv("OPENRASA_UISIGDIFF") != nullptr; diffLog) {   // what made this frame draw
        size_t k = 0;
        while (k < signature.size() && k < s.signature.size() && signature[k] == s.signature[k]) ++k;
        const size_t from = k > 80 ? k - 80 : 0;
        std::fprintf(stderr, "[ui sigdiff] at %zu/%zu: ...%s | was ...%s\n", k, signature.size(), signature.substr(from, 320).c_str(),
                     k < s.signature.size() ? s.signature.substr(from, 320).c_str() : "");
    }
    s.signature = std::move(signature);
    s.width = pixelWidth;
    s.height = pixelHeight;
    UiCanvas::Options options = optionsFor(s, pixelWidth, pixelHeight);
    options.quads = &quads;
    options.quadCache = quadCacheOn() ? &s.quadCache : nullptr;
    std::vector<uint8_t> none;
    s.canvas.render({}, options, none);
    cacheHits = s.quadCache.hits;
    cacheMisses = s.quadCache.misses;
    timing(start, built, SDL_GetPerformanceCounter(), 1);
    return true;
}

void renderScenes(SDL_GPUCommandBuffer* commands, SDL_GPUDevice* device, std::vector<UiCanvas::Quad>& quads, float dt) {
    State& s = state();
    for (UiCanvas::Quad& q : quads) {
        if (q.movie) {
            // The picture keeps the movie's shape (FUN_008e14a0): the widget's width, centred; its height instead when that overflows.
            q.texture = nullptr;
            const auto it = s.live.movies.find(q.movie);
            if (it == s.live.movies.end() || !it->second->moviePlayback) continue;
            MoviePlayback& m = *it->second->moviePlayback;
            const UiCanvas::Rect a = q.area;
            const float scaleW = float(a.width()) / float(m.width());
            int w = int(std::lround(float(m.width()) * scaleW)), h = int(std::lround(float(m.height()) * scaleW));
            if (h > a.height()) {
                const float scaleH = float(a.height()) / float(m.height());
                w = int(std::lround(float(m.width()) * scaleH));
                h = int(std::lround(float(m.height()) * scaleH));
            }
            const int x = (a.x0 + a.x1) / 2 - w / 2, y = (a.y0 + a.y1) / 2 - h / 2;
            q.rect = UiCanvas::Rect{x, y, x + w, y + h};
            q.texture = m.texture(device, commands);
            continue;
        }
        if (!q.scene) continue;
        q.texture = nullptr;
        const auto it = s.live.sceneViews.find(q.scene);
        if (it == s.live.sceneViews.end()) continue;
        Widget* w = it->second;
        scene::SceneSettings settings = w->scene;
        settings.hasPreview = entityBodySnapshot(settings.previewEntity, settings.preview);   // the preview body as it is now
        if (settings.bodies.empty() && !settings.hasPreview) continue;
        if (!w->sceneRenderer) w->sceneRenderer = std::make_shared<scene::SceneWidget>(device, assets());
        // A buffered view keeps its image until its interval has run (FUN_0068a680) or its size changes (FUN_0068a4c0).
        const uint64_t now = SDL_GetTicks();
        w->sceneSkipped += dt;
        if (w->sceneBuffered && w->sceneImage && w->sceneDrawnW == q.rect.width() && w->sceneDrawnH == q.rect.height() &&
            uint32_t(now - w->sceneDrawnAt) < w->sceneInterval) {
            q.texture = w->sceneImage;
            continue;
        }
        q.texture = w->sceneRenderer->render(commands, settings, q.rect.width(), q.rect.height(), w->sceneSkipped);
        // The view's camera and projection as the drawing left them (UpdateCameraToHead).
        w->scene.camera = settings.camera;
        w->scene.orthographic = settings.orthographic;
        w->scene.ortho = settings.ortho;
        w->scene.orthoForWidth = settings.orthoForWidth;
        w->scene.orthoForHeight = settings.orthoForHeight;
        w->sceneSkipped = 0;
        w->sceneImage = q.texture;
        w->sceneDrawnAt = now;
        w->sceneDrawnW = q.rect.width();
        w->sceneDrawnH = q.rect.height();
    }
}

void renderUiCpu(int pixelWidth, int pixelHeight, std::vector<uint8_t>& rgba) {
    State& s = state();
    if (!s.ready) return;
    UiCanvas::Options options = optionsFor(s, pixelWidth, pixelHeight);
    s.canvas.render({}, options, rgba);
}

}  // namespace engine::ui
