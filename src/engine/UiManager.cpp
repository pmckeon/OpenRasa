// UiManager.cpp - see UiManager.h.
#include <cmath>
#include <cctype>
#include "UiManager.h"
#include "Cursor.h"

#include "trformats.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>

namespace engine::ui {



Manager& manager() {
    static Manager m;
    return m;
}

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Widget: return "Widget";
        case Kind::ColoredRect: return "ColoredRect";
        case Kind::Bitmap: return "Bitmap";
        case Kind::Text: return "Text";
        case Kind::RichText: return "RichText";
        case Kind::WidgetComposite: return "WidgetComposite";
        case Kind::Window: return "Window";
        case Kind::Frame: return "Frame";
        case Kind::PushButton: return "PushButton";
        case Kind::ToggleButton: return "ToggleButton";
        case Kind::CheckBox: return "CheckBox";
        case Kind::ComboBox: return "ComboBox";
        case Kind::EditBox: return "EditBox";
        case Kind::PasswordBox: return "PasswordBox";
        case Kind::MultiLineEditBox: return "MultiLineEditBox";
        case Kind::ProgressBar: return "ProgressBar";
        case Kind::ScrollBar: return "ScrollBar";
        case Kind::Slider: return "Slider";
        case Kind::Spinner: return "Spinner";
        case Kind::SceneView: return "SceneView";
        case Kind::Movie: return "Movie";
        case Kind::MovieComposite: return "MovieComposite";
        case Kind::DNDWidget: return "DNDWidget";
    }
    return "Widget";
}

bool isComposite(Kind k) {
    switch (k) {
        case Kind::Widget: case Kind::ColoredRect: case Kind::Bitmap: case Kind::Text: case Kind::RichText: case Kind::SceneView:
        case Kind::Movie:
            return false;
        default:
            return true;
    }
}

Kind kindForTag(const std::string& tag, const std::string& parentTag, bool& isWidget) {
    static const std::map<std::string, Kind> kinds = {
        {"Movie", Kind::Movie}, {"MovieComposite", Kind::MovieComposite}, {"ColoredRect", Kind::ColoredRect}, {"Bitmap", Kind::Bitmap},
        {"Text", Kind::Text}, {"WidgetComposite", Kind::WidgetComposite}, {"CheckBox", Kind::CheckBox}, {"ComboBox", Kind::ComboBox},
        {"EditBox", Kind::EditBox}, {"Frame", Kind::Frame}, {"MultiLineEditBox", Kind::MultiLineEditBox},
        {"PasswordBox", Kind::PasswordBox}, {"ProgressBar", Kind::ProgressBar}, {"PushButton", Kind::PushButton},
        {"RichText", Kind::RichText}, {"SceneView", Kind::SceneView}, {"ScrollBar", Kind::ScrollBar}, {"Slider", Kind::Slider},
        {"Spinner", Kind::Spinner}, {"ToggleButton", Kind::ToggleButton}, {"Window", Kind::Window},
        // Owned parts, found by their owner's loader by tag.
        {"Filler", Kind::Bitmap}, {"Caret", Kind::Bitmap}, {"Highlight", Kind::Bitmap},
        {"ScrollUpBtn", Kind::PushButton}, {"ScrollDownBtn", Kind::PushButton}, {"Track", Kind::PushButton},
        {"Thumb", Kind::PushButton}, {"Entry", Kind::PushButton}, {"DropDown", Kind::WidgetComposite}, {"Button", Kind::ToggleButton},
        {"TopLeft", Kind::Bitmap}, {"Top", Kind::Bitmap}, {"TopRight", Kind::Bitmap}, {"Right", Kind::Bitmap},
        {"BottomRight", Kind::Bitmap}, {"Bottom", Kind::Bitmap}, {"BottomLeft", Kind::Bitmap}, {"Left", Kind::Bitmap},
        {"Center", Kind::Bitmap}, {"TopLeftVert", Kind::Bitmap}, {"TopLeftHoriz", Kind::Bitmap}, {"TopRightVert", Kind::Bitmap},
        {"TopRightHoriz", Kind::Bitmap}, {"BottomLeftVert", Kind::Bitmap}, {"BottomLeftHoriz", Kind::Bitmap},
        {"BottomRightVert", Kind::Bitmap}, {"BottomRightHoriz", Kind::Bitmap}};
    (void)parentTag;
    auto it = kinds.find(tag);
    isWidget = it != kinds.end();
    return isWidget ? it->second : Kind::Widget;
}

Widget::~Widget() {
    if (kind == Kind::EditBox || kind == Kind::PasswordBox || kind == Kind::MultiLineEditBox) cursor::pop("TextEntry");   // FUN_008ce9e0
    manager().forget(this);
    releaseWidgetPython(this);
    auto drop = [](PyObject*& o) {
        Py_XDECREF(o);
        o = nullptr;
    };
    drop(script);
    drop(updateCallback);
    drop(resolutionCallback);
    drop(anyHandler);
    for (auto& [k, v] : messageHandlers) Py_XDECREF(v);
    for (auto& h : keyHandlers) Py_XDECREF(h.callback);
    for (auto& h : mouseHandlers) Py_XDECREF(h.callback);
    for (auto& [k, v] : animHandlers) Py_XDECREF(v);
    for (auto& [k, v] : imeHandlers) Py_XDECREF(v);
}

Widget* Widget::child(const std::string& name) const {
    for (const auto& c : children)
        if (c->uiid == name) return c.get();
    return nullptr;
}

// ---- Loading ----------------------------------------------------------------------------------------------------------------
namespace {

int intAttr(const tr::XmlNode& n, const char* name, int fallback) {
    if (!n.hasAttribute(name)) return fallback;
    return int(std::strtol(n.attribute(name).c_str(), nullptr, 10));
}
bool boolAttr(const tr::XmlNode& n, const char* name, bool fallback) {
    if (!n.hasAttribute(name)) return fallback;
    std::string v = n.attribute(name);
    return v == "true" || v == "1";
}
std::u16string widen(const std::string& s) {
    std::u16string out;
    for (unsigned char c : s) out += char16_t(c);
    return out;
}

}  // namespace

std::unique_ptr<Widget> Manager::build(const tr::XmlNode& n, Widget* parent) {
    bool isWidget = false;
    Kind kind = kindForTag(n.tag, parent ? parent->tag : "", isWidget);
    std::string type = n.attribute("xsi:type");
    if (!type.empty()) kind = kindForTag(type, "", isWidget);
    if (!isWidget) return nullptr;
    auto w = std::make_unique<Widget>();
    w->kind = kind;
    w->tag = n.tag;
    w->source = &n;
    w->parent = parent;
    w->uiid = n.attribute("uiid");
    w->offsetX = intAttr(n, "anchorOffsetX", 0);
    w->offsetY = intAttr(n, "anchorOffsetY", 0);
    w->width = intAttr(n, "sizeW", 0);
    w->height = intAttr(n, "sizeH", 0);
    w->childAnchor = intAttr(n, "childAnchor", 0);
    w->parentAnchor = intAttr(n, "parentAnchor", 0);
    w->resizeToParent = boolAttr(n, "resizeToParent", false);
    w->visible = boolAttr(n, "visible", true);
    w->pickable = boolAttr(n, "pickable", false);
    // A composite's loader (WidgetComposite FUN_008ae9c0, Window FUN_008dc630) loads its children FIRST and then runs the Widget
    // loader (FUN_0089c630), whose "enabled" goes through SetEnabled (slot 114): the composite's SetEnabled (FUN_008aed30) passes a
    // change on to every child - so an enabled="true" composite enables everything loaded under it (the chat's EntryEdit has no
    // attribute of its own). Other widgets take the attribute as it is.
    if (!isComposite(kind)) w->enabled = boolAttr(n, "enabled", false);
    w->r = uint8_t(intAttr(n, "colorR", 255));
    w->g = uint8_t(intAttr(n, "colorG", 255));
    w->b = uint8_t(intAttr(n, "colorB", 255));
    w->a = uint8_t(intAttr(n, "colorA", 255));
    w->effect = n.attribute("effect");
    w->widgetAnimName = n.attribute("widgetAnimName");
    w->bitmapAnimName = n.attribute("bitmapAnimName");
    if (!w->widgetAnimName.empty() || !w->bitmapAnimName.empty()) defaultAnimated.insert(&*w);
    w->widgetAnimStart = w->bitmapAnimStart = clockMs;   // a default animation starts when it is set (at load)
    if (widgetAnimsOnLastFrame && !w->widgetAnimName.empty()) w->widgetAnimStart = clockMs - 52000.0;
    // Sounds: the constructors' defaults, then the attributes.
    if (kind == Kind::PushButton || kind == Kind::ToggleButton) w->pressSound = 10000005;
    if (kind == Kind::EditBox || kind == Kind::PasswordBox || kind == Kind::MultiLineEditBox) w->addCharSound = w->delCharSound = 10000025;
    w->highlightSound = intAttr(n, "HighlightSound", w->highlightSound);
    w->unhighlightSound = intAttr(n, "UnHighlightSound", w->unhighlightSound);
    w->pressSound = intAttr(n, "PressSound", w->pressSound);
    w->unpressSound = intAttr(n, "UnPressSound", w->unpressSound);
    w->toggleSound = intAttr(n, "ToggleSound", w->toggleSound);
    w->untoggleSound = intAttr(n, "UnToggleSound", w->untoggleSound);
    w->addCharSound = intAttr(n, "AddCharSound", w->addCharSound);
    w->delCharSound = intAttr(n, "DelCharSound", w->delCharSound);
    w->cursorSound = intAttr(n, "CursorSound", w->cursorSound);
    w->autoRestartAnims = boolAttr(n, "autoRestartAnims", false);
    w->clipChildren = boolAttr(n, "clipChildren", true);
    w->mouseCapture = boolAttr(n, "mouseCapture", false);
    w->fontId = n.attribute("fontId");
    w->textId = n.attribute("textId");
    w->wrapText = boolAttr(n, "wrapText", false);
    if (kind == Kind::SceneView) {
        w->sceneBuffered = boolAttr(n, "buffered", false);   // the loader FUN_008b9170
        auto real = [&](const char* name, float fallback) { return n.hasAttribute(name) ? float(std::atof(n.attribute(name).c_str())) : fallback; };
        w->scene.fieldOfView = real("fieldOfView", w->scene.fieldOfView);
        w->scene.nearPlane = real("nearPlane", w->scene.nearPlane);
        w->scene.farPlane = real("farPlane", w->scene.farPlane);
        w->scene.camera = {real("cameraPositionX", 0), real("cameraPositionY", 0), real("cameraPositionZ", 0)};
        // cameraYaw / cameraPitch / cameraRoll (read only when all three are present - each read stops the chain on failure) ->
        // D3DXQuaternionRotationYawPitchRoll(yaw, pitch, roll) (0x0043d3c0) -> the camera node's rotation (matTransform::SetQuaternion).
        // The element form cameraOrientation (FUN_008cb1b0) is not read here (the export has none).
        if (n.hasAttribute("cameraYaw") && n.hasAttribute("cameraPitch") && n.hasAttribute("cameraRoll")) {
            const float y = real("cameraYaw", 0) * 0.5f, p = real("cameraPitch", 0) * 0.5f, r = real("cameraRoll", 0) * 0.5f;
            const float cy = std::cos(y), sy = std::sin(y), cp = std::cos(p), sp = std::sin(p), cr = std::cos(r), sr = std::sin(r);
            w->scene.orientation = {cy * sp * cr + sy * cp * sr, sy * cp * cr - cy * sp * sr, cy * cp * sr - sy * sp * cr, cy * cp * cr + sy * sp * sr};
        }
        w->scene.clear = {real("clearColorR", 0) / 255, real("clearColorG", 0) / 255, real("clearColorB", 0) / 255, real("clearColorA", 255) / 255};
    }
    w->consumeAllKeys = boolAttr(n, "consumeAllKeyboardEvents", false);
    w->allowIme = boolAttr(n, "allowIME", false);
    w->scaleToFit = boolAttr(n, "scaleToFit", false);
    w->lengthLimit = intAttr(n, "lengthLimit", 0);
    if (n.hasAttribute("InputValidator")) setValidator(w.get(), n.attribute("InputValidator"));   // the loader FUN_008cebb0 (no exported box sets one)
    w->textureDef = n.attribute("textureDef");
    w->opaqueClick = boolAttr(n, "opaqueClick", false);   // the Bitmap loader FUN_008f4070 (+ 0xE0)
    if (n.hasAttribute("percentComplete")) w->percentComplete = float(std::strtod(n.attribute("percentComplete").c_str(), nullptr));
    w->rangeMin = intAttr(n, "minRange", 0);
    w->rangeMax = intAttr(n, "maxRange", 0);
    w->pageSize = intAttr(n, "pageSize", 0);
    w->value = intAttr(n, "currentValue", 0);
    if (kind == Kind::ScrollBar) w->horizontal = n.attribute("style") == "horizontal";   // the loader FUN_008d6ce0
    // Slider (loader FUN_008da640): style "horizontal" (the default, + 0xC0 = 0) or vertical.
    if (kind == Kind::Slider) w->horizontal = !n.hasAttribute("style") || n.attribute("style") == "horizontal";
    if (isComposite(kind))
        for (const tr::XmlNode& c : n.children)
            if (auto child = build(c, w.get())) w->children.push_back(std::move(child));
    if (isComposite(kind) && n.hasAttribute("enabled")) setEnabled(w.get(), boolAttr(n, "enabled", false));
    if (kind == Kind::ScrollBar) layoutScrollThumb(w.get());   // the loader ends with SetValue(currentValue)
    // ComboBox (loader FUN_008f0ee0): its Button, Text and DropDown are added as children, its Entry is kept only as the pattern the
    // drop-down's items are copied from (FUN_008e2c80) - never drawn or hit.
    if (kind == Kind::ComboBox)
        for (const auto& c : w->children)
            if (c->tag == "Entry") c->visible = false;
    return w;
}

void Manager::runScripts(Widget* w) {
    // Children first (a window's script runs once its children are loaded).
    for (const auto& c : w->children) runScripts(c.get());
    if (w->kind != Kind::Window || !w->source) return;
    std::string script = w->source->attribute("script");
    if (script.empty()) return;

    std::string moduleName = scriptPackage_ + "." + script;
    PyObject* module = PyImport_ImportModule(const_cast<char*>(moduleName.c_str()));
    if (!module) {
        PyErr_Print();
        std::fprintf(stderr, "[ui] Failed to load script [%s]\n", moduleName.c_str());
        return;
    }
    PyObject* cls = PyObject_GetAttrString(module, const_cast<char*>(script.c_str()));
    Py_DECREF(module);
    if (!cls) {
        PyErr_Print();
        std::fprintf(stderr, "[ui] Failed to find the [%s] class in script [%s]\n", script.c_str(), moduleName.c_str());
        return;
    }
    PyObject* instance = PyClass_Check(cls) ? PyInstance_New(cls, nullptr, nullptr) : PyObject_CallObject(cls, nullptr);
    Py_DECREF(cls);
    if (!instance) {
        PyErr_Print();
        std::fprintf(stderr, "[ui] Failed to create class [%s] from script [%s]\n", script.c_str(), moduleName.c_str());
        return;
    }
    w->script = instance;
    PyObject* self = pythonOf(w);
    if (PyObject_SetAttrString(instance, const_cast<char*>("window"), self) < 0) {
        PyErr_Print();
        std::fprintf(stderr, "[ui] Failed to insert 'window' attribute from script [%s]\n", script.c_str());
    }
    Py_DECREF(self);
    PyObject* r = PyObject_CallMethod(instance, const_cast<char*>("Init"), nullptr);
    if (!r) {
        PyErr_Print();
        std::fprintf(stderr, "[ui] Init() call on script failed (%s)\n", moduleName.c_str());
        return;
    }
    Py_DECREF(r);
}

void Manager::createRoot(bool wide) {
    wideScreen = wide;
    root_ = std::make_unique<Widget>();
    root_->kind = Kind::Window;   // the root takes Window methods (gameui.Initialize: RegisterForResolutionUpdate)
    root_->tag = "RootWindow";
    root_->uiid = "RootWindow";
    root_->width = wide ? 1706 : 1280;
    root_->height = 960;
    root_->enabled = true;
}

bool Manager::loadLayout() {
    if (!root_) createRoot(wideScreen);
    std::ifstream in("data/xml/Tabula_Rasa_UI_EXPORT.xml", std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "[ui] Failed to find XML file to load data/xml/Tabula_Rasa_UI_EXPORT.xml\n");
        return false;
    }
    tr::Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        export_ = tr::readUiExport(bytes);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[ui] Failed to load or parse XML: %s\n", e.what());
        return false;
    }
    for (const tr::XmlNode& c : export_.widgets.children)
        if (auto w = build(c, root_.get())) root_->children.push_back(std::move(w));
    runScripts(root_.get());
    // FUN_004cd8c0 then adds a MovieComposite named "MoviePlayer" to the root (the movie player's widget): resizeToParent (slot 76,
    // FUN_0089d4f0 sets + 0x44), hidden (slot 65). Its loader (FUN_008f35c0) makes its two children: a ColoredRect of colour
    // 0xFF000000 (black, opaque; slot 0x110 with DAT_00d08b38) the size of the composite, then a Movie centred in it (child and parent
    // anchor 8, resizeToParent). A movie shows them all (see MoviePlayer.h).
    auto composite = std::make_unique<Widget>();
    composite->kind = Kind::MovieComposite;
    composite->tag = "MovieComposite";
    composite->uiid = "MoviePlayer";
    composite->parent = root_.get();
    composite->resizeToParent = true;
    composite->visible = false;
    auto backdrop = std::make_unique<Widget>();
    backdrop->kind = Kind::ColoredRect;
    backdrop->tag = "ColoredRect";
    backdrop->r = backdrop->g = backdrop->b = 0;
    backdrop->resizeToParent = true;
    backdrop->visible = false;
    backdrop->parent = composite.get();
    auto movie = std::make_unique<Widget>();
    movie->kind = Kind::Movie;
    movie->tag = "Movie";
    movie->childAnchor = movie->parentAnchor = 8;
    movie->resizeToParent = true;
    movie->visible = false;
    movie->parent = composite.get();
    composite->children.push_back(std::move(backdrop));
    composite->children.push_back(std::move(movie));
    root_->children.push_back(std::move(composite));
    return true;
}

// The template map (UIManager + 8, filled by LoadXMLTemplateWidgets 0x00897cf0) is a hash map keyed ignoring case
// (2007 corHashTraitsStringNoCase); a later template of the same uiid replaces (and deletes) the earlier one - the export has two
// MissionRewardCredits, and conversationwindow.py reads the second's Label.
const tr::XmlNode* Manager::findTemplate(const std::string& templateName) const {
    const tr::XmlNode* found = nullptr;
    for (const tr::XmlNode& t : export_.templates.children) {
        const std::string& id = t.attribute("uiid");
        if (id.size() != templateName.size()) continue;
        bool same = true;
        for (size_t i = 0; i < id.size() && same; ++i)
            same = std::tolower(static_cast<unsigned char>(id[i])) == std::tolower(static_cast<unsigned char>(templateName[i]));
        if (same) found = &t;
    }
    return found;
}

// WidgetComposite::InsertTemplateWidget (retail 0x008af6a0, 2007 0x0088f740): no template -> null (the Python binding returns
// None, no exception). Placement by `where`: null appends (InsertChild(child), slot 0x1d8); the composite itself puts it first
// (InsertChild(child, 0)); another widget puts it right after that sibling, or first when it is not a child (2007 0x0088ed60).
Widget* Manager::instantiateTemplate(const std::string& templateName, const std::string& newName, Widget* parent, Widget* where) {
    const tr::XmlNode* t = findTemplate(templateName);
    if (!t) return nullptr;
    dirty = true;
    std::unique_ptr<Widget> w = build(*t, parent);
    if (!w) return nullptr;
    w->uiid = newName;
    Widget* raw = w.get();
    auto& kids = parent->children;
    if (!where) {
        kids.push_back(std::move(w));
    } else {
        auto at = kids.begin();
        if (where != parent)
            for (auto it = kids.begin(); it != kids.end(); ++it)
                if (it->get() == where) { at = it + 1; break; }
        kids.insert(at, std::move(w));
    }
    runScripts(raw);
    return raw;
}

std::unique_ptr<Widget> Manager::buildTemplate(const std::string& templateName, Widget* parent) {
    const tr::XmlNode* t = findTemplate(templateName);
    return t ? build(*t, parent) : nullptr;
}

void Manager::detachLater(Widget* w) {
    dirty = true;
    if (!w || !w->parent) return;
    auto& siblings = w->parent->children;
    for (auto it = siblings.begin(); it != siblings.end(); ++it)
        if (it->get() == w) {
            doomed.push_back(std::move(*it));
            siblings.erase(it);
            w->parent = nullptr;
            return;
        }
}

// UIManager::DestroyWidget (2007 0x008991a0; WidgetComposite::DeleteTemplateWidget and the engine's own deletes): a widget already on the
// deferred list is left alone; otherwise its modal / keyboard focus / mouse capture / hover / animation references are dropped now, it is
// taken off its parent and put on the deferred delete list - deleted later (retail UIManager + 0x54, FUN_00896720), so its Python handle
// keeps working until then. statusupdaterwindow.py relies on it: an indicator's CleanUp deletes its widgets, then UpdateIndicatorLayout ->
// Reset still calls Enable / SetVisible on them; deleting at once raised "AttributeError: Enable" (retail's ~Widget does empty the
// handle's method table, PyUIObject_Invalidate - but only once the widget is really deleted).
void Manager::destroy(Widget* w) {
    dirty = true;
    if (!w || !w->parent) return;
    forget(w);
    detachLater(w);
}

// ---- Layout (FUN_008b6fb0, FUN_008b7060; verified against the game in RasaEdit's UiCanvas) ----------------------------------------
static void anchorPoint(int anchor, int w, int h, int& x, int& y) {
    switch (anchor) {
        case 1: x = w / 2; y = 0; break;
        case 2: x = w; y = 0; break;
        case 3: x = w; y = h / 2; break;
        case 4: x = w; y = h; break;
        case 5: x = w / 2; y = h; break;
        case 6: x = 0; y = h; break;
        case 7: x = 0; y = h / 2; break;
        case 8: x = w / 2; y = h / 2; break;
        default: x = 0; y = 0;
    }
}

static void sizeOf(const Widget* w, int& width, int& height) {
    width = w->width;
    height = w->height;
    const Widget* parent = layoutParentOf(w);
    if (!parent) return;
    int pw, ph;
    sizeOf(parent, pw, ph);
    if (w->resizeToParent) {
        width = pw;
        height = ph;
    }   // full-width windows on a wide root: the scripts set ResizeToParent (frontend.py, hud.py, ...)
}

Rect Manager::screenRect(const Widget* w) const {
    int width, height;
    sizeOf(w, width, height);
    const Widget* parent = layoutParentOf(w);
    if (!parent) return {0, 0, width, height};
    Rect p = screenRect(parent);
    int px, py, cx, cy;
    anchorPoint(w->parentAnchor, p.right - p.left, p.bottom - p.top, px, py);
    anchorPoint(w->childAnchor, width, height, cx, cy);
    int x = p.left + px + w->offsetX - cx;
    int y = p.top + py + w->offsetY - cy;
    return {x, y, x + width, y + height};
}

// Where the widget is drawn and hit: its layout rectangle moved by every ancestor's SetOffset (the composite render adds it to the
// translation, FUN_008aee70; the composite hit test takes it off the point, FUN_008af240 via FUN_008aede0).
Rect Manager::drawnRect(const Widget* w) const {
    Rect r = screenRect(w);
    for (const Widget* p = w->parent; p; p = p->parent) {
        r.left += p->scrollX;
        r.right += p->scrollX;
        r.top += p->scrollY;
        r.bottom += p->scrollY;
    }
    return r;
}

// An edit box being hidden, or a visible one whose ancestor is (its SetVisible FUN_008cf750), drops the "TextEntry" cursor.
bool holdsVisibleEditBox(const Widget* w, bool self) {
    if (!self && !w->visible) return false;
    if (w->kind == Kind::EditBox || w->kind == Kind::PasswordBox || w->kind == Kind::MultiLineEditBox) return true;
    for (const auto& c : w->children)
        if (holdsVisibleEditBox(c.get(), false)) return true;
    return false;
}

void setVisible(Widget* w, bool visible) {
    if (w->visible == visible) return;
    if (!visible && holdsVisibleEditBox(w, true)) cursor::pop("TextEntry");
    w->visible = visible;
    if (visible && w->autoRestartAnims) restartAnimations(w, true, true, true);
}

void restartAnimations(Widget* w, bool widgetAnims, bool bitmapAnims, bool recursive) {
    // A restart is the player's PlayAnim(handle, 0) (FUN_0088d3a0): time 0, the last applied -1.
    if (widgetAnims) w->widgetAnimStart = manager().clockMs, w->widgetAnimLast = -1, restartDefaultPlayer(w, false);
    if (bitmapAnims) w->bitmapAnimStart = manager().clockMs, w->bitmapAnimLast = -1, restartDefaultPlayer(w, true);
    if (recursive)
        for (const auto& c : w->children) restartAnimations(c.get(), widgetAnims, bitmapAnims, true);
}

void Manager::forget(Widget* w) {
    focusStack.erase(std::remove(focusStack.begin(), focusStack.end(), w), focusStack.end());
    captureStack.erase(std::remove(captureStack.begin(), captureStack.end(), w), captureStack.end());
    if (hovered == w) hovered = nullptr;
    if (imeBox == w) imeBox = nullptr;
    repeatingBars.erase(std::remove(repeatingBars.begin(), repeatingBars.end(), w), repeatingBars.end());
    repeatableButtons.erase(std::remove(repeatableButtons.begin(), repeatableButtons.end(), w), repeatableButtons.end());
    modalStack.erase(std::remove(modalStack.begin(), modalStack.end(), w), modalStack.end());
    forgetAnimPlayers(w);
    // SetParent's parent going: the exe keeps the pointer (+ 0x48); here the widgets fall back to their real parents.
    reparented.erase(w);
    defaultAnimated.erase(w);
    for (Widget* r : reparented)
        if (r->layoutParent == w) r->layoutParent = nullptr;
    // The exe keeps raw pointers here (a widget deleted while dragged over would be called through); here they are cleared.
    if (dndWindow == w) dndWindow = nullptr;
    if (imeWindow == w) imeWindow = nullptr;
    dndWidgets.erase(std::remove(dndWidgets.begin(), dndWidgets.end(), w), dndWidgets.end());
    for (Widget* d : dndWidgets) {
        if (d->dndSource == w) d->dndSource = nullptr;
        if (d->dndTarget == w) d->dndTarget = nullptr;
        if (d->dndVisual == w) d->dndVisual = nullptr;
    }
}

std::string Manager::visibleWindows() const {
    std::string out;
    std::function<void(const Widget*, const std::string&)> walk = [&](const Widget* w, const std::string& path) {
        if (!w->visible) return;
        std::string p = path.empty() ? w->uiid : path + "/" + w->uiid;
        if (w->kind == Kind::Window) {
            Rect r = screenRect(w);
            out += "  (" + std::to_string(r.left) + ", " + std::to_string(r.top) + ") - (" + std::to_string(r.right) + ", " +
                   std::to_string(r.bottom) + ")  " + p + "\n";
        }
        for (const auto& c : w->children) walk(c.get(), p);
    };
    if (root_) walk(root_.get(), "");
    return out;
}

bool Manager::colour(const std::string& id, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const {
    auto it = export_.colours.find(id);
    if (it == export_.colours.end()) return false;
    r = it->second.r;
    g = it->second.g;
    b = it->second.b;
    a = it->second.a;
    return true;
}

}  // namespace engine::ui
