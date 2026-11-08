// UiManager.h - the game's UI at run time: the widget tree built from data\xml\Tabula_Rasa_UI_EXPORT.xml (as the original UIManager,
// src\client\tr\ui\base\uimanager.cpp, loads it), its layout, templates, and the window scripts.
//
// Loading (UIManager FUN_00895aa0, Window::Load FUN_008dc630, LoadPythonScript FUN_008dd060; traced 2026-10-24): the export's
// Definitions, then the widget tree under a root window ("RootWindow", 1280 x 960, or 1706 x 960 on screens wider than 3:2). A
// Window with a `script` attribute, once its children are loaded, imports <script package>.<script> (client.ui), creates an
// instance of the class of the same name (no arguments), sets its `window` attribute to the window's Python object and calls its
// Init() - so inner windows are initialised before outer ones. Templates (Definitions/TemplateWidgets) are instantiated by
// InsertTemplateWidget(templateName, newName, ...) under a composite, scripts included.
#pragma once

#include "ui.h"

#include "../scene/SceneSettings.h"

#include <Python.h>

#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

namespace engine::ui {

class MoviePlayback;

// Widget classes (the factory's kinds, FUN_00897260, and the owned parts' kinds).
enum class Kind {
    Widget, ColoredRect, Bitmap, Text, RichText, WidgetComposite, Window, Frame, PushButton, ToggleButton, CheckBox, ComboBox,
    EditBox, PasswordBox, MultiLineEditBox, ProgressBar, ScrollBar, Slider, Spinner, SceneView, Movie, MovieComposite,
    DNDWidget                            // made only by UIManager.CreateDNDWidget (dndwidget.h)
};
const char* kindName(Kind k);
bool isComposite(Kind k);

struct KeyHandler {
    std::string key;     // "tab", "enter", ...
    int keyCode = 0;     // its scan code (keyCodeForName)
    int eventType = 0;   // uievent.keyboard.eKeyDown ...
    int modifiers = 0;   // uievent.eMod*
    PyObject* callback = nullptr;
};
struct MouseHandler {
    int buttons = 0;     // uievent.mouse.eButton* / eWheel*
    int eventType = 0;   // uievent.mouse.eMouse*
    PyObject* callback = nullptr;
};

struct Widget {
    Kind kind = Kind::Widget;
    std::string tag;                     // the element's tag in the export
    std::string uiid;
    Widget* parent = nullptr;
    std::vector<std::unique_ptr<Widget>> children;
    const tr::XmlNode* source = nullptr; // the element it was built from (kept alive by the manager)

    // Widget (constructor FUN_0089c280 defaults; loader FUN_0089c630).
    int offsetX = 0, offsetY = 0;        // anchorOffset
    int width = 0, height = 0;
    int childAnchor = 0, parentAnchor = 0;
    bool resizeToParent = false, visible = true, pickable = false, enabled = false;
    uint8_t r = 255, g = 255, b = 255, a = 255;
    std::string effect, widgetAnimName, bitmapAnimName;
    bool autoRestartAnims = false;
    double widgetAnimStart = 0, bitmapAnimStart = 0;   // Manager::clockMs when the default animations (re)started
    // The animation an AnimManager player last applied (CreateAnimPlayer / SetElapsedTime / PlayAnim ...; see UiPython.cpp), drawn
    // in place of the default one at playerTime ms; playerTime < 0 = none.
    std::string playerAnim;
    bool playerBitmap = false;
    double playerTime = -1;
    // Sounds (audio set ids, 0 none), in the loaders' order: PushButton Highlight / UnHighlight / Press / UnPress (+ 0x170 ..), Press
    // 10000005 by default (FUN_008e2b20); ToggleButton Toggle / UnToggle / Highlight / UnHighlight / Press / UnPress (+ 0x18C ..),
    // Press 10000005 (FUN_008e7a10); EditBox AddChar / DelChar / Cursor (+ 0x1B4 ..), the first two 10000025 (FUN_008ce7a0).
    long defaultWidgetPlayer = 0, defaultBitmapPlayer = 0;   // the AnimManager handles GetDefault*AnimId gave out (0 none yet)
    long widgetAnimLast = -1, bitmapAnimLast = -1;          // the default animations' elapsed ms at the last update (-1 restarted)
    int highlightSound = 0, unhighlightSound = 0, pressSound = 0, unpressSound = 0, toggleSound = 0, untoggleSound = 0;
    int addCharSound = 0, delCharSound = 0, cursorSound = 0;
    // WidgetComposite.
    bool clipChildren = true, mouseCapture = false;
    int scrollX = 0, scrollY = 0;        // SetOffset
    // Text / RichText / EditBox.
    std::string fontId, textId;
    std::u16string text;
    bool textSet = false;                // SetText used (else the text comes from textId)
    bool wrapText = false, scaleToFit = false;
    int lengthLimit = 0;
    std::u16string reservedText;
    // Bitmap.
    std::string textureDef;
    std::string textureFile;
    // A Bitmap's texture state once a script has set its texture or region (Bitmap.SetTexture / SetTextureRegion; the textureDef's
    // otherwise): UIBitmap + 0xCC .. + 0xD8 the region (left, top, right, bottom; -1 = the whole texture), + 0xDC the mode bits (1 tile U,
    // 4 tile V - the textureDef's modeU / modeV, FUN_008ca530). Used while textureFile is set.
    int32_t regionLeft = -1, regionTop = -1, regionRight = -1, regionBottom = -1;
    int tileFlags = 0;
    bool opaqueClick = false;             // Bitmap opaqueClick (+ 0xE0): its point test ignores the texture's alpha
    bool radarMasked = false;            // GameUIManager.SetupRadarMaskWidget: the radar mask centred on (radarMaskX, radarMaskY)
    int radarMaskX = 0, radarMaskY = 0;             // SetTexture(name): a texture file drawn whole instead of the definition ("" = textureDef)
    // Controls.
    float percentComplete = 0;
    int rangeMin = 0, rangeMax = 0, pageSize = 0, value = 0;
    // ScrollBar (scrollbar.cpp; fields + 0xC0 .. + 0xF0): style "horizontal" (+ 0xC0 = 0, anything else vertical), the part a held press
    // repeats (+ 0xD8) and its timer (+ 0xDC), the last mouse position seen over the track / thumb (+ 0xE0 / + 0xE4), the thumb drag's
    // anchor (+ 0xE8 / + 0xEC) and flag (+ 0xF0).
    bool horizontal = false;
    Widget* repeatPart = nullptr;
    int repeatMs = 0;                       // also a repeatable push button's timer (+ 0x13C)
    int partMouseX = 0, partMouseY = 0, dragX = 0, dragY = 0;
    bool dragging = false;
    bool highlighted = false, repeatable = false, toggled = false, locked = false, checked = false;
    bool pushed = false;                 // PushButton + 0x139 / ToggleButton + 0x18B: the mouse is down on it
    size_t caret = std::u16string::npos; // EditBox: the caret (npos = the end)
    // EditBox selection: anchor (+ 0x13C) and end (+ 0x140), equal = none; a press is selecting while the button is down (+ 0x18F).
    size_t selAnchor = 0, selEnd = 0;
    bool selecting = false;
    bool floatText = false;               // an overhead manager FloatText instance (its animation's end deletes it)
    // Widget.SetParent's parent (slot 63, FUN_0089d2b0: + 0x48 and + 4): used for layout, event bubbling and GetParent; the widget stays
    // in its old parent's children (drawn there), nullptr = its real parent.
    Widget* layoutParent = nullptr;
    bool consumeAllKeys = false;         // EditBox consumeAllKeyboardEvents (+ 0xEF)
    // EditBox input validator (+ 0xE4; none from the constructor FUN_008ce7a0): 0 none, 1 AlphaNumeric, 2 Numeric, 3 Password,
    // 4 Unrestricted (the UI manager's registry, FUN_00897770); Numeric's limits (+ 4 / + 8, 0 / 0 when made).
    int validator = 0;
    int validatorMin = 0, validatorMax = 0;
    bool allowIme = false;               // EditBox allowIME (+ 0xEC)
    bool imeOpen = false;                // EditBox + 0xED: the IME manager's "opened" event set it (see builtIn)
    bool imeComposing = false;           // EditBox + 0xEE: in an IME composition (eIMECompStart .. eIMECompEnd)
    std::vector<std::u16string> stringList;
    // DNDWidget (FUN_008cbe10, 0x10C bytes): the visual (+ 0xE8, its only child), the visual offset (+ 0xEC / + 0xF0), the source
    // (+ 0xF4), the widget under the cursor (+ 0xF8), the drop target accepts (+ 0xFC, SetTargetValid), armed (+ 0xFD: the next
    // release without a valid target cancels), the state (+ 0x100: 4 dragging / cancelled, 0 dropped, 1 accepted, 2 deferred,
    // 3 rejected), the drop point (+ 0x104 / + 0x108) and the data (+ 0xC0: name -> decimal / text, "type" = SetDataType's).
    Widget* dndVisual = nullptr;
    int dndOffsetX = 0, dndOffsetY = 0;
    Widget* dndSource = nullptr;
    Widget* dndTarget = nullptr;
    bool dndTargetValid = false, dndArmed = false;
    int dndState = 4;
    int dndDropX = 0, dndDropY = 0;
    std::map<std::string, std::u16string> dndData;
    // SceneView: what its 3D view shows (Python SetupSceneView / AddBody / SetCameraPos / SetClearColor; the export's fieldOfView,
    // nearPlane, farPlane, cameraPosition), and the engine's renderer of it (scene::SceneWidget; goes with the widget).
    scene::SceneSettings scene;
    std::shared_ptr<scene::SceneWidget> sceneRenderer;
    // A buffered SceneView (the loader's "buffered": its view is a gfxBufferedView, tag 'VBFG') draws into its own image and redraws it
    // only when its timer has run + 0x2C0 ms (FUN_0068a680: elapsed < interval -> the last image again; 0 from the constructor = every
    // frame; RenderStaticImage sets it) or its size changed (FUN_0068a4c0 clears the wait). EnableStaticRender (+ 0x2D1, the static list)
    // moves its drawing to the start of the frame (FUN_0058d320 -> FUN_0068a730) - here the order is not modelled.
    bool sceneBuffered = false, sceneStatic = false;
    uint32_t sceneInterval = 0;            // ms; 0xFFFFFFFF = never again
    uint64_t sceneDrawnAt = 0;             // SDL ticks of the last image
    int sceneDrawnW = 0, sceneDrawnH = 0;
    float sceneSkipped = 0;                // seconds not yet given to the scene (frames that reused the image)
    void* sceneImage = nullptr;            // the last image (the SceneWidget's colour target)
    // Movie / MovieComposite.
    std::u16string movie;                // Movie: the movie last played (Play(name); + 0xB8)
    std::shared_ptr<MoviePlayback> moviePlayback;   // Movie: the open movie (+ 0xD4, the Bink handle); null when none
    // Window.
    PyObject* script = nullptr;          // the script instance (GetScript)
    PyObject* updateCallback = nullptr;  // RegisterForUpdate
    PyObject* resolutionCallback = nullptr;

    PyObject* python = nullptr;          // the widget's PyUIObject (created on first use, then kept)
    std::multimap<int, PyObject*> messageHandlers;
    std::vector<KeyHandler> keyHandlers;
    std::vector<MouseHandler> mouseHandlers;
    std::map<int, PyObject*> animHandlers, imeHandlers;
    PyObject* anyHandler = nullptr;

    ~Widget();
    Widget* child(const std::string& name) const;       // the first direct child with that uiid
};

struct Rect {
    int left = 0, top = 0, right = 0, bottom = 0;
};

class Manager {
public:
    // The root window (the UI manager's init, FUN_00895430: 1280 x 960, 1706 x 960 on screens wider than 3:2). Exists before the
    // scripts run (client.main.Initialize asks for its size).
    void createRoot(bool wideScreen);
    void setScriptPackage(const std::string& package) { scriptPackage_ = package; }   // UIManager slot 3 (FUN_00895a10)
    // Loads the export (the game folder is the working directory) and builds the widget tree under the root; window scripts are
    // imported from the script package (client.ui) as each window loads. Problems are logged; false when the export cannot be read.
    bool loadLayout();
    Widget* root() const { return root_.get(); }
    void unload() { root_.reset(); }
    std::string visibleWindows() const;                        // the visible windows (path, kind, rectangle) - a debug summary                           // before Py_Finalize
    const tr::UiExport& definitions() const { return export_; }
    // A template's widgets under parent, named newName; null when there is no such template.
    Widget* instantiateTemplate(const std::string& templateName, const std::string& newName, Widget* parent, Widget* where = nullptr);
    const tr::XmlNode* findTemplate(const std::string& templateName) const;
    // A widget tree built from an element, for parent (not added): a combo box's items are copies of its Entry.
    std::unique_ptr<Widget> buildCopy(const tr::XmlNode& node, Widget* parent) { return build(node, parent); }
    void destroy(Widget* w);                                   // remove from its parent and delete
    std::vector<Widget*> repeatableButtons;                    // push buttons with SetRepeatable(True) (scrollBarsFrame)
    std::vector<Widget*> repeatingBars;                        // scroll bars with a held press repeating (scrollBarsFrame)
    Rect screenRect(const Widget* w) const;
    std::set<Widget*> reparented;        // widgets with a layoutParent (cleared when that parent goes)
    std::set<Widget*> defaultAnimated;   // widgets with a widgetAnimName / bitmapAnimName (defaultAnimEvents walks only these)
    bool displayStats = false;           // the root window's + 0xC5 (UIManager.ToggleDisplayStats)                    // logical (root) coordinates: the layout (GetScreenRect, slot 0x1A8)
    Rect drawnRect(const Widget* w) const;                     // the layout moved by the ancestors' SetOffset (drawing, hit tests)
    bool colour(const std::string& id, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const;
    int mouseX = 0, mouseY = 0;                                // logical
    bool mousePointerShown = true;
    bool wideScreen = false;
    // Something may have changed what the UI shows (a widget method was looked up, the mouse moved, an event was delivered, the
    // layout or a template loaded): the next frame redraws it. Conservative - getters count too.
    bool dirty = true;
    double clockMs = 0;                                        // the UI's clock (frame()); animations are sampled on it
    // Widget.StartDefaultWidgetAnimOnLastFrame(flag) (0x0089F340): a global byte (0x00d451fa). While it is set, a widget's default widget
    // animation player starts at 52,000 ms rather than 0 (FUN_0089d580 -> FUN_0088d490 with 0xCB20): a fade-in shows its end at once.
    // mapwindow.py sets it around InsertTemplateWidget for its markers.
    bool widgetAnimsOnLastFrame = false;

    // The focus manager (tabula_rasa.exe global 0x00d516e0): the keyboard-focus stack (+ 0x10) and the mouse-capture stack (+ 0x14),
    // the top being the one in effect, and the widget under the mouse (+ 0x1C). Pushing a widget moves it to the top and sends the
    // old top eMessageKeyboardFocusLost / eMessageMouseCaptureLost and the new one ...Received (FUN_008aafc0 / FUN_008aac50);
    // removing the top sends it ...Lost and the new top ...Received (FUN_008ab100 / FUN_008aadc0).
    std::vector<Widget*> focusStack, captureStack;
    bool imeOpenStatus = false;          // the IME context's open status (ImmGetOpenStatus on the IME manager's context)
    std::vector<Widget*> modalStack;                 // focus manager + 0x0C: the top first (SetModal)
    Widget* hovered = nullptr;
    Widget* focus() const { return focusStack.empty() ? nullptr : focusStack.back(); }
    Widget* capture() const { return captureStack.empty() ? nullptr : captureStack.back(); }
    void forget(Widget* w);                                    // a widget going away: off the stacks
    // Drag and drop (UIManager + 0x18 the DND window, SetDNDWindow; + 0x50 the live DND widgets; + 0x54 those waiting to be deleted).
    Widget* dndWindow = nullptr;
    Widget* imeWindow = nullptr;          // the IME manager's window (+ 0xD58, SetIMEWindow)
    // The IME manager's state (imemanager.cpp, see UiInput.cpp "IME"): the edit box whose handlers are registered with it (FUN_008d0400,
    // on an allowIME box's focus), the composition (+ 0x24c), its cursor (+ 0x748), the result (+ 0xb48), "in composition" (+ 0x49),
    // and candidate list 0 (the one SDL reports): open, its strings and selection.
    Widget* imeBox = nullptr;
    std::u16string imeComposition, imeResult;
    int imeCursor = 0;
    bool imeComposing = false;
    bool imeCandidatesOpen = false;
    std::vector<std::u16string> imeCandidates;
    int imeCandidateSelection = 0;
    std::vector<Widget*> dndWidgets;
    std::vector<std::unique_ptr<Widget>> doomed;
    // A template's widgets for parent, not added (FUN_008968b0: the template by name, FUN_00898d50; built by the factory, FUN_008a8ca0).
    std::unique_ptr<Widget> buildTemplate(const std::string& templateName, Widget* parent);
    // Off its parent, kept alive until the next frame (FUN_00896720: the deferred delete list).
    void detachLater(Widget* w);

private:
    std::unique_ptr<Widget> build(const tr::XmlNode& node, Widget* parent);
    void runScripts(Widget* w);                                // window scripts, children first
    tr::UiExport export_;
    std::unique_ptr<Widget> root_;
    std::string scriptPackage_;
};

Manager& manager();
Kind kindForTag(const std::string& tag, const std::string& parentTag, bool& isWidget);

// UiPython.cpp - the Python side.
void initPython();                         // the widget types and managers; call once after Py_Initialize
bool pythonInit();
PyObject* cppLookupObject();               // GameUIManager + 4: client.clientlanguagemanager.CPPLookup (borrowed; null before pythonInit)                         // GameUIManager::PythonInit (FUN_0051f2f0): uievent, uianchor, CPPLookup - InitGameClient
PyObject* pythonOf(Widget* w);             // the widget's PyUIObject (new reference)
void releaseWidgetPython(Widget* w);       // detach it (the widget is being destroyed)
PyObject* uiManagerObject();               // gameclient.GetUIManager() (new reference)
PyObject* gameUiManagerObject();           // gameclient.GetGameUIManager() (new reference)
const std::u16string& displayText(Widget* w);
// Widget::SetVisible (FUN_0089d2f0): on a change; becoming visible with autoRestartAnims, the default widget and bitmap animations of
// the widget and everything under it restart (RestartDefaultWidgetAnim / ...BitmapAnim(1), slots + 0x170 / + 0x17C).
void setVisible(Widget* w, bool visible);
// EditBox.SetValidator(name) (0x008D3590 -> FUN_008cfa70): a validator of that name from the registry replaces the box's; an unknown
// name changes nothing. Numeric starts with the limits 0 / 0.
void setValidator(Widget* w, const std::string& name);
// UIBitmap slot 133 (GetPixelColor, FUN_008f4de0): the texel under the widget point (x, y), widget size w x h (the bitmap's + 0x18 /
// + 0x1C); false (colour 0) when there is none to read. Slot 103 (FUN_008f5100), the point test the hit tests use for bitmaps and the
// buttons' Bitmap parts: inside the rectangle and opaqueClick, or the texel's alpha above 0.
bool bitmapPixel(const Widget* bitmap, int x, int y, int width, int height, uint8_t rgba[4]);
bool bitmapPointIn(const Widget* bitmap, int x, int y);
void restartAnimations(Widget* w, bool widgetAnims, bool bitmapAnims, bool recursive);
void restartDefaultPlayer(Widget* w, bool bitmap);   // UiPython.cpp: the default animation's player, when a script made one
// The parent layout and bubbling use: SetParent's when set, else the real one.
inline Widget* layoutParentOf(const Widget* w) { return w->layoutParent ? w->layoutParent : w->parent; }
struct Event;
// The handler a FloatText instance gets for its animation's end (overhead manager FUN_0055b460, UiPython.cpp).
int floatTextAnimEvent(Widget* w, const Event& e);
// A widget's sound (UIManager + 0x9C, FUN_004d8000: the audio set played); only while the widget is visible.
void playSound(Widget* w, int audioSet);     // a Text's string: set by a script, else its textId's (CPPLookup)
const std::string& radarMaskTexture();     // GameUIManager.InitRadarMask's texture ("" before)
std::string unimplementedCallReport();     // exe methods the scripts called that are not implemented yet
bool worldDisplayedNow();
void forgetAnimPlayers(Widget* w);          // the widget is going: its AnimManager players go with it
PyObject* imeManagerObject();               // gameclient.GetIMEManager() (new reference)
PyObject* animManagerObject();              // gameclient.GetAnimManager() (new reference)                   // GameUIManager.DisplayWorld (the world shown behind the UI)

// Events (the exe's event dict, FUN_008b7480): a message event to the widget's handler for that message, delivered on the next
// frame (as the engine queues its own); data becomes event['Data'] (unicode values).
void postMessage(Widget* w, int message, std::vector<std::pair<std::string, std::u16string>> data = {});

// ---- Input (UiInput.cpp) --------------------------------------------------------------------------------------------------------
// The engine's UI event (the C++ fields FUN_008b7480 turns into the event dict; offsets of the exe's event object).
struct Event {
    int type = 2;                        // + 8: uievent.eType* (0 keyboard, 1 mouse, 2 message, 3 anim, 4 IME)
    int phase = 0;                       // + 4: uievent.ePhase*
    Widget* source = nullptr;            // + 0xC
    Widget* dest = nullptr;              // + 0x10
    int modifiers = 0;                   // + 0x14: 1 Shift, 2 Alt, 4 Control
    // Keyboard: + 0x20 key code (DirectInput scan code), + 0x24 eKey*, + 0x28 sub code, + 0x2C the character (eKeyChar).
    // Mouse: + 0x20 button, + 0x24 buttons held, + 0x28 eMouse*, + 0x2C / + 0x30 LocX / LocY, + 0x34 scroll, + 0x38 clicked widget.
    int code = 0, code2 = 0, code3 = 0;
    char16_t character = 0;
    int locX = 0, locY = 0, scroll = 0;
    Widget* clicked = nullptr;
    int message = 0;                     // + 0x48: uievent.message.eMessage*; an anim event's eAnim* (AnimEvent + 0x20)
    // Anim events (type 3, AnimEvent: + 0x24 the player's handle, + 0x28 the animation's name - the dict's AnimId / AnimDataId).
    long animId = 0;
    std::string animName;
    std::vector<std::pair<std::string, std::u16string>> data;   // event['Data']
    std::vector<std::pair<std::string, int>> intData;          // event['Data'], integer values (eMessageHTMLLink's LocX / LocY)
};
// EventHandler::HandleEvent (FUN_008b4ce0): the widget's Python handlers for the event's pattern (when the event's source is the
// widget or none), then its built-in handler, then - unless eHandledStop (2) - its parent. Returns the highest result.
int handleEvent(Widget* w, Event& e);
// Drag and drop (see UiInput.cpp): UIManager.CreateDNDWidget / DestroyDNDWidget, and a DND widget's AcceptDrop / RejectDrop end.
Widget* createDndWidget(Widget* source, const std::string& templateName, int x, int y, bool fromDrag);
void destroyDndWidgets();
void finishDnd(Widget* d);
void setDndVisualOffset(Widget* d, int x, int y);
// A message event from w to w, delivered now (the engine's widgets send theirs synchronously).
int sendMessage(Widget* w, int message, int phase = 0, std::vector<std::pair<std::string, std::u16string>> data = {});
void pushFocus(Widget* w);
void removeFocus(Widget* w);
void pushCapture(Widget* w);
void releaseCapture(Widget* w);
// Widget.SetModal (0x0089EED0): True -> FUN_008ab340, False -> FUN_008ab470 (see UiInput.cpp).
void setModal(Widget* w, bool modal);
// Widget SetEnabled (FUN_0089dd70): on a change - disabling first drops the widget's capture and focus - the flag, then eMessageEnable
// (phase 1). A composite (FUN_008aed30) passes it on to every child; a combo box (FUN_008f15d0) also closes its list.
void setEnabled(Widget* w, bool enabled);
// UIManager FUN_00896220: the screen size; the root 1706 x 960 when its aspect is above 1.5, else 1280 x 960; then (FUN_00897190) every
// window's RegisterForResolutionUpdate callable, a window before its children (Window slot 111, FUN_008dc9e0 / FUN_008af1c0).
void setScreenSize(int pixelWidth, int pixelHeight);
Widget* hitTest(int x, int y);           // logical coordinates
// Input from the window: logical coordinates; button 1 / 2 / 4 / 8 / 0x10 (uievent.mouse.eButton*); scan code = DirectInput's.
void inputMouseMove(int x, int y, int modifiers);
// The scripts' state setters run the widgets' own setters, events and sounds included: ToggleButton.SetToggled (0x008E8EA0 ->
// FUN_008e86e0), CheckBox.SetChecked (0x008D55B0 -> FUN_008d50c0), PushButton.SetHighlighted (0x008E4800 -> FUN_008e3e50).
void scriptSetToggled(Widget* w, bool v);
void scriptSetChecked(Widget* w, bool v);
void scriptSetHighlighted(Widget* w, bool v);
// The wheel (WM_MOUSEWHEEL, FUN_00902870): notches up positive; the event's ScrollCount is -notches, button eWheelUp (0x20) when that is
// negative, else eWheelDown (0x40), event code eMouseUp.
void inputMouseWheel(int notches, int x, int y, int modifiers);
// ScrollBar::SetValue (FUN_008d79e0): clamped to min .. max - pageSize + 1 (min when that range is empty); a change sends
// eMessageValueChanged {'value'} (phase 1); then the thumb is laid out (FUN_008d84f0).
void setScrollValue(Widget* bar, int value);
void layoutScrollThumb(Widget* bar);
// ComboBox (combobox.cpp, traced 2026-10-25): SetStringList (FUN_008f1e50 -> FUN_008f1320), SetValue (FUN_008f13a0), SetValueAsString
// (FUN_008f14f0); see UiInput.cpp.
void comboSetStringList(Widget* combo, std::vector<std::u16string> strings);
void comboSetValue(Widget* combo, int index);
// Slider::SetValue (FUN_008db030): clamped to min .. max; a change sends eMessageValueChanged {'value'} (phase 1) and lays out the thumb.
void setSliderValue(Widget* slider, int value);
void scrollBarsFrame(int msElapsed);   // held-press repeats (ScrollBar slot 116, FUN_008d7c00)
void inputMouseButton(int button, bool down, int x, int y, int modifiers);
void inputKey(int scanCode, bool down, int modifiers);
void inputKeyRepeat(int scanCode, int modifiers);   // a held key's repeat, to a focused edit box only
// Test runs: the NumLock state the edit boxes read (-1 = the keyboard's, SDL_GetModState).
void setTestNumLock(int state);
void inputChar(char16_t c, int modifiers);
// IME input (SDL's text editing events, see UiInput.cpp "IME"): the composition text and cursor (empty: the composition ended), a
// committed result (false when no composition was running - then it is typed as characters), the candidates (none: closed), and a
// keyboard layout change.
void imeEditing(const std::u16string& text, int cursor);
bool imeCommit(const std::u16string& text);
void imeCandidates(const std::vector<std::u16string>& candidates, int selected);
void imeLanguageChanged();
int keyCodeForName(const std::string& name);                // "tab" -> 0x0F; 0 if unknown
const char* keyName(int scanCode);                          // the exe's key name table (keyboardevent.cpp)
int scanCodeForSdl(int sdlScancode);                        // SDL_Scancode -> DirectInput scan code (0 if none)
// The key code an SDL key event is given: its DirectInput code, else an extended code (OpenRasa: every key can be bound - see
// UiInput.cpp "Extended keys"); 0 only when the key has neither an SDL scancode nor a platform one.
int keyCodeForSdl(int sdlScancode, int raw);
const char* sdlKeyText(int code);                            // SDL's own name of an extended key, else nullptr
void inputMouseTilt(int notches, int x, int y, int modifiers);   // OpenRasa: a tilting wheel, right positive
// gameclient.GetInputMapper(): key mappings per input state, calling the input state router's method of the mapped command.
PyObject* inputMapperObject();
void setInputRouter(PyObject* router);
// Once a frame (GameClient::PreRender's UI update): queued events, then the GameUIManager's update callable with the milliseconds
// since the last frame.
void frame(int msElapsed);

}  // namespace engine::ui
