// UiPython.cpp - the UI's Python face: widget objects (PyUIObject), gameclient.GetUIManager() / GetGameUIManager() and the uievent
// modules, as tabula_rasa.exe provides them (traced 2026-10-24; see tabula_rasa_python_api.md "UI widgets").
//
// PyUIObject (the exe's classic type at 0x00D13070, tp_name "PyUIObject"): attribute access looks for a CHILD of that uiid first,
// then a method of the widget's class chain (Py_FindMethodInChain); a subscript (string keys only) finds a child, else raises
// '"<name>" not found for widget "<uiid>"'. repr "<PyUIObject type=<kind> @0x...>". A destroyed widget's object keeps no methods.
// Methods the exe has that are not implemented here yet are callable, logged once and return None (unimplementedCallReport), so
// the scripts run on while the engine is filled in.
#include <cmath>
#include "UiManager.h"
#include "PyProfile.h"
#include "World.h"
#include "Player.h"
#include "EntityBody.h"

#include "FontMetrics.h"
#include "UiRender.h"
#include "Stub.h"
#include "Assets.h"
#include "Audio.h"
#include "MoviePlayer.h"
#include "Cursor.h"
#include "Engine.h"

#include "assetlibrary.h"
#include "dds.h"

#include <algorithm>
#include <array>
#include <memory>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>

namespace engine::ui {
extern PyObject* cppLookup;   // pythonInit
namespace {

long defaultAnimPlayer(Widget* w, bool bitmap);   // see the AnimManager below

struct PyUIObject {
    PyObject_HEAD
    Widget* widget;
};

PyTypeObject uiObjectType = {PyObject_HEAD_INIT(nullptr) 0};

std::map<std::string, int> unimplementedCalls;   // "Kind.Method" -> count

// ---- Argument and result helpers ----------------------------------------------------------------------------------------------
Widget* self(PyObject* o) {
    if (!o || o->ob_type != &uiObjectType) {
        PyErr_SetString(PyExc_TypeError, "Object is not a PyUIObject!");
        return nullptr;
    }
    Widget* w = reinterpret_cast<PyUIObject*>(o)->widget;
    if (!w) PyErr_SetString(PyExc_RuntimeError, "The widget has been destroyed");
    return w;
}

PyObject* none() {
    Py_INCREF(Py_None);
    return Py_None;
}
PyObject* boolean(bool b) { return PyBool_FromLong(b ? 1 : 0); }

std::u16string toText(PyObject* o) {
    std::u16string out;
    if (PyUnicode_Check(o)) {
        Py_UNICODE* u = PyUnicode_AS_UNICODE(o);
        int n = PyUnicode_GET_SIZE(o);
        for (int i = 0; i < n; ++i) out += char16_t(u[i]);
    } else if (PyString_Check(o)) {
        const char* s = PyString_AS_STRING(o);
        int n = PyString_GET_SIZE(o);
        for (int i = 0; i < n; ++i) out += char16_t(static_cast<unsigned char>(s[i]));
    } else {
        PyObject* s = PyObject_Unicode(o);
        if (s) {
            out = toText(s);
            Py_DECREF(s);
        } else {
            PyErr_Clear();
        }
    }
    return out;
}

PyObject* fromText(const std::u16string& t) {
    static_assert(sizeof(Py_UNICODE) == 2 || sizeof(Py_UNICODE) == 4);
    std::basic_string<Py_UNICODE> u(t.begin(), t.end());
    return PyUnicode_FromUnicode(u.data(), int(u.size()));
}

PyObject* pair(int a, int b) { return Py_BuildValue(const_cast<char*>("(ii)"), a, b); }

// ---- Widget (the exe's table at 0x00d03c08) --------------------------------------------------------------------------------------
#define WIDGET(o)              \
    Widget* w = self(o);       \
    if (!w) return nullptr;
#define PARSE(fmt, ...)                                                  \
    if (!PyArg_ParseTuple(args, const_cast<char*>(fmt), __VA_ARGS__)) return nullptr;

PyObject* wSetVisible(PyObject* o, PyObject* args) { WIDGET(o) int v; PARSE("i:SetVisible", &v) setVisible(w, v != 0); return none(); }
PyObject* wRestartDefaultWidgetAnim(PyObject* o, PyObject* args) {
    WIDGET(o) unsigned char recursive = 0;
    PARSE("|b:RestartDefaultWidgetAnim", &recursive)
    restartAnimations(w, true, false, recursive != 0);
    return none();
}
PyObject* wRestartDefaultBitmapAnim(PyObject* o, PyObject* args) {
    WIDGET(o) unsigned char recursive = 0;
    PARSE("|b:RestartDefaultBitmapAnim", &recursive)
    restartAnimations(w, false, true, recursive != 0);
    return none();
}
PyObject* wAutoRestartAnims(PyObject* o, PyObject* args) {
    WIDGET(o) unsigned char v = 0;
    PARSE("b:AutoRestartAnims", &v)
    w->autoRestartAnims = v != 0;
    return none();
}
PyObject* wStartDefaultWidgetAnimOnLastFrame(PyObject* o, PyObject* args) {
    unsigned char flag = 0;
    PARSE("b:StartDefaultWidgetAnimOnLastFrame", &flag)
    (void)o;
    manager().widgetAnimsOnLastFrame = flag != 0;
    return none();
}
PyObject* wIsVisible(PyObject* o, PyObject*) { WIDGET(o) return boolean(w->visible); }
PyObject* wSetColor(PyObject* o, PyObject* args) {
    WIDGET(o) int r, g, b, a;
    PARSE("(iiii):SetColor", &r, &g, &b, &a)
    w->r = uint8_t(r), w->g = uint8_t(g), w->b = uint8_t(b), w->a = uint8_t(a);
    return none();
}
PyObject* wSetColorByID(PyObject* o, PyObject* args) {
    WIDGET(o) char* id;
    PARSE("s:SetColorByID", &id)
    // Widget::SetColorByID (0x0089d380): UIManager::GetColor (slot 6) then SetColor with the widget's OWN alpha kept (MOV CL, [ESI + 0x4f]
    // into the colour's alpha byte) - the definition's A is not used. The bark background stays at its alpha 100 (translucent).
    uint8_t a = w->a;
    manager().colour(id, w->r, w->g, w->b, a);
    return none();
}
PyObject* colourTuple(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { return Py_BuildValue(const_cast<char*>("(iiii)"), r, g, b, a); }
PyObject* wGetColor(PyObject* o, PyObject* args) {
    WIDGET(o) char* id = nullptr;
    PARSE("|s:GetColor", &id)
    if (id) {
        uint8_t r = 255, g = 255, b = 255, a = 255;
        manager().colour(id, r, g, b, a);
        return colourTuple(r, g, b, a);
    }
    return colourTuple(w->r, w->g, w->b, w->a);
}
PyObject* wSetAlpha(PyObject* o, PyObject* args) { WIDGET(o) int a; PARSE("i:SetAlpha", &a) w->a = uint8_t(a); return none(); }
PyObject* wGetAlpha(PyObject* o, PyObject*) { WIDGET(o) return PyInt_FromLong(w->a); }
PyObject* wSetAnchorOffset(PyObject* o, PyObject* args) { WIDGET(o) PARSE("ii:SetAnchorOffset", &w->offsetX, &w->offsetY) return none(); }
PyObject* wGetAnchorOffset(PyObject* o, PyObject*) { WIDGET(o) return pair(w->offsetX, w->offsetY); }
PyObject* wSetChildAnchor(PyObject* o, PyObject* args) { WIDGET(o) PARSE("i:SetChildAnchor", &w->childAnchor) return none(); }
PyObject* wGetChildAnchor(PyObject* o, PyObject*) { WIDGET(o) return PyInt_FromLong(w->childAnchor); }
PyObject* wSetParentAnchor(PyObject* o, PyObject* args) { WIDGET(o) PARSE("i:SetParentAnchor", &w->parentAnchor) return none(); }
PyObject* wGetParentAnchor(PyObject* o, PyObject*) { WIDGET(o) return PyInt_FromLong(w->parentAnchor); }
PyObject* wSetSize(PyObject* o, PyObject* args) { WIDGET(o) PARSE("ii:SetSize", &w->width, &w->height) return none(); }
PyObject* wGetSize(PyObject* o, PyObject*) {
    WIDGET(o) Rect r = manager().screenRect(w);
    return pair(r.right - r.left, r.bottom - r.top);
}
PyObject* wSetWidth(PyObject* o, PyObject* args) { WIDGET(o) PARSE("i:SetWidth", &w->width) return none(); }
PyObject* wSetHeight(PyObject* o, PyObject* args) { WIDGET(o) PARSE("i:SetHeight", &w->height) return none(); }
PyObject* wResizeToParent(PyObject* o, PyObject* args) { WIDGET(o) int v; PARSE("i:ResizeToParent", &v) w->resizeToParent = v != 0; return none(); }
PyObject* wGetScreenRect(PyObject* o, PyObject*) {
    WIDGET(o) Rect r = manager().screenRect(w);
    return Py_BuildValue(const_cast<char*>("(iiii)"), r.left, r.top, r.right, r.bottom);
}
PyObject* wEnable(PyObject* o, PyObject* args) { WIDGET(o) int v; PARSE("i:Enable", &v) setEnabled(w, v != 0); return none(); }
PyObject* wIsEnabled(PyObject* o, PyObject*) { WIDGET(o) return boolean(w->enabled); }
PyObject* wSetPickable(PyObject* o, PyObject* args) { WIDGET(o) int v; PARSE("i:SetPickable", &v) w->pickable = v != 0; return none(); }
PyObject* wIsPickable(PyObject* o, PyObject*) { WIDGET(o) return boolean(w->pickable); }
PyObject* wGetUIManager(PyObject* o, PyObject*) { WIDGET(o) (void)w; return uiManagerObject(); }
PyObject* wGetParent(PyObject* o, PyObject*) { WIDGET(o) return layoutParentOf(w) ? pythonOf(layoutParentOf(w)) : none(); }
// SetParent(widget) (0x0089EE30, "O:SetParent") -> slot 63 (FUN_0089d2b0): the parent fields + 0x48 and + 4 set to it, invalidated and
// laid out again. The children lists are not touched - the widget stays in (and is drawn by) its old parent, placed by the new one. A
// non-UI argument raises TypeError "Object is not a PyUIObject!" (the exe then calls through a null pointer).
PyObject* wSetParent(PyObject* o, PyObject* args) {
    WIDGET(o) PyObject* p = nullptr;
    PARSE("O:SetParent", &p)
    if (!p || p->ob_type != &uiObjectType) {
        PyErr_SetString(PyExc_TypeError, "Object is not a PyUIObject!");
        return nullptr;
    }
    Widget* parent = reinterpret_cast<PyUIObject*>(p)->widget;
    if (!parent) return nullptr;
    w->layoutParent = parent == w->parent ? nullptr : parent;
    if (w->layoutParent) manager().reparented.insert(w);
    else manager().reparented.erase(w);
    manager().dirty = true;
    return none();
}
PyObject* wGetID(PyObject* o, PyObject*) { WIDGET(o) return PyString_FromString(w->uiid.c_str()); }
PyObject* wSetID(PyObject* o, PyObject* args) { WIDGET(o) char* s; PARSE("s:SetID", &s) w->uiid = s; return none(); }
PyObject* wGetEffect(PyObject* o, PyObject*) { WIDGET(o) return PyString_FromString(w->effect.c_str()); }
PyObject* wSetEffect(PyObject* o, PyObject* args) { WIDGET(o) char* s; PARSE("s:SetEffect", &s) w->effect = s; return none(); }
PyObject* wSetKeyboardFocus(PyObject* o, PyObject* args) {
    WIDGET(o) int v;
    PARSE("i:SetKeyboardFocus", &v)
    if (v) pushFocus(w);
    else removeFocus(w);
    return none();
}
PyObject* wSetMouseCapture(PyObject* o, PyObject* args) {
    WIDGET(o) int v;
    PARSE("i:SetMouseCapture", &v)
    if (v) pushCapture(w);
    else releaseCapture(w);
    return none();
}
void keep(std::multimap<int, PyObject*>& handlers, int key, PyObject* callback) {   // the exe keeps every registration
    Py_INCREF(callback);
    handlers.emplace(key, callback);
}
void keep(std::map<int, PyObject*>& handlers, int key, PyObject* callback) {
    Py_INCREF(callback);
    auto [it, added] = handlers.emplace(key, callback);
    if (!added) {
        Py_DECREF(it->second);
        it->second = callback;
    }
}
PyObject* wRegisterMessage(PyObject* o, PyObject* args) {
    WIDGET(o) int type;
    PyObject* cb;
    PARSE("iO:RegisterPythonMessageEvent", &type, &cb)
    keep(w->messageHandlers, type, cb);
    return none();
}
PyObject* wUnRegisterMessage(PyObject* o, PyObject* args) {
    WIDGET(o) int type = -1;
    PARSE("|i:UnRegisterPythonMessageEvent", &type)
    for (auto it = w->messageHandlers.begin(); it != w->messageHandlers.end();) {
        if (type == -1 || it->first == type) {
            Py_DECREF(it->second);
            it = w->messageHandlers.erase(it);
        } else {
            ++it;
        }
    }
    return none();
}
PyObject* wRegisterKeyboard(PyObject* o, PyObject* args) {
    WIDGET(o) char* key;
    KeyHandler h;
    PARSE("siiO:RegisterPythonKeyboardEvent", &key, &h.eventType, &h.modifiers, &h.callback)
    h.key = key;
    h.keyCode = keyCodeForName(key);   // FUN_008dddf0: the name's scan code
    if (!h.keyCode) std::fprintf(stderr, "[ui] RegisterPythonKeyboardEvent: unknown key name %s\n", key);
    Py_INCREF(h.callback);
    w->keyHandlers.push_back(h);
    return none();
}
PyObject* wRegisterMouse(PyObject* o, PyObject* args) {
    WIDGET(o) MouseHandler h;
    PARSE("iiO:RegisterPythonMouseEvent", &h.buttons, &h.eventType, &h.callback)   // (button, event type, callable): gameui.py
    Py_INCREF(h.callback);
    w->mouseHandlers.push_back(h);
    return none();
}
PyObject* wRegisterAnim(PyObject* o, PyObject* args) {
    WIDGET(o) int type;
    PyObject* cb;
    PARSE("iO:RegisterPythonAnimEvent", &type, &cb)
    keep(w->animHandlers, type, cb);
    return none();
}
PyObject* wRegisterIME(PyObject* o, PyObject* args) {
    WIDGET(o) int type;
    PyObject* cb;
    PARSE("iO:RegisterPythonIMEEvent", &type, &cb)
    keep(w->imeHandlers, type, cb);
    return none();
}
PyObject* wRegisterAny(PyObject* o, PyObject* args) {
    WIDGET(o) PyObject* cb;
    PARSE("O:RegisterPythonAnyEvent", &cb)
    Py_INCREF(cb);
    Py_XDECREF(w->anyHandler);
    w->anyHandler = cb;
    return none();
}

#define M(name, fn, flags) {const_cast<char*>(name), reinterpret_cast<PyCFunction>(fn), flags, nullptr}
PyObject* wSetModal(PyObject* o, PyObject* args) {
    WIDGET(o) char v;
    PARSE("b:SetModal", &v)
    setModal(w, v != 0);
    return none();
}
// GetDefaultWidgetAnimId() (0x0089F410): the handle of the widget's default animation player (Widget slot 90 - the player its
// widgetAnimName made, see the AnimManager), None when it has none (-1). GetDefaultBitmapAnimId() (0x008F6080): the same for a Bitmap's
// bitmapAnimName (UIBitmap slot 137). overheadwindow.py HandleDisplayName passes it to PlayAnim / StopAnim.
PyObject* wGetDefaultWidgetAnimId(PyObject* o, PyObject*) {
    WIDGET(o) const long id = defaultAnimPlayer(w, false);
    return id ? PyInt_FromLong(id) : none();
}
PyObject* bGetDefaultBitmapAnimId(PyObject* o, PyObject*) {
    WIDGET(o) const long id = defaultAnimPlayer(w, true);
    return id ? PyInt_FromLong(id) : none();
}
PyMethodDef widgetMethods[] = {
    M("GetDefaultWidgetAnimId", wGetDefaultWidgetAnimId, METH_NOARGS),
    M("SetModal", wSetModal, METH_VARARGS),
    M("SetVisible", wSetVisible, METH_VARARGS), M("IsVisible", wIsVisible, METH_NOARGS),
    M("RestartDefaultWidgetAnim", wRestartDefaultWidgetAnim, METH_VARARGS),
    M("RestartDefaultBitmapAnim", wRestartDefaultBitmapAnim, METH_VARARGS), M("AutoRestartAnims", wAutoRestartAnims, METH_VARARGS),
    M("StartDefaultWidgetAnimOnLastFrame", wStartDefaultWidgetAnimOnLastFrame, METH_VARARGS), M("SetColor", wSetColor, METH_VARARGS),
    M("SetColorByID", wSetColorByID, METH_VARARGS), M("GetColor", wGetColor, METH_VARARGS), M("SetAlpha", wSetAlpha, METH_VARARGS),
    M("GetAlpha", wGetAlpha, METH_NOARGS), M("SetAnchorOffset", wSetAnchorOffset, METH_VARARGS),
    M("GetAnchorOffset", wGetAnchorOffset, METH_NOARGS), M("SetChildAnchor", wSetChildAnchor, METH_VARARGS),
    M("GetChildAnchor", wGetChildAnchor, METH_NOARGS), M("SetParentAnchor", wSetParentAnchor, METH_VARARGS),
    M("GetParentAnchor", wGetParentAnchor, METH_NOARGS), M("SetParent", wSetParent, METH_VARARGS), M("SetSize", wSetSize, METH_VARARGS), M("GetSize", wGetSize, METH_NOARGS),
    M("SetWidth", wSetWidth, METH_VARARGS), M("SetHeight", wSetHeight, METH_VARARGS), M("ResizeToParent", wResizeToParent, METH_VARARGS),
    M("GetScreenRect", wGetScreenRect, METH_NOARGS), M("Enable", wEnable, METH_VARARGS), M("IsEnabled", wIsEnabled, METH_NOARGS),
    M("SetPickable", wSetPickable, METH_VARARGS), M("IsPickable", wIsPickable, METH_NOARGS), M("GetUIManager", wGetUIManager, METH_NOARGS),
    M("GetParent", wGetParent, METH_NOARGS), M("GetID", wGetID, METH_NOARGS), M("SetID", wSetID, METH_VARARGS),
    M("GetEffect", wGetEffect, METH_NOARGS), M("SetEffect", wSetEffect, METH_VARARGS),
    M("SetKeyboardFocus", wSetKeyboardFocus, METH_VARARGS), M("SetMouseCapture", wSetMouseCapture, METH_VARARGS),
    M("RegisterPythonMessageEvent", wRegisterMessage, METH_VARARGS), M("RegisterPythonKeyboardEvent", wRegisterKeyboard, METH_VARARGS),
    M("RegisterPythonMouseEvent", wRegisterMouse, METH_VARARGS), M("RegisterPythonAnimEvent", wRegisterAnim, METH_VARARGS),
    M("RegisterPythonIMEEvent", wRegisterIME, METH_VARARGS), M("RegisterPythonAnyEvent", wRegisterAny, METH_VARARGS),
    M("UnRegisterPythonMessageEvent", wUnRegisterMessage, METH_VARARGS), {nullptr, nullptr, 0, nullptr}};

// ---- WidgetComposite (0x00d039d0) and Window (0x00d034a0) --------------------------------------------------------------------
PyObject* cSetOffset(PyObject* o, PyObject* args) { WIDGET(o) PARSE("ii:SetOffset", &w->scrollX, &w->scrollY) return none(); }
PyObject* cGetScaledOffset(PyObject* o, PyObject*) { WIDGET(o) return pair(w->scrollX, w->scrollY); }
PyObject* cInsertTemplateWidget(PyObject* o, PyObject* args) {
    WIDGET(o) char* templateName;
    char* newName;
    PyObject* where = nullptr;
    PARSE("ssO:InsertTemplateWidget", &templateName, &newName, &where)
    // pyInsertTemplateWidget (retail 0x008afc70): a missing template gives None, not an error (directionalhitwindow.py asks for
    // DirectionalHitIndicator_Miss, which the export lacks, and checks for None).
    Widget* at = nullptr;
    if (where != Py_None) {
        at = self(where);
        if (!at) return nullptr;
    }
    Widget* made = manager().instantiateTemplate(templateName, newName, w, at);
    if (!made) return none();
    return pythonOf(made);
}
PyObject* cDeleteTemplateWidget(PyObject* o, PyObject* args) {
    WIDGET(o) PyObject* target;
    PARSE("O:DeleteTemplateWidget", &target)
    Widget* t = self(target);
    if (!t) return nullptr;
    if (t->parent == w) manager().destroy(t);
    return none();
}
PyObject* cReverseWidgetOrder(PyObject* o, PyObject*) {
    WIDGET(o) std::reverse(w->children.begin(), w->children.end());
    return none();
}
PyObject* cGetChildren(PyObject* o, PyObject*) {
    WIDGET(o) PyObject* list = PyList_New(0);
    for (const auto& c : w->children) {
        PyObject* p = pythonOf(c.get());
        PyList_Append(list, p);
        Py_DECREF(p);
    }
    return list;
}
PyObject* cSetCaptureMouseInput(PyObject* o, PyObject* args) { WIDGET(o) int v; PARSE("i:SetCaptureMouseInput", &v) w->mouseCapture = v != 0; return none(); }
PyObject* cGetCaptureMouseInput(PyObject* o, PyObject*) { WIDGET(o) return boolean(w->mouseCapture); }
PyMethodDef compositeMethods[] = {
    M("SetOffset", cSetOffset, METH_VARARGS), M("GetScaledOffset", cGetScaledOffset, METH_NOARGS),
    M("InsertTemplateWidget", cInsertTemplateWidget, METH_VARARGS), M("DeleteTemplateWidget", cDeleteTemplateWidget, METH_VARARGS),
    M("ReverseWidgetOrder", cReverseWidgetOrder, METH_NOARGS), M("GetChildren", cGetChildren, METH_NOARGS),
    M("SetCaptureMouseInput", cSetCaptureMouseInput, METH_VARARGS), M("GetCaptureMouseInput", cGetCaptureMouseInput, METH_NOARGS),
    {nullptr, nullptr, 0, nullptr}};

PyObject* winGetScript(PyObject* o, PyObject*) {
    WIDGET(o) if (!w->script) return none();
    Py_INCREF(w->script);
    return w->script;
}
PyObject* winRegisterForUpdate(PyObject* o, PyObject* args) {
    WIDGET(o) PyObject* cb;
    PARSE("O:RegisterForUpdate", &cb)
    Py_INCREF(cb);
    Py_XDECREF(w->updateCallback);
    w->updateCallback = cb;
    return none();
}
PyObject* winRegisterForResolutionUpdate(PyObject* o, PyObject* args) {
    WIDGET(o) PyObject* cb;
    PARSE("O:RegisterForResolutionUpdate", &cb)
    Py_INCREF(cb);
    Py_XDECREF(w->resolutionCallback);
    w->resolutionCallback = cb;
    return none();
}
PyObject* winMakeTopmost(PyObject* o, PyObject* args) {
    WIDGET(o) int a = 0, b = 0;
    PARSE("|ii:MakeTopmost", &a, &b)   // the two ints' meaning is not traced
    if (Widget* p = w->parent) {
        for (auto it = p->children.begin(); it != p->children.end(); ++it) {
            if (it->get() == w) {
                std::unique_ptr<Widget> keepIt = std::move(*it);
                p->children.erase(it);
                p->children.push_back(std::move(keepIt));
                break;
            }
        }
    }
    return none();
}
PyMethodDef windowMethods[] = {
    M("GetScript", winGetScript, METH_NOARGS), M("RegisterForUpdate", winRegisterForUpdate, METH_VARARGS),
    M("RegisterForResolutionUpdate", winRegisterForResolutionUpdate, METH_VARARGS), M("MakeTopmost", winMakeTopmost, METH_VARARGS),
    {nullptr, nullptr, 0, nullptr}};

// ---- Text (0x00d03900), RichText (0x00d033b0), EditBox (0x00d03638) -------------------------------------------------------------
// A textId's string: the exe builds the constant "uielement." + textId and asks
// client.clientlanguagemanager.CPPLookup("uielementlanguage", constant, ...) (FUN_004d80b0 -> FUN_0051fa00; the index argument is
// not traced - 0 here). Resolved once, when a script first needs the text.
const std::u16string& widgetText(Widget* w) {
    if (w->textSet || w->textId.empty()) return w->text;
    w->textSet = true;
    PyObject* lookup = cppLookup;   // GameUIManager + 4 (pythonInit)
    Py_XINCREF(lookup);
    std::string constant = "uielement." + w->textId;
    PyObject* value = lookup ? PyObject_CallFunction(lookup, const_cast<char*>("ssi"), "uielementlanguage", constant.c_str(), 0) : nullptr;
    Py_XDECREF(lookup);
    if (value && value != Py_None) w->text = toText(value);
    Py_XDECREF(value);
    if (!value) PyErr_Clear();
    return w->text;
}
PyObject* tGetText(PyObject* o, PyObject*) { WIDGET(o) return fromText(widgetText(w)); }
TextSize textSize(Widget* w) {
    TextSize t = measureText(w->fontId, widgetText(w), w->wrapText ? manager().screenRect(w).right - manager().screenRect(w).left : 0);
    t.height = textLayoutHeight(w, widgetText(w));     // the exe's lines x line height (see UiRender textLayoutHeight)
    return t;
}
PyObject* tGetTextHeight(PyObject* o, PyObject*) { WIDGET(o) return PyInt_FromLong(textSize(w).height); }
PyObject* tGetTextWidth(PyObject* o, PyObject*) { WIDGET(o) return PyInt_FromLong(textSize(w).width); }
// ResizeTo...: sets that side of the widget to the text's and returns it (chatwindow.py uses the result as its line height).
PyObject* tResizeToTextHeight(PyObject* o, PyObject*) { WIDGET(o) w->height = textSize(w).height; return PyInt_FromLong(w->height); }
PyObject* tResizeToTextWidth(PyObject* o, PyObject*) { WIDGET(o) w->width = textSize(w).width; return PyInt_FromLong(w->width); }
PyObject* tSetText(PyObject* o, PyObject* args) {
    WIDGET(o) PyObject* t;
    PARSE("O:SetText", &t)
    w->text = toText(t);
    w->textSet = true;
    return none();
}
PyObject* tSetFont(PyObject* o, PyObject* args) { WIDGET(o) char* s; PARSE("s:SetFont", &s) w->fontId = s; return none(); }
PyObject* tSetWrapText(PyObject* o, PyObject* args) { WIDGET(o) int v; PARSE("i:SetWrapText", &v) w->wrapText = v != 0; return none(); }
PyObject* tSetScaleToFit(PyObject* o, PyObject* args) { WIDGET(o) int v; PARSE("i:SetScaleToFit", &v) w->scaleToFit = v != 0; return none(); }
// WrapText(text) (0x008B2940; format "O:SetText" - sic): the text as the list of unicode lines it breaks into at the widget's width in its
// font (FUN_008f9860; the font by id, UIManager slot 7 - none gives an empty list). RuntimeError "argument must be unicode in pyWrapText"
// for a non-unicode argument, "argument had zero length in pyWrapText" for an empty one.
PyObject* tWrapText(PyObject* o, PyObject* args) {
    PyObject* text = nullptr;
    PARSE("O:SetText", &text)
    WIDGET(o)
    if (!PyUnicode_Check(text)) {
        PyErr_SetString(PyExc_RuntimeError, "argument must be unicode in pyWrapText");
        return nullptr;
    }
    const std::u16string t = toText(text);
    if (t.empty()) {
        PyErr_SetString(PyExc_RuntimeError, "argument had zero length in pyWrapText");
        return nullptr;
    }
    PyObject* list = PyList_New(0);
    for (const auto& [start, length] : wrapTextLines(w, t)) {
        PyObject* line = fromText(t.substr(size_t(start), size_t(length)));
        PyList_Append(list, line);
        Py_DECREF(line);
    }
    return list;
}
PyMethodDef textMethods[] = {
    M("GetText", tGetText, METH_NOARGS), M("SetText", tSetText, METH_VARARGS), M("SetFont", tSetFont, METH_VARARGS),
    M("SetWrapText", tSetWrapText, METH_VARARGS), M("SetScaleToFit", tSetScaleToFit, METH_VARARGS), M("WrapText", tWrapText, METH_VARARGS),
    M("GetTextHeight", tGetTextHeight, METH_NOARGS), M("GetTextWidth", tGetTextWidth, METH_NOARGS),
    M("ResizeToTextHeight", tResizeToTextHeight, METH_NOARGS), M("ResizeToTextWidth", tResizeToTextWidth, METH_NOARGS),
    {nullptr, nullptr, 0, nullptr}};
// RichText (table 0x00d033b0): GetTextHeight (FUN_008e5410) is the layout's height - the last line's y + height -, ResizeToTextHeight
// (FUN_008e5490) sets the widget's height to it and returns it.
PyObject* rGetTextHeight(PyObject* o, PyObject*) { WIDGET(o) return PyInt_FromLong(richTextLayout(w).height()); }
PyObject* rResizeToTextHeight(PyObject* o, PyObject*) {
    WIDGET(o)
    const int h = richTextLayout(w).height();
    if (h != w->height) {
        w->height = h;
        manager().dirty = true;
    }
    return PyInt_FromLong(h);
}
PyMethodDef richTextMethods[] = {
    M("SetText", tSetText, METH_VARARGS), M("SetFont", tSetFont, METH_VARARGS), M("GetTextHeight", rGetTextHeight, METH_NOARGS),
    M("ResizeToTextHeight", rResizeToTextHeight, METH_NOARGS), {nullptr, nullptr, 0, nullptr}};

PyObject* eClear(PyObject* o, PyObject*) { WIDGET(o) w->text.clear(); return none(); }
PyObject* eAddText(PyObject* o, PyObject* args) {
    WIDGET(o) PyObject* t;
    PARSE("O:AddText", &t)
    w->text += toText(t);
    if (w->lengthLimit > 0 && int(w->text.size()) > w->lengthLimit) w->text.resize(size_t(w->lengthLimit));
    return none();
}
PyObject* eGetReservedText(PyObject* o, PyObject*) { WIDGET(o) return fromText(w->reservedText); }
PyObject* eSetReservedText(PyObject* o, PyObject* args) { WIDGET(o) PyObject* t; PARSE("O:SetReservedText", &t) w->reservedText = toText(t); return none(); }
PyObject* eSetLengthLimit(PyObject* o, PyObject* args) { WIDGET(o) PARSE("i:SetLengthLimit", &w->lengthLimit) return none(); }
PyObject* eSetValidator(PyObject* o, PyObject* args) {
    WIDGET(o) char* name;
    PARSE("s:SetValidator", &name)
    setValidator(w, name);
    return none();
}
// SetNumericValidatorLimits(min, max) (0x008D3660): the validator's + 4 / + 8; with no validator the exe returns NULL without an exception.
PyObject* eSetNumericValidatorLimits(PyObject* o, PyObject* args) {
    WIDGET(o) int lo = 0, hi = 0;
    PARSE("ii:SetNumericValidatorLimits", &lo, &hi)
    if (w->validator == 0) return nullptr;
    w->validatorMin = lo;
    w->validatorMax = hi;
    return none();
}
// SelectAll (0x008D36F0 -> FUN_008d3010): the selection cleared (FUN_008d1b60), then from 0 to the text's length (+ 0x110); the caret
// stays where it is.
PyObject* eSelectAll(PyObject* o, PyObject* args) {
    WIDGET(o) PARSE(":SelectAll", nullptr)
    w->selAnchor = 0;
    w->selEnd = w->text.size();
    manager().dirty = true;
    return none();
}
// GetCandidateScreenPos() (0x008D3760, ":GetCandidateScreenPos"): the Highlight part's (+ 0xF4) GetScreenRect (slot 0x1A8) as the list
// [left, bottom + 5] - where imewindow.py puts the candidate window. The Highlight is placed on the selection when the box is laid out
// (FUN_008d2710; here by UiCanvas when drawn: the last place it had, editHighlightRect), else its own rectangle.
PyObject* eGetCandidateScreenPos(PyObject* o, PyObject* args) {
    PARSE(":GetCandidateScreenPos", nullptr)
    WIDGET(o)
    const Widget* highlight = nullptr;
    for (const auto& c : w->children)
        if (c->tag == "Highlight") highlight = c.get();
    Rect r = highlight ? manager().screenRect(highlight) : Rect{};
    editHighlightRect(w, r.left, r.top, r.right, r.bottom);
    PyObject* list = PyList_New(2);
    PyList_SetItem(list, 0, PyInt_FromLong(r.left));
    PyList_SetItem(list, 1, PyInt_FromLong(r.bottom + 5));
    return list;
}
PyMethodDef editMethods[] = {
    M("Clear", eClear, METH_NOARGS), M("GetText", tGetText, METH_NOARGS), M("SetText", tSetText, METH_VARARGS),
    M("SelectAll", eSelectAll, METH_VARARGS), M("GetCandidateScreenPos", eGetCandidateScreenPos, METH_VARARGS),
    M("AddText", eAddText, METH_VARARGS), M("GetReservedText", eGetReservedText, METH_NOARGS),
    M("SetReservedText", eSetReservedText, METH_VARARGS), M("SetLengthLimit", eSetLengthLimit, METH_VARARGS),
    M("SetValidator", eSetValidator, METH_VARARGS), M("SetNumericValidatorLimits", eSetNumericValidatorLimits, METH_VARARGS), {nullptr, nullptr, 0, nullptr}};

// ---- Bitmap (0x00d031e0) ------------------------------------------------------------------------------------------------------
PyObject* bGetTextureDefID(PyObject* o, PyObject*) { WIDGET(o) return PyString_FromString(w->textureDef.c_str()); }
PyObject* bSetTextureByDef(PyObject* o, PyObject* args) {
    WIDGET(o) PyObject* id;
    PARSE("O:SetTextureByDef", &id)
    PyObject* s = PyObject_Str(id);
    if (!s) return nullptr;
    w->textureDef = PyString_AsString(s);
    w->textureFile.clear();
    Py_DECREF(s);
    return none();
}
// A decoded texture, cached by name (the bitmaps' point tests and pixel reads).
struct DecodedTexture {
    int32_t width = 0, height = 0;
    std::vector<tr::Rgba8> pixels;
};
const DecodedTexture* decodedTexture(const std::string& name) {
    static std::map<std::string, DecodedTexture> cache;
    auto it = cache.find(name);
    if (it == cache.end()) {
        DecodedTexture t;
        tr::Bytes bytes;
        try {
            tr::DdsHeader header;
            if (assets().tryRead(name, bytes)) t.pixels = tr::decodeDds(bytes, header);
            t.width = int32_t(header.width);
            t.height = int32_t(header.height);
        } catch (const std::exception&) {
            t.pixels.clear();
        }
        if (size_t(t.width) * size_t(std::max(t.height, 0)) > t.pixels.size()) t.pixels.clear();
        it = cache.emplace(name, std::move(t)).first;
    }
    return it->second.pixels.empty() ? nullptr : &it->second;
}
// The bitmap's texture, region and mode bits: the script's (textureFile set) or its textureDef's.
struct BitmapTexture {
    std::string name;
    int32_t left = -1, top = -1, right = -1, bottom = -1;
    int flags = 0;
};
bool bitmapTexture(const Widget* w, BitmapTexture& t) {
    if (!w->textureFile.empty()) {
        t.name = w->textureFile;
        t.left = w->regionLeft, t.top = w->regionTop, t.right = w->regionRight, t.bottom = w->regionBottom;
        t.flags = w->tileFlags;
        return true;
    }
    const auto& defs = manager().definitions().textures;
    auto def = defs.find(w->textureDef);
    if (def == defs.end()) return false;
    t.name = def->second.name;
    t.left = def->second.left, t.top = def->second.top, t.right = def->second.right, t.bottom = def->second.bottom;
    t.flags = (def->second.tileU ? 0x1 : 0) | (def->second.tileV ? 0x4 : 0);
    return true;
}
// FUN_008f5190 (after a texture or region is set): a region other than (-1, -1, -1, -1) clamped to the texture - left 0 .. w - 1, top
// 0 .. h - 1, right 1 .. w, bottom 1 .. h - and back to (-1, -1, -1, -1) when it is then the whole texture.
void clampRegion(Widget* w) {
    if (w->regionLeft == -1 && w->regionTop == -1 && w->regionRight == -1 && w->regionBottom == -1) return;
    const DecodedTexture* t = decodedTexture(w->textureFile);
    if (!t) return;     // UNVERIFIED: the exe reads the texture's size (+ 0x20 / + 0x24) whatever was loaded
    w->regionLeft = std::clamp(w->regionLeft, 0, t->width - 1);
    w->regionTop = std::clamp(w->regionTop, 0, t->height - 1);
    w->regionRight = std::clamp(w->regionRight, 1, t->width);
    w->regionBottom = std::clamp(w->regionBottom, 1, t->height);
    if (w->regionLeft == 0 && w->regionTop == 0 && w->regionRight == t->width && w->regionBottom == t->height)
        w->regionLeft = w->regionTop = w->regionRight = w->regionBottom = -1;
}
// The exe keeps one texture state per bitmap; OpenRasa takes it over from the textureDef before a script changes part of it.
void takeTextureState(Widget* w) {
    if (!w->textureFile.empty()) return;
    BitmapTexture t;
    if (!bitmapTexture(w, t)) return;
    w->textureFile = t.name;
    w->regionLeft = t.left, w->regionTop = t.top, w->regionRight = t.right, w->regionBottom = t.bottom;
    w->tileFlags = t.flags;
}
// The region's size and origin (FUN_008f4de0 / FUN_008f4f40): (-1, -1, -1, -1) = the texture (+ 0x20 / + 0x24) from (0, 0).
void regionOf(const BitmapTexture& b, const DecodedTexture& t, int32_t& left, int32_t& top, int32_t& width, int32_t& height) {
    if (b.left == -1 && b.top == -1 && b.right == -1 && b.bottom == -1) {
        left = top = 0, width = t.width, height = t.height;
    } else {
        left = b.left, top = b.top, width = b.right - b.left, height = b.bottom - b.top;
    }
}
// _ftol of an x87 result; out of range gives 0x80000000. The FPU is taken to run at single precision (Direct3D 9's default without
// D3DCREATE_FPU_PRESERVE; the device flag comes from settings + 0xB of the creation struct, FUN_00677850, NOT traced).
int32_t ftol(float v) {
    if (!(v > -2147483648.0f && v < 2147483648.0f)) return int32_t(0x80000000u);
    return int32_t(v);
}
}  // namespace

bool bitmapPixel(const Widget* w, int x, int y, int width, int height, uint8_t rgba[4]) {
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
    BitmapTexture b;
    if (!bitmapTexture(w, b)) return false;
    const DecodedTexture* t = decodedTexture(b.name);
    if (!t) return false;
    int32_t left, top, rw, rh;
    regionOf(b, *t, left, top, rw, rh);
    // Tiled axes take the remainder (IDIV: the sign follows x); stretched ones scale through the FPU: (x / widget size) x region size.
    int32_t px, py;
    if (b.flags & 0x1) {
        if (rw == 0) return false;
        px = x % rw;
    } else {
        px = ftol(float(x) / float(width) * float(rw));
    }
    if (b.flags & 0x4) {
        if (rh == 0) return false;
        py = y % rh;
    } else {
        py = ftol(float(y) / float(height) * float(rh));
    }
    px += left;
    py += top;
    // effTexture FUN_006cb080: "Requesting color of pixel outside of texture!" - the exe asserts "GetPixelColor failed" and gives 0.
    if (px < 0 || py < 0 || px >= t->width || py >= t->height) return false;
    const tr::Rgba8& p = t->pixels[size_t(py) * size_t(t->width) + size_t(px)];
    rgba[0] = p.red, rgba[1] = p.green, rgba[2] = p.blue, rgba[3] = p.alpha;
    return true;
}

bool bitmapPointIn(const Widget* w, int x, int y) {
    const Rect r = manager().drawnRect(w);
    if (x < r.left || y < r.top || x >= r.right || y >= r.bottom) return false;
    if (w->opaqueClick) return true;
    uint8_t rgba[4];
    bitmapPixel(w, x - r.left, y - r.top, r.right - r.left, r.bottom - r.top, rgba);
    return rgba[3] > 0;
}

namespace {
// SetTexture(name [, resetRegion = 1]) (0x008F59B0): loads the texture file by name (FUN_005eed50 - a RuntimeError when it fails); with
// resetRegion the region (slot 122, + 0xCC) set to (-1, -1, -1, -1) = the whole texture (0x00d1079c), then the texture set (slot 120,
// FUN_008f49c0) and the region clamped to it (FUN_008f5190). The mode bits are kept. The radar window shows its map tiles this way
// (radarwindow.py _UpdateMapImages).
PyObject* bSetTexture(PyObject* o, PyObject* args) {
    WIDGET(o) PyObject* name;
    int resetRegion = 1;
    PARSE("O|i:SetTexture", &name, &resetRegion)
    if (!PyString_Check(name)) {
        PyErr_SetString(PyExc_RuntimeError, "SetTexture: the texture name is not a string");
        return nullptr;
    }
    const std::string file = PyString_AsString(name);
    if (!assets().exists(file)) {
        PyErr_SetString(PyExc_RuntimeError, ("SetTexture: could not load " + file).c_str());
        return nullptr;
    }
    takeTextureState(w);
    w->textureFile = file;
    if (resetRegion) w->regionLeft = w->regionTop = w->regionRight = w->regionBottom = -1;
    clampRegion(w);
    return none();
}
// GetTexture() (0x008F5880 -> UIBitmap slot 121, + 0xC8): the texture's name (texture + 0x50; "" when it has none), None without a
// texture. Scripts hand it back to SetTexture (craftingwindow.py, radialwindow.py, mapwindow.py). UNVERIFIED: that + 0x50 is the name as
// it was asked for (case and path as given).
PyObject* bGetTexture(PyObject* o, PyObject*) {
    WIDGET(o) BitmapTexture t;
    if (!bitmapTexture(w, t)) return none();
    return PyString_FromString(t.name.c_str());
}
// SetTextureRegion((a, b, c, d)) (0x008F5FC0): "(iiii)" read into a rectangle as (b, a, d, c) - the scripts pass (top, left, bottom,
// right), gameuiutil.py's logos icons - then slot 122 (FUN_008f4a30): the region set and clamped (FUN_008f5190).
PyObject* bSetTextureRegion(PyObject* o, PyObject* args) {
    WIDGET(o) int a = 0, b = 0, c = 0, d = 0;
    PARSE("(iiii):SetTextureRegion", &a, &b, &c, &d)
    takeTextureState(w);
    if (w->textureFile.empty()) return none();     // no texture (UNVERIFIED: the exe's clamp reads + 0xC8 without a check)
    w->regionLeft = b, w->regionTop = a, w->regionRight = d, w->regionBottom = c;
    clampRegion(w);
    return none();
}
// SetOpaqueClick(flag) (0x008F5F40 -> slot 131, FUN_008f50d0): + 0xE0 = flag != 0.
PyObject* bSetOpaqueClick(PyObject* o, PyObject* args) {
    WIDGET(o) int flag = 0;
    PARSE("i:SetOpaqueClick", &flag)
    w->opaqueClick = flag != 0;
    return none();
}
// Bitmap.GetPixelColor(x, y) (0x008F5D80 -> slot 133, FUN_008f4de0): bitmapPixel at the widget point - (r, g, b, a); (0, 0, 0, 0) when
// nothing can be read (the colour starts as 0xFFFFFFFF, but the slot clears it on failure).
PyObject* bGetPixelColor(PyObject* o, PyObject* args) {
    WIDGET(o) int x, y;
    PARSE("ii:GetPixelColor", &x, &y)
    const Rect rect = manager().drawnRect(w);
    uint8_t rgba[4];
    bitmapPixel(w, x, y, rect.right - rect.left, rect.bottom - rect.top, rgba);
    return Py_BuildValue("(iiii)", rgba[0], rgba[1], rgba[2], rgba[3]);
}
// Bitmap.FindPixelForColor((r, g, b, a)) (0x008F5E60, "(iiii):FindColorForPixel" -> slot 134, FUN_008f4f40): the region's texels row
// by row from the top, left to right; the first exactly that colour gives (x, y) in the region - a tiled axis as is, a stretched one
// x widget size / region size through the FPU (widget size x i / region size); (-1, -1) when none is.
PyObject* bFindPixelForColor(PyObject* o, PyObject* args) {
    WIDGET(o) int r, g, b, a;
    PARSE("(iiii):FindColorForPixel", &r, &g, &b, &a)
    BitmapTexture bt;
    const DecodedTexture* t = bitmapTexture(w, bt) ? decodedTexture(bt.name) : nullptr;
    if (t) {
        int32_t left, top, rw, rh;
        regionOf(bt, *t, left, top, rw, rh);
        const Rect rect = manager().drawnRect(w);
        const int32_t ww = rect.right - rect.left, wh = rect.bottom - rect.top;
        for (int32_t y = 0; y < rh; ++y)
            for (int32_t x = 0; x < rw; ++x) {
                const int32_t px = left + x, py = top + y;
                if (px < 0 || py < 0 || px >= t->width || py >= t->height) continue;
                const tr::Rgba8& p = t->pixels[size_t(py) * size_t(t->width) + size_t(px)];
                if (p.red != r || p.green != g || p.blue != b || p.alpha != a) continue;
                const int32_t ox = (bt.flags & 0x1) ? x : ftol(float(ww) * float(x) / float(rw));
                const int32_t oy = (bt.flags & 0x4) ? y : ftol(float(wh) * float(y) / float(rh));
                return Py_BuildValue("(ii)", ox, oy);
            }
    }
    return Py_BuildValue("(ii)", -1, -1);
}
PyMethodDef bitmapMethods[] = {
    M("GetTextureDefID", bGetTextureDefID, METH_NOARGS), M("SetTextureByDef", bSetTextureByDef, METH_VARARGS),
    M("GetDefaultBitmapAnimId", bGetDefaultBitmapAnimId, METH_NOARGS),
    M("SetTexture", bSetTexture, METH_VARARGS), M("GetTexture", bGetTexture, METH_NOARGS),
    M("SetTextureRegion", bSetTextureRegion, METH_VARARGS), M("SetOpaqueClick", bSetOpaqueClick, METH_VARARGS),
    M("GetPixelColor", bGetPixelColor, METH_VARARGS), M("FindPixelForColor", bFindPixelForColor, METH_VARARGS), {nullptr, nullptr, 0, nullptr}};

// ---- Buttons: PushButton (0x00d03408), ToggleButton (0x00d03350), CheckBox (0x00d035f8) ----------------------------------------
// PushButton.SetRepeatable (0x008E4790 -> FUN_008e4000): on a change the flag (+ 0x13A), the timer (+ 0x13C) to 0, and the button joins
// (or leaves) the widgets updated each frame (FUN_0089fdd0 / FUN_0089fe90); see scrollBarsFrame.
PyObject* pSetRepeatable(PyObject* o, PyObject* args) {
    WIDGET(o) int v;
    PARSE("i:SetRepeatable", &v)
    if (w->repeatable != (v != 0)) {
        w->repeatable = v != 0;
        w->repeatMs = 0;
        auto& list = manager().repeatableButtons;
        list.erase(std::remove(list.begin(), list.end(), w), list.end());
        if (w->repeatable) list.push_back(w);
    }
    return none();
}
PyObject* pSetHighlighted(PyObject* o, PyObject* args) { WIDGET(o) int v; PARSE("i:SetHighlighted", &v) scriptSetHighlighted(w, v != 0); return none(); }
PyObject* pIsHighlighted(PyObject* o, PyObject*) { WIDGET(o) return boolean(w->highlighted); }
PyMethodDef pushMethods[] = {
    M("SetRepeatable", pSetRepeatable, METH_VARARGS), M("SetHighlighted", pSetHighlighted, METH_VARARGS),
    M("IsHighlighted", pIsHighlighted, METH_NOARGS), {nullptr, nullptr, 0, nullptr}};
PyObject* gSetToggled(PyObject* o, PyObject* args) { WIDGET(o) int v; PARSE("i:SetToggled", &v) scriptSetToggled(w, v != 0); return none(); }
PyObject* gIsToggled(PyObject* o, PyObject*) { WIDGET(o) return boolean(w->toggled); }
PyObject* gSetLocked(PyObject* o, PyObject* args) { WIDGET(o) int v; PARSE("i:SetLocked", &v) w->locked = v != 0; return none(); }
PyObject* gIsLocked(PyObject* o, PyObject*) { WIDGET(o) return boolean(w->locked); }
PyMethodDef toggleMethods[] = {
    M("SetToggled", gSetToggled, METH_VARARGS), M("IsToggled", gIsToggled, METH_NOARGS), M("SetLocked", gSetLocked, METH_VARARGS),
    M("IsLocked", gIsLocked, METH_NOARGS), {nullptr, nullptr, 0, nullptr}};
Widget* boxButton(Widget* box) {
    for (const auto& c : box->children)
        if (c->kind == Kind::ToggleButton) return c.get();
    return nullptr;
}
PyObject* kSetChecked(PyObject* o, PyObject* args) {
    WIDGET(o) int v;
    PARSE("i:SetChecked", &v)
    scriptSetChecked(w, v != 0);
    return none();
}
PyObject* kIsChecked(PyObject* o, PyObject*) {
    WIDGET(o) Widget* b = boxButton(w);
    return boolean(b ? b->toggled : w->checked);
}
PyMethodDef checkMethods[] = {M("SetChecked", kSetChecked, METH_VARARGS), M("IsChecked", kIsChecked, METH_NOARGS), {nullptr, nullptr, 0, nullptr}};

// ---- Ranges: ScrollBar (0x00d03588), Slider (0x00d03508), Spinner (0x00d03190), ProgressBar (0x00d038c4), ComboBox (0x00d032d0) -
// ScrollBar: SetRange (0x008D8760 -> FUN_008d7950: max below min asserts and takes min; then SetValue(current)), SetPageSize (0x008D87E0:
// stored only), SetCurrentValue (0x008D8850 -> SetValue, FUN_008d79e0). The other ranges store the values (not traced).
PyObject* rSetRange(PyObject* o, PyObject* args) {
    WIDGET(o) int lo, hi;
    PARSE("ii:SetRange", &lo, &hi)
    w->rangeMin = lo;
    w->rangeMax = hi < lo ? lo : hi;
    if (w->kind == Kind::ScrollBar) setScrollValue(w, w->value);
    if (w->kind == Kind::Slider) setSliderValue(w, w->value);   // SetRange 0x008DB760 -> FUN_008dafa0: then SetValue(value)
    return none();
}
PyObject* rSetPageSize(PyObject* o, PyObject* args) { WIDGET(o) PARSE("i:SetPageSize", &w->pageSize) return none(); }
PyObject* rSetCurrentValue(PyObject* o, PyObject* args) {
    WIDGET(o) int v;
    PARSE("i:SetCurrentValue", &v)
    if (w->kind == Kind::ScrollBar) setScrollValue(w, v);
    else if (w->kind == Kind::Slider) setSliderValue(w, v);
    else w->value = v;
    return none();
}
PyObject* rGetCurrentValue(PyObject* o, PyObject*) { WIDGET(o) return PyInt_FromLong(w->value); }
// GetMaxValue (0x008D8920): the largest value SetValue allows - max - pageSize + 1 (+ 0xf8 - + 0xfc + 1), at least min (+ 0xf4). So
// chatwindow.py's "at the bottom" test (GetCurrentValue() == GetMaxValue()) holds when scrolled to the end, and new lines follow.
PyObject* rGetMaxValue(PyObject* o, PyObject*) {
    WIDGET(o)
    if (w->kind != Kind::ScrollBar) return PyInt_FromLong(w->rangeMax);
    return PyInt_FromLong(std::max(w->rangeMin, w->rangeMax - w->pageSize + 1));
}
// Slider ScrollUp (0x008DB920) / ScrollDown (0x008DB9A0): SetValue (FUN_008db030) of max(min, value - 1) / min(max, value + 1).
PyObject* sScrollUp(PyObject* o, PyObject*) {
    WIDGET(o) setSliderValue(w, std::max(w->rangeMin, w->value - 1));
    return none();
}
PyObject* sScrollDown(PyObject* o, PyObject*) {
    WIDGET(o) setSliderValue(w, std::min(w->rangeMax, w->value + 1));
    return none();
}
PyMethodDef sliderMethods[] = {M("ScrollUp", sScrollUp, METH_NOARGS), M("ScrollDown", sScrollDown, METH_NOARGS), {nullptr, nullptr, 0, nullptr}};
PyMethodDef rangeMethods[] = {
    M("SetRange", rSetRange, METH_VARARGS), M("SetPageSize", rSetPageSize, METH_VARARGS), M("SetCurrentValue", rSetCurrentValue, METH_VARARGS),
    M("GetCurrentValue", rGetCurrentValue, METH_NOARGS), M("GetMaxValue", rGetMaxValue, METH_NOARGS), {nullptr, nullptr, 0, nullptr}};
// SetPercentComplete (0x008B4890 -> FUN_008b4270): below 0 -> 0, above 1 -> 1 (COMISS: a NaN is stored as it is), then laid out.
float clampedPercent(float p) { return 0.0f > p ? 0.0f : p > 1.0f ? 1.0f : p; }
PyObject* barSetPercentComplete(PyObject* o, PyObject* args) {
    WIDGET(o) float p = 0;
    PARSE("f:SetPercentComplete", &p)
    w->percentComplete = clampedPercent(p);
    return none();
}
PyObject* barGetPercentComplete(PyObject* o, PyObject*) { WIDGET(o) return PyFloat_FromDouble(w->percentComplete); }
PyMethodDef progressMethods[] = {
    M("SetPercentComplete", barSetPercentComplete, METH_VARARGS), M("GetPercentComplete", barGetPercentComplete, METH_NOARGS),
    {nullptr, nullptr, 0, nullptr}};
PyObject* comboSetStringList(PyObject* o, PyObject* args) {
    WIDGET(o) PyObject* list;
    PARSE("O:SetStringList", &list)
    // FUN_008f1e50: a list (else ignored); items that are not unicode become "".
    if (!PyList_Check(list)) return none();
    std::vector<std::u16string> strings;
    for (int i = 0; i < PyList_Size(list); ++i) {
        PyObject* item = PyList_GetItem(list, i);
        strings.push_back(PyUnicode_Check(item) ? toText(item) : std::u16string());
    }
    if (w->kind == Kind::ComboBox) comboSetStringList(w, std::move(strings));
    else w->stringList = std::move(strings);
    return none();
}
PyObject* comboSetValue(PyObject* o, PyObject* args) {
    WIDGET(o) int v;
    PARSE("i:SetValue", &v)
    if (w->kind == Kind::ComboBox) comboSetValue(w, v);
    else w->value = v;
    return none();
}
PyObject* comboGetValue(PyObject* o, PyObject*) { WIDGET(o) return PyInt_FromLong(w->value); }
PyObject* comboGetValueAsString(PyObject* o, PyObject*) {
    WIDGET(o) if (w->value < 0 || w->value >= int(w->stringList.size())) return fromText(u"");
    return fromText(w->stringList[size_t(w->value)]);
}
PyObject* comboSetValueAsString(PyObject* o, PyObject* args) {
    WIDGET(o) PyObject* t;
    PARSE("O:SetValueAsString", &t)
    std::u16string s = toText(t);
    for (size_t i = 0; i < w->stringList.size(); ++i)
        if (w->stringList[i] == s) {
            if (w->kind == Kind::ComboBox) comboSetValue(w, int(i));
            else w->value = int(i);
            break;
        }
    return none();
}
PyMethodDef comboMethods[] = {
    M("SetStringList", comboSetStringList, METH_VARARGS), M("SetValue", comboSetValue, METH_VARARGS), M("GetValue", comboGetValue, METH_NOARGS),
    M("SetValueAsString", comboSetValueAsString, METH_VARARGS), M("GetValueAsString", comboGetValueAsString, METH_NOARGS),
    {nullptr, nullptr, 0, nullptr}};

// ---- Movie (0x00d03458) / MovieComposite (0x00d0328c) ---------------------------------------------------------------------------
// The player is MoviePlayer.h's. Play (FUN_008e1940 / FUN_008f39a0, "O:Play", a string or RuntimeError "movie file name not a string")
// returns whether the movie opened; Stop (0x008e1a80, "|i" - the argument is not used) closes it. A MovieComposite does both through
// its Movie (its second child).
Widget* movieOf(Widget* w) {
    if (w->kind == Kind::Movie) return w;
    for (const auto& c : w->children)
        if (c->kind == Kind::Movie) return c.get();
    return nullptr;
}
std::vector<Widget*> playingMovies;          // Movies with a player (frame() updates them)
std::vector<std::pair<Widget*, std::u16string>> endedMovies;   // closed, the end message not sent yet (UIMovie + 0xB0 list)

// FUN_008e10d0: hides the movie (and through its observer the composite), closes it, queues its name for the end message.
void closeMovie(Widget* m) {
    setVisible(m, false);
    if (m->parent && m->parent->kind == Kind::MovieComposite) {
        setVisible(m->parent, false);
        for (const auto& c : m->parent->children) setVisible(c.get(), false);
    }
    if (m->moviePlayback) std::fprintf(stderr, "[movie] %s closed\n", std::string(m->movie.begin(), m->movie.end()).c_str());
    m->moviePlayback.reset();
    playingMovies.erase(std::remove(playingMovies.begin(), playingMovies.end(), m), playingMovies.end());
    endedMovies.emplace_back(m, m->movie);
}

PyObject* moviePlay(PyObject* o, PyObject* args) {
    WIDGET(o) PyObject* name = nullptr;
    PARSE("O:Play", &name)
    if (!PyString_Check(name)) {
        PyErr_SetString(PyExc_RuntimeError, "movie file name not a string");
        return nullptr;
    }
    Widget* m = movieOf(w);
    if (!m) return PyBool_FromLong(0);
    const std::string file = PyString_AsString(name);
    m->movie = std::u16string(file.begin(), file.end());
    auto playback = std::make_shared<MoviePlayback>();
    const auto where = assets().locate(file);
    if (!where || where->inArchive || !playback->open(where->filePath, audio::masterGain())) {
        if (!where) std::fprintf(stderr, "[ui] movie %s not found\n", file.c_str());
        closeMovie(m);
        return PyBool_FromLong(0);
    }
    m->moviePlayback = playback;
    std::fprintf(stderr, "[movie] %s playing\n", file.c_str());
    if (std::find(playingMovies.begin(), playingMovies.end(), m) == playingMovies.end()) playingMovies.push_back(m);
    setVisible(m, true);
    if (m->parent && m->parent->kind == Kind::MovieComposite) {
        setVisible(m->parent, true);
        for (const auto& c : m->parent->children) setVisible(c.get(), true);
    }
    manager().dirty = true;
    return PyBool_FromLong(1);
}
PyObject* movieStop(PyObject* o, PyObject* args) {
    WIDGET(o) int unused = 0;
    PARSE("|i:Stop", &unused)
    if (Widget* m = movieOf(w)) closeMovie(m);
    return none();
}
PyMethodDef movieMethods[] = {M("Play", moviePlay, METH_VARARGS), M("Stop", movieStop, METH_VARARGS), {nullptr, nullptr, 0, nullptr}};

// ---- DNDWidget (table 0x00D03700; dndwidget.h, traced 2026-10-27; see UiInput.cpp) ------------------------------------------------
// GetVisual / GetSource / GetTarget: + 0xE8 / + 0xF4 / + 0xF8 (None when null); SetSource(widget) + 0xF4; SetTargetValid(b) + 0xFC;
// GetVisualOffset () -> (x, y) + 0xEC / + 0xF0; SetVisualOffset (FUN_008cc030). The data (+ 0xC0, name -> text): SetData(name, long long)
// ("sL:SetDataID") stores it as decimal text (FUN_008ccfc0, a wide ostringstream of the unsigned value); GetData(name) reads it back as
// an unsigned 64-bit number (FUN_008cd140, a wide istringstream), None when the name is not there; SetDataType(name) (its format reads
// "s:GetDataType") stores data "type" (FUN_008cc080, widened byte by byte); GetDataType() the "type" text (FUN_008cc0f0), None when
// unset. AcceptDrop: state 1 and the end; DeferDrop: state 2; RejectDrop: state 3 and the end; WasAccepted / WasRejected: state 1 / 3.
// UNVERIFIED: GetData of text that is not a number returns an uninitialised value in the exe (here 0).
PyObject* dndWidgetOrNone(Widget* w) { return w ? pythonOf(w) : none(); }
PyObject* dGetVisual(PyObject* o, PyObject*) { WIDGET(o) return dndWidgetOrNone(w->dndVisual); }
PyObject* dGetSource(PyObject* o, PyObject*) { WIDGET(o) return dndWidgetOrNone(w->dndSource); }
PyObject* dGetTarget(PyObject* o, PyObject*) { WIDGET(o) return dndWidgetOrNone(w->dndTarget); }
PyObject* dSetSource(PyObject* o, PyObject* args) {
    WIDGET(o) PyObject* source;
    PARSE("O:SetSource", &source)
    Widget* src = self(source);
    if (!src) return nullptr;
    w->dndSource = src;
    return none();
}
PyObject* dSetTargetValid(PyObject* o, PyObject* args) {
    WIDGET(o) unsigned char v = 0;
    PARSE("b:SetTargetValid", &v)
    w->dndTargetValid = v != 0;
    return none();
}
PyObject* dGetData(PyObject* o, PyObject* args) {
    WIDGET(o) char* name;
    PARSE("s:GetData", &name)
    auto it = w->dndData.find(name);
    if (it == w->dndData.end()) return none();
    unsigned long long v = 0;
    for (char16_t c : it->second) {
        if (c < u'0' || c > u'9') break;
        v = v * 10 + unsigned(c - u'0');
    }
    return PyLong_FromLongLong(static_cast<PY_LONG_LONG>(v));
}
PyObject* dSetData(PyObject* o, PyObject* args) {
    WIDGET(o) char* name;
    PY_LONG_LONG v = 0;
    PARSE("sL:SetDataID", &name, &v)
    const std::string digits = std::to_string(static_cast<unsigned long long>(v));
    w->dndData[name] = std::u16string(digits.begin(), digits.end());
    return none();
}
PyObject* dGetDataType(PyObject* o, PyObject* args) {
    WIDGET(o) if (!PyArg_ParseTuple(args, const_cast<char*>(":GetDataType"))) return nullptr;
    auto it = w->dndData.find("type");
    if (it == w->dndData.end()) return none();
    std::string narrow;
    for (char16_t c : it->second) narrow += char(c);
    return PyString_FromString(narrow.c_str());
}
PyObject* dSetDataType(PyObject* o, PyObject* args) {
    WIDGET(o) char* type;
    PARSE("s:GetDataType", &type)
    const std::string t = type;
    w->dndData["type"] = std::u16string(t.begin(), t.end());
    return none();
}
PyObject* dGetVisualOffset(PyObject* o, PyObject*) { WIDGET(o) return Py_BuildValue(const_cast<char*>("(ii)"), w->dndOffsetX, w->dndOffsetY); }
PyObject* dSetVisualOffset(PyObject* o, PyObject* args) {
    WIDGET(o) int x = 0, y = 0;
    PARSE("ii:SetVisualOffset", &x, &y)
    setDndVisualOffset(w, x, y);
    return none();
}
PyObject* dAcceptDrop(PyObject* o, PyObject*) { WIDGET(o) w->dndState = 1; finishDnd(w); return none(); }
PyObject* dDeferDrop(PyObject* o, PyObject*) { WIDGET(o) w->dndState = 2; return none(); }
PyObject* dRejectDrop(PyObject* o, PyObject*) { WIDGET(o) w->dndState = 3; finishDnd(w); return none(); }
PyObject* dWasAccepted(PyObject* o, PyObject*) { WIDGET(o) return boolean(w->dndState == 1); }
PyObject* dWasRejected(PyObject* o, PyObject*) { WIDGET(o) return boolean(w->dndState == 3); }
PyMethodDef dndMethods[] = {
    M("GetVisual", dGetVisual, METH_NOARGS), M("GetSource", dGetSource, METH_NOARGS), M("SetSource", dSetSource, METH_VARARGS),
    M("GetTarget", dGetTarget, METH_NOARGS), M("SetTargetValid", dSetTargetValid, METH_VARARGS), M("GetData", dGetData, METH_VARARGS),
    M("SetData", dSetData, METH_VARARGS), M("GetDataType", dGetDataType, METH_VARARGS), M("SetDataType", dSetDataType, METH_VARARGS),
    M("GetVisualOffset", dGetVisualOffset, METH_NOARGS), M("SetVisualOffset", dSetVisualOffset, METH_VARARGS),
    M("AcceptDrop", dAcceptDrop, METH_NOARGS), M("DeferDrop", dDeferDrop, METH_NOARGS), M("RejectDrop", dRejectDrop, METH_NOARGS),
    M("WasAccepted", dWasAccepted, METH_NOARGS), M("WasRejected", dWasRejected, METH_NOARGS), {nullptr, nullptr, 0, nullptr}};

// ---- SceneView (0x00d03840) and the game's TRSceneView (0x00d07e38) -------------------------------------------------------------
bool floats(PyObject* tuple, float* out, int n) {
    if (!PySequence_Check(tuple) || PySequence_Size(tuple) < n) return false;
    for (int i = 0; i < n; ++i) {
        PyObject* item = PySequence_GetItem(tuple, i);
        out[i] = item ? float(PyFloat_AsDouble(item)) : 0.0f;
        Py_XDECREF(item);
    }
    if (PyErr_Occurred()) PyErr_Clear();
    return true;
}
// SetupSceneView(lights): gameuiutil.FrontEndLightInfo.GetInfoTuple() = (dir1 direction, dir1 colour, dir2 direction, dir2 colour,
// hemi top colour, hemi bottom colour), three floats each.
PyObject* sSetupSceneView(PyObject* o, PyObject* args) {
    WIDGET(o) PyObject* info;
    PARSE("O:SetupSceneView", &info)
    std::array<float, 3>* parts[6] = {&w->scene.dir1, &w->scene.dir1Colour, &w->scene.dir2, &w->scene.dir2Colour, &w->scene.hemiTop,
                                      &w->scene.hemiBottom};
    if (PySequence_Check(info) && PySequence_Size(info) >= 6) {
        for (int i = 0; i < 6; ++i) {
            PyObject* part = PySequence_GetItem(info, i);
            if (part) floats(part, parts[i]->data(), 3);
            Py_XDECREF(part);
        }
        w->scene.lights = true;
    }
    return none();
}
PyObject* sSetClearColor(PyObject* o, PyObject* args) {
    WIDGET(o) int r, g, b, a;
    PARSE("(iiii):SetClearColor", &r, &g, &b, &a)
    w->scene.clear = {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    return none();
}
PyObject* sSetCameraPos(PyObject* o, PyObject* args) {
    WIDGET(o) float x, y, z;
    PARSE("(fff):SetCameraPos", &x, &y, &z)
    w->scene.camera = {x, y, z};
    return none();
}
PyObject* sAddBody(PyObject* o, PyObject* args) {
    WIDGET(o) char* name;
    PARSE("s:AddBody", &name)
    w->scene.bodies.push_back(name);
    return none();
}
PyObject* sClearBodies(PyObject* o, PyObject*) {
    WIDGET(o) w->scene.bodies.clear();
    return none();
}
// SetPreviewEntity(entityId) (TRSceneView, FUN_005604d0 -> SetPreviewBody FUN_0055f500): the entity's body joins this view's scene
// (an id with no body clears the preview); ClearPreviewEntity(entityId) (FUN_005605c0): clears it when the id is 0 or the preview's.
PyObject* sSetPreviewEntity(PyObject* o, PyObject* args) {
    WIDGET(o) long long id;
    PARSE("L:SetPreviewEntity", &id)
    w->scene.previewEntity = uint64_t(id);
    return none();
}
PyObject* sClearPreviewEntity(PyObject* o, PyObject* args) {
    WIDGET(o) long long id;
    PARSE("L:ClearPreviewEntity", &id)
    if (id == 0 || uint64_t(id) == w->scene.previewEntity) w->scene.previewEntity = 0;
    return none();
}
// GetCameraDir() (0x008BA250): the view's camera orientation (camera + 0x84, a quaternion) as a D3DX matrix decomposed by FUN_004433e0:
// (atan2(m31, m33), atan2(-m32, sqrt(m31^2 + m33^2)), atan2(m12, m22)) - the forward axis's yaw and pitch (positive looking down) and the
// right axis's roll; when sqrt(m31^2 + m33^2) is below a small epsilon (0x00bc3c28): (atan2(-m13, m11), atan2(-m32, m22), 0). The world
// display (GameUIManager.GetDisplayWorld) answers with the world's camera, a scene widget with its own (the loader's cameraYaw /
// Pitch / Roll, else identity: (0, 0, 0)). The epsilon is 1e-5 (0x00bc3c28).
PyObject* sGetCameraDir(PyObject* o, PyObject*) {
    WIDGET(o) float yaw = 0, pitch = 0, roll = 0;
    if (w->uiid == "WorldDisplay" && w->parent == manager().root()) {
        world::cameraDir(yaw, pitch, roll);
    } else {
        const float x = w->scene.orientation[0], y = w->scene.orientation[1], z = w->scene.orientation[2], q = w->scene.orientation[3];
        const float m11 = 1 - 2 * (y * y + z * z), m12 = 2 * (x * y + z * q), m13 = 2 * (x * z - y * q);
        const float m22 = 1 - 2 * (x * x + z * z);
        const float m31 = 2 * (x * z + y * q), m32 = 2 * (y * z - x * q), m33 = 1 - 2 * (x * x + y * y);
        const float h = std::sqrt(m31 * m31 + m33 * m33);
        if (h >= 1e-5f) {
            yaw = std::atan2(m31, m33);
            pitch = std::atan2(-m32, h);
            roll = std::atan2(m12, m22);
        } else {
            yaw = std::atan2(-m13, m11);
            pitch = std::atan2(-m32, m22);
        }
    }
    return Py_BuildValue(const_cast<char*>("(fff)"), yaw, pitch, roll);
}
// EnableStaticRender(flag) (0x008BA560, "i:EnableStaticRender"): on a buffered view (tag 'VBFG'), FUN_0068a380 puts it on / takes it
// off the static list (+ 0x2D1). RenderStaticImage(flag) (0x008BA4A0, "i:pyRenderStaticImage"): on a buffered view, the redraw interval
// (+ 0x2C0) = 66 ms when the video quality level (render settings + 0xB8) is 4 or more, else flag ? 0xFFFFFFFF (never again) : 1000 ms.
// headshot.py: EnableStaticRender(True), then RenderStaticImage(True) - at the default level (6, Custom) a 66 ms portrait.
PyObject* sEnableStaticRender(PyObject* o, PyObject* args) {
    WIDGET(o) int flag = 0;
    PARSE("i:EnableStaticRender", &flag)
    if (w->sceneBuffered) w->sceneStatic = flag != 0;
    return none();
}
PyObject* sRenderStaticImage(PyObject* o, PyObject* args) {
    WIDGET(o) int flag = 0;
    PARSE("i:pyRenderStaticImage", &flag)
    if (w->sceneBuffered) w->sceneInterval = videoQuality() >= 4 ? 66u : flag ? 0xFFFFFFFFu : 1000u;
    return none();
}
// UpdateCameraToHead((x, y, z)) (0x00560960, "(fff):UpdateCameraToHead"): a request SceneWidget carries out (see there). Without the
// tuple the exe parses again with ":UpdateCameraToHead" and on success places the camera with (0, 0, 0) - but returns NULL with the
// first parse's TypeError still set (so the call raises).
PyObject* sUpdateCameraToHead(PyObject* o, PyObject* args) {
    float x = 0, y = 0, z = 0;
    if (!PyArg_ParseTuple(args, "(fff):UpdateCameraToHead", &x, &y, &z)) {
        if (PyTuple_Size(args) == 0) {
            if (Widget* w = self(o)) {
                w->scene.headOffset = {0, 0, 0};
                ++w->scene.headRequest;
            }
        }
        return nullptr;
    }
    WIDGET(o)
    w->scene.headOffset = {x, y, z};
    ++w->scene.headRequest;
    return none();
}
// SetOrthogonalProjection(left, right, bottom, top) (0x005608A0, "ffff:SetkOrthogonalProjection" - sic): FUN_00560240 stores the window
// (+ 0x260 .. + 0x26C) and orthographic mode (+ 0x234 = 1); a buffered view redraws when it changed.
PyObject* sSetOrthogonalProjection(PyObject* o, PyObject* args) {
    float l = 0, r = 0, b = 0, t = 0;
    PARSE("ffff:SetkOrthogonalProjection", &l, &r, &b, &t)
    WIDGET(o)
    if (w->scene.ortho != std::array<float, 4>{l, r, b, t}) w->sceneImage = nullptr;
    w->scene.ortho = {l, r, b, t};
    w->scene.orthographic = true;
    return none();
}
// SetOrthogonal(flag) (0x008BA330, "b:SetOrthogonal"): the view's orthographic mode (+ 0x234) set or cleared and its projection marked
// for rebuilding (+ 0x1F8, + 0x20C); the extents stay what SetOrthogonalProjection last gave (none: zero).
PyObject* sSetOrthogonal(PyObject* o, PyObject* args) {
    unsigned char flag = 0;
    PARSE("b:SetOrthogonal", &flag)
    WIDGET(o)
    if (w->scene.orthographic != (flag != 0)) w->sceneImage = nullptr;
    w->scene.orthographic = flag != 0;
    return none();
}
PyMethodDef sceneMethods[] = {
    M("UpdateCameraToHead", sUpdateCameraToHead, METH_VARARGS), M("SetOrthogonalProjection", sSetOrthogonalProjection, METH_VARARGS),
    M("SetOrthogonal", sSetOrthogonal, METH_VARARGS),
    M("EnableStaticRender", sEnableStaticRender, METH_VARARGS), M("RenderStaticImage", sRenderStaticImage, METH_VARARGS),
    M("GetCameraDir", sGetCameraDir, METH_NOARGS),
    M("SetupSceneView", sSetupSceneView, METH_VARARGS), M("SetClearColor", sSetClearColor, METH_VARARGS),
    M("SetCameraPos", sSetCameraPos, METH_VARARGS), M("AddBody", sAddBody, METH_VARARGS), M("ClearBodies", sClearBodies, METH_NOARGS),
    M("SetPreviewEntity", sSetPreviewEntity, METH_VARARGS), M("ClearPreviewEntity", sClearPreviewEntity, METH_VARARGS),
    {nullptr, nullptr, 0, nullptr}};

// The exe's method names per class (its tables, listed in tabula_rasa_python_api.md) - those not implemented above answer with a
// logging no-op.
const std::map<std::string, std::set<std::string>>& exeMethods() {
    static const std::map<std::string, std::set<std::string>> m = {
        {"Widget", {}},
        {"Composite", {}},
        {"Text", {}},
        {"RichText", {}},
        {"Bitmap", {}},
        {"EditBox", {}},
        {"Slider", {}},
        {"SceneView", {}},
        {"Movie", {}},
    };
    return m;
}

struct ClassInfo {
    std::vector<PyMethodDef*> tables;
    std::vector<const char*> exeGroups;
};

ClassInfo classInfo(Kind k) {
    switch (k) {
        case Kind::Window: return {{windowMethods, compositeMethods, widgetMethods}, {"Composite", "Widget"}};
        case Kind::WidgetComposite: case Kind::Frame: return {{compositeMethods, widgetMethods}, {"Composite", "Widget"}};
        case Kind::MovieComposite: return {{movieMethods, compositeMethods, widgetMethods}, {"Composite", "Widget"}};
        case Kind::Text: return {{textMethods, widgetMethods}, {"Text", "Widget"}};
        case Kind::RichText: return {{richTextMethods, widgetMethods}, {"RichText", "Widget"}};
        case Kind::Bitmap: return {{bitmapMethods, widgetMethods}, {"Bitmap", "Widget"}};
        case Kind::PushButton: return {{pushMethods, compositeMethods, widgetMethods}, {"Composite", "Widget"}};
        case Kind::ToggleButton: return {{toggleMethods, compositeMethods, widgetMethods}, {"Composite", "Widget"}};
        case Kind::CheckBox: return {{checkMethods, compositeMethods, widgetMethods}, {"Composite", "Widget"}};
        case Kind::ComboBox: return {{comboMethods, compositeMethods, widgetMethods}, {"Composite", "Widget"}};
        case Kind::EditBox: case Kind::PasswordBox: case Kind::MultiLineEditBox:
            return {{editMethods, compositeMethods, widgetMethods}, {"EditBox", "Composite", "Widget"}};
        case Kind::ProgressBar: return {{progressMethods, compositeMethods, widgetMethods}, {"Composite", "Widget"}};
        case Kind::ScrollBar: case Kind::Spinner: return {{rangeMethods, compositeMethods, widgetMethods}, {"Composite", "Widget"}};
        case Kind::Slider: return {{sliderMethods, rangeMethods, compositeMethods, widgetMethods}, {"Slider", "Composite", "Widget"}};
        case Kind::SceneView: return {{sceneMethods, widgetMethods}, {"SceneView", "Widget"}};
        case Kind::Movie: return {{movieMethods, widgetMethods}, {"Widget"}};
        case Kind::DNDWidget: return {{dndMethods, compositeMethods, widgetMethods}, {"Composite", "Widget"}};
        default: return {{widgetMethods}, {"Widget"}};
    }
}

// A no-op standing in for an exe method not implemented yet: logs its first use, returns None.
PyObject* unimplemented(PyObject* nameObject, PyObject*) {
    std::string name = PyString_AsString(nameObject);
    if (unimplementedCalls[name]++ == 0) std::fprintf(stderr, "[ui] not implemented yet: %s\n", name.c_str());
    return none();
}
PyMethodDef unimplementedDef = M("unimplemented", unimplemented, METH_VARARGS);

// ---- The PyUIObject type --------------------------------------------------------------------------------------------------------
void uiDealloc(PyObject* o) {
    if (Widget* w = reinterpret_cast<PyUIObject*>(o)->widget) w->python = nullptr;
    PyObject_Del(o);
}

PyObject* uiGetAttr(PyObject* o, char* name) {
    Widget* w = reinterpret_cast<PyUIObject*>(o)->widget;
    if (!w) {
        // A deleted widget's handle. Retail (~Widget 0x0089c4d0) points the handle's method chain (+ 0x10) at DAT_00d454b0 - zero-filled,
        // never written: {methods NULL, link NULL} - and deletes and clears the getattr helper (+ 0x14); its getattr (0x008ae4a0) then
        // calls Py_FindMethodInChain, which in stock Python 2.4 reads chain->methods->ml_name: a NULL dereference, i.e. retail most
        // likely crashes here (the shipped python24.dll's version NOT checked). Deletion is deferred (Manager::destroy), so scripts reach
        // this only with a handle kept past the frame its widget was deleted in; OpenRasa raises AttributeError(name) instead.
        PyErr_SetString(PyExc_AttributeError, name);
        return nullptr;
    }
    if (Widget* c = w->child(name)) return pythonOf(c);
    // Anything but a getter may change what is drawn (redraw on the next frame).
    if (std::strncmp(name, "Get", 3) != 0 && std::strncmp(name, "Is", 2) != 0) manager().dirty = true;
    ClassInfo info = classInfo(w->kind);
    for (PyMethodDef* table : info.tables) {
        for (PyMethodDef* d = table; d->ml_name; ++d)
            if (std::strcmp(d->ml_name, name) == 0) return PyCFunction_New(d, o);
    }
    for (const char* group : info.exeGroups) {
        const auto& names = exeMethods().at(group);
        if (names.count(name)) {
            PyObject* label = PyString_FromFormat("%s.%s", kindName(w->kind), name);
            PyObject* f = PyCFunction_New(&unimplementedDef, label);
            Py_DECREF(label);
            return f;
        }
    }
    if (std::strcmp(name, "__members__") == 0 || std::strcmp(name, "__methods__") == 0) return PyList_New(0);
    PyErr_Format(PyExc_AttributeError, "'%s' widget \"%s\" has no attribute '%s'", kindName(w->kind), w->uiid.c_str(), name);
    return nullptr;
}

PyObject* uiSubscript(PyObject* o, PyObject* key) {
    Widget* w = self(o);
    if (!w) return nullptr;
    if (!PyString_Check(key)) {
        PyErr_SetString(PyExc_KeyError, "Expected a string");
        return nullptr;
    }
    if (Widget* c = w->child(PyString_AS_STRING(key))) return pythonOf(c);
    PyErr_Format(PyExc_KeyError, "\"%s\" not found for widget \"%s\"", PyString_AS_STRING(key), w->uiid.c_str());
    return nullptr;
}
PyMappingMethods uiMapping = {nullptr, uiSubscript, nullptr};

PyObject* uiRepr(PyObject* o) {
    Widget* w = reinterpret_cast<PyUIObject*>(o)->widget;
    return PyString_FromFormat("<PyUIObject type=%s @0x%p>", w ? kindName(w->kind) : "destroyed", static_cast<void*>(w));
}

// ---- gameclient.GetUIManager() (0x00d03ef8) and GetGameUIManager() (0x00d082b8) ---------------------------------------------------
PyObject* uiManager = nullptr;
PyObject* gameUiManager = nullptr;
PyObject* updateCallable = nullptr;
bool worldDisplayed = false;

void put(PyObject* stub, const char* name, PyCFunction fn, int flags = METH_VARARGS) {
    auto* def = new PyMethodDef{const_cast<char*>(name), fn, flags, nullptr};   // lives as long as the process
    PyObject* f = PyCFunction_New(def, nullptr);
    setStubAttribute(stub, name, f);
    Py_DECREF(f);
}

// ToggleDisplayStats() (0x00896F20, ":ToggleDisplayStats"): the root window's (UIManager + 0x14) + 0xC5 flipped and the window
// invalidated; while set its post-render draws the statistics (UiRender.cpp).
PyObject* mToggleDisplayStats(PyObject*, PyObject* args) {
    PARSE(":ToggleDisplayStats", nullptr)
    manager().displayStats = !manager().displayStats;
    manager().dirty = true;
    return none();
}
PyObject* mGetColor(PyObject*, PyObject* args) {
    char* id = nullptr;
    PARSE("|s:GetColor", &id)
    uint8_t r = 255, g = 255, b = 255, a = 255;
    if (id && !manager().colour(id, r, g, b, a)) std::fprintf(stderr, "[ui] unknown colour %s\n", id);
    return colourTuple(r, g, b, a);
}
PyObject* mGetMousePos(PyObject*, PyObject*) { return pair(manager().mouseX, manager().mouseY); }
PyObject* mGetRootWindow(PyObject*, PyObject*) { return manager().root() ? pythonOf(manager().root()) : none(); }
PyObject* mGetWideScreen(PyObject*, PyObject*) { return boolean(manager().wideScreen); }
// GetKeyText(key): the text for a key name the language tables do not translate (gameui.GetKeyText falls back to it, for "w", "6",
// ...). The exe's implementation is not traced (likely the OS key name): here the name upper-cased (APPROXIMATE).
// UIManager.CreateDNDWidget(source, template, (x, y), fromDrag) (0x00896CA0): the DND widget's Python object, or NULL (no template:
// the exe returns NULL without an exception) - see UiInput.cpp createDndWidget. DestroyDNDWidget() (0x00896E10) rejects and ends every
// live one; SetDNDWindow(widget) (0x00896E80) keeps the window new ones go in (UIManager + 0x18; ingame.py gives InGame/DND).
PyObject* mCreateDNDWidget(PyObject*, PyObject* args) {
    PyObject* source;
    char* templateName;
    int x = 0, y = 0, fromDrag = 0;
    if (!PyArg_ParseTuple(args, const_cast<char*>("Os(ii)i:CreateDNDWidget"), &source, &templateName, &x, &y, &fromDrag)) return nullptr;
    Widget* src = self(source);
    if (!src) return nullptr;
    Widget* d = createDndWidget(src, templateName, x, y, fromDrag != 0);
    return d ? pythonOf(d) : nullptr;
}
PyObject* mDestroyDNDWidget(PyObject*, PyObject* args) {
    if (!PyArg_ParseTuple(args, const_cast<char*>(":DestroyDNDWidget"))) return nullptr;
    destroyDndWidgets();
    return none();
}
PyObject* mSetDNDWindow(PyObject*, PyObject* args) {
    PyObject* window;
    if (!PyArg_ParseTuple(args, const_cast<char*>("O:SetDNDWindow"), &window)) return nullptr;
    Widget* w = self(window);
    manager().dndWindow = w;
    if (!w) return nullptr;
    return none();
}
PyObject* mGetKeyText(PyObject*, PyObject* args) {
    char* key;
    PARSE("s:GetKeyText", &key)
    std::u16string t;
    // OpenRasa's extended keys show SDL's own name ("VolumeUp"), extra mouse buttons "Mouse 6" ...
    if (const char* sdl = sdlKeyText(keyCodeForName(key)); sdl && *sdl) {
        for (const char* c = sdl; *c; ++c) t += char16_t(static_cast<unsigned char>(*c));
        return fromText(t);
    }
    if (std::strncmp(key, "button", 6) == 0 && std::atoi(key + 6) >= 6) {
        for (const char* c = "Mouse "; *c; ++c) t += char16_t(*c);
        for (const char* c = key + 6; *c; ++c) t += char16_t(*c);
        return fromText(t);
    }
    for (const char* c = key; *c; ++c) t += char16_t(std::toupper(static_cast<unsigned char>(*c)));
    return fromText(t);
}
PyObject* mShowMousePointer(PyObject*, PyObject* args) {
    int v;
    PARSE("i:ShowMousePointer", &v)
    manager().mousePointerShown = v != 0;
    cursor::setShown(v != 0);
    return none();
}
// GameUIManager.PushCursor / PopCursor (0x00520360 / 0x00520450): a string or None, else RuntimeError "cursor name not a string";
// the cursor stack is Cursor.h's.
bool cursorName(PyObject* args, const char* format, std::string& name) {
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, const_cast<char*>(format), &o)) return false;
    if (o != Py_None && !PyString_Check(o)) {
        PyErr_SetString(PyExc_RuntimeError, "cursor name not a string");
        return false;
    }
    name = o == Py_None ? std::string() : std::string(PyString_AsString(o));
    return true;
}
PyObject* gPushCursor(PyObject*, PyObject* args) {
    std::string name;
    if (!cursorName(args, "O:PushCursor", name)) return nullptr;
    cursor::push(name);
    return none();
}
PyObject* gPopCursor(PyObject*, PyObject* args) {
    std::string name;
    if (!cursorName(args, "O:PopCursor", name)) return nullptr;
    cursor::pop(name);
    return none();
}

PyObject* gSetUpdateCallable(PyObject*, PyObject* args) {
    PyObject* cb;
    PARSE("O:SetUpdateCallable", &cb)
    Py_XDECREF(updateCallable);
    updateCallable = cb == Py_None ? nullptr : cb;
    Py_XINCREF(updateCallable);
    return none();
}
// GameUIManager.GetOverheadManager() (0x005206C0, GameUIManager + 0xC; constructor FUN_00559e90): the overhead manager - its screen
// position and distance (World.h), speech bubbles (below), SetVisible; the rest the stand-in (the scripts never call FloatText,
// DisplayName, DisplayHealth or DisplayIndicator, whose widgets would also move the bubbles up, FUN_0055b770).
PyObject* overheadManager = nullptr;
// The manager's records (FUN_0055ad20: one per entity that has a body, a map ordered by entity id; FUN_0055d4c0 finds one), here only
// their bubbles (record + 0x50: {0, remaining ms, is the current manifestation, widget}) and what FUN_0055b1e0 keeps for them.
struct Bark {
    uint32_t remainingMs = 0;
    bool manifestation = false;
    Widget* widget = nullptr;
};
struct OverheadRecord {
    bool visible = false;                // + 0x30 (0 when the record is made)
    int anchorX = 0, anchorY = 0;        // + 0x28 / + 0x2c
    std::unique_ptr<Bark> bark;
    std::vector<PyObject*> floatTexts;   // + 0x44: FloatText's widgets, oldest first (their Python objects: a widget may go meanwhile)
    // DisplayName's Text (+ 0x54), DisplayIndicator's widget (+ 0x58), DisplayHealth's bar (+ 0x5c) - Python objects, or nullptr.
    PyObject* name = nullptr;
    PyObject* indicator = nullptr;
    PyObject* health = nullptr;
};
std::map<uint64_t, OverheadRecord> overheadRecords;
bool overheadVisible = true;             // + 0x30, 1 from the constructor; SetVisible
// FUN_0055bc60 / FUN_0055bd50: the widget named "Overhead" (the name registry, FUN_008acdb0; here the first in the tree), its child
// "Bark" - the window the bubbles go in.
Widget* findNamed(Widget* w, const std::string& name) {
    if (!w) return nullptr;
    if (w->uiid == name) return w;
    for (auto& c : w->children)
        if (Widget* f = findNamed(c.get(), name)) return f;
    return nullptr;
}
Widget* overheadWindow() { return findNamed(manager().root(), "Overhead"); }
Widget* barkWindow() {
    Widget* o = overheadWindow();
    return o ? o->child("Bark") : nullptr;
}
// FUN_0055b1e0 for a record (World.h overheadPlace; left as it was while the body loads).
void placeOverhead(uint64_t entity, OverheadRecord& r) {
    bool visible = r.visible;
    int x = r.anchorX, y = r.anchorY;
    if (!world::overheadPlace(entity, visible, x, y)) return;
    r.visible = visible;
    r.anchorX = x;
    r.anchorY = y;
}
// FUN_0055b770 with the elapsed milliseconds: a bubble whose time is up is destroyed (FUN_008af760); one whose body is not visible is
// hidden; else it goes to the anchor (75 higher for the current manifestation's; SetAnchorOffset), is pushed back inside the root by
// whatever of its screen rectangle sticks out (FUN_008b6f20: left / top / right / bottom), and shown as the manager is.
Widget* liveWidgetOrNull(PyObject* o);
void layoutBark(OverheadRecord& r, uint32_t elapsedMs) {
    if (!barkWindow() || !r.bark) return;
    Bark& b = *r.bark;
    if (b.remainingMs <= elapsedMs) {
        if (b.widget) manager().destroy(b.widget);
        r.bark.reset();
        return;
    }
    b.remainingMs -= elapsedMs;
    if (!b.widget) return;
    if (!r.visible) {
        setVisible(b.widget, false);
        return;
    }
    int x = r.anchorX, y = r.anchorY;
    // Above the indicator when there is one, else above the health bar and the name (their heights, GetSize slot 300).
    if (Widget* indicator = liveWidgetOrNull(r.indicator)) {
        y -= indicator->height;
    } else {
        if (Widget* health = liveWidgetOrNull(r.health)) y -= health->height;
        if (Widget* name = liveWidgetOrNull(r.name)) y -= name->height;
    }
    if (b.manifestation) y -= 75;
    b.widget->offsetX = x;
    b.widget->offsetY = y;
    const Rect rect = manager().screenRect(b.widget);
    const Widget* root = manager().root();
    const int rootWidth = root ? root->width : 0, rootHeight = root ? root->height : 0;
    if (rect.left < 0) x += -rect.left;
    if (rect.top < 0) y += -rect.top;
    if (rect.right > rootWidth) x -= rect.right - rootWidth;
    if (rect.bottom > rootHeight) y -= rect.bottom - rootHeight;
    b.widget->offsetX = x;
    b.widget->offsetY = y;
    setVisible(b.widget, overheadVisible);
    manager().dirty = true;
}
// The float texts' container (FUN_0055bcc0, kept at the manager's + 0x40): the Overhead window's child "FloatText".
Widget* floatTextWindow() {
    Widget* o = overheadWindow();
    return o ? o->child("FloatText") : nullptr;
}
Widget* liveWidget(PyObject* o) { return reinterpret_cast<PyUIObject*>(o)->widget; }
Widget* liveWidgetOrNull(PyObject* o) { return o ? liveWidget(o) : nullptr; }
void dropRef(PyObject*& o) {
    Py_XDECREF(o);
    o = nullptr;
}
// The removal test the Clear* functions and the frame make (FUN_0055a920 ...): no float text, bark, name, indicator or health left.
bool recordEmpty(const OverheadRecord& r) { return r.floatTexts.empty() && !r.bark && !r.name && !r.indicator && !r.health; }

// FUN_0055b8f0 / FUN_0055b980 / FUN_0055b9f0: the name, health bar and indicator - hidden while the record is not visible, else at the
// anchor (the name raised by the health bar's height, GetSize slot 300) and shown as the manager is (+ 0x30).
void layoutNameHealthIndicator(OverheadRecord& r) {
    Widget* name = liveWidgetOrNull(r.name);
    Widget* health = liveWidgetOrNull(r.health);
    Widget* indicator = liveWidgetOrNull(r.indicator);
    auto place = [&](Widget* w, int raise) {
        if (!w) return;
        if (!r.visible) {
            setVisible(w, false);
            return;
        }
        w->offsetX = r.anchorX;
        w->offsetY = r.anchorY - raise;
        setVisible(w, overheadVisible);
        manager().dirty = true;
    };
    place(name, health ? health->height : 0);
    place(health, 0);
    place(indicator, 0);
}

// FUN_0055b530 with a record: each float text, oldest first - not visible: hidden; else put at the record's anchor (SetAnchorOffset),
// then, while its screen rectangle overlaps one placed before it this frame, moved by the next of (100, 0), (-200, 0), (100, -25)
// (cycling; DAT_00d55200, set once) and tested again from the first; shown and its rectangle added to the placed ones.
void layoutFloatTexts(OverheadRecord& r) {
    static const int moves[3][2] = {{100, 0}, {-200, 0}, {100, -25}};
    std::vector<Rect> placed;
    for (PyObject* o : r.floatTexts) {
        Widget* w = liveWidget(o);
        if (!w) continue;
        if (!r.visible) {
            setVisible(w, false);
            continue;
        }
        w->offsetX = r.anchorX;
        w->offsetY = r.anchorY;
        Rect rect = manager().screenRect(w);
        int next = 0;
        for (size_t k = 0; k < placed.size();) {
            const Rect& p = placed[k];
            if (rect.left < p.right && p.left < rect.right && rect.top < p.bottom && p.top < rect.bottom) {
                w->offsetX += moves[next][0];
                w->offsetY += moves[next][1];
                next = (next + 1) % 3;
                rect = manager().screenRect(w);
                k = 0;
                continue;
            }
            ++k;
        }
        setVisible(w, true);
        placed.push_back(rect);
    }
    if (!r.floatTexts.empty()) manager().dirty = true;
}

// FloatText(entity, text, colour, template) (0x0055BEE0, "LOss:FloatText"; the text must be unicode -> RuntimeError "The second argument
// is must be a unicode string") -> FUN_0055a170. Only for an entity whose body was drawn this frame with a corner in front: the record
// (FUN_0055ad20), an instance of the template (FloatText / CriticalHitFloatText) in the FloatText window (FUN_008af6a0), its child
// "Text" given the text (FUN_008b1c00) and the colour of that id (UIManager slot 6; SetColor), the instance not pickable; added last to
// the record's float texts, the oldest destroyed while there are more than 15 (+ 0x38, set at 0x0051f2b7); the record placed and its
// float texts laid out; the instance's default widget animation restarted (slot 92). Each instance gets a handler for its animation's
// end (FUN_0055b460, an AnimEvent pattern eAnimFinished): when the event's handle is the instance's default widget animation's, it is
// deleted (deferred, FUN_00896720) if still in the FloatText window and dropped from its record; the handler stops the event either
// way. HitFloatText rises 100 units and fades out over 1.5 s.
void floatText(uint64_t entity, const std::u16string& text, const std::string& colour, const std::string& templateName) {
    bool shown = false, visible = false;
    int ax = 0, ay = 0;
    if (!world::overheadPlace(entity, visible, ax, ay, &shown) || !shown) return;
    OverheadRecord& r = overheadRecords[entity];
    placeOverhead(entity, r);
    Widget* parent = floatTextWindow();
    if (!parent) return;
    Widget* w = manager().instantiateTemplate(templateName, templateName, parent);
    if (!w) return;
    if (Widget* t = w->child("Text")) {
        t->text = text;
        t->textSet = true;
        manager().colour(colour, t->r, t->g, t->b, t->a);
    }
    w->pickable = false;
    w->floatText = true;
    r.floatTexts.push_back(pythonOf(w));
    while (r.floatTexts.size() > 15) {
        if (Widget* old = liveWidget(r.floatTexts.front())) manager().destroy(old);
        Py_DECREF(r.floatTexts.front());
        r.floatTexts.erase(r.floatTexts.begin());
    }
    placeOverhead(entity, r);
    layoutFloatTexts(r);
    restartAnimations(w, true, false, false);
}

// FUN_0055b460 for an anim event reaching a float text (see floatText).
int floatTextAnimEnd(Widget* w, const Event& e) {
    if (e.type != 3 || e.message != 2) return 0 /* eNotHandled */;
    if (e.animId != w->defaultWidgetPlayer) return 2 /* eHandledStop */;
    for (auto& [entity, r] : overheadRecords)
        for (auto it = r.floatTexts.begin(); it != r.floatTexts.end(); ++it)
            if (liveWidget(*it) == w) {
                Py_DECREF(*it);
                r.floatTexts.erase(it);
                if (w->parent == floatTextWindow()) manager().detachLater(w);
                return 2 /* eHandledStop */;
            }
    return 2 /* eHandledStop */;
}

// FUN_0055a740 (ClearBark, and first in every DisplayBark): the bubble's widget destroyed. (The exe keeps the bubble with a null widget,
// which its next layout would call through; here the bubble goes, and the record with it.)
void clearBark(uint64_t entity) {
    auto it = overheadRecords.find(entity);
    if (it == overheadRecords.end() || !it->second.bark) return;
    if (barkWindow() && it->second.bark->widget) manager().destroy(it->second.bark->widget);
    it->second.bark.reset();
    if (recordEmpty(it->second)) overheadRecords.erase(it);
}
// FUN_0055a3f0: a bubble over the entity for durationMs. A text bubble shows BarkText (SetText, SetColor, not pickable, its height
// fitted to the text - ResizeToTextHeight, slot 0x1d8) and hides the voice indicator; a voice bubble the other way round with the
// indicator's height; the OverheadChat instance takes that height (SetSize, its width kept) and, unless `chat` is set, its background
// OverheadChatBubble_NPC's colour (SetColorByID); then it is placed and laid out at once (elapsed 0).
void displayBark(uint64_t entity, const std::u16string& text, uint32_t durationMs, const uint8_t colour[4], bool chat, bool voice) {
    clearBark(entity);
    std::array<float, 3> place;
    float yaw = 0;
    if (!engine::entityBodyPlace(entity, place, yaw)) return;    // FUN_0055ad20: no record without an entity with a body
    OverheadRecord& r = overheadRecords[entity];
    r.bark = std::make_unique<Bark>();
    r.bark->remainingMs = durationMs;
    Widget* parent = barkWindow();
    if (!parent) return;
    Widget* w = manager().instantiateTemplate("OverheadChat", "OverheadChat", parent);
    r.bark->widget = w;
    if (!w) return;
    Widget* textWidget = w->child("BarkText");
    Widget* voiceWidget = w->child("VoiceChatIndicator");
    if (!textWidget || !voiceWidget) return;
    int height = w->height;
    if (!voice) {
        setVisible(textWidget, true);
        setVisible(voiceWidget, false);
        textWidget->text = text;
        textWidget->textSet = true;
        textWidget->r = colour[0], textWidget->g = colour[1], textWidget->b = colour[2], textWidget->a = colour[3];
        textWidget->pickable = false;
        textWidget->height = textSize(textWidget).height;
        height = textWidget->height;
    } else {
        setVisible(textWidget, false);
        setVisible(voiceWidget, true);
        height = voiceWidget->height;
    }
    w->height = height;
    Widget* background = w->child("Bknd");
    if (!background) return;
    if (!chat) {    // SetColorByID (slot 0x114): rgb only, the alpha kept
        uint8_t a = background->a;
        manager().colour("OverheadChatBubble_NPC", background->r, background->g, background->b, a);
    }
    placeOverhead(entity, r);
    r.bark->manifestation = entity == player::currentManifestation();
    layoutBark(r, 0);
}
// FUN_0055ac40 (after the update callable, FUN_0051f980): every record placed and laid out with the frame's milliseconds; a record left
// with nothing is removed.
void overheadFrame(int msElapsed) {
    for (auto it = overheadRecords.begin(); it != overheadRecords.end();) {
        OverheadRecord& r = it->second;
        placeOverhead(it->first, r);
        layoutFloatTexts(r);
        layoutBark(r, uint32_t(std::max(msElapsed, 0)));
        layoutNameHealthIndicator(r);
        for (PyObject** o : {&r.name, &r.indicator, &r.health})   // widgets gone some other way
            if (*o && !liveWidget(*o)) dropRef(*o);
        for (auto f = r.floatTexts.begin(); f != r.floatTexts.end();) {   // widgets gone some other way
            if (liveWidget(*f)) {
                ++f;
            } else {
                Py_DECREF(*f);
                f = r.floatTexts.erase(f);
            }
        }
        if (recordEmpty(r)) it = overheadRecords.erase(it);
        else ++it;
    }
}
// The record's widget in that slot destroyed (FUN_008af760) when the Overhead window exists, the slot cleared, and the record removed
// when nothing is left (ClearName FUN_0055a920, ClearIndicator FUN_0055abc0, ClearHealth FUN_0055aac0). ClearIndicator destroys the
// script's widget too.
void clearSlot(uint64_t entity, PyObject* OverheadRecord::*slot) {
    auto it = overheadRecords.find(entity);
    if (it == overheadRecords.end() || !(it->second.*slot)) return;
    if (overheadWindow()) {
        if (Widget* w = liveWidget(it->second.*slot)) manager().destroy(w);
        dropRef(it->second.*slot);
    }
    if (recordEmpty(it->second)) overheadRecords.erase(it);
}
// FUN_0055ad20: the entity's record, made only for an entity with a body.
OverheadRecord* recordFor(uint64_t entity) {
    std::array<float, 3> place;
    float yaw = 0;
    if (!engine::entityBodyPlace(entity, place, yaw)) {
        auto it = overheadRecords.find(entity);
        return it == overheadRecords.end() ? nullptr : &it->second;
    }
    return &overheadRecords[entity];
}

// DisplayName(entity, unicode, (r, g, b)) (0x0055C3C0, "LOO:DisplayName"; "The second argument is must be a unicode string" / "The
// third argument is must be a tuple with 3 items (R, G, B)" as RuntimeError) -> FUN_0055a7d0: the old name cleared (ClearName), the
// record made, an OverheadName instance (a Text) in the Overhead window given the text, the colour (alpha 255), its height fitted to the
// text (ResizeToTextHeight, slot 0x1d8), not pickable; then placed and laid out. (Without a record - no body - the exe calls through the
// map's end node; OpenRasa does nothing.)
PyObject* ohDisplayName(PyObject*, PyObject* args) {
    PY_LONG_LONG id = 0;
    PyObject* text = nullptr;
    PyObject* colour = nullptr;
    PARSE("LOO:DisplayName", &id, &text, &colour)
    if (!PyUnicode_Check(text)) {
        PyErr_SetString(PyExc_RuntimeError, "The second argument is must be a unicode string");
        return nullptr;
    }
    if (!PyTuple_Check(colour) || PyTuple_Size(colour) != 3) {
        PyErr_SetString(PyExc_RuntimeError, "The third argument is must be a tuple with 3 items (R, G, B)");
        return nullptr;
    }
    const uint8_t r = uint8_t(PyInt_AsLong(PyTuple_GetItem(colour, 0))), g = uint8_t(PyInt_AsLong(PyTuple_GetItem(colour, 1))),
                  b = uint8_t(PyInt_AsLong(PyTuple_GetItem(colour, 2)));
    const uint64_t entity = uint64_t(id);
    clearSlot(entity, &OverheadRecord::name);
    OverheadRecord* rec = recordFor(entity);
    Widget* window = overheadWindow();
    if (!rec || !window) return none();
    Widget* w = manager().instantiateTemplate("OverheadName", "OverheadName", window);
    if (!w) return none();
    w->text = toText(text);
    w->textSet = true;
    w->r = r, w->g = g, w->b = b, w->a = 255;
    w->height = textSize(w).height;
    w->pickable = false;
    rec->name = pythonOf(w);
    placeOverhead(entity, *rec);
    layoutNameHealthIndicator(*rec);
    return none();
}
PyObject* ohClearName(PyObject*, PyObject* args) {
    PY_LONG_LONG id = 0;
    PARSE("L:ClearName", &id)
    clearSlot(uint64_t(id), &OverheadRecord::name);
    return none();
}
// DisplayHealth(entity, fraction) (0x0055C630, "Lf:DisplayHealth") -> FUN_0055a9a0: the record made; without a bar yet an OverheadHealth
// instance (a ProgressBar) in the Overhead window coloured with the manager's + 0x34 (0xFFFF0000: opaque red, set at 0x0051f2a9); its
// SetPercentComplete (FUN_008b4270, clamped 0 .. 1); placed and laid out.
PyObject* ohDisplayHealth(PyObject*, PyObject* args) {
    PY_LONG_LONG id = 0;
    float fraction = 0;
    PARSE("Lf:DisplayHealth", &id, &fraction)
    const uint64_t entity = uint64_t(id);
    OverheadRecord* rec = recordFor(entity);
    if (!rec) return none();
    Widget* w = liveWidgetOrNull(rec->health);
    if (!w) {
        Widget* window = overheadWindow();
        if (!window) return none();
        w = manager().instantiateTemplate("OverheadHealth", "OverheadHealth", window);
        if (!w) return none();
        w->r = 255, w->g = 0, w->b = 0, w->a = 255;
        dropRef(rec->health);
        rec->health = pythonOf(w);
    }
    w->percentComplete = clampedPercent(fraction);
    placeOverhead(entity, *rec);
    layoutNameHealthIndicator(*rec);
    return none();
}
PyObject* ohClearHealth(PyObject*, PyObject* args) {
    PY_LONG_LONG id = 0;
    PARSE("L:ClearHealth", &id)
    clearSlot(uint64_t(id), &OverheadRecord::health);
    return none();
}
// DisplayIndicator(entity, widget) (0x0055C740, "LO:DisplayIndicator"; TypeError "Object is not a PyUIObject!") -> FUN_0055ab40: the
// record made; a record with no indicator takes the widget only when its parent is the Overhead window (else nothing); one that has an
// indicator keeps it. Placed and laid out.
PyObject* ohDisplayIndicator(PyObject*, PyObject* args) {
    PY_LONG_LONG id = 0;
    PyObject* o = nullptr;
    PARSE("LO:DisplayIndicator", &id, &o)
    if (!o || o->ob_type != &uiObjectType) {
        PyErr_SetString(PyExc_TypeError, "Object is not a PyUIObject!");
        return nullptr;
    }
    Widget* w = liveWidget(o);
    if (!w) return nullptr;
    const uint64_t entity = uint64_t(id);
    OverheadRecord* rec = recordFor(entity);
    if (!rec) return none();
    if (!liveWidgetOrNull(rec->indicator)) {
        Widget* window = overheadWindow();
        if (!window || w->parent != window) return none();
        dropRef(rec->indicator);
        Py_INCREF(o);
        rec->indicator = o;
    }
    placeOverhead(entity, *rec);
    layoutNameHealthIndicator(*rec);
    return none();
}
PyObject* ohClearIndicator(PyObject*, PyObject* args) {
    PY_LONG_LONG id = 0;
    PARSE("L:ClearIndicator", &id)
    clearSlot(uint64_t(id), &OverheadRecord::indicator);
    return none();
}
// DismissEntity(entity) (0x0055C880) -> FUN_0055ba60 and the record removed. Float texts still in the FloatText window are deleted
// (deferred, FUN_00896720). With a bark only the bubble is deleted (deferred, from the Bark window); without one the name, health bar and
// indicator are (each when its parent is the Overhead window). So a dismissed entity with a bubble leaves its name, bar and indicator
// where they were - as the exe does.
PyObject* ohDismissEntity(PyObject*, PyObject* args) {
    PY_LONG_LONG id = 0;
    PARSE("L:DismissEntity", &id)
    auto it = overheadRecords.find(uint64_t(id));
    if (it == overheadRecords.end()) return none();
    OverheadRecord& r = it->second;
    Widget* floats = floatTextWindow();
    for (PyObject*& o : r.floatTexts) {
        if (Widget* w = liveWidget(o); w && floats && w->parent == floats) manager().detachLater(w);
        dropRef(o);
    }
    if (r.bark) {
        if (Widget* bark = barkWindow(); bark && r.bark->widget && r.bark->widget->parent == bark) manager().detachLater(r.bark->widget);
    } else if (Widget* window = overheadWindow()) {
        for (PyObject** o : {&r.name, &r.health, &r.indicator})
            if (Widget* w = liveWidgetOrNull(*o); w && w->parent == window) manager().detachLater(w);
    }
    for (PyObject** o : {&r.name, &r.health, &r.indicator}) dropRef(*o);
    overheadRecords.erase(it);
    return none();
}

PyObject* ohFloatText(PyObject*, PyObject* args) {
    PY_LONG_LONG id = 0;
    PyObject* text = nullptr;
    char* colour = nullptr;
    char* templateName = nullptr;
    PARSE("LOss:FloatText", &id, &text, &colour, &templateName)
    if (!PyUnicode_Check(text)) {
        PyErr_SetString(PyExc_RuntimeError, "The second argument is must be a unicode string");
        return nullptr;
    }
    floatText(uint64_t(id), toText(text), colour, templateName);
    return none();
}
PyObject* ohDisplayBark(PyObject*, PyObject* args) {
    PY_LONG_LONG id = 0;
    PyObject* text = nullptr;
    int duration = 0, r = 0, g = 0, b = 0, a = 0;
    unsigned char chat = 0;
    PARSE("LOi(iiii)b:DisplayBark", &id, &text, &duration, &r, &g, &b, &a, &chat)
    if (!PyUnicode_Check(text)) {
        PyErr_SetString(PyExc_RuntimeError, "The second argument is must be a unicode string");
        return nullptr;
    }
    const uint8_t colour[4] = {uint8_t(r), uint8_t(g), uint8_t(b), uint8_t(a)};
    displayBark(uint64_t(id), toText(text), uint32_t(duration), colour, chat != 0, false);
    return none();
}
PyObject* ohDisplayVoiceChatIndicator(PyObject*, PyObject* args) {   // FUN_0055c250: a voice bubble, colour 0, the chat flag set
    PY_LONG_LONG id = 0;
    int duration = 0;
    PARSE("Li:DisplayVoiceChatIndicator", &id, &duration)
    const uint8_t colour[4] = {0, 0, 0, 0};
    displayBark(uint64_t(id), std::u16string(), uint32_t(duration), colour, true, true);
    return none();
}
PyObject* ohClearBark(PyObject*, PyObject* args) {
    PY_LONG_LONG id = 0;
    PARSE("L:ClearBark", &id)
    clearBark(uint64_t(id));
    return none();
}
PyObject* ohSetVisible(PyObject*, PyObject* args) {   // FUN_0055bde0: the flag, and the Overhead window shown or hidden with it
    int v = 0;
    PARSE("i:SetVisible", &v)
    overheadVisible = v != 0;
    if (Widget* o = overheadWindow()) setVisible(o, overheadVisible);
    return none();
}
PyObject* ohGetEntityScreenPos(PyObject*, PyObject* args) {
    PY_LONG_LONG id = 0;
    PARSE("L:GetEntityScreenPos", &id)
    int x = 0, y = 0;
    world::entityScreenPos(uint64_t(id), x, y);
    return Py_BuildValue(const_cast<char*>("(ii)"), x, y);
}
PyObject* ohGetEntityScreenPosAndDistance(PyObject*, PyObject* args) {
    PY_LONG_LONG id = 0;
    PARSE("L:GetEntityScreenPosAndDistance", &id)
    int x = 0, y = 0;
    world::entityScreenPos(uint64_t(id), x, y);
    return Py_BuildValue(const_cast<char*>("(iif)"), x, y, double(world::entityDistanceToPlayer(uint64_t(id))));
}
// The map's radar tiles (GameUIManager.SetupRadarTextureResolver(glm) 0x00520950 -> FUN_0051fff0): the previous radar archive is let go
// (FUN_00520190), then <the UIRadar load-by-name folder>/<glm> - if it is a regular file - is mounted as its own pack (FUN_005ef580) and
// registered as a texture source (FUN_00595440), so Bitmap.SetTexture finds the tiles by name; True when mounted. RadarFileExists(name)
// (0x00520A80 -> FUN_00520260): the name is in that pack (FUN_005ef130); False without one. Here the archive's tiles are put into the
// asset library as in-memory files (each map's radar archive is 0.1 - 5 MB).
std::unique_ptr<tr::GlmArchive> radarArchive;
PyObject* gSetupRadarTextureResolver(PyObject*, PyObject* args) {
    PyObject* name;
    if (!PyArg_ParseTuple(args, "O:SetupRadarTextureResolver", &name)) return nullptr;
    if (!PyString_Check(name)) {
        PyErr_SetString(PyExc_RuntimeError, "Radar glm filename not a string");
        return nullptr;
    }
    std::string glm = PyString_AsString(name);
    if (radarArchive) assets().clearOverrides();
    radarArchive.reset();
    const std::string suffix = "_radar.glm";
    std::string path;
    if (glm.size() > suffix.size() && glm.compare(glm.size() - suffix.size(), suffix.size(), suffix) == 0)
        path = assets().radarArchivePath(glm.substr(0, glm.size() - suffix.size()));
    if (path.empty()) Py_RETURN_FALSE;
    try {
        radarArchive = std::make_unique<tr::GlmArchive>(path);
        for (const tr::GlmEntry& e : radarArchive->entries()) assets().setOverride(e.name, radarArchive->read(e));
    } catch (const std::exception&) {
        radarArchive.reset();
        assets().clearOverrides();
        Py_RETURN_FALSE;
    }
    Py_RETURN_TRUE;
}
PyObject* gRadarFileExists(PyObject*, PyObject* args) {
    PyObject* name;
    if (!PyArg_ParseTuple(args, "O:RadarFileExists", &name)) return nullptr;
    if (!PyString_Check(name)) {
        PyErr_SetString(PyExc_RuntimeError, "Radar file name not a string");
        return nullptr;
    }
    return PyBool_FromLong(radarArchive && radarArchive->find(PyString_AsString(name)) ? 1 : 0);
}

// InitRadarMask(name) (0x00520740 -> FUN_0051fb30): loads the mask texture (UI manager + 0x10; "failed to init radar mask texture"
// when it cannot). SetupRadarMaskWidget(widget, (x, y)) (0x00520880 -> FUN_0051fcc0): sets the widget effect's UIRadarMaskTexture to it
// and its mask transform to centre the mask on (x, y), UI logical units (see UiCanvas drawTexture, TRUIRadarMask).
std::string radarMaskName;
PyObject* gInitRadarMask(PyObject*, PyObject* args) {
    PyObject* name;
    if (!PyArg_ParseTuple(args, "O:InitRadarMask", &name)) return nullptr;
    if (!PyString_Check(name)) {
        PyErr_SetString(PyExc_RuntimeError, "Radar mask texture name not a string");
        return nullptr;
    }
    if (!assets().exists(PyString_AsString(name))) {
        PyErr_SetString(PyExc_RuntimeError, "failed to init radar mask texture");
        return nullptr;
    }
    radarMaskName = PyString_AsString(name);
    return none();
}
PyObject* gSetupRadarMaskWidget(PyObject*, PyObject* args) {
    PyObject* widget;
    int x = 0, y = 0;
    if (!PyArg_ParseTuple(args, "O(ii):SetupRadarMaskWidget", &widget, &x, &y)) return nullptr;
    Widget* w = self(widget);
    if (!w) return nullptr;
    if (radarMaskName.empty()) {
        PyErr_SetString(PyExc_RuntimeError, "Initialize m_pRadarMaskTexture before calling SetupRadarMaskWidget");
        return nullptr;
    }
    w->radarMasked = true;
    w->radarMaskX = x;
    w->radarMaskY = y;
    return none();
}

PyObject* gGetOverheadManager(PyObject*, PyObject*) {
    if (!overheadManager) {
        overheadManager = newStub("GameUIManager.GetOverheadManager()");
        put(overheadManager, "GetEntityScreenPos", ohGetEntityScreenPos);
        put(overheadManager, "GetEntityScreenPosAndDistance", ohGetEntityScreenPosAndDistance);
        put(overheadManager, "DisplayBark", ohDisplayBark);
        put(overheadManager, "FloatText", ohFloatText);
        put(overheadManager, "DisplayName", ohDisplayName);
        put(overheadManager, "ClearName", ohClearName);
        put(overheadManager, "DisplayHealth", ohDisplayHealth);
        put(overheadManager, "ClearHealth", ohClearHealth);
        put(overheadManager, "DisplayIndicator", ohDisplayIndicator);
        put(overheadManager, "ClearIndicator", ohClearIndicator);
        put(overheadManager, "DismissEntity", ohDismissEntity);
        put(overheadManager, "DisplayVoiceChatIndicator", ohDisplayVoiceChatIndicator);
        put(overheadManager, "ClearBark", ohClearBark);
        put(overheadManager, "SetVisible", ohSetVisible);
    }
    Py_INCREF(overheadManager);
    return overheadManager;
}
// ToggleScreenshotMode() (0x00520CE0, ":ToggleScreenshotMode"): the GameUIManager's flag (+ 0x18) flipped, and the widgets the name
// registry (FUN_008acdb0) finds as MessageBoxes, FrontEnd, InGame and FadeWindow shown when it is clear, hidden when set (SetVisible,
// slot 0x104). The exe calls through a missing widget; OpenRasa skips it.
bool screenshotMode = false;
PyObject* gToggleScreenshotMode(PyObject*, PyObject* args) {
    PARSE(":ToggleScreenshotMode", nullptr)
    screenshotMode = !screenshotMode;
    for (const char* name : {"MessageBoxes", "FrontEnd", "InGame", "FadeWindow"})
        if (Widget* w = findNamed(manager().root(), name)) setVisible(w, !screenshotMode);
    return none();
}
// SetRenderMode(name) (0x00520C30, "s:SetRenderMode"): "Normal" sets the scene view's renderer's frame render mode (+ 0xb0 -> + 0x250)
// to 0, "Overdraw" to 2 (the effects' RenderMode 2 techniques); any other name changes nothing. No script calls it. OpenRasa keeps the
// mode (renderMode()) but draws no overdraw view.
int sceneRenderMode = 0;
PyObject* gSetRenderMode(PyObject*, PyObject* args) {
    char* name = nullptr;
    PARSE("s:SetRenderMode", &name)
    if (std::strcmp(name, "Normal") == 0) sceneRenderMode = 0;
    if (std::strcmp(name, "Overdraw") == 0) sceneRenderMode = 2;
    return none();
}

PyObject* gDisplayWorld(PyObject*, PyObject* args) { int v; PARSE("i:DisplayWorld", &v) worldDisplayed = v != 0; return none(); }
PyObject* gIsWorldDisplayed(PyObject*, PyObject*) { return boolean(worldDisplayed); }
// GetDisplayWorld: the widget the 3D world is shown in (gameui.py registers mouse moves on it). The engine makes it - it is not in the
// export; where it sits in the tree is not traced (here: the root's first child, the root's size).
PyObject* gGetDisplayWorld(PyObject*, PyObject*) {
    Widget* root = manager().root();
    if (!root) return none();
    Widget* world = root->child("WorldDisplay");
    if (!world) {
        auto made = std::make_unique<Widget>();
        made->kind = Kind::SceneView;   // the exe's is a scene view (GetCameraDir); drawn by the world pass, not as a UI scene widget
        made->tag = "WorldDisplay";
        made->uiid = "WorldDisplay";
        made->parent = root;
        made->resizeToParent = true;
        made->pickable = made->enabled = true;
        world = made.get();
        root->children.insert(root->children.begin(), std::move(made));
    }
    return pythonOf(world);
}

}  // namespace

// ---- Public -------------------------------------------------------------------------------------------------------------------
int floatTextAnimEvent(Widget* w, const Event& e) { return floatTextAnimEnd(w, e); }
const std::u16string& displayText(Widget* w) { return widgetText(w); }

// TRasa::GameUIManager::PythonInit (FUN_0051f2f0, called by InitGameClient's FUN_004d4c10 right after the script package is set):
// client.clientlanguagemanager imported and its CPPLookup kept (GameUIManager + 4; "Failed to import 'client.clientlanguagemanager'" /
// "CPPLookup attribute missing or not callable" fail it), then the module uievent (PyImport_AddModule) with its constants and the
// submodules keyboard / mouse / message / anim / ime made by PyModule_New - attributes of uievent only, not in sys.modules (so
// "import uievent.keyboard" fails as in the exe; no script does it) - and uianchor's anchor constants.
PyObject* cppLookup = nullptr;
PyObject* cppLookupObject() { return cppLookup; }
bool pythonInit() {
    PyObject* languageManager = PyImport_ImportModule(const_cast<char*>("client.clientlanguagemanager"));
    if (!languageManager) {
        if (PyErr_Occurred()) PyErr_Print();
        std::fprintf(stderr, "[ui] Failed to import 'client.clientlanguagemanager'\n");
        return false;
    }
    Py_XDECREF(cppLookup);
    cppLookup = PyObject_GetAttrString(languageManager, const_cast<char*>("CPPLookup"));
    Py_DECREF(languageManager);
    if (!cppLookup || !PyCallable_Check(cppLookup)) {
        PyErr_Clear();
        std::fprintf(stderr, "[ui] CPPLookup attribute missing or not callable\n");
        return false;
    }
    PyObject* ev = PyImport_AddModule(const_cast<char*>("uievent"));
    auto sub = [&](const char* name, std::initializer_list<std::pair<const char*, long>> constants) {
        std::string full = std::string("uievent.") + name;
        PyObject* m = PyModule_New(const_cast<char*>(full.c_str()));
        for (const auto& [n, v] : constants) PyModule_AddIntConstant(m, const_cast<char*>(n), v);
        PyModule_AddObject(ev, const_cast<char*>(name), m);   // steals m
    };
    sub("keyboard", {{"eKeyDown", 0}, {"eKeyUp", 1}, {"eKeyChar", 2}});
    sub("mouse", {{"eMouseDown", 0}, {"eMouseUp", 1}, {"eMouseClicked", 2}, {"eMouseDoubleClicked", 3}, {"eMouseMove", 4},
                  {"eMouseDrag", 5}, {"eButtonNone", 0}, {"eButton1", 1}, {"eButton2", 2}, {"eButton3", 4}, {"eButton4", 8},
                  {"eButton5", 0x10}, {"eWheelUp", 0x20}, {"eWheelDown", 0x40}});
    sub("message", {{"eMessageMove", 0}, {"eMessageSize", 1}, {"eMessageOpen", 2}, {"eMessageClose", 3}, {"eMessageEnable", 4},
                    {"eMessageMouseEnter", 5}, {"eMessageMouseExit", 6}, {"eMessageMouseCaptureReceived", 7},
                    {"eMessageMouseCaptureLost", 8}, {"eMessageKeyboardFocusReceived", 9}, {"eMessageKeyboardFocusLost", 10},
                    {"eMessageButtonUp", 11}, {"eMessageButtonDown", 12}, {"eMessageButtonPressed", 13},
                    {"eMessageButtonHighlighted", 14}, {"eMessageDNDBegin", 15}, {"eMessageDNDEnter", 16}, {"eMessageDNDExit", 17},
                    {"eMessageDNDDrop", 18}, {"eMessageDNDEnd", 19}, {"eMessageMovieEnded", 20}, {"eMessageSelectionChanged", 21},
                    {"eMessageValueChanged", 22}, {"eMessageHTMLLink", 23}});
    sub("anim", {{"eAnimPlayed", 0}, {"eAnimStopped", 1}, {"eAnimFinished", 2}});
    sub("ime", {{"eIMEActivated", 0}, {"eIMEDeactivated", 1}, {"eIMELangChanged", 2}, {"eIMEEnabled", 3}, {"eIMEDisabled", 4},
                {"eIMECompTyping", 5}, {"eIMECompStart", 6}, {"eIMECompChange", 7}, {"eIMECompFull", 8}, {"eIMECompEnd", 9},
                {"eIMECompCancel", 10}, {"eIMECompResult", 11}, {"eIMECandidateOpen", 12}, {"eIMECandidateChange", 13},
                {"eIMECandidateClose", 14}});
    for (const auto& [n, v] : std::initializer_list<std::pair<const char*, long>>{
             {"ePhaseNone", 0}, {"ePhaseOn", 1}, {"ePhaseOff", 2}, {"eTypeInvalid", -1}, {"eTypeKeyboard", 0}, {"eTypeMouse", 1},
             {"eTypeMessage", 2}, {"eNotHandled", 0}, {"eHandledContinue", 1}, {"eHandledStop", 2}, {"eModNone", 0}, {"eModShift", 1},
             {"eModAlt", 2}, {"eModControl", 4}})
        PyModule_AddIntConstant(ev, const_cast<char*>(n), v);
    if (PyObject* anchor = PyImport_AddModule(const_cast<char*>("uianchor"))) {
        for (const auto& [n, v] : std::initializer_list<std::pair<const char*, long>>{
                 {"eWidgetAnchor_TopLeft", 0}, {"eWidgetAnchor_Top", 1}, {"eWidgetAnchor_TopRight", 2}, {"eWidgetAnchor_Right", 3},
                 {"eWidgetAnchor_BotRight", 4}, {"eWidgetAnchor_Bot", 5}, {"eWidgetAnchor_BotLeft", 6}, {"eWidgetAnchor_Left", 7},
                 {"eWidgetAnchor_Center", 8}})
            PyModule_AddIntConstant(anchor, const_cast<char*>(n), v);
    }
    return true;
}

void initPython() {
    uiObjectType.ob_type = &PyType_Type;
    uiObjectType.tp_name = const_cast<char*>("PyUIObject");
    uiObjectType.tp_basicsize = sizeof(PyUIObject);
    uiObjectType.tp_dealloc = uiDealloc;
    uiObjectType.tp_getattr = uiGetAttr;
    uiObjectType.tp_repr = uiRepr;
    uiObjectType.tp_str = uiRepr;
    uiObjectType.tp_as_mapping = &uiMapping;
    uiObjectType.tp_flags = Py_TPFLAGS_DEFAULT;
    uiObjectType.tp_doc = const_cast<char*>("PyCObject-like object.");
    PyType_Ready(&uiObjectType);

    uiManager = newStub("UIManager");
    put(uiManager, "GetColor", mGetColor);
    put(uiManager, "ToggleDisplayStats", mToggleDisplayStats);
    put(uiManager, "GetMousePos", mGetMousePos, METH_NOARGS);
    put(uiManager, "GetRootWindow", mGetRootWindow, METH_NOARGS);
    put(uiManager, "GetWideScreen", mGetWideScreen, METH_NOARGS);
    put(uiManager, "ShowMousePointer", mShowMousePointer);
    put(uiManager, "GetKeyText", mGetKeyText);
    put(uiManager, "CreateDNDWidget", mCreateDNDWidget);
    put(uiManager, "DestroyDNDWidget", mDestroyDNDWidget);
    put(uiManager, "SetDNDWindow", mSetDNDWindow);
    gameUiManager = newStub("GameUIManager");
    put(gameUiManager, "SetUpdateCallable", gSetUpdateCallable);
    put(gameUiManager, "PushCursor", gPushCursor);
    put(gameUiManager, "PopCursor", gPopCursor);
    put(gameUiManager, "DisplayWorld", gDisplayWorld);
    put(gameUiManager, "ToggleScreenshotMode", gToggleScreenshotMode);
    put(gameUiManager, "SetRenderMode", gSetRenderMode);
    put(gameUiManager, "GetOverheadManager", gGetOverheadManager, METH_NOARGS);
    put(gameUiManager, "SetupRadarTextureResolver", gSetupRadarTextureResolver);
    put(gameUiManager, "RadarFileExists", gRadarFileExists);
    put(gameUiManager, "InitRadarMask", gInitRadarMask);
    put(gameUiManager, "SetupRadarMaskWidget", gSetupRadarMaskWidget);
    put(gameUiManager, "IsWorldDisplayed", gIsWorldDisplayed, METH_NOARGS);
    put(gameUiManager, "GetDisplayWorld", gGetDisplayWorld, METH_NOARGS);
}

PyObject* pythonOf(Widget* w) {
    if (!w->python) {
        PyUIObject* o = PyObject_New(PyUIObject, &uiObjectType);
        o->widget = w;
        w->python = reinterpret_cast<PyObject*>(o);   // the widget keeps one reference
    }
    Py_INCREF(w->python);
    return w->python;
}

void releaseWidgetPython(Widget* w) {
    playingMovies.erase(std::remove(playingMovies.begin(), playingMovies.end(), w), playingMovies.end());
    endedMovies.erase(std::remove_if(endedMovies.begin(), endedMovies.end(), [w](const auto& e) { return e.first == w; }), endedMovies.end());
    if (!w->python) return;
    PyObject* o = w->python;
    reinterpret_cast<PyUIObject*>(o)->widget = nullptr;
    w->python = nullptr;
    Py_DECREF(o);
}

// ---- Events ----------------------------------------------------------------------------------------------------------------------
namespace {
struct PendingMessage {
    PyObject* widget;   // the widget's PyUIObject (its widget pointer is cleared if the widget goes)
    int message;
    std::vector<std::pair<std::string, std::u16string>> data;
};
std::vector<PendingMessage> pendingMessages;

void setItem(PyObject* dict, const char* key, PyObject* value) {
    PyDict_SetItemString(dict, const_cast<char*>(key), value);
    Py_DECREF(value);
}

void call(PyObject* callable, PyObject* args) {
    PyObject* r = PyObject_CallObject(callable, args);
    if (!r) PyErr_Print();
    Py_XDECREF(r);
}
}  // namespace

void postMessage(Widget* w, int message, std::vector<std::pair<std::string, std::u16string>> data) {
    pendingMessages.push_back({pythonOf(w), message, std::move(data)});
}

// ---- gameclient.GetAnimManager() (table 0x00D03FB0; client\tr\ui\anim\animmanager.cpp, traced 2026-10-25) -------------------------
// CreateAnimPlayer(name, widget) (0x0088DD50 -> FUN_0088d100) makes a WidgetAnimPlayer / BitmapAnimPlayer (by the AnimData's type) and
// returns its handle (1 + the largest, or 1), None when the animation is unknown. A new player is PAUSED (its constructor FUN_008bfc50
// zeroes the playing byte + 8) and applies nothing until told. Fields: + 0xc time (ms x rate), + 0x10 raw ms, + 0x14 start, + 0x18 the
// last applied, + 0x1c rate (1.0 from 0x00bc4a80). Update (FUN_008bf780(dt)): while playing raw += dt and time = round(raw x rate);
// the animation is applied at max(time - start, 0) (wrapped by its playback type, FUN_008bf440) whenever that changed. SetElapsedTime
// (0x0088E630 -> FUN_0088d490): raw = ms, time = round(ms x rate), applied at once - wonkavatorwindow.py drives the loading bar
// (LoadingBar_WS: Top moved 0 -> 1706 over "time" 0 -> 100) with the load percent this way. UnpauseAnim (FUN_008bf620): playing,
// applied. StopAnim (FUN_008bf6d0): paused. PlayAnim(id, start) (FUN_0088d3a0): start, time 0, raw 0, then unpause. GetElapsedTime
// returns + 0xc. The start / stop / end messages the players send (FUN_008b4ce0) are not posted here. SetPlaybackRate, IsPlaying,
// GetAnimPlayer and DestroyAnimPlayer not decompiled: the rate as a factor, the playing byte, the first player of that widget and
// animation, removal (the widget keeps the last applied state - UNVERIFIED).
namespace {
struct AnimPlayer {
    Widget* widget = nullptr;
    std::string name;
    bool bitmap = false, playing = false;
    long raw = 0, time = 0, start = 0;
    long last = -1;   // + 0x18: the elapsed time last applied (-1 after PlayAnim)
    float rate = 1.0f;
};
std::map<long, AnimPlayer> animPlayers;
long reservedHandleHigh = 0;   // the largest handle given out (default animations' end events take handles without a player)
PyObject* animManager = nullptr;
long newHandle();

void applyPlayer(AnimPlayer& p) {
    p.widget->playerAnim = p.name;
    p.widget->playerBitmap = p.bitmap;
    p.widget->playerTime = double(p.time > p.start ? p.time - p.start : 0);
    manager().dirty = true;
}

AnimPlayer* player(long id) {
    auto it = animPlayers.find(id);
    return it != animPlayers.end() ? &it->second : nullptr;
}

PyObject* amCreateAnimPlayer(PyObject*, PyObject* args) {
    char* name = nullptr;
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, "sO:CreateAnimPlayer", &name, &o)) return nullptr;
    Widget* w = self(o);
    if (!w) return nullptr;
    const tr::XmlNode* data = nullptr;
    for (const tr::XmlNode& a : manager().definitions().animations.children)
        if (const tr::XmlNode* id = a.child("id"); id && id->text == name) data = &a;
    if (!data) return none();
    AnimPlayer p;
    p.widget = w;
    p.name = name;
    const tr::XmlNode* type = data->child("animType");
    p.bitmap = type && type->text == "Bitmap";
    const long id = newHandle();
    animPlayers[id] = p;
    return PyLong_FromLong(id);
}
PyObject* amDestroyAnimPlayer(PyObject*, PyObject* args) {
    long id = 0;
    if (!PyArg_ParseTuple(args, "l:DestroyAnimPlayer", &id)) return nullptr;
    animPlayers.erase(id);
    return none();
}
PyObject* amGetAnimPlayer(PyObject*, PyObject* args) {
    PyObject* o = nullptr;
    char* name = nullptr;
    if (!PyArg_ParseTuple(args, "Os:GetAnimPlayer", &o, &name)) return nullptr;
    Widget* w = self(o);
    if (!w) return nullptr;
    for (const auto& [id, p] : animPlayers)
        if (p.widget == w && p.name == name) return PyLong_FromLong(id);
    return none();
}
PyObject* amPlayAnim(PyObject*, PyObject* args) {
    long id = 0, start = 0;
    if (!PyArg_ParseTuple(args, "ll:PlayAnim", &id, &start)) return nullptr;
    if (AnimPlayer* p = player(id)) {
        p->start = start;
        p->time = p->raw = 0;
        p->last = -1;
        p->playing = true;
        applyPlayer(*p);
    }
    return none();
}
PyObject* amUnpauseAnim(PyObject*, PyObject* args) {
    long id = 0;
    if (!PyArg_ParseTuple(args, "l:UnpauseAnim", &id)) return nullptr;
    if (AnimPlayer* p = player(id)) {
        p->playing = true;
        applyPlayer(*p);
    }
    return none();
}
PyObject* amStopAnim(PyObject*, PyObject* args) {
    long id = 0;
    if (!PyArg_ParseTuple(args, "l:StopAnim", &id)) return nullptr;
    if (AnimPlayer* p = player(id)) p->playing = false;
    return none();
}
PyObject* amIsPlaying(PyObject*, PyObject* args) {
    long id = 0;
    if (!PyArg_ParseTuple(args, "l:IsPlaying", &id)) return nullptr;
    const AnimPlayer* p = player(id);
    return boolean(p && p->playing);
}
PyObject* amSetElapsedTime(PyObject*, PyObject* args) {
    long id = 0;
    int ms = 0;
    if (!PyArg_ParseTuple(args, "li:SetElapsedTime", &id, &ms)) return nullptr;
    if (AnimPlayer* p = player(id)) {
        p->raw = ms;
        p->time = std::lround(float(ms) * p->rate);
        applyPlayer(*p);
    }
    return none();
}
PyObject* amGetElapsedTime(PyObject*, PyObject* args) {
    long id = 0;
    if (!PyArg_ParseTuple(args, "l:GetElapsedTime", &id)) return nullptr;
    const AnimPlayer* p = player(id);
    return PyInt_FromLong(p ? p->time : 0);
}
PyObject* amSetPlaybackRate(PyObject*, PyObject* args) {
    long id = 0;
    float rate = 1;
    if (!PyArg_ParseTuple(args, "lf:SetPlaybackRate", &id, &rate)) return nullptr;
    if (AnimPlayer* p = player(id)) p->rate = rate;
    return none();
}

// A widget's default animation is played by an AnimManager player like any other (Widget slot 88 -> 91: FUN_0088d100 makes it when the
// animation is set). OpenRasa plays default animations from the widget's own clock (widgetAnimStart / bitmapAnimStart) and makes the
// player when a script asks for its handle: playing, at the default animation's current time, so nothing jumps until a script uses it.
// A handle given out earlier with the animation's end event (defaultAnimEvents) is kept. (The exe makes every default player when the
// animation is set, so its handles are numbered in load order; OpenRasa's in the order they are first needed.)
long newHandle() {
    reservedHandleHigh = std::max(reservedHandleHigh, animPlayers.empty() ? 0L : animPlayers.rbegin()->first);
    return ++reservedHandleHigh;
}
long defaultAnimPlayer(Widget* w, bool bitmap) {
    long& handle = bitmap ? w->defaultBitmapPlayer : w->defaultWidgetPlayer;
    if (handle && animPlayers.count(handle)) return handle;
    const std::string& name = bitmap ? w->bitmapAnimName : w->widgetAnimName;
    if (name.empty()) return 0;
    AnimPlayer p;
    p.widget = w;
    p.name = name;
    p.bitmap = bitmap;
    p.playing = true;
    p.raw = p.time = long(manager().clockMs - (bitmap ? w->bitmapAnimStart : w->widgetAnimStart));
    p.last = bitmap ? w->bitmapAnimLast : w->widgetAnimLast;
    if (!handle) handle = newHandle();
    animPlayers[handle] = p;
    return handle;
}

// An animation's length (the longest track's last key, AnimData slot 5) and playback type (+ 0x20: 0 stall, 1 repeat, 2 pingpong).
struct AnimInfo {
    uint32_t length = 0;
    int playback = 0;
};
const AnimInfo* animInfo(const std::string& name) {
    static std::unordered_map<std::string, AnimInfo> cache;     // looked up for every default animation every frame
    static bool built = false;
    if (!built) {
        built = true;
        for (const tr::XmlNode& a : manager().definitions().animations.children) {
            const tr::XmlNode* id = a.child("id");
            if (!id || id->text.empty()) continue;
            AnimInfo info;
            if (const tr::XmlNode* playback = a.child("playbackType"))
                info.playback = playback->text == "pingpong" ? 2 : playback->text == "repeat" ? 1 : 0;
            for (const tr::XmlNode& track : a.children)
                for (const tr::XmlNode& key : track.children)
                    if (key.hasAttribute("time"))
                        info.length = std::max(info.length, uint32_t(std::strtoul(key.attribute("time").c_str(), nullptr, 10)));
            cache.emplace(id->text, info);
        }
    }
    const auto it = cache.find(name);
    return it == cache.end() ? nullptr : &it->second;
}

// Whether an update from elapsed `previous` to `current` reached the animation's end (FUN_008bf440's result): with no length (or an
// unknown animation) always; repeat when the time wrapped (previous mod length > current mod length), pingpong likewise over twice
// the length, stall when it crossed the length (previous < length <= current).
bool animFinished(const AnimInfo* info, unsigned long previous, unsigned long current) {
    if (!info || info->length == 0) return true;
    const unsigned long length = info->length;
    if (info->playback == 1) return previous % length > current % length;
    if (info->playback == 2) return previous % (2 * length) > current % (2 * length);
    return current >= length && previous < length;
}

// The anim events a frame's updates produced, sent once every player has moved on.
struct PendingAnimEvent {
    PyObject* widget;   // its Python object: the widget may go while earlier events are handled
    long id;
    std::string name;
};
std::vector<PendingAnimEvent> pendingAnimEvents;

// The player update (FUN_008bf780(dt)): while playing raw += dt and time = round(raw x rate); elapsed = time - start (0 before the
// start). When elapsed differs from the last applied (+ 0x18; -1 after PlayAnim) the animation is applied there and, when that
// update reached the end (animFinished from the elapsed before this update to the new one), an AnimEvent goes to the player's widget
// (FUN_008b4ce0, bubbling to its parents): type 3, phase 1, no source, eAnimFinished (2), the handle (+ 0x24) and the animation's
// name (+ 0x28, FUN_0088c810).
void advanceAnimPlayers(int msElapsed) {
    for (auto& [id, p] : animPlayers) {
        const long before = p.time > p.start ? p.time - p.start : 0;
        if (p.playing) {
            p.raw += msElapsed;
            p.time = std::lround(float(p.raw) * p.rate);
        }
        const long elapsed = p.time > p.start ? p.time - p.start : 0;
        if (elapsed == p.last) continue;
        p.last = elapsed;
        applyPlayer(p);
        if (animFinished(animInfo(p.name), (unsigned long)before, (unsigned long)elapsed))
            pendingAnimEvents.push_back({pythonOf(p.widget), id, p.name});
    }
}

// The same for the default animations no script has a handle for: on the widget's clock from its start, the manager clock moving
// from clockBefore to clockAfter. The exe's players run whether the widget is shown or not.
void defaultAnimEvents(Widget* w, double clockBefore, double clockAfter) {
    for (int bitmap = 0; bitmap < 2; ++bitmap) {
        // (Called for the widgets with a default animation only - Manager::defaultAnimated.)
        const std::string& name = bitmap ? w->bitmapAnimName : w->widgetAnimName;
        if (name.empty()) continue;
        long& handle = bitmap ? w->defaultBitmapPlayer : w->defaultWidgetPlayer;
        if (handle && animPlayers.count(handle)) continue;   // driven by its player (advanceAnimPlayers)
        const double start = bitmap ? w->bitmapAnimStart : w->widgetAnimStart;
        const long before = long(std::max(clockBefore - start, 0.0)), elapsed = long(std::max(clockAfter - start, 0.0));
        long& last = bitmap ? w->bitmapAnimLast : w->widgetAnimLast;
        if (elapsed == last) continue;
        last = elapsed;
        if (!animFinished(animInfo(name), (unsigned long)before, (unsigned long)elapsed)) continue;
        if (!handle) handle = newHandle();
        pendingAnimEvents.push_back({pythonOf(w), handle, name});
    }
}

void sendAnimEvents() {
    std::vector<PendingAnimEvent> now;
    now.swap(pendingAnimEvents);
    for (PendingAnimEvent& a : now) {
        if (Widget* w = reinterpret_cast<PyUIObject*>(a.widget)->widget) {
            Event e;
            e.type = 3;
            e.phase = 1;
            e.message = 2;   // eAnimFinished
            e.animId = a.id;
            e.animName = a.name;
            handleEvent(w, e);
        }
        Py_DECREF(a.widget);
    }
}
}  // namespace

// RestartDefaultWidgetAnim / RestartDefaultBitmapAnim (Widget slot 92 / UIBitmap's): PlayAnim(handle, 0) on the widget's default player
// (FUN_0088d3a0: start 0, time 0, unpaused). A default animation a script has a handle for runs on that player (defaultAnimPlayer), so
// the restart must go to it - overheadwindow.py pauses an OverheadCon's DrawerFadeOut at frame 0 (PlayAnim + StopAnim) and plays it
// with RestartDefaultWidgetAnim when the name clears; restarting only the widget's own clock left the fade paused and the con shown.
void restartDefaultPlayer(Widget* w, bool bitmap) {
    const long handle = bitmap ? w->defaultBitmapPlayer : w->defaultWidgetPlayer;
    AnimPlayer* p = handle ? player(handle) : nullptr;
    if (!p) return;
    p->start = 0;
    p->time = p->raw = 0;
    p->last = -1;
    p->playing = true;
    applyPlayer(*p);
}

// ---- gameclient.GetIMEManager() (0x004D2EA0; methods table 0x00D03B40) -----------------------------------------------------------
// The IME manager (DAT_00d516e8; its Python object cached at + 0xD5C). SetIMEWindow(window) (0x008A24F0, "O:SetIMEWindow") keeps the
// window (+ 0xD58; a non-UI object stores 0 with "Object is not a PyUIObject!"); GetIMEWindow (0x008A25A0) returns it or None;
// GetIMEWidget (0x008A2630) the keyboard focus (the focus manager's deque, DAT_00d516e0 + 0x10) or None; IsDisabled (0x008A26B0)
// + 0x1C > 0 (UNVERIFIED: 0 without an IME); GetDescription / GetResultString / GetCompositionString / GetReadingString /
// GetTypingString (FUN_008a12a0 / 12c0 / 12e0 / 1310 / 1330 -> PyUnicode_FromWideChar) the IME's strings - "" here, as no IME
// composition runs (the IME events, composition and candidates are not implemented); GetCompositionCursorPos (0x008A28D0) + 0x748
// (0 here). GetCandidateList(i) (0x008A2A50, "i:GetCandidateList"): the i-th of the 32 candidate lists (+ 0xD4C, 0x1C bytes each,
// made by FUN_008a2b60 when the IME attaches to the window: window 0, id -1, + 8 0, one-based 1, list none), its Python object
// "IMECandidate" (methods 0x00D030C0) made once and cached at + 0x18; the index is not checked (out of range: None here).
// IMECandidate (FUN_008f8a10 .. FUN_008f9350): a list opens (IMN_OPENCANDIDATE, FUN_008a21e0) with the IME's window, the bit's
// index as id, + 8 = IME + 0x24 and one-based = IME + 0x48, and its CANDIDATELIST (+ 0x14) once read; it closes (FUN_008a2380) back to
// the defaults. IsValid = id >= 0 and a list; GetId the id; GetCount / GetSelection / GetPageStart / GetPageSize the list's dwCount /
// dwSelection / dwPageStart / dwPageSize, 0 without one; IsOneBasedIndex the byte; GetCandidateText(i) the i-th string, but with no
// list, i >= count or a count of 1 it asserts "GetCandidateText called with bad index %d with count of %d" and gives u""; SetPageStart(i)
// (i < count) ImmNotifyIME(NI_SETCANDIDATE_PAGESTART, id, i); SelectCandidate(i) NI_SELECTCANDIDATESTR then NI_CLOSECANDIDATE (no
// check); SetupPage(top, bottom) (top < bottom < count) NI_SETCANDIDATE_PAGESIZE bottom - top + 1 then PAGESTART top - each asserting
// and doing nothing without the IME context ("... could not get hIMC") or with bad values; all return None. OpenRasa has no IME, so
// every list keeps its defaults (asserts are silent here).
namespace {
struct ImeCandidate {
    long id = -1;
    bool oneBased = true;
    PyObject* object = nullptr;
};
ImeCandidate imeCandidates[32];
ImeCandidate* candidateOf(PyObject* self) {
    const long i = PyInt_AsLong(self);
    return i >= 0 && i < 32 ? &imeCandidates[i] : nullptr;
}
// OpenRasa's IME is SDL's: list 0 is the one SDL reports (Manager::imeCandidates - the page SDL gives, as the whole list: page
// start 0, page size its count); the others keep their defaults.
bool candidateOpen(PyObject* self) { return PyInt_AsLong(self) == 0 && manager().imeCandidatesOpen; }
PyObject* icIsValid(PyObject* self, PyObject* args) {
    if (!PyArg_ParseTuple(args, ":IsValid")) return nullptr;
    return boolean(candidateOpen(self));
}
PyObject* icGetId(PyObject* self, PyObject* args) {
    if (!PyArg_ParseTuple(args, ":GetId")) return nullptr;
    return PyInt_FromLong(candidateOpen(self) ? 0 : candidateOf(self)->id);
}
PyObject* icZero(PyObject*, PyObject*) { return PyInt_FromLong(0); }
PyObject* icGetCount(PyObject* self, PyObject* args) {
    if (!PyArg_ParseTuple(args, ":GetCount")) return nullptr;
    return PyInt_FromLong(candidateOpen(self) ? long(manager().imeCandidates.size()) : 0);
}
PyObject* icGetSelection(PyObject* self, PyObject* args) {
    if (!PyArg_ParseTuple(args, ":GetSelection")) return nullptr;
    return PyInt_FromLong(candidateOpen(self) ? manager().imeCandidateSelection : 0);
}
PyObject* icGetPageStart(PyObject* self, PyObject* args) {
    if (!PyArg_ParseTuple(args, ":GetPageStart")) return nullptr;
    return PyInt_FromLong(0);
}
PyObject* icGetPageSize(PyObject* self, PyObject* args) {
    if (!PyArg_ParseTuple(args, ":GetPageSize")) return nullptr;
    return PyInt_FromLong(candidateOpen(self) ? long(manager().imeCandidates.size()) : 0);
}
PyObject* icSetPageStart(PyObject*, PyObject* args) {
    int i = 0;
    if (!PyArg_ParseTuple(args, "i:SetPageStart", &i)) return nullptr;
    return none();
}
PyObject* icSelectCandidate(PyObject*, PyObject* args) {
    int i = 0;
    if (!PyArg_ParseTuple(args, "i:SelectCandidate", &i)) return nullptr;
    return none();
}
PyObject* icSetupPage(PyObject*, PyObject* args) {
    int top = 0, bottom = 0;
    if (!PyArg_ParseTuple(args, "ii:SetupPage", &top, &bottom)) return nullptr;
    return none();
}
PyObject* icGetCandidateText(PyObject* self, PyObject* args) {
    int i = 0;
    if (!PyArg_ParseTuple(args, "i:GetCandidateText", &i)) return nullptr;
    const std::vector<std::u16string>& c = manager().imeCandidates;
    // FUN_008f8a10: no list, an index past the count, or a count of 1 -> the assert and u"".
    if (!candidateOpen(self) || i < 0 || size_t(i) >= c.size() || c.size() == 1) return PyUnicode_FromUnicode(nullptr, 0);
    return fromText(c[size_t(i)]);
}
PyObject* icIsOneBasedIndex(PyObject* self, PyObject* args) {
    if (!PyArg_ParseTuple(args, ":IsOneBasedIndex")) return nullptr;
    return boolean(candidateOf(self)->oneBased);
}
void putBound(PyObject* stub, const char* name, PyCFunction fn, PyObject* self) {
    auto* def = new PyMethodDef{const_cast<char*>(name), fn, METH_VARARGS, nullptr};
    PyObject* f = PyCFunction_New(def, self);
    setStubAttribute(stub, name, f);
    Py_DECREF(f);
}
PyObject* imeGetCandidateList(PyObject*, PyObject* args) {
    int i = 0;
    if (!PyArg_ParseTuple(args, "i:GetCandidateList", &i)) return nullptr;
    if (i < 0 || i >= 32) return none();
    ImeCandidate& c = imeCandidates[i];
    if (!c.object) {
        c.object = newStub("IMECandidate");
        PyObject* index = PyInt_FromLong(i);
        putBound(c.object, "IsValid", icIsValid, index);
        putBound(c.object, "GetId", icGetId, index);
        putBound(c.object, "GetCount", icGetCount, index);
        putBound(c.object, "GetSelection", icGetSelection, index);
        putBound(c.object, "GetPageStart", icGetPageStart, index);
        putBound(c.object, "GetPageSize", icGetPageSize, index);
        putBound(c.object, "SetPageStart", icSetPageStart, index);
        putBound(c.object, "SelectCandidate", icSelectCandidate, index);
        putBound(c.object, "SetupPage", icSetupPage, index);
        putBound(c.object, "GetCandidateText", icGetCandidateText, index);
        putBound(c.object, "IsOneBasedIndex", icIsOneBasedIndex, index);
        Py_DECREF(index);
    }
    Py_INCREF(c.object);
    return c.object;
}
PyObject* imeManager = nullptr;
PyObject* imeSetIMEWindow(PyObject*, PyObject* args) {
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, "O:SetIMEWindow", &o)) return nullptr;
    Widget* w = self(o);
    manager().imeWindow = w;
    if (!w) return nullptr;
    return none();
}
PyObject* imeGetIMEWindow(PyObject*, PyObject* args) {
    if (!PyArg_ParseTuple(args, ":GetIMEWindow")) return nullptr;
    return manager().imeWindow ? pythonOf(manager().imeWindow) : none();
}
PyObject* imeGetIMEWidget(PyObject*, PyObject* args) {
    if (!PyArg_ParseTuple(args, ":GetIMEWidget")) return nullptr;
    Widget* focus = manager().focus();
    return focus ? pythonOf(focus) : none();
}
PyObject* imeIsDisabled(PyObject*, PyObject* args) {
    if (!PyArg_ParseTuple(args, ":IsDisabled")) return nullptr;
    Py_RETURN_FALSE;
}
PyObject* imeEmptyString(PyObject*, PyObject*) { return PyUnicode_FromUnicode(nullptr, 0); }
PyObject* imeGetResultString(PyObject*, PyObject*) { return fromText(manager().imeResult); }
PyObject* imeGetCompositionString(PyObject*, PyObject*) { return fromText(manager().imeComposition); }
PyObject* imeGetCompositionCursorPos(PyObject*, PyObject* args) {
    if (!PyArg_ParseTuple(args, ":GetCompositionCursorPos")) return nullptr;
    return PyInt_FromLong(manager().imeCursor);
}
}  // namespace

PyObject* imeManagerObject() {
    if (!imeManager) {
        imeManager = newStub("gameclient.IMEManager");
        put(imeManager, "SetIMEWindow", imeSetIMEWindow);
        put(imeManager, "GetIMEWindow", imeGetIMEWindow);
        put(imeManager, "GetIMEWidget", imeGetIMEWidget);
        put(imeManager, "IsDisabled", imeIsDisabled);
        for (const char* name : {"GetDescription", "GetReadingString", "GetTypingString"}) put(imeManager, name, imeEmptyString);
        put(imeManager, "GetResultString", imeGetResultString);
        put(imeManager, "GetCompositionString", imeGetCompositionString);
        put(imeManager, "GetCompositionCursorPos", imeGetCompositionCursorPos);
        put(imeManager, "GetCandidateList", imeGetCandidateList);
    }
    Py_INCREF(imeManager);
    return imeManager;
}

PyObject* animManagerObject() {
    if (!animManager) {
        animManager = newStub("gameclient.AnimManager");
        put(animManager, "CreateAnimPlayer", amCreateAnimPlayer);
        put(animManager, "DestroyAnimPlayer", amDestroyAnimPlayer);
        put(animManager, "GetAnimPlayer", amGetAnimPlayer);
        put(animManager, "PlayAnim", amPlayAnim);
        put(animManager, "UnpauseAnim", amUnpauseAnim);
        put(animManager, "StopAnim", amStopAnim);
        put(animManager, "IsPlaying", amIsPlaying);
        put(animManager, "SetElapsedTime", amSetElapsedTime);
        put(animManager, "GetElapsedTime", amGetElapsedTime);
        put(animManager, "SetPlaybackRate", amSetPlaybackRate);
    }
    Py_INCREF(animManager);
    return animManager;
}

void forgetAnimPlayers(Widget* w) {
    for (auto it = animPlayers.begin(); it != animPlayers.end();) {
        if (it->second.widget == w) it = animPlayers.erase(it);
        else ++it;
    }
}

void frame(int msElapsed) {
    manager().doomed.clear();   // the deferred deletes (DND widgets ended last frame)
    pyprof::section("ui: before");
    advanceAnimPlayers(msElapsed);
    pyprof::section("ui: anim players");
    for (Widget* w : manager().defaultAnimated) defaultAnimEvents(w, manager().clockMs, manager().clockMs + msElapsed);
    pyprof::section("ui: default anim events");
    std::vector<PendingMessage> now;
    now.swap(pendingMessages);
    if (!now.empty()) manager().dirty = true;
    manager().clockMs += msElapsed;
    sendAnimEvents();
    for (PendingMessage& m : now) {
        Widget* w = reinterpret_cast<PyUIObject*>(m.widget)->widget;
        if (w) {
            Event e;
            e.source = w;
            e.message = m.message;
            e.data = std::move(m.data);
            handleEvent(w, e);
        }
        Py_DECREF(m.widget);
    }
    pyprof::section("ui: messages");
    scrollBarsFrame(msElapsed);
    pyprof::section("ui: scroll bars");
    // Movies (FUN_008e1160): one that has reached its end is closed, then each closed movie's end message goes out - from the
    // MovieComposite holding it (its observer, FUN_008f3890), else from the Movie.
    for (Widget* m : std::vector<Widget*>(playingMovies))
        if (m->moviePlayback && !m->moviePlayback->update()) closeMovie(m);
    for (auto& [m, name] : std::vector<std::pair<Widget*, std::u16string>>(std::move(endedMovies))) {
        Widget* source = m->parent && m->parent->kind == Kind::MovieComposite ? m->parent : m;
        postMessage(source, 20, {{"Movie", name}});
    }
    endedMovies.clear();
    pyprof::section("ui: movies");
    if (updateCallable) {
        PyObject* cb = updateCallable;
        Py_INCREF(cb);
        PyObject* args = Py_BuildValue("(i)", msElapsed);
        call(cb, args);
        Py_DECREF(args);
        Py_DECREF(cb);
    }
    pyprof::section("ui: update callable");
    overheadFrame(msElapsed);   // FUN_0051f980: the overhead manager after the update callable
    pyprof::section("ui: overhead");
}

void notifyResolution(Widget* w) {
    if (w->resolutionCallback) {
        PyObject* cb = w->resolutionCallback;
        Py_INCREF(cb);
        PyObject* r = PyObject_CallObject(cb, nullptr);
        if (!r) PyErr_Print();
        Py_XDECREF(r);
        Py_DECREF(cb);
    }
    std::vector<Widget*> children;
    for (const auto& c : w->children) children.push_back(c.get());
    for (Widget* c : children) notifyResolution(c);
}
void setScreenSize(int pixelWidth, int pixelHeight) {
    Widget* root = manager().root();
    if (!root || pixelWidth <= 0 || pixelHeight <= 0) return;
    const bool wide = double(pixelWidth) / double(pixelHeight) > 1.5;
    manager().wideScreen = wide;   // UIManager + 0x60 (GetWideScreen)
    root->width = wide ? 1706 : 1280;
    root->height = 960;
    manager().dirty = true;
    notifyResolution(root);
}

PyObject* uiManagerObject() {
    Py_INCREF(uiManager);
    return uiManager;
}
PyObject* gameUiManagerObject() {
    Py_INCREF(gameUiManager);
    return gameUiManager;
}

const std::string& radarMaskTexture() { return radarMaskName; }

std::string unimplementedCallReport() {
    std::string out;
    for (const auto& [name, count] : unimplementedCalls) out += std::to_string(count) + "  " + name + "\n";
    return out;
}

bool worldDisplayedNow() { return worldDisplayed; }

}  // namespace engine::ui
