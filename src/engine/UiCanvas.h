#pragma once

#include "assetlibrary.h"
#include "gamedata.h"
#include "ui.h"
#include "RichText.h"

#include <cstdint>
#include <map>
#include <unordered_map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Lays out and draws a branch of the game's UI export (tr::UiExport, data\xml\Tabula_Rasa_UI_EXPORT.xml) into an RGBA image of
// the export's logical size (1280 x 960), on the CPU.
//
// Layout (tabula_rasa.exe, verified 2026-10-15): a widget's position is relative to its parent: the parent's anchor point
// (parentAnchor) + anchorOffset - its own anchor point (childAnchor) (FUN_008b6fb0); anchor points (FUN_008b7060): 0 top left,
// 1 top (w/2, 0), 2 top right, 3 right (w, h/2), 4 bottom right, 5 bottom (w/2, h), 6 bottom left, 7 left (0, h/2), 8 centre
// (w/2, h/2), halves by integer division - the uianchor module's eWidgetAnchor_TopLeft .. Center (FUN_0051f2f0); both anchors
// default to 0 (Widget constructor FUN_0089c280); resizeToParent takes the parent's size (Widget layout FUN_0089d740). The
// screen's root widgets sit in the logical frame.
//
// Drawing: widgets with a textureDef (Bitmap, Filler, frame pieces, caret, …) draw that texture region, ColoredRect a colour,
// Text / RichText their textId's string; every other kind only holds children, which draw after their parent in file order.
// Colour = colorR/G/B/A (default 255) x texel. Blending per effect (effects.glm UI effects, pass states and pixel shaders read
// 2026-10-15): PalUIDefault (texture x colour), PalUIColoredRect (colour) and PalUIMultiTex blend SRCALPHA / INVSRCALPHA;
// TRUIAdditiveAlpha and PalUIMultiTexAdd SRCALPHA / ONE; PalUIMultiTex* and TRUIRadarMask multiply by a second map
// (maskTextureName); widgets that name no effect draw as PalUIDefault and TRUIBackgroundMask (the "wire" lines) as texture x the
// mouse mask, ONE / ONE (both verified in the fountain trace, see drawTexture). A composite's colour multiplies into
// everything under it (the render context, see place). Animations, progress bars, scroll bars, button states: see the .cpp.
// UNVERIFIED: the maskTextureName masks' coordinates (spread over the widget); text: see drawText - the game's GDI glyphs reproduced
// with FreeType.
class UiCanvas {
public:
    // A font range's pixel em at a screen scale, as the exe sizes it: InitMultiFont (FUN_009013c0) size = trunc(pointSize x UIManager
    // + 0x88 (screen / logical, y)), at least 7, then gfxFontImpl::Create's lfHeight = -MulDiv(size, LOGPIXELSY 96, 72). At 1080:
    // point 14 -> 15 -> 20, point 12 -> 13 -> 17 (retail's map list, measured), point 9 -> 10 -> 13. The earlier rule round(trunc(point
    // x 4 / 3) x scale) gave 18 for point 12.
    static int32_t fontEm(int32_t pointSize, float scale) {
        int32_t size = int32_t(float(pointSize) * scale);
        if (size < 7) size = 7;
        return (size * 96 + 36) / 72;
    }
    struct Rect {
        int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        int32_t width() const { return x1 - x0; }
        int32_t height() const { return y1 - y0; }
        bool contains(int32_t x, int32_t y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    };
    // A decoded texture (a .dds of the UI, or - OpenRasa - a text widget's string drawn the game's way). The serial identifies it
    // for a GPU texture cache (text images are replaced when their text changes).
    struct Image {
        int32_t width = 0, height = 0;
        std::vector<uint8_t> rgba;
        uint64_t serial = 0;
        mutable uint64_t lastUsed = 0;      // the last render that drew it (text images: kept while drawn - Options::quadCache replays)
    };
    // OpenRasa: what a draw is on the GPU (Options::quads) - one textured quad, as the CPU path would have composited it.
    struct Quad {
        const Image* image = nullptr;       // null: plain colour (ColoredRect)
        const Image* mask = nullptr;        // PalUIMultiTex* / TRUIRadarMask second map, over the widget
        const Image* mouseMask = nullptr;   // TRUIBackgroundMask: ui_mouse_mask.dds around the mouse (or the radar mask, below)
        bool radarMask = false;             // mouseMask is TRUIRadarMask's mask: all four channels multiplied
        int blend = 0;                      // 0 SRCALPHA / INVSRCALPHA, 1 SRCALPHA / ONE, 2 ONE / ONE (output alpha 1)
        float colour[4] = {1, 1, 1, 1};
        Rect rect, clip;                    // pixels
        float region[4] = {0, 0, 1, 1};     // texture coordinates
        float tileU = 0, tileV = 0;         // region repeats across the quad (0: stretched)
        bool wrap = false;                  // animated region: coordinates wrap over the whole texture
        bool rotate = false;
        float rotation[4] = {1, 0, 0.5f, 0.5f};   // cos, sin, anchor x, anchor y
        float mouseX = 0, mouseY = 0, mouseSize = 0;   // pixels
        const tr::XmlNode* scene = nullptr;   // a SceneView widget: its 3D view goes here (the engine renders it, see texture)
        void* texture = nullptr;              // a GPU texture to draw instead of image (SDL_GPUTexture*: the SceneView's view)
        const tr::XmlNode* movie = nullptr;   // a Movie widget: its picture goes here (the engine fits rect into area, sets texture)
        Rect area;                            // a Movie's widget rectangle (pixels)
    };
    struct Options {
        bool showHidden = false;            // draw widgets with visible="false" below the chosen branch
        // TRUIBackgroundMask widgets (the window "wires") glow only around the mouse: texture x ui_mouse_mask.dds over a 256-unit square
        // centred on it (see drawTexture). Off: no mouse over the UI - the mask's clamped edge (1/255) leaves them invisible.
        bool mouseHighlight = false;
        float mouseX = 0, mouseY = 0;       // logical units
        // Button state (see drawWidget): 0 as exported (the export's Bitmap / Text children: Up, or Disabled for a few), 1 up,
        // 2 highlighted, 3 down (pushed), 4 disabled, 5 pressed (toggle buttons; push buttons show down). toggledOn: toggle buttons'
        // Down variants.
        int buttonState = 0;
        bool toggledOn = false;
        // Interactive buttons (replaces buttonState / toggledOn): each button's state from the mouse - disabled when not enabled,
        // pushed (down / pressed) while the mouse is down on the hovered button, highlighted when hovered, else up; toggle buttons
        // are on when in toggledButtons.
        bool interactiveButtons = false;
        const tr::XmlNode* hoveredButton = nullptr;
        const tr::XmlNode* pushedButton = nullptr;
        const std::set<const tr::XmlNode*>* toggledButtons = nullptr;
        // Progress bars' fill, 0..1; below 0: each bar's own percentComplete (as the engine applies it at load).
        float progress = -1.0f;
        // Scroll bars' and sliders' thumbs, 0..1 of their travel; below 0: as exported (each one's currentValue).
        float scroll = -1.0f;
        // Interactive mode: each scroll bar's / slider's own thumb position (0..1 of its travel), taking precedence over `scroll`.
        const std::map<const tr::XmlNode*, float>* scrollValues = nullptr;
        // Animations (Definitions/Animations, see sampleAnimation): milliseconds since every widget's animation started; below 0: none
        // applied (the export's own state).
        float animTime = -1.0f;
        // Interactive mode: text typed into edit boxes (EditBox / PasswordBox, UTF-8) and the box that has the keyboard (its caret
        // shows); see drawEditText.
        const std::map<const tr::XmlNode*, std::string>* editTexts = nullptr;
        const tr::XmlNode* focusedEdit = nullptr;
        bool caretVisible = true;           // the focused box's caret in its blink (EditBox FUN_008cffd0: toggled every 500 ms)
        // The focused box's caret (+ 0x144) and selection (+ 0x13c / + 0x140) in characters; SIZE_MAX = the end. Equal ends = none.
        size_t caretIndex = SIZE_MAX;
        size_t selectionStart = 0, selectionEnd = 0;
        // Each box's first visible character (the game's + 0x184), kept from render to render: the window scrolls only as far as
        // the caret needs (FUN_008d2710). nullptr: every box shows from its start.
        std::map<const tr::XmlNode*, size_t>* editWindows = nullptr;
        // Edit mode: the widget being dragged and by how much (logical units), drawn moved before the edit is written.
        const tr::XmlNode* dragNode = nullptr;
        int32_t dragX = 0, dragY = 0;          // added to its anchorOffset
        int32_t dragW = 0, dragH = 0;          // added to its size (resizing)
        int background = 0;                 // 0 black, 1 grey, 2 checkerboard
        // The screen the UI is drawn for, in pixels (0: the export's logical size, 1280 x 960, at one pixel per unit). Wider
        // than 1.5 the root is 1706 x 960 logical, as in the game; widgets are laid out in logical units and drawn at pixels.
        int32_t screenWidth = 0, screenHeight = 0;
        // Draw over this image instead of the background colour (screen-sized RGBA; for comparisons with game frames and for
        // drawing several branches in turn).
        const std::vector<uint8_t>* backgroundRgba = nullptr;
        // OpenRasa: the text each Text / RichText widget shows (UTF-8), set by the game's scripts or resolved from its textId - taken
        // before the textId.
        const std::map<const tr::XmlNode*, std::string>* texts = nullptr;
        // OpenRasa: each element's own animation clocks (milliseconds since its default widget / bitmap animation started), taken
        // before animTime.
        const std::map<const tr::XmlNode*, float>* widgetAnimTimes = nullptr;
        const std::map<const tr::XmlNode*, float>* bitmapAnimTimes = nullptr;
        // OpenRasa: draw as GPU quads into this list instead of compositing pixels (render()'s rgba is left empty).
        std::vector<Quad>* quads = nullptr;
        // OpenRasa: a cache of the quads (and drawn-widget entries) each subtree at `depth` produced, keyed by its node; a valid
        // entry is appended instead of drawing the subtree again (UiRender keeps it across frames while only animation clocks
        // move, marking the subtrees of changed animations invalid). Subtrees holding an edit box are never cached (their layouts
        // live in per-render tables).
        struct QuadCache;
        QuadCache* quadCache = nullptr;
        // OpenRasa: the scripts widen the windows they want full width (ResizeToParent); no guessing from the declared size.
        bool scriptedWidening = false;
    };
    int32_t width() const { return width_; }              // the last render's root, logical units
    int32_t height() const { return height_; }
    int32_t pixelWidth() const { return pixelWidth_; }    // the last render's image
    int32_t pixelHeight() const { return pixelHeight_; }
    // A widget as drawn: its path (child indices from the export's <Widgets>) and screen rectangle.
    struct DrawnWidget {
        std::vector<int32_t> path;
        Rect rect;
        bool visual = false;                // drew a texture, colour or text
    };

    UiCanvas();
    ~UiCanvas();
    UiCanvas(const UiCanvas&) = delete;
    UiCanvas& operator=(const UiCanvas&) = delete;

    void setSources(const tr::UiExport* ui, const tr::AssetLibrary* assets, tr::GameData* game, const std::string& gameRoot);
    // The same game, an edited export (keeps the loaded textures and fonts).
    void setExport(const tr::UiExport* ui);

    // Which tree paths start from: <Widgets> (the screen), or Definitions/TemplateWidgets (unparented trees the scripts instantiate;
    // drawn where they would sit on the screen's root).
    void setTemplates(bool templates) { templates_ = templates; }
    bool templates() const { return templates_; }
    const tr::XmlNode* root() const { return ui_ ? (templates_ ? &ui_->templates : &ui_->widgets) : nullptr; }
    // The node at a path from the root (null if the path is invalid) and its screen rectangle.
    const tr::XmlNode* node(const std::vector<int32_t>& path) const;
    Rect layout(const std::vector<int32_t>& path) const;
    // The anchorOffset that puts the element at path at rect (logical), given its anchors and the rect's size (the layout rule
    // backwards: offset = rect corner - parent position - parent anchor point + own anchor point).
    void offsetsFor(const std::vector<int32_t>& path, const Rect& rect, int32_t& offsetX, int32_t& offsetY) const;

    // Draws the branch at path (the branch's root is drawn even when hidden; ancestors are laid out, not drawn) into rgba
    // (pixelWidth() x pixelHeight() x 4 bytes). Returns the widgets drawn (logical rectangles), in drawing order.
    struct Options::QuadCache {
        struct Entry {
            std::vector<Quad> quads;
            std::vector<DrawnWidget> drawn;
            bool valid = false;
            int cacheable = -1;        // -1 not yet known, 0 holds an edit box, 1 cacheable
        };
        std::unordered_map<const tr::XmlNode*, Entry> entries;
        size_t minDepth = 2, maxDepth = 6;   // path lengths of the cached subtrees (nested: an outer miss still reuses inner hits)
        size_t hits = 0, misses = 0;   // per render (diagnostics)
    };
    std::vector<DrawnWidget> render(const std::vector<int32_t>& path, const Options& options, std::vector<uint8_t>& rgba);
    // Definitions previews: draws `widget` - a node the caller built, not part of the export (a Text with a fontId, a copy of an
    // element that uses an animation, ...) - on its own in a pixelWidth x pixelHeight image at `scale` pixels per logical unit (the
    // game's screen scale: 1.125 on a 1080-line screen), laid out in that frame as a root widget. A Text widget shows sampleText
    // instead of its textId. Options as for render (animTime, background, buttonState, ...; screen size and backgroundRgba ignored).
    // Until the next render(), paths (node, the hit tests) are relative to `widget`, which must outlive those queries.
    void renderDetached(const tr::XmlNode& widget, int32_t pixelWidth, int32_t pixelHeight, float scale, const Options& options,
                        const std::string& sampleText, std::vector<uint8_t>& rgba);
    uint32_t animationLength(const std::string& name) const;       // milliseconds (the longest track's last key); 0 if none
    // The time an animation is sampled at for elapsed time t (wrapped by its playback type; a stall animation held at its end), or
    // -1 when nothing is applied - two times with the same value draw the same frame.
    long sampledAnimationTime(const std::string& name, bool bitmap, double t) const;
    // The widget under a point of the last render: the last drawn visual widget containing it, else the deepest container.
    std::vector<int32_t> pick(int32_t x, int32_t y) const;
    // The button (a widget with button textures) under a point of the last render: the picked widget or its nearest such ancestor.
    const tr::XmlNode* buttonAt(int32_t x, int32_t y) const;
    // Interactive scroll bars / sliders: the thumb drawn under (x, y) (or nullptr), its bar, and the thumb position (0..1) that puts
    // the thumb's top left at (x - grabX, y - grabY) - applyScroll's layout backwards.
    const tr::XmlNode* thumbAt(int32_t x, int32_t y) const;
    const tr::XmlNode* parentOf(const tr::XmlNode* child) const;
    bool drawnRect(const tr::XmlNode* widget, Rect& rect) const;     // where the last draw put it
    float thumbRatioAt(const tr::XmlNode& bar, int32_t x, int32_t y) const;
    // A press on a scroll bar's arrow button or track in the last render (see the .cpp): the bar and the step it makes (+1 / -1, 0 =
    // on the track at the thumb); false if (x, y) is on none. scrollRatioStepped: the thumb ratio after `step` value units from
    // `ratio` (below 0: the exported currentValue), clamped as the game's setValue.
    bool scrollStepAt(int32_t x, int32_t y, const tr::XmlNode*& bar, int& step) const;
    static float scrollRatioStepped(const tr::XmlNode& bar, float ratio, int step);
    // A press on a slider's Track (slider.cpp FUN_008db250): the slider and its thumb ratio after the jump; false if (x, y) is on none.
    bool sliderPressAt(int32_t x, int32_t y, const tr::XmlNode*& slider, float& ratio) const;
    // The scroll bar or slider (any of its parts) under (x, y) in the last render - the mouse wheel's target; nullptr if none.
    const tr::XmlNode* wheelTargetAt(int32_t x, int32_t y) const;
    // The character boundary of an edit box nearest logical x in the last render (the caret position a press there gives); 0 when the
    // box was not drawn.
    size_t editIndexAt(const tr::XmlNode* box, int32_t x) const;
    // The edit box's Highlight as last placed on a selection by layoutEdit (FUN_008d2710), relative to the box (logical); false when
    // it never was (the exe's Highlight keeps its last place while hidden).
    bool editHighlight(const tr::XmlNode* box, int32_t& x, int32_t& y, int32_t& width, int32_t& height) const;
    // A slider's thumb ratio after `step` value units (the wheel), clamped to min .. max.
    static float sliderRatioStepped(const tr::XmlNode& slider, float ratio, int step);
    // Push buttons (PushButton and the parts loaded the same way: ScrollUpBtn / ScrollDownBtn, Thumb, Track, Entry) and toggle buttons
    // (Button, ToggleButton) - by kind, not by attributes: every state attribute is optional (e.g. the user's DiscordBtn has no
    // texture states at all).
    static bool isButton(const tr::XmlNode& n) {
        return n.tag == "PushButton" || n.tag == "ScrollUpBtn" || n.tag == "ScrollDownBtn" || n.tag == "Thumb" || n.tag == "Track" ||
               n.tag == "Entry" || n.tag == "Button" || n.tag == "ToggleButton";
    }
    // Whether the last render sampled an animation that has not come to rest (repeat / pingpong, or before its end).
    bool animating() const { return animating_; }

    std::string text(const tr::XmlNode& widget);                    // a Text widget's string ("" if none)
    // OpenRasa: RichText metrics at a screen scale (RichText.h) - glyph advances in pixels as drawn, line heights in logical units (the
    // largest tmHeight of the font's ranges x 1 / sy, truncated).
    engine::ui::RichMeasure richMeasure(float sy);
    // OpenRasa: Text.WrapText's line breaking (FUN_008f9860) of text in a font of the export at a screen scale (sy for the em), into
    // widthPixels: each line's (start, length). Empty when the font is unknown.
    // OpenRasa: a character's advance in the range of the font that holds it, at a screen scale (0 when no face has it).
    int32_t glyphAdvance(const std::string& fontId, char16_t c, float sy);
    std::vector<std::pair<int, int>> wrapLines(const std::string& fontId, const std::u16string& text, int widthPixels, float sy);
    const std::set<std::string>& missingTextures() const { return missingTextures_; }
    const std::set<std::string>& missingFonts() const { return missingFonts_; }
    const std::set<std::string>& missingTextIds() const { return missingTextIds_; }

private:
    // OpenRasa: text widgets' images for the GPU path, by everything that shapes them; dropped when unused for a while.
    struct TextImage {
        std::unique_ptr<Image> image;
        uint64_t lastUsed = 0;
    };
    std::map<std::string, TextImage> textImages_;
    uint64_t renderCount_ = 0;
    uint64_t nextSerial_ = 1;
    struct Face;
    struct FontLibrary;
    struct Target;
    const Image* image(const std::string& name);
    Face* face(const std::string& family);
    // A laid-out widget as its children see it (Widget layout FUN_0089d740): its logical rectangle, its size property (unscaled; the
    // anchor points use it), the animation scale it passes down (Widget slot 107) and the render context colour (WidgetComposite render
    // FUN_008aee70).
    struct Placed {
        Rect rect;
        int32_t w = 0, h = 0;
        float sx = 1, sy = 1;
        uint8_t colour[4] = {255, 255, 255, 255};
        const tr::XmlNode* frame = nullptr;   // the widget is a Frame: its pieces are laid out from its size (framePiece)
    };
    Placed place(const tr::XmlNode& widget, const Placed& parent) const;
    Placed placedAt(const std::vector<int32_t>& path) const;   // the widget at path (the <Widgets> root for an empty path)
    void drawWidget(const tr::XmlNode& widget, std::vector<int32_t>& path, const Placed& parent, const Rect& clip, bool branchRoot, Target& target);
    static bool holdsEditBox(const tr::XmlNode& n);
    void drawTexture(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target);
    void drawColour(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target);
    void drawText(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target);
    void drawRichText(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target);
    int32_t textShiftX_ = 0;                // drawText: pixels added to the text's rectangle (RichText runs sit on pixels)
    void fillBackground(Target& target) const;                      // options_.background / backgroundRgba
    void finish(const Target& target, std::vector<uint8_t>& rgba) const;
    // Edit boxes (drawEditText): drawText measures without drawing (measureOnly_), keeps the end of a text too long for the box
    // (scrollTail_, instead of "..."), and reports the width / em of the line it laid out (pixels).
    bool measureOnly_ = false, scrollTail_ = false;
    Rect parentRect_;                       // drawWidget: the drawn widget's parent (logical), for the radar mask's default centre
    int32_t measuredWidth_ = 0, measuredEm_ = 0;
    std::map<const tr::XmlNode*, std::string> textOverrides_;
    bool editText(const tr::XmlNode& box, std::string& text) const;
    // layoutEdit (before the children: the Highlight and Caret are children) works out the visible window, the text runs and places
    // the Highlight and the caret; drawEditText (after them) draws the runs.
    void layoutEdit(const tr::XmlNode& box, const Rect& rect, const Rect& clip, Target& target);
    void drawEditText(const tr::XmlNode& box, const Rect& rect, const Rect& clip, Target& target);
    int32_t measureEdit(const tr::XmlNode& box, const Rect& rect, const Rect& clip, Target& target, const std::string& text);
    struct EditRun {
        std::string text;
        int32_t x = 0;                      // logical, from the box's left
        bool highlighted = false;
    };
    struct EditLayout {
        int32_t left = 0;                   // the box's left (logical)
        size_t first = 0;                   // the first visible character
        std::vector<int32_t> edges;         // logical x of each visible boundary, from first
        std::vector<EditRun> runs;
    };
    std::map<const tr::XmlNode*, EditLayout> editLayouts_;
    struct HighlightPlace {
        int32_t x = 0, y = 0, width = 0, height = 0;
    };
    std::map<const tr::XmlNode*, HighlightPlace> highlightPlaces_;   // by edit box
    const tr::XmlNode* sampleNode_ = nullptr;                       // renderDetached's widget, whose text is sampleText_
    const tr::XmlNode* detachedRoot_ = nullptr;                     // renderDetached's widget: node(path) starts there until render()
    std::string sampleText_;

    const tr::UiExport* ui_ = nullptr;
    bool templates_ = false;
    const tr::AssetLibrary* assets_ = nullptr;
    tr::GameData* game_ = nullptr;
    std::string gameRoot_;
    Options options_;
    // A button's state applied to its Bitmap / Text children (drawWidget): texture, text colour, text offset.
    struct ButtonOverride {
        bool texture = false, colour = false, offset = false, size = false;
        bool show = false;                  // drawn although visible="false" (an edit box's caret while focused)
        std::string textureDef;
        float rgba[4] = {1, 1, 1, 1};
        int32_t offsetX = 0, offsetY = 0;
        int32_t width = 0, height = 0;
    };
    std::map<const tr::XmlNode*, Rect> childClips_;      // a widget's clip for its children (progress bars), relative to it
    void applyProgress(const tr::XmlNode& bar);
    void applyScroll(const tr::XmlNode& bar);
    std::map<const tr::XmlNode*, ButtonOverride> buttonOverrides_;
    void applyButtonState(const tr::XmlNode& button);
    void colourOf(const tr::XmlNode& widget, float rgba[4]) const;      // the widget's colour x the context colour, 0..1
    void ownColour(const tr::XmlNode& widget, uint8_t rgba[4]) const;   // Widget slot 73: own colour x the animation's colour
    uint8_t context_[4] = {255, 255, 255, 255};                         // the render context colour of the widget being drawn
    // Animations (Definitions/Animations; tabula_rasa.exe ui\anim, decompiled 2026-10-15, see sampleAnimation).
    struct AnimKey {
        uint32_t time = 0;                  // milliseconds
        float v[4] = {0, 0, 0, 0};          // scale / trans (x, y), uv (X, Y, Z, W), rot, colour (r, g, b, a bytes)
    };
    struct AnimTrack {
        bool discrete = false;
        std::vector<AnimKey> keys;          // sorted by time
    };
    struct Animation {
        bool bitmap = false;                // animType Bitmap (else Widget)
        int playback = 0;                   // 0 stall, 1 repeat, 2 pingpong
        AnimTrack scale, trans, colour;     // Widget
        AnimTrack uv, rot;                  // Bitmap
        float anchorX = 0.5f, anchorY = 0.5f;   // rotTrack's
        uint32_t length = 0;                // the longest track's last key
    };
    struct AnimFrame {
        bool scale = false, trans = false, colour = false, uv = false, rot = false;
        float scaleX = 1, scaleY = 1, transX = 0, transY = 0;
        uint8_t rgba[4] = {255, 255, 255, 255};
        float uvRect[4] = {0, 0, 1, 1};
        float rotation = 0, anchorX = 0.5f, anchorY = 0.5f;
    };
    std::map<std::string, Animation> animations_;
    mutable bool animating_ = false;
    // The attributes a widget's drawing reads, parsed once per snapshot (the text lookups cost more than the drawing); keyed by the
    // node, so cleared whenever the nodes' contents may have changed (invalidateNodeCache) and off for temporary nodes (noCacheDepth_).
    struct NodeInfo {
        int32_t sizeW = 0, sizeH = 0, parentAnchor = 0, childAnchor = 0, offsetX = 0, offsetY = 0;
        bool resizeToParent = false, hidden = false, clipChildren = true, hasTextureDef = false, hasTextureFile = false;
        bool wrapText = false, scaleToFit = false;
        float colour[4] = {1, 1, 1, 1};
        std::string widgetAnimName, bitmapAnimName, effect /* lower case */, textureDef, fontId, horizAlign, vertAlign;
    };
    mutable std::unordered_map<const tr::XmlNode*, NodeInfo> nodeCache_;
    mutable NodeInfo temporaryInfo_;
    int noCacheDepth_ = 0;
    const NodeInfo& info(const tr::XmlNode& n) const;
public:
    void invalidateNodeCache() { nodeCache_.clear(); }
    void invalidateNodeCache(const tr::XmlNode* n) { nodeCache_.erase(n); }   // one node whose attributes were rewritten
private:
    bool sampleAnimation(const std::string& name, bool bitmap, AnimFrame& frame, const tr::XmlNode* widget = nullptr) const;
    void loadAnimations();
    int32_t width_ = 1280, height_ = 960;
    int32_t pixelWidth_ = 1280, pixelHeight_ = 960;
    std::map<std::string, std::unique_ptr<Image>> images_;          // by lower-case name; null = failed
    std::map<std::string, std::unique_ptr<Face>> faces_;            // by family name; null = not found
    std::unique_ptr<FontLibrary> fontLibrary_;                      // FreeType
    std::vector<std::pair<std::string, std::shared_ptr<std::vector<uint8_t>>>> fontFiles_;   // (path, data), loaded on demand
    bool fontFilesListed_ = false;
    std::vector<DrawnWidget> drawn_;
    std::set<std::string> missingTextures_, missingFonts_, missingTextIds_;
};
