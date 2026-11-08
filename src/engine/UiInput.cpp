// UiInput.cpp - UI events: dispatch through the widget tree, the focus manager, hit testing, the built-in behaviour of buttons, check
// boxes and edit boxes, and the input mapper. Traced in tabula_rasa.exe 2026-10-25 (see tabula_rasa_python_api.md "UI input");
// the routing of input to a widget (who gets a mouse / keyboard event) is the engine's queue consumer, which was not traced: here
// mouse events go to the capture widget, else the widget under the mouse, and hover changes send MouseExit / MouseEnter;
// keyboard events go to the focus widget, and unhandled ones to the input mapper (UNVERIFIED).
#include "UiManager.h"
#include "UiRender.h"
#include "Cursor.h"

#include "Audio.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cwctype>
#include <map>
#include <string>

namespace engine::ui {

static int testNumLock = -1;   // setTestNumLock

namespace {

constexpr int kNotHandled = 0, kHandledStop = 2;

// ---- Keys (keyboardevent.cpp's name table, indexed by DirectInput scan code) ------------------------------------------------------
const std::map<int, std::string>& keyNames() {
    static const std::map<int, std::string> names = [] {
        std::map<int, std::string> m;
        const char* row1 = "1234567890";
        for (int i = 0; i < 10; ++i) m[0x02 + i] = std::string(1, row1[i]);
        const char* q = "qwertyuiop";
        for (int i = 0; i < 10; ++i) m[0x10 + i] = std::string(1, q[i]);
        const char* a = "asdfghjkl";
        for (int i = 0; i < 9; ++i) m[0x1E + i] = std::string(1, a[i]);
        const char* z = "zxcvbnm";
        for (int i = 0; i < 7; ++i) m[0x2C + i] = std::string(1, z[i]);
        const std::pair<int, const char*> rest[] = {
            {0x01, "esc"}, {0x0C, "minus"}, {0x0D, "equals"}, {0x0E, "backspace"}, {0x0F, "tab"}, {0x1A, "leftbracket"},
            {0x1B, "rightbracket"}, {0x1C, "enter"}, {0x1D, "leftcontrol"}, {0x27, "semicolon"}, {0x28, "apostrophe"},
            {0x29, "grave"}, {0x2A, "leftshift"}, {0x2B, "backslash"}, {0x33, "comma"}, {0x34, "period"}, {0x35, "slash"},
            {0x36, "rightshift"}, {0x37, "nummultiply"}, {0x38, "leftalt"}, {0x39, "space"}, {0x3A, "caps"}, {0x3B, "f1"},
            {0x3C, "f2"}, {0x3D, "f3"}, {0x3E, "f4"}, {0x3F, "f5"}, {0x40, "f6"}, {0x41, "f7"}, {0x42, "f8"}, {0x43, "f9"},
            {0x44, "f10"}, {0x45, "numlock"}, {0x46, "scrolllock"}, {0x47, "num7"}, {0x48, "num8"}, {0x49, "num9"},
            {0x4A, "numsubtract"}, {0x4B, "num4"}, {0x4C, "num5"}, {0x4D, "num6"}, {0x4E, "numadd"}, {0x4F, "num1"},
            {0x50, "num2"}, {0x51, "num3"}, {0x52, "num0"}, {0x53, "numdecimal"}, {0x56, "oem102"}, {0x57, "f11"},
            {0x58, "f12"}, {0x64, "f13"}, {0x65, "f14"}, {0x66, "f15"}, {0x9C, "numenter"}, {0x9D, "rightcontrol"},
            {0xB5, "numdivide"}, {0xB7, "printscreen"}, {0xB8, "rightalt"}, {0xC5, "pause"}, {0xC7, "home"}, {0xC8, "uparrow"},
            {0xC9, "pageup"}, {0xCB, "leftarrow"}, {0xCD, "rightarrow"}, {0xCF, "end"}, {0xD0, "downarrow"}, {0xD1, "pagedown"},
            {0xD2, "insert"}, {0xD3, "delete"}, {0xDB, "leftwin"}, {0xDC, "rightwin"}, {0xDD, "appmenu"},
            // The rest of the exe's names (its Win32 table, keyed by virtual key, built from 0x008dded4 on: kana, convert, numequals,
            // kanji, ax, systemsleep) at their DirectInput codes (DIK_KANA 0x70, DIK_CONVERT 0x79, DIK_NUMPADEQUALS 0x8D, DIK_KANJI 0x94,
            // DIK_AX 0x96, DIK_SLEEP 0xDF). f16 - f24 have no DirectInput code: they are extended keys (below).
            {0x70, "kana"}, {0x79, "convert"}, {0x8D, "numequals"}, {0x94, "kanji"}, {0x96, "ax"}, {0xDF, "systemsleep"}};
        for (const auto& [c, n] : rest) m[c] = n;
        return m;
    }();
    return names;
}

// ---- Python ---------------------------------------------------------------------------------------------------------------------
PyObject* unicodeOf(const std::u16string& t) {
    std::basic_string<Py_UNICODE> u(t.begin(), t.end());
    return PyUnicode_FromUnicode(u.data(), int(u.size()));
}

void setItem(PyObject* dict, const char* key, PyObject* value) {
    PyDict_SetItemString(dict, const_cast<char*>(key), value);
    Py_DECREF(value);
}

// An event's string form (its StringRepr; what the input mapper matches and the key-bind window stores): the modifier prefix
// (FUN_008d8a40: "alt-", "ctrl-", "shift-", in that order), then for a key (FUN_008dd780) "keyboard." + prefix + the key's name, for a
// mouse event (FUN_008d8b30) "mouse." + prefix + "button" + 1 / 2 / 3 / 4 / 5 for the buttons 1 / 2 / 4 / 8 / 0x10, "wheelup" /
// "wheeldown" for 0x20 / 0x40, else "none".
std::string modifierPrefix(int modifiers) {
    std::string s;
    if (modifiers & 2) s += "alt-";
    if (modifiers & 4) s += "ctrl-";
    if (modifiers & 1) s += "shift-";
    return s;
}
std::string keyString(int code, int modifiers) { return modifierPrefix(modifiers) + keyName(code); }
constexpr int kExtendedButton = 0x10000, kWheelLeft = 0x20000, kWheelRight = 0x40000;
std::string mouseString(int button, int modifiers) {
    std::string s = "mouse." + modifierPrefix(modifiers) + "button";
    switch (button) {
        case 1: return s + "1";
        case 2: return s + "2";
        case 4: return s + "3";
        case 8: return s + "4";
        case 0x10: return s + "5";
        case 0x20: return s + "wheelup";
        case 0x40: return s + "wheeldown";
        // OpenRasa: buttons past the exe's five (kExtendedButton + n -> "button<n>") and a tilting wheel.
        case kWheelLeft: return s + "wheelleft";
        case kWheelRight: return s + "wheelright";
        default:
            if (button > kExtendedButton && button < kExtendedButton + 256) return s + std::to_string(button - kExtendedButton);
            return s + "none";
    }
}

// The event dict (FUN_008b7480).
PyObject* eventDict(const Event& e) {
    PyObject* d = PyDict_New();
    setItem(d, "Type", PyInt_FromLong(e.type));
    setItem(d, "Phase", PyInt_FromLong(e.phase));
    if (e.source) setItem(d, "Source", pythonOf(e.source));
    if (e.dest) setItem(d, "Dest", pythonOf(e.dest));
    if (e.type == 0) {
        setItem(d, "KeyCode", PyInt_FromLong(e.code));
        setItem(d, "EventCode", PyInt_FromLong(e.code2));
        setItem(d, "EventSubCode", PyInt_FromLong(e.code3));
        setItem(d, "StringRepr", PyString_FromString(("keyboard." + keyString(e.code, e.modifiers)).c_str()));
    } else if (e.type == 1) {
        setItem(d, "ButtonID", PyInt_FromLong(e.code));
        setItem(d, "DownButtons", PyInt_FromLong(e.code2));
        setItem(d, "EventCode", PyInt_FromLong(e.code3));
        setItem(d, "LocX", PyInt_FromLong(e.locX));
        setItem(d, "LocY", PyInt_FromLong(e.locY));
        setItem(d, "ScrollCount", PyInt_FromLong(e.scroll));
        setItem(d, "StringRepr", PyString_FromString(mouseString(e.code, e.modifiers).c_str()));
        if (e.clicked) setItem(d, "ClickedWidget", pythonOf(e.clicked));
    } else if (e.type == 2) {
        setItem(d, "Message", PyInt_FromLong(e.message));
        PyObject* data = PyDict_New();
        for (const auto& [k, v] : e.data) setItem(data, k.c_str(), unicodeOf(v));
        for (const auto& [k, v] : e.intData) setItem(data, k.c_str(), PyInt_FromLong(v));
        setItem(d, "Data", data);
    } else if (e.type == 3) {   // FUN_008b7480: Message (+ 0x20), AnimId (+ 0x24), AnimDataId (+ 0x28)
        setItem(d, "Message", PyInt_FromLong(e.message));
        setItem(d, "AnimId", PyInt_FromLong(e.animId));
        setItem(d, "AnimDataId", PyString_FromString(e.animName.c_str()));
    } else if (e.type == 4) {   // FUN_008b7480: IMEType (+ 0x20)
        setItem(d, "IMEType", PyInt_FromLong(e.message));
    }
    PyObject* mods = PyList_New(0);
    for (int bit : {2, 4, 1})
        if (e.modifiers & bit) {
            PyObject* b = PyInt_FromLong(bit);
            PyList_Append(mods, b);
            Py_DECREF(b);
        }
    setItem(d, "Modifiers", mods);
    return d;
}

// A Python handler (FUN_008f86b0): callable(widget, event); None counts as eHandledStop, else the int returned.
int callHandler(PyObject* callable, Widget* w, const Event& e) {
    Py_INCREF(callable);
    PyObject* self = pythonOf(w);
    PyObject* dict = eventDict(e);
    PyObject* r = PyObject_CallFunctionObjArgs(callable, self, dict, nullptr);
    Py_DECREF(dict);
    Py_DECREF(self);
    Py_DECREF(callable);
    int result = kHandledStop;
    if (!r) {
        PyErr_Print();
    } else {
        if (r != Py_None) result = int(PyInt_AsLong(r));
        if (PyErr_Occurred()) PyErr_Clear();
        Py_DECREF(r);
    }
    manager().dirty = true;
    return result;
}

// The widget's Python handlers for the event's pattern (FUN_008b4ed0): keyboard type, modifiers, key code, event code; mouse button and
// event code; message the message; anim / IME their code.
int pythonHandlers(Widget* w, const Event& e) {
    std::vector<PyObject*> matching;
    if (e.type == 2) {
        auto [a, b] = w->messageHandlers.equal_range(e.message);
        for (auto it = a; it != b; ++it) matching.push_back(it->second);
    } else if (e.type == 0) {
        for (const KeyHandler& h : w->keyHandlers)
            if (h.keyCode == e.code && h.eventType == e.code2 && (h.modifiers & 0xF) == (e.modifiers & 0xF)) matching.push_back(h.callback);
    } else if (e.type == 1) {
        for (const MouseHandler& h : w->mouseHandlers)
            if (h.buttons == e.code && h.eventType == e.code3) matching.push_back(h.callback);
    } else if (e.type == 3) {
        if (auto it = w->animHandlers.find(e.message); it != w->animHandlers.end()) matching.push_back(it->second);
    } else if (e.type == 4) {
        if (auto it = w->imeHandlers.find(e.message); it != w->imeHandlers.end()) matching.push_back(it->second);
    }
    // Newest registration first: the registration (FUN_008b4b40) inserts each handler at the front of the pattern's std::list and the
    // dispatch (FUN_008b4ce0) walks it from the front until one returns eHandledStop. (So intromovie.py's end-of-movie handler, newer
    // than intrologo.py's on the same MoviePlayer, sees intro.bik's end; the oldest-first order let intrologo's swallow it.)
    std::reverse(matching.begin(), matching.end());
    if (w->anyHandler) matching.push_back(w->anyHandler);
    int result = kNotHandled;
    for (PyObject* cb : matching) {
        result = std::max(result, callHandler(cb, w, e));
        if (result == kHandledStop) break;
    }
    return result;
}

// ---- Built-in behaviour -----------------------------------------------------------------------------------------------------------
// PushButton (pushbutton.cpp, traced): SetHighlighted FUN_008e3e50 sends eMessageButtonHighlighted, phase On / Off; SetPushed
// FUN_008e3f20 sends eMessageButtonDown / Up. ToggleButton: FUN_008e8820 / FUN_008e8900 likewise (+ 0x18A / + 0x18B), SetToggled
// FUN_008e86e0 - unless locked - eMessageValueChanged {'value'} (phase 1) and eMessageButtonPressed.
// Sounds: PushButton SetHighlighted / SetPushed play Highlight / UnHighlight and Press / UnPress by the new state; ToggleButton's
// SetPressed and SetToggled likewise (Press / UnPress, Toggle / UnToggle), but its SetHighlighted (FUN_008e8820) picks the sound
// from the OLD state before storing the new one - highlighting plays UnHighlightSound, un-highlighting HighlightSound - as the exe does.
// A locked ToggleButton (SetLocked, + 0x188) is neither highlighted nor pressed: SetHighlighted FUN_008e8820, SetPressed FUN_008e8900
// and SetToggled FUN_008e86e0 all return at once while it is set - so pressing the selected (locked) row of the server or character
// list plays no click.
void setHighlighted(Widget* w, bool v) {
    if (w->kind == Kind::ToggleButton && w->locked) return;
    if (w->highlighted == v) return;
    if (w->kind == Kind::ToggleButton) playSound(w, w->highlighted ? w->highlightSound : w->unhighlightSound);
    else playSound(w, v ? w->highlightSound : w->unhighlightSound);
    w->highlighted = v;
    sendMessage(w, 14, v ? 1 : 2);
}
void setPushed(Widget* w, bool v) {
    if (w->kind == Kind::ToggleButton && w->locked) return;
    if (w->pushed == v) return;
    w->pushed = v;
    playSound(w, v ? w->pressSound : w->unpressSound);
    sendMessage(w, v ? 12 : 11);
}
void setToggled(Widget* w, bool v) {
    if (w->locked || w->toggled == v) return;
    w->toggled = v;
    playSound(w, v ? w->toggleSound : w->untoggleSound);
    sendMessage(w, 22, 1, {{"value", v ? u"1" : u"0"}});
    sendMessage(w, 13);
}
Widget* checkBoxButton(Widget* box) {
    for (const auto& c : box->children)
        if (c->kind == Kind::ToggleButton) return c.get();
    return nullptr;
}
// CheckBox::SetChecked (FUN_008d50c0): its Button's SetToggled, then eMessageValueChanged {'value'} (phase 1) from the box.
void setChecked(Widget* box, bool v) {
    Widget* button = checkBoxButton(box);
    if (!button || button->toggled == v) return;
    setToggled(button, v);
    box->checked = v;
    sendMessage(box, 22, 1, {{"value", v ? u"1" : u"0"}});
}

bool contains(Widget* w, int x, int y) {
    Rect r = manager().drawnRect(w);
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// Mouse on a button (PushButton FUN_008e44e0 / FUN_008e4530; ToggleButton FUN_008e8ca0 / FUN_008e8cf0; CheckBox FUN_008d53a0 /
// FUN_008d53f0 driving its Button): left button down captures and pushes, up releases and - when pushed - un-pushes and presses.
int buttonMouse(Widget* w, const Event& e) {
    if (!w->enabled || e.code != 1) return kNotHandled;
    Widget* face = w->kind == Kind::CheckBox ? checkBoxButton(w) : w;
    if (!face) return kNotHandled;
    if (e.code3 == 0) {
        pushCapture(w);
        setPushed(face, true);
        return kHandledStop;
    }
    if (e.code3 != 1) return kNotHandled;
    releaseCapture(w);
    if (!face->pushed) {
        setHighlighted(face, false);
        return kHandledStop;
    }
    setPushed(face, false);
    if (w->kind == Kind::PushButton) {
        // eMessageButtonPressed, even when released outside, then un-highlighted if outside (FUN_008e4530).
        Event pressed;
        pressed.message = 13;
        pressed.source = w;
        pressed.modifiers = e.modifiers;
        handleEvent(w, pressed);
        if (!contains(w, manager().mouseX, manager().mouseY)) setHighlighted(w, false);
    } else if (w->kind == Kind::CheckBox) {
        setChecked(w, !face->toggled);
    } else {
        setToggled(w, !w->toggled);
    }
    return kHandledStop;
}

// MouseEnter / MouseExit on a button (FUN_008e4650 / FUN_008e46d0, FUN_008e8d60 / FUN_008e8de0, FUN_008d5470 / FUN_008d54f0): pushed
// while it has the capture, else highlighted.
int buttonHover(Widget* w, bool enter) {
    if (!w->enabled) return kNotHandled;
    Widget* face = w->kind == Kind::CheckBox ? checkBoxButton(w) : w;
    if (!face) return kNotHandled;
    if (manager().capture() == w) setPushed(face, enter);
    else setHighlighted(face, enter);
    return 1;
}

// The input validators (InputValidator*, vtables 0x00c0de28 AlphaNumeric / 0x00c11da4 Numeric / 0x00c0de34 Password / 0x00c0de40
// Unrestricted): slot 1 takes a character - iswalnum / iswdigit / iswalnum or iswpunct / iswprint -, slot 0 a whole text (every character
// by slot 1; Numeric then wcstol base 10, rejecting LONG_MAX / LONG_MIN (32-bit on the game's Windows) and anything outside min .. max).
// UNVERIFIED: the isw* results for characters above 255 follow this platform's C library (the exe: MSVC's).
bool validatorChar(int validator, char16_t c) {
    const wint_t w = wint_t(c);
    switch (validator) {
        case 1: return std::iswalnum(w) != 0;
        case 2: return std::iswdigit(w) != 0;
        case 3: return std::iswalnum(w) != 0 || std::iswpunct(w) != 0;
        case 4: return std::iswprint(w) != 0;
        default: return true;
    }
}
bool validatorTakes(const Widget* w, char16_t c, const std::u16string& text) {
    if (w->validator == 0) return true;
    if (!validatorChar(w->validator, c)) return false;
    for (char16_t t : text)
        if (!validatorChar(w->validator, t)) return false;
    if (w->validator != 2) return true;
    long long v = 0;
    for (char16_t t : text) {
        if (t < u'0' || t > u'9') break;
        v = v * 10 + (t - u'0');
        if (v > 0x7fffffffLL) return false;     // wcstol's overflow: LONG_MAX
    }
    if (v == 0x7fffffffLL) return false;
    return w->validatorMin <= v && v <= w->validatorMax;
}

}  // namespace

void setValidator(Widget* w, const std::string& name) {
    int v = 0;
    if (name == "AlphaNumeric") v = 1;
    else if (name == "Numeric") v = 2;
    else if (name == "Password") v = 3;
    else if (name == "Unrestricted") v = 4;
    if (v == 0) return;
    w->validator = v;
    w->validatorMin = w->validatorMax = 0;
}

namespace {
void editChanged(Widget* w) {
    // EditBox insert / erase (FUN_008d17b0, FUN_008d1880, FUN_008d1960): eMessageValueChanged from the box.
    sendMessage(w, 22, 1);
}

size_t caretOf(Widget* w) { return std::min(w->caret, w->text.size()); }

// The selection (editbox.cpp): FUN_008d1b60 clears it (anchor = end = 0); FUN_008d1e20 sets anchor and end; FUN_008d1bb0 removes the
// selected text (the caret to its start, the characters erased - FUN_008d1960, DelCharSound -, the selection cleared).
bool hasSelection(const Widget* w) { return w->selAnchor != w->selEnd; }
void clearSelection(Widget* w) {
    if (!hasSelection(w)) return;
    w->selAnchor = w->selEnd = 0;
    manager().dirty = true;
}
void setSelection(Widget* w, size_t anchor, size_t end) {
    w->selAnchor = anchor;
    w->selEnd = end;
    manager().dirty = true;
}
void deleteSelection(Widget* w) {
    if (!hasSelection(w)) return;
    const size_t n = w->text.size();
    const size_t lo = std::min(std::min(w->selAnchor, w->selEnd), n), hi = std::min(std::max(w->selAnchor, w->selEnd), n);
    w->caret = lo;
    if (hi > lo) {
        w->text.erase(lo, hi - lo);
        playSound(w, w->delCharSound);
        editChanged(w);
    }
    w->selAnchor = w->selEnd = 0;
    manager().dirty = true;
}
// A caret key's move (FUN_008d1f20): with Shift (and the box selecting, + 0x138 - 1 from the constructor) the selection runs from its
// anchor - the old caret when there was none - to the new caret, else it is cleared.
void moveCaret(Widget* w, size_t from, size_t to, bool shift) {
    if (!shift) clearSelection(w);
    else setSelection(w, hasSelection(w) ? w->selAnchor : from, to);
    w->caret = to;
    playSound(w, w->cursorSound);
}

// ---- IME (imemanager.cpp / editbox.cpp, traced 2026-10-28) -----------------------------------------------------------------------
// The exe's IMEManager handles the window's IME messages (FUN_008a1880) and sends IMEEvents (type 4, + 0x20 the code = uievent.ime):
// 0 activated / 1 deactivated when it opens / closes the context (FUN_008a1600 / FUN_008a16a0, an allowIME box's focus), 2 language
// changed (WM_INPUTLANGCHANGE, FUN_008a1980), 3 / 4 enabled / disabled (FUN_008a17f0 / FUN_008a1770), 6 composition start
// (WM_IME_STARTCOMPOSITION, FUN_008a1c00: + 0x49 set, the result cleared), and per WM_IME_COMPOSITION (FUN_008a1e30) 5 typing, then 10
// cancel (lParam 0: the composition cleared), 11 result (GCS_RESULTSTR into + 0xb48) or 7 change (the composition + 0x24c, its
// attributes / clauses, the cursor + 0x748); 8 composition full (WM_IME_COMPOSITIONFULL); at WM_IME_ENDCOMPOSITION (FUN_008a1c70) 11
// with the result (or the composition when the context is closed), the state cleared, 9 end; 12 / 13 / 14 candidate list open / change /
// close (WM_IME_NOTIFY, FUN_008a21e0 / FUN_008a22c0 / FUN_008a2380). Each goes to the manager's handlers - the focused allowIME box's
// (registered by FUN_008d0400) - and then, unless one stopped it, to the IME window's (FUN_008a2480: imewindow.py, which shows the
// candidate list). The box's handlers (0x008d0d00 ..): 0 / 1 set / clear + 0xED; 6 sets + 0xEE (in composition); 7 and 9
// (FUN_008d2ec0) delete the selection (the composition shown so far), insert the composition at the caret (the length limit only -
// FUN_008d1880) and select it, the caret after it - so the composition is the box's highlighted text; 9 also clears + 0xEE; 10 nothing;
// 11 (FUN_008d0dc0): with a selection, it is replaced by the result (FUN_008d1cd0) and cleared; without one, the result goes in at the
// caret when the validator takes it, selected, with the AddChar sound; 2 (keyboard layout calls) is not reproduced. All return
// eHandled (1). OpenRasa takes the IME from SDL (cross-platform): SDL_EVENT_TEXT_EDITING / TEXT_INPUT / TEXT_EDITING_CANDIDATES with
// SDL_HINT_IME_IMPLEMENTED_UI "composition,candidates" (the game draws both, as the exe handles those messages). The sequence above is
// rebuilt from them (imeEditing / imeCommit / imeCandidates); the IME's own paging and selection (ImmNotifyIME) cannot be asked of SDL.
// UNVERIFIED: the exact Win32 message order of a commit (the exe's ENDCOMPOSITION re-reads the result) - one 11 per commit here.
void imeReset() {
    Manager& m = manager();
    m.imeComposing = false;
    m.imeComposition.clear();
    m.imeCursor = 0;
}
// Insert at a position (FUN_008d1880): only the length limit is checked; eMessageValueChanged.
bool imeInsert(Widget* w, size_t at, const std::u16string& text) {
    if (w->lengthLimit > 0 && int(text.size() + w->text.size()) > w->lengthLimit) return false;
    w->text.insert(std::min(at, w->text.size()), text);
    w->textSet = true;
    editChanged(w);
    manager().dirty = true;
    return true;
}
void imeBoxEvent(Widget* w, int code) {
    Manager& m = manager();
    switch (code) {
        case 0: w->imeOpen = true; break;
        case 1: w->imeOpen = false; break;
        case 6: w->imeComposing = true; break;
        case 7:
        case 9: {
            deleteSelection(w);
            const size_t c = caretOf(w);
            if (imeInsert(w, c, m.imeComposition)) {
                setSelection(w, c, c + m.imeComposition.size());
                w->caret = c + m.imeComposition.size();
            }
            if (code == 9) w->imeComposing = false;
            break;
        }
        case 11:
            if (!hasSelection(w)) {
                if (!m.imeResult.empty() && !validatorTakes(w, m.imeResult[0], m.imeResult)) break;
                const size_t c = caretOf(w);
                if (imeInsert(w, c, m.imeResult)) {
                    setSelection(w, c, c + m.imeResult.size());
                    w->caret = c + m.imeResult.size();
                    playSound(w, w->addCharSound);
                }
            } else {
                const size_t n = w->text.size();
                const size_t lo = std::min(std::min(w->selAnchor, w->selEnd), n);
                deleteSelection(w);
                if (imeInsert(w, lo, m.imeResult)) w->caret = lo + m.imeResult.size();
                clearSelection(w);
            }
            break;
        default: break;
    }
}
void imeSend(int code) {
    Manager& m = manager();
    Event e;
    e.type = 4;
    e.message = code;
    if (m.imeBox && m.imeBox->enabled) imeBoxEvent(m.imeBox, code);
    if (m.imeWindow) handleEvent(m.imeWindow, e);
    m.dirty = true;
}

// EditBox (editbox.cpp; the keys as RasaEdit's notes trace them: FUN_008d1f20 / FUN_008d20e0). Selection is not handled here.
int editKey(Widget* w, const Event& e) {
    if (!w->enabled) return kNotHandled;
    if (e.code2 == 2) {   // eKeyChar (FUN_008d0070)
        // FUN_008d0070: a character is eHandledStop whether or not it goes in. It goes in when the font has it (FUN_008d1a10; here:
        // not a control character), the validator takes the character and then the text it would make, and the length limit allows.
        if (e.character == 0) return w->consumeAllKeys ? kHandledStop : kNotHandled;
        // The validator sees the text with the character in place of the selection (FUN_008d1a10); an accepted character first
        // removes the selection (FUN_008d1bb0), then goes in at the caret.
        std::u16string next = w->text;
        if (hasSelection(w)) {
            const size_t lo = std::min(std::min(w->selAnchor, w->selEnd), next.size()), hi = std::min(std::max(w->selAnchor, w->selEnd), next.size());
            next.replace(lo, hi - lo, 1, e.character);
        } else {
            next.insert(std::min(caretOf(w), next.size()), 1, e.character);
        }
        if (e.character < 0x20 || !validatorTakes(w, e.character, next)) return kHandledStop;
        deleteSelection(w);
        if (w->lengthLimit <= 0 || int(w->text.size()) < w->lengthLimit) {
            size_t c = caretOf(w);
            w->text.insert(c, 1, e.character);
            w->caret = c + 1;
            playSound(w, w->addCharSound);
            w->textSet = true;
            editChanged(w);
            return kHandledStop;
        }
        return kHandledStop;
    }
    if (e.code2 != 0) return w->consumeAllKeys ? kHandledStop : kNotHandled;
    // Key down (FUN_008d0190): a key neither the caret keys (FUN_008ddc30) nor the edit keys (FUN_008ddd40) take is eHandledStop while
    // the box's IME flag (+ 0xED) is set - so a focused box with allowIME keeps every key from the input mapper (FUN_008aa8a0 passes
    // a key to the mapper only when the focus widget did not stop it).
    size_t c = caretOf(w);
    bool ctrl = (e.modifiers & 4) != 0, shift = (e.modifiers & 1) != 0;
    // The numpad's 7 / 8 / 4 / 6 / 1 / 2 / . are caret / edit keys only with NumLock off: the exe's Win32 keyboard device gets VK_HOME /
    // VK_UP / VK_LEFT ... from Windows then, but VK_NUMPAD7 ... (a digit, typed through WM_CHAR) with NumLock on. OpenRasa's codes are
    // DirectInput's, the same for both, so the lock state is read here.
    const bool numLockOff = testNumLock >= 0 ? testNumLock == 0 : (SDL_GetModState() & SDL_KMOD_NUM) == 0;
    if (!numLockOff && (e.code == 0x47 || e.code == 0x48 || e.code == 0x4B || e.code == 0x4D || e.code == 0x4F || e.code == 0x50 ||
                        e.code == 0x53))
        return w->consumeAllKeys || w->imeOpen ? kHandledStop : kNotHandled;
    switch (e.code) {
        // Caret moves play CursorSound (FUN_008d1f20), deletions DelCharSound (FUN_008d20e0). Up / Down keep the caret.
        case 0xCB: case 0x4B: moveCaret(w, c, ctrl ? 0 : (c > 0 ? c - 1 : 0), shift); return kHandledStop;
        case 0xCD: case 0x4D: moveCaret(w, c, ctrl ? w->text.size() : std::min(c + 1, w->text.size()), shift); return kHandledStop;
        case 0xC7: case 0x47: moveCaret(w, c, 0, shift); return kHandledStop;
        case 0xCF: case 0x4F: moveCaret(w, c, w->text.size(), shift); return kHandledStop;
        case 0xC8: case 0x48: case 0xD0: case 0x50: moveCaret(w, c, c, shift); return kHandledStop;
        case 0x0E:   // backspace: the selection, else the character before the caret
            if (hasSelection(w)) {
                deleteSelection(w);
            } else if (c > 0) {
                w->text.erase(c - 1, 1);
                w->caret = c - 1;
                playSound(w, w->delCharSound);
                editChanged(w);
            }
            return kHandledStop;
        case 0xD3: case 0x53:   // delete: the selection, else the character after the caret
            if (hasSelection(w)) {
                deleteSelection(w);
            } else if (c < w->text.size()) {
                w->text.erase(c, 1);
                playSound(w, w->delCharSound);
                editChanged(w);
            }
            return kHandledStop;
        default:
            return w->consumeAllKeys || w->imeOpen ? kHandledStop : kNotHandled;
    }
}



// The edit box's mouse (editbox.cpp, traced 2026-10-28). Down of button 1 (FUN_008d0ec0) on an enabled box: the keyboard focus
// (FUN_008aafc0) and the mouse capture (FUN_008aac50), the character under the mouse (hit test + 0x204); with Shift and the caret
// elsewhere the selection runs from its anchor - the old caret when there was none - to it, else it is cleared; the caret moves there
// and the box is selecting (+ 0x18F). A move while selecting (FUN_008d1040) does the same without Shift. Up of button 1
// (FUN_008d0fe0): the selection cleared unless it was selecting, selecting ended, the capture let go.
int editMouse(Widget* w, const Event& e) {
    if (!w->enabled) return kNotHandled;
    auto pointed = [&] {
        size_t i = w->text.size();
        editIndexAt(w, e.locX, i);   // a box not drawn yet: the end
        return i;
    };
    if (e.code3 == 0 && e.code == 1) {
        pushFocus(w);
        pushCapture(w);
        const size_t i = pointed(), c = caretOf(w);
        if (!(e.modifiers & 1) || i == c) clearSelection(w);
        else setSelection(w, hasSelection(w) ? w->selAnchor : c, i);
        w->caret = i;
        w->selecting = true;
        manager().dirty = true;
        return kHandledStop;
    }
    if ((e.code3 == 4 || e.code3 == 5) && w->selecting) {
        const size_t i = pointed(), c = caretOf(w);
        if (i != c) {
            setSelection(w, hasSelection(w) ? w->selAnchor : c, i);
            w->caret = i;
        }
        return kHandledStop;
    }
    if (e.code3 == 1 && e.code == 1) {
        if (!w->selecting) clearSelection(w);
        w->selecting = false;
        releaseCapture(w);
        return kHandledStop;
    }
    return kNotHandled;
}

// ---- ComboBox (combobox.cpp, traced 2026-10-25) ------------------------------------------------------------------------------------
// The loader (FUN_008f0ee0) keeps the parts Button (+ 0xC8, a ToggleButton), Text (+ 0xCC), DropDown (+ 0xD0, a WidgetComposite) - its
// children - and Entry (+ 0xD4, a PushButton pattern, not added); the strings (+ 0xE8 ..), the selected index (+ 0xC4) and the drop-down's
// items (+ 0xDC ..). Mapped commands: eMessageValueChanged on the Button -> 0x008f1ad0 (show / hide the list by the Button's toggled
// state), mouse up of button 1 or 2 on the DropDown -> FUN_008f1af0 (outside the list while open: untoggle the Button), capture lost
// on the DropDown -> 0x008f1b50 (while open, when the new capture is not inside the combo box: untoggle), and on each item
// eMessageButtonPressed -> 0x008f1a70 (select it, untoggle the Button). Each returns eHandledStop when it acts.
Widget* comboPart(Widget* combo, const char* tag) {
    for (const auto& c : combo->children)
        if (c->tag == tag) return c.get();
    return nullptr;
}

// FUN_008f16e0: shown - only with strings - the DropDown takes the mouse capture (FUN_008aac50) and is made visible; hidden, it lets the
// capture go (FUN_008aadc0) and is hidden.
void comboShow(Widget* combo, bool show) {
    Widget* drop = comboPart(combo, "DropDown");
    if (!drop) return;
    if (!show) {
        releaseCapture(drop);
    } else {
        if (combo->stringList.empty()) return;
        pushCapture(drop);
    }
    setVisible(drop, show);
    manager().dirty = true;
}

int comboSelect(Widget* combo, int index) {
    // FUN_008f13a0: the index stored, the Text part given that string ("" past the end), then eMessageValueChanged {'value': index}
    // (phase 1) from the combo box - sent even when the index is unchanged.
    combo->value = index;
    Widget* text = comboPart(combo, "Text");
    if (text) {
        text->text = index >= 0 && size_t(index) < combo->stringList.size() ? combo->stringList[size_t(index)] : u"";
        text->textSet = true;
    }
    manager().dirty = true;
    const std::string digits = std::to_string(index);
    return sendMessage(combo, 22, 1, {{"value", std::u16string(digits.begin(), digits.end())}});
}

// FUN_008f1980: the list hidden and its capture dropped, the items destroyed. FUN_008f1760: one copy of the Entry pattern per string, at
// (0, entry height x i), its Text part given the string, then the DropDown sized entry width x (entry height x count).
void comboRebuild(Widget* combo) {
    Widget* drop = comboPart(combo, "DropDown");
    Widget* entry = comboPart(combo, "Entry");
    if (!drop) return;
    releaseCapture(drop);
    setVisible(drop, false);
    drop->children.clear();
    if (!entry || !entry->source || combo->stringList.empty()) return;
    for (size_t i = 0; i < combo->stringList.size(); ++i) {
        std::unique_ptr<Widget> item = manager().buildCopy(*entry->source, drop);
        if (!item) continue;
        item->visible = true;
        item->offsetX = 0;
        item->offsetY = entry->height * int(i);
        for (const auto& c : item->children)
            if (c->kind == Kind::Text) {
                c->text = combo->stringList[i];
                c->textSet = true;
            }
        drop->children.push_back(std::move(item));
    }
    drop->width = entry->width;
    drop->height = entry->height * int(combo->stringList.size());
    manager().dirty = true;
}

int comboPartCommands(Widget* w, const Event& e) {
    if (w->parent && w->parent->kind == Kind::ComboBox) {
        Widget* combo = w->parent;
        Widget* button = comboPart(combo, "Button");
        if (w->tag == "Button" && e.type == 2 && e.message == 22) {
            comboShow(combo, w->toggled);
            return kHandledStop;
        }
        if (w->tag == "DropDown" && button) {
            if (e.type == 1 && e.code3 == 1 && (e.code == 1 || e.code == 2) && button->toggled && !contains(w, e.locX, e.locY)) {
                setToggled(button, false);
                return kHandledStop;
            }
            if (e.type == 2 && e.message == 8 && button->toggled) {
                bool inside = false;
                for (Widget* p = manager().capture(); p; p = p->parent)
                    if (p == combo) inside = true;
                if (!inside) {
                    setToggled(button, false);
                    return kHandledStop;
                }
            }
        }
        return kNotHandled;
    }
    Widget* drop = w->parent;
    if (w->tag == "Entry" && drop && drop->tag == "DropDown" && drop->parent && drop->parent->kind == Kind::ComboBox && e.type == 2 &&
        e.message == 13) {
        Widget* combo = drop->parent;
        int index = 0;
        for (size_t i = 0; i < drop->children.size(); ++i)
            if (drop->children[i].get() == w) index = int(i);
        comboSelect(combo, index);
        if (Widget* button = comboPart(combo, "Button")) setToggled(button, false);
        return kHandledStop;
    }
    return kNotHandled;
}


// ---- Slider (slider.cpp, traced 2026-10-25) -----------------------------------------------------------------------------------------
// The loader (FUN_008da640) keeps Track (+ 0xC4) and Thumb (+ 0xC8) and registers on both: eMessageButtonDown -> FUN_008db250,
// eMessageButtonUp -> FUN_008db380, mouse move -> FUN_008db3c0; its own mouse handler (slot 13, FUN_008db220) takes the wheel. Fields:
// min + 0xE0, max + 0xE4, value + 0xEC, the last mouse position + 0xCC / + 0xD0, the drag anchor + 0xD4 / + 0xD8 and flag + 0xDC.
Widget* sliderPart(Widget* slider, const char* tag) {
    for (const auto& c : slider->children)
        if (c->tag == tag) return c.get();
    return nullptr;
}

// FUN_008db630: r = (value - min) / (max - min) (0 for an empty range); horizontal: the thumb at (trunc((width - thumb width) x r),
// (height - thumb height) / 2), vertical: ((width - thumb width) / 2, trunc((height - thumb height) x r)) - the slider's own size.
void sliderLayout(Widget* slider) {
    Widget* thumb = sliderPart(slider, "Thumb");
    if (!thumb || !sliderPart(slider, "Track")) return;
    const int span = slider->rangeMax - slider->rangeMin;
    const float r = span != 0 ? float(slider->value - slider->rangeMin) / float(span) : 0.0f;
    if (slider->horizontal) {
        thumb->offsetX = int(r * float(slider->width - thumb->width));
        thumb->offsetY = (slider->height - thumb->height) / 2;
    } else {
        thumb->offsetX = (slider->width - thumb->width) / 2;
        thumb->offsetY = int(r * float(slider->height - thumb->height));
    }
    manager().dirty = true;
}

void sliderValue(Widget* slider, int value) {
    const int v = std::clamp(value, slider->rangeMin, std::max(slider->rangeMin, slider->rangeMax));
    if (v == slider->value) return;
    slider->value = v;
    const std::string digits = std::to_string(v);
    sendMessage(slider, 22, 1, {{"value", std::u16string(digits.begin(), digits.end())}});
    sliderLayout(slider);
}

// ButtonDown (FUN_008db250): on the track the value jumps to the mouse - min + trunc((max - min) x f + 0.5), f = the mouse's place along
// the track, clamped to 0 .. 1 (the expression as RasaEdit's notes trace it) - and the thumb, when not pushed, is made unpickable and
// pushed (its own ButtonDown starts a drag); on the thumb a drag starts, anchored at the last mouse position. ButtonUp (FUN_008db380):
// the drag ends, the thumb and track un-pushed, the thumb pickable again. Mouse move (FUN_008db3c0): the position kept; while dragging,
// the thumb's offset moved by the mouse since the anchor, clamped to 0 .. size - thumb size, sets the value in proportion and the anchor
// moves by what the thumb moved (assembly 0x008db3c0: value = trunc(ratio x (max - min) + min + 0.5), 0.5 = the double at 0x00c2b4c0).
// ButtonHighlighted on the track / thumb (0x008db530): the other part's highlight follows when it differs; stopped. MouseEnter /
// MouseExit on the thumb (0x008db590 / 0x008db5e0): stopped while the thumb has the capture.
int sliderPartCommands(Widget* w, const Event& e) {
    Widget* slider = w->parent;
    if (!slider || slider->kind != Kind::Slider || (w->tag != "Track" && w->tag != "Thumb")) return kNotHandled;
    Widget* track = sliderPart(slider, "Track");
    Widget* thumb = sliderPart(slider, "Thumb");
    if (!track || !thumb) return kNotHandled;
    const int range = slider->horizontal ? slider->width - thumb->width : slider->height - thumb->height;
    if (e.type == 2 && e.message == 12) {
        if (w == track) {
            const Rect r = manager().drawnRect(track);
            const float length = float(slider->horizontal ? r.right - r.left : r.bottom - r.top);
            const float at = float(slider->horizontal ? slider->partMouseX - r.left : slider->partMouseY - r.top);
            const float f = length > 0 ? std::clamp(at / length, 0.0f, 1.0f) : 0.0f;
            sliderValue(slider, slider->rangeMin + int(float(slider->rangeMax - slider->rangeMin) * f + 0.5f));
            if (!thumb->pushed) {
                thumb->pickable = false;
                setPushed(thumb, true);
            }
        } else {
            slider->dragX = slider->partMouseX;
            slider->dragY = slider->partMouseY;
            slider->dragging = true;
        }
        return 1;
    }
    if (e.type == 2 && e.message == 11) {
        slider->dragging = false;
        setPushed(track, false);
        setPushed(thumb, false);
        thumb->pickable = true;
        return 1;
    }
    if (e.type == 1 && e.code3 == 4) {
        slider->partMouseX = e.locX;
        slider->partMouseY = e.locY;
        if (!slider->dragging) return 1;
        const int along = slider->horizontal ? e.locX - slider->dragX + thumb->offsetX : e.locY - slider->dragY + thumb->offsetY;
        const int d = std::min(range, std::max(0, along));
        const float f = range > 0 ? float(d) / float(range) : 0.0f;
        const int oldX = thumb->offsetX, oldY = thumb->offsetY;
        sliderValue(slider, int(f * float(slider->rangeMax - slider->rangeMin) + float(slider->rangeMin) + 0.5f));
        slider->dragX += thumb->offsetX - oldX;
        slider->dragY += thumb->offsetY - oldY;
        return 1;
    }
    if (e.type == 2 && e.message == 14) {
        Widget* other = w == thumb ? track : thumb;
        if (other->highlighted != w->highlighted) setHighlighted(other, w->highlighted);
        return kHandledStop;
    }
    if (e.type == 2 && (e.message == 5 || e.message == 6) && w == thumb) return manager().capture() == thumb ? kHandledStop : kNotHandled;
    return kNotHandled;
}

// ---- ScrollBar (scrollbar.cpp, traced 2026-10-25) ----------------------------------------------------------------------------------
// The loader (FUN_008d6ce0) keeps its parts - ScrollUpBtn (+ 0xC8, "GreaterBtn"), ScrollDownBtn (+ 0xCC, "LesserBtn"), Track (+ 0xD0),
// Thumb (+ 0xD4) - and registers mapped commands on them (MappedCommandC<ScrollBar>, in the parts' handler lists like Python handlers):
// eMessageButtonDown on all four -> 0x008d7cd0, eMessageButtonUp on all four -> FUN_008d7e10, mouse move on the track and the thumb ->
// FUN_008d7e60, MouseEnter / MouseExit on the thumb -> 0x008d7fe0 / 0x008d8030, ButtonHighlighted on the track and the thumb ->
// 0x008d8080. Its own mouse handler (slot 13, FUN_008d7ca0) takes the wheel.
Widget* scrollPart(Widget* bar, const char* tag) {
    for (const auto& c : bar->children)
        if (c->tag == tag) return c.get();
    return nullptr;
}

// FUN_008d84f0 (the layout, as RasaEdit's UiCanvas::applyScroll, which matches all 63 exported thumbs): r = (value - min) / ((max - min
// + 1) - pageSize); vertical: the thumb at (0, ScrollUpBtn's height + trunc((track height - thumb height) x r)), horizontal: at
// (ScrollDownBtn's width + trunc((track width - thumb width) x r), 0).
void thumbLayout(Widget* bar) {
    Widget* thumb = scrollPart(bar, "Thumb");
    Widget* track = scrollPart(bar, "Track");
    if (!thumb) return;
    const int span = (bar->rangeMax - bar->rangeMin + 1) - bar->pageSize;
    const float r = span != 0 ? float(bar->value - bar->rangeMin) / float(span) : 0.0f;
    const int trackW = track ? track->width : bar->width, trackH = track ? track->height : bar->height;
    if (bar->horizontal) {
        Widget* down = scrollPart(bar, "ScrollDownBtn");
        thumb->offsetX = (down ? down->width : 0) + int(float(trackW - thumb->width) * r);
        thumb->offsetY = 0;
    } else {
        Widget* up = scrollPart(bar, "ScrollUpBtn");
        thumb->offsetX = 0;
        thumb->offsetY = (up ? up->height : 0) + int(float(trackH - thumb->height) * r);
    }
    manager().dirty = true;
}

void scrollValue(Widget* bar, int value) {
    const int top = bar->rangeMax - bar->pageSize + 1;
    const int v = bar->rangeMin < top ? std::clamp(value, bar->rangeMin, top) : bar->rangeMin;
    if (v != bar->value) {
        bar->value = v;
        const std::string digits = std::to_string(v);
        sendMessage(bar, 22, 1, {{"value", std::u16string(digits.begin(), digits.end())}});
    }
    thumbLayout(bar);
}

void startRepeat(Widget* bar, Widget* part) {
    if (bar->repeatPart) return;
    bar->repeatPart = part;
    bar->repeatMs = 0;
    manager().repeatingBars.push_back(bar);
}

// eMessageButtonDown from a part (0x008d7cd0): ScrollDownBtn value + 1, ScrollUpBtn max(min, value - 1) - each starting the repeat when
// none runs; the track starts the repeat (the first time also making the thumb unpickable and pushed - SetPickable, SetPushed, which
// sends the thumb's own ButtonDown) and steps one unit towards the mouse: before the thumb - 1 (FUN_008d7b20), past it + 1, on it
// nothing (FUN_008d8690: the last mouse position against the thumb's rectangle); the thumb starts a drag anchored at the last mouse
// position. eMessageButtonUp from a part (FUN_008d7e10): repeat and drag end, the thumb pickable and un-pushed again.
int scrollPartMessage(Widget* bar, Widget* part, const Event& e) {
    Widget* thumb = scrollPart(bar, "Thumb");
    if (e.message == 12) {
        if (part->tag == "ScrollDownBtn") {
            startRepeat(bar, part);
            scrollValue(bar, bar->value + 1);
        } else if (part->tag == "ScrollUpBtn") {
            startRepeat(bar, part);
            scrollValue(bar, std::max(bar->rangeMin, bar->value - 1));
        } else if (part->tag == "Track") {
            if (!bar->repeatPart) {
                startRepeat(bar, part);
                if (thumb && !thumb->pushed) {
                    thumb->pickable = false;
                    setPushed(thumb, true);
                }
            }
            if (thumb) {
                const Rect r = manager().drawnRect(thumb);
                const int mouse = bar->horizontal ? bar->partMouseX : bar->partMouseY;
                const int lo = bar->horizontal ? r.left : r.top, hi = bar->horizontal ? r.right : r.bottom;
                if (mouse < lo) scrollValue(bar, std::max(bar->rangeMin, bar->value - 1));
                else if (mouse > hi) scrollValue(bar, bar->value + 1);
            }
        } else if (part == thumb) {
            bar->dragX = bar->partMouseX;
            bar->dragY = bar->partMouseY;
            bar->dragging = true;
        }
        return 1;
    }
    if (e.message == 11) {
        bar->repeatPart = nullptr;
        bar->repeatMs = 0;
        bar->dragging = false;
        auto& list = manager().repeatingBars;
        list.erase(std::remove(list.begin(), list.end(), bar), list.end());
        if (thumb) {
            thumb->pickable = true;
            setPushed(thumb, false);
        }
        return 1;
    }
    // MouseEnter / MouseExit on the thumb: stopped while the thumb has the mouse capture (so it stays pushed through a drag);
    // ButtonHighlighted on the track / thumb: always stopped.
    if ((e.message == 5 || e.message == 6) && part == thumb) return manager().capture() == thumb ? kHandledStop : kNotHandled;
    if (e.message == 14 && (part == thumb || part->tag == "Track")) return kHandledStop;
    return kNotHandled;
}

// Mouse move on the track / thumb (FUN_008d7e60): the position is kept; while dragging, the thumb's offset along the bar moved by the
// mouse since the anchor, clamped to 0 .. track - thumb, gives the ratio and value = trunc(ratio x ((max - pageSize - min) + 1) + min)
// (SetValue clamps); the anchor then moves by what the thumb moved. (The offset includes the ScrollUpBtn / ScrollDownBtn size the layout
// adds, so a grab first steps the value by that many pixels' worth - as the exe computes it.)
int scrollPartMove(Widget* bar, const Event& e) {
    bar->partMouseX = e.locX;
    bar->partMouseY = e.locY;
    Widget* thumb = scrollPart(bar, "Thumb");
    Widget* track = scrollPart(bar, "Track");
    if (!bar->dragging || !thumb || !track) return 1;
    const int rangeX = track->width - thumb->width, rangeY = track->height - thumb->height;
    const int dx = std::min(rangeX, std::max(0, e.locX - bar->dragX + thumb->offsetX));
    const int dy = std::min(rangeY, std::max(0, e.locY - bar->dragY + thumb->offsetY));
    const float ratio = bar->horizontal ? (rangeX != 0 ? float(dx) / float(rangeX) : 0.0f) : (rangeY != 0 ? float(dy) / float(rangeY) : 0.0f);
    const int oldX = thumb->offsetX, oldY = thumb->offsetY;
    scrollValue(bar, int(ratio * float(bar->rangeMax - bar->pageSize - bar->rangeMin + 1) + float(bar->rangeMin)));
    bar->dragX += thumb->offsetX - oldX;
    bar->dragY += thumb->offsetY - oldY;
    return 1;
}

// The mapped commands a widget carries as a scroll bar's part (in its handler list ahead of the Python ones).
int partCommands(Widget* w, const Event& e) {
    if (w->floatText)
        if (int r = floatTextAnimEvent(w, e)) return r;
    if (int r = comboPartCommands(w, e)) return r;
    if (int r = sliderPartCommands(w, e)) return r;
    Widget* bar = w->parent;
    if (!bar || bar->kind != Kind::ScrollBar) return kNotHandled;
    if (w->tag != "ScrollUpBtn" && w->tag != "ScrollDownBtn" && w->tag != "Track" && w->tag != "Thumb") return kNotHandled;
    if (e.type == 2) return scrollPartMessage(bar, w, e);
    if (e.type == 1 && e.code3 == 4 && (w->tag == "Track" || w->tag == "Thumb")) return scrollPartMove(bar, e);
    return kNotHandled;
}

// ScrollBar::Update (slot 116, FUN_008d7c00): a held part's timer runs; past 599 ms it loses 100 and the part's eMessageButtonDown
// (phase 1) is sent again - repeats at 600, 700, 800 ... ms.
// PushButton::Update (slot 116, FUN_008e4050): a repeatable button that is pushed adds the frame's milliseconds to its timer; at 50 or
// more the timer goes back to 0 and eMessageButtonPressed is sent through the button's handlers - every 50 ms while held (on top of the
// press sent when it is let go).
void repeatsFrame(int msElapsed) {
    const std::vector<Widget*> buttons = manager().repeatableButtons;
    for (Widget* b : buttons) {
        const auto& now = manager().repeatableButtons;
        if (std::find(now.begin(), now.end(), b) == now.end() || !b->pushed) continue;
        b->repeatMs += msElapsed;
        if (b->repeatMs >= 50) {
            b->repeatMs = 0;
            sendMessage(b, 13);
        }
    }
    const std::vector<Widget*> bars = manager().repeatingBars;
    for (Widget* bar : bars) {
        const auto& now = manager().repeatingBars;
        if (std::find(now.begin(), now.end(), bar) == now.end() || !bar->repeatPart) continue;
        bar->repeatMs += msElapsed;
        if (bar->repeatMs > 599) {
            bar->repeatMs -= 100;
            Event e;
            e.type = 2;
            e.phase = 1;
            e.source = bar->repeatPart;
            e.message = 12;
            handleEvent(bar->repeatPart, e);
        }
    }
}

int richTextPress(Widget* w, const Event& e) {
    RichMeasure measure;
    const RichLayout layout = richTextLayout(w, &measure);
    const Rect r = manager().screenRect(w);
    const float sx = uiScaleX();
    const float x = float(e.locX - r.left) * sx;
    const int y = e.locY - r.top;
    for (const RichRunPlace& place : placeRichRuns(layout, measure)) {
        if (x < float(place.xPixels) || x >= float(place.xPixels + place.widthPixels)) continue;
        if (y < place.yLogical || y >= place.yLogical + place.heightLogical) continue;
        const RichLayout::Link* link = layout.linkAt(layout.runs[size_t(place.run)].start);
        if (!link) continue;
        Event message;
        message.type = 2;
        message.phase = 1;
        message.source = w;
        message.message = 23;
        message.data = {{"linkData", link->data}};
        message.intData = {{"LocX", e.locX}, {"LocY", e.locY}};
        handleEvent(w, message);
        return kHandledStop;
    }
    return kNotHandled;
}

// The widget's own handler for the event (EventHandler vtable + 0xC keyboard, + 0x10 mouse, + 0x14 message ...).
// ---- Drag and drop (src\client\tr\ui\base\dndwidget.h; traced 2026-10-27) ----------------------------------------------------------
// A DNDWidget is a composite holding a template's widgets (its visual) that follows the mouse. While it lives it is the mouse capture's
// top, so it sees every mouse event first, and its handlers return eHandledStop for all of them (the mouse fallback, slot 18
// FUN_008cc390, catches what the per-event slots leave): nothing under it gets the mouse during a drag.
//   move (slot 16, FUN_008cc240): its anchor offset = the mouse + the visual offset; the widget under the mouse (the root's hit test,
//     which never finds a DND widget) is the target - on a change the old one gets eMessageDNDExit, the new one eMessageDNDEnter (each
//     from the DND widget, phase On) and the target is no longer valid until a handler calls SetTargetValid(True).
//   release (slot 13, FUN_008cc1d0; a wheel step is left to the fallback): with a valid target, the drop; else, when armed, cancelled;
//     else armed. So a drag started by eMouseDrag (the button still down) drops on its release or, with no valid target there, stays
//     and drops (or cancels) on the next click; one started otherwise drops or cancels on the first release.
//   drop (FUN_008cc400): the point kept; the widget under it gets eMessageDNDDrop with the state 0 - not handled (eNotHandled) or no
//     widget there: cancelled. The handler decides: AcceptDrop / RejectDrop end it, DeferDrop leaves it (state 2) for later.
//   capture lost (message 8, slot 27, FUN_008cc3a0): in state 0 (dropped, undecided) cancelled.
// The end (FUN_008cc4e0): eMessageDNDEnd to the source (phase On; it bubbles to the source's parents), the UI sound 10000026 (UIManager
// slot 8 -> the game client's callback FUN_004d8000 = audio set 10000026) when cancelled, the widget off the DND list and its window,
// the capture let go, and the widget deleted later (the deferred list, FUN_00896720).
int dndMessage(Widget* d, Widget* to, int message) {
    Event e;
    e.type = 2;
    e.phase = 1;
    e.source = d;
    e.message = message;
    return handleEvent(to, e);
}
void dndDrop(Widget* d, int x, int y) {
    d->dndDropX = x;
    d->dndDropY = y;
    Widget* to = hitTest(x, y);
    if (!to) {
        d->dndState = 4;
        finishDnd(d);
        return;
    }
    d->dndState = 0;
    if (dndMessage(d, to, 18) == kNotHandled) {
        d->dndState = 4;
        finishDnd(d);
    }
}
int dndBuiltIn(Widget* d, Event& e) {
    if (e.type == 2) {
        if (e.message == 8 && d->dndState == 0) {
            d->dndState = 4;
            finishDnd(d);
            return 1;
        }
        return kNotHandled;
    }
    if (e.type != 1) return kNotHandled;
    if (e.code3 == 4) {   // move
        d->offsetX = e.locX + d->dndOffsetX;
        d->offsetY = e.locY + d->dndOffsetY;
        manager().dirty = true;
        Widget* under = hitTest(e.locX, e.locY);
        if (under != d->dndTarget) {
            if (d->dndTarget) dndMessage(d, d->dndTarget, 17);
            d->dndTarget = under;
            d->dndTargetValid = false;
            if (under) dndMessage(d, under, 16);
        }
        return kHandledStop;
    }
    if (e.code3 == 1 && e.code != 0x20 && e.code != 0x40) {   // release
        if (d->dndTargetValid) dndDrop(d, e.locX, e.locY);
        else if (d->dndArmed) {
            d->dndState = 4;
            finishDnd(d);
        } else {
            d->dndArmed = true;
        }
    }
    return kHandledStop;
}

int builtIn(Widget* w, Event& e) {
    switch (w->kind) {
        case Kind::DNDWidget: return dndBuiltIn(w, e);
        case Kind::PushButton: case Kind::ToggleButton: case Kind::CheckBox:
            if (e.type == 1) return buttonMouse(w, e);
            if (e.type == 2 && e.dest == w && (e.message == 5 || e.message == 6)) return buttonHover(w, e.message == 5);
            if (e.type == 2 && e.source == w && e.message == 8 && w->enabled) {   // capture lost (FUN_008e4730)
                releaseCapture(w);
                return 1;
            }
            return kNotHandled;
        case Kind::EditBox: case Kind::PasswordBox: case Kind::MultiLineEditBox:
            // MouseEnter / MouseExit on an enabled box (FUN_008d12b0 / FUN_008d1340): the "TextEntry" cursor pushed / removed.
            if (e.type == 2 && e.dest == w && (e.message == 5 || e.message == 6) && w->enabled) {
                if (e.message == 5) cursor::push("TextEntry");
                else cursor::pop("TextEntry");
                return kHandledStop;
            }
            if (e.type == 0) return editKey(w, e);
            // Keyboard focus received / lost (FUN_008d13e0 / FUN_008d1480). With allowIME the box opens the IME (FUN_008a1600: when the
            // context is not open, ImmSetOpenStatus(1) and an IME event with code 0, whose handler 0x008d0d40 sets the box's + 0xED) and
            // on losing the focus closes it (FUN_008a16a0: when open, ImmSetOpenStatus(0) and code 1 -> 0x008d0d50 clears + 0xED). Both
            // return eHandledStop for an enabled box. UNVERIFIED: whether ImmGetOpenStatus reports open after ImmSetOpenStatus(1) on a
            // keyboard layout without an IME (then the close event would not come; the flag only matters while the box has the focus).
            if (e.type == 2 && e.source == w && (e.message == 9 || e.message == 10)) {
                Manager& m = manager();
                if (w->allowIme && e.message == 9) {
                    m.imeBox = w;                                   // FUN_008d0400: its IME handlers on the manager
                    if (!m.imeOpenStatus) {
                        m.imeOpenStatus = true;
                        imeSend(0);                                 // eIMEActivated -> 0x008d0d40 sets + 0xED
                    }
                } else if (w->allowIme && e.message == 10) {
                    if (m.imeOpenStatus) {
                        m.imeOpenStatus = false;
                        imeSend(1);                                 // eIMEDeactivated -> 0x008d0d50 clears + 0xED
                    }
                    imeReset();                                     // FUN_008a16a0 -> FUN_008a1bb0
                    if (m.imeBox == w) m.imeBox = nullptr;
                }
                return w->enabled ? kHandledStop : kNotHandled;
            }
            if (e.type == 1) return editMouse(w, e);
            return kNotHandled;
        case Kind::RichText:
            // Mouse down (slot 12, FUN_008e54f0): left button over a run whose first character is in a link -> eMessageHTMLLink (23),
            // phase 1, Data {linkData, LocX, LocY}, from the widget; eHandledStop. The run test (slot 1 of the run, FUN_008e5d00): the
            // point within the run's width and its font's height from the run's top.
            if (e.type == 1 && e.code == 1 && e.code3 == 0) return richTextPress(w, e);
            return kNotHandled;
        case Kind::Slider:   // slot 13 (FUN_008db220): the wheel steps the value by the event's count
            if (e.type == 1 && (e.code == 0x20 || e.code == 0x40)) {
                sliderValue(w, w->value + e.scroll);
                return kHandledStop;
            }
            return kNotHandled;
        case Kind::ScrollBar:   // slot 13 (FUN_008d7ca0): the wheel steps the value by the event's count
            if (e.type == 1 && (e.code == 0x20 || e.code == 0x40)) {
                scrollValue(w, w->value + e.scroll);
                return kHandledStop;
            }
            return kNotHandled;
        default:
            return kNotHandled;
    }
}

// ---- Input mapper ---------------------------------------------------------------------------------------------------------------
struct KeyMapping {
    std::string key;            // a key name with optional "ctrl-" / "shift-" / "alt-" prefixes
    std::string state;          // the input state's module name, lower case
    std::string command;        // the router's method
    bool keyMapping = true;     // AddKeyMapping (else AddMapping: "keyboard.<key>" / "mouse.<button>")
};
std::vector<KeyMapping> mappings;
std::string currentState;
PyObject* router = nullptr;

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}



int mapKey(Event& e) {
    if (!router) return kNotHandled;
    if (e.type == 0 && e.code2 == 2) return kNotHandled;
    if (e.type != 0 && e.type != 1) return kNotHandled;
    const std::string name = e.type == 0 ? "keyboard." + keyString(e.code, e.modifiers) : mouseString(e.code, e.modifiers);
    if (std::getenv("OPENRASA_KEYLOG")) std::printf("[key] %s phase %d (state %s)\n", name.c_str(), e.phase, currentState.c_str());
    for (const KeyMapping& m : mappings) {
        if (m.state != currentState) continue;
        const std::string key = m.keyMapping ? (e.type == 0 ? "keyboard." + m.key : std::string()) : m.key;
        if (key != name) continue;
        if (std::getenv("OPENRASA_KEYLOG")) std::printf("[key]   -> %s\n", m.command.c_str());
        PyObject* dict = eventDict(e);
        PyObject* r = PyObject_CallMethod(router, const_cast<char*>(m.command.c_str()), const_cast<char*>("(O)"), dict);
        Py_DECREF(dict);
        if (!r) PyErr_Print();
        Py_XDECREF(r);
        manager().dirty = true;
        return kHandledStop;
    }
    return kNotHandled;
}

PyObject* none() {
    Py_INCREF(Py_None);
    return Py_None;
}
PyObject* imAddKeyMapping(PyObject*, PyObject* args) {
    char *key, *state, *command;
    if (!PyArg_ParseTuple(args, const_cast<char*>("sss:AddKeyMapping"), &key, &state, &command)) return nullptr;
    mappings.push_back({lower(key), lower(state), command, true});
    return none();
}
PyObject* imAddMapping(PyObject*, PyObject* args) {
    char *key, *state, *command;
    if (!PyArg_ParseTuple(args, const_cast<char*>("sss:AddMapping"), &key, &state, &command)) return nullptr;
    mappings.push_back({lower(key), lower(state), command, false});
    return none();
}
PyObject* imRemove(PyObject*, PyObject* args) {
    char *key, *state;
    if (!PyArg_ParseTuple(args, const_cast<char*>("ss"), &key, &state)) return nullptr;
    const std::string k = lower(key), st = lower(state);
    mappings.erase(std::remove_if(mappings.begin(), mappings.end(), [&](const KeyMapping& m) { return m.key == k && m.state == st; }),
                   mappings.end());
    return none();
}
PyObject* imClear(PyObject*, PyObject*) {
    mappings.clear();
    return none();
}
PyObject* imSwitchInputState(PyObject*, PyObject* args) {
    char* state;
    if (!PyArg_ParseTuple(args, const_cast<char*>("s:SwitchInputState"), &state)) return nullptr;
    currentState = lower(state);
    return none();
}

}  // namespace

// ---- Extended keys (OpenRasa: any key SDL reports can be bound) -------------------------------------------------------------------
// Keys the exe's tables do not name get codes above DirectInput's 0..0xFF and a stable name the key-binding window can store
// ("keyboard.<name>": no "." or "-", which gameui.GetKeyText splits on): F16 - F24 the exe's own "f16" .. "f24"; any other SDL scancode
// kExtendedSdl + scancode, named by SDL_GetScancodeName lower-cased, letters and digits only ("VolumeUp" -> "volumeup"), or "key<n>" when
// that is empty or already a name; a key SDL cannot identify (SDL_SCANCODE_UNKNOWN) kExtendedRaw + its platform scancode, "rawkey<n>".
constexpr int kExtendedSdl = 0x1000, kExtendedRaw = 0x2000;
struct ExtendedKeys {
    std::map<int, std::string> names;   // code -> name
    std::map<std::string, int> codes;   // name -> code
};
const ExtendedKeys& extendedKeys();

// ---- Public -----------------------------------------------------------------------------------------------------------------------
void playSound(Widget* w, int audioSet) {
    if (audioSet > 0 && w->visible) audio::playAudioSet(audioSet);
}

const ExtendedKeys& extendedKeys() {
    static const ExtendedKeys keys = [] {
        ExtendedKeys k;
        std::map<std::string, int> taken;
        for (const auto& [c, n] : keyNames()) taken[n] = c;
        for (int s = 1; s < SDL_SCANCODE_COUNT; ++s) {
            if (scanCodeForSdl(s)) continue;
            std::string name;
            if (s >= SDL_SCANCODE_F16 && s <= SDL_SCANCODE_F24) name = "f" + std::to_string(16 + (s - SDL_SCANCODE_F16));
            else
                for (const char* c = SDL_GetScancodeName(SDL_Scancode(s)); c && *c; ++c)
                    if (std::isalnum(static_cast<unsigned char>(*c))) name += char(std::tolower(static_cast<unsigned char>(*c)));
            if (name.empty() || taken.count(name) || k.codes.count(name)) name = "key" + std::to_string(s);
            k.names[kExtendedSdl + s] = name;
            k.codes[name] = kExtendedSdl + s;
        }
        return k;
    }();
    return keys;
}

int keyCodeForName(const std::string& name) {
    for (const auto& [code, n] : keyNames())
        if (n == name) return code;
    const ExtendedKeys& x = extendedKeys();
    if (auto it = x.codes.find(name); it != x.codes.end()) return it->second;
    if (name.rfind("rawkey", 0) == 0 && name.size() > 6 && name.size() < 12 &&
        std::all_of(name.begin() + 6, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return kExtendedRaw + std::atoi(name.c_str() + 6);
    return 0;
}
const char* keyName(int scanCode) {
    auto it = keyNames().find(scanCode);
    if (it != keyNames().end()) return it->second.c_str();
    const ExtendedKeys& x = extendedKeys();
    if (auto e = x.names.find(scanCode); e != x.names.end()) return e->second.c_str();
    if (scanCode >= kExtendedRaw) {
        static std::map<int, std::string> raw;
        auto r = raw.find(scanCode);
        if (r == raw.end()) r = raw.emplace(scanCode, "rawkey" + std::to_string(scanCode - kExtendedRaw)).first;
        return r->second.c_str();
    }
    return "";
}
int keyCodeForSdl(int sdlScancode, int raw) {
    if (int code = scanCodeForSdl(sdlScancode)) return code;
    if (sdlScancode > 0 && sdlScancode < SDL_SCANCODE_COUNT) return kExtendedSdl + sdlScancode;
    return raw > 0 ? kExtendedRaw + (raw & 0xFFFF) : 0;
}
const char* sdlKeyText(int code) {
    if (code > kExtendedSdl && code < kExtendedSdl + SDL_SCANCODE_COUNT) return SDL_GetScancodeName(SDL_Scancode(code - kExtendedSdl));
    return nullptr;
}

int scanCodeForSdl(int s) {
    switch (s) {
        case SDL_SCANCODE_ESCAPE: return 0x01;
        case SDL_SCANCODE_MINUS: return 0x0C;
        case SDL_SCANCODE_EQUALS: return 0x0D;
        case SDL_SCANCODE_BACKSPACE: return 0x0E;
        case SDL_SCANCODE_TAB: return 0x0F;
        case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
        case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
        case SDL_SCANCODE_RETURN: return 0x1C;
        case SDL_SCANCODE_LCTRL: return 0x1D;
        case SDL_SCANCODE_SEMICOLON: return 0x27;
        case SDL_SCANCODE_APOSTROPHE: return 0x28;
        case SDL_SCANCODE_GRAVE: return 0x29;
        case SDL_SCANCODE_LSHIFT: return 0x2A;
        case SDL_SCANCODE_BACKSLASH: return 0x2B;
        case SDL_SCANCODE_COMMA: return 0x33;
        case SDL_SCANCODE_PERIOD: return 0x34;
        case SDL_SCANCODE_SLASH: return 0x35;
        case SDL_SCANCODE_RSHIFT: return 0x36;
        case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
        case SDL_SCANCODE_LALT: return 0x38;
        case SDL_SCANCODE_SPACE: return 0x39;
        case SDL_SCANCODE_CAPSLOCK: return 0x3A;
        case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
        case SDL_SCANCODE_SCROLLLOCK: return 0x46;
        case SDL_SCANCODE_KP_7: return 0x47;
        case SDL_SCANCODE_KP_8: return 0x48;
        case SDL_SCANCODE_KP_9: return 0x49;
        case SDL_SCANCODE_KP_MINUS: return 0x4A;
        case SDL_SCANCODE_KP_4: return 0x4B;
        case SDL_SCANCODE_KP_5: return 0x4C;
        case SDL_SCANCODE_KP_6: return 0x4D;
        case SDL_SCANCODE_KP_PLUS: return 0x4E;
        case SDL_SCANCODE_KP_1: return 0x4F;
        case SDL_SCANCODE_KP_2: return 0x50;
        case SDL_SCANCODE_KP_3: return 0x51;
        case SDL_SCANCODE_KP_0: return 0x52;
        case SDL_SCANCODE_KP_PERIOD: return 0x53;
        case SDL_SCANCODE_NONUSBACKSLASH: return 0x56;
        case SDL_SCANCODE_F11: return 0x57;
        case SDL_SCANCODE_F12: return 0x58;
        case SDL_SCANCODE_KP_ENTER: return 0x9C;
        case SDL_SCANCODE_RCTRL: return 0x9D;
        case SDL_SCANCODE_KP_DIVIDE: return 0xB5;
        case SDL_SCANCODE_PRINTSCREEN: return 0xB7;
        case SDL_SCANCODE_RALT: return 0xB8;
        case SDL_SCANCODE_PAUSE: return 0xC5;
        case SDL_SCANCODE_HOME: return 0xC7;
        case SDL_SCANCODE_UP: return 0xC8;
        case SDL_SCANCODE_PAGEUP: return 0xC9;
        case SDL_SCANCODE_LEFT: return 0xCB;
        case SDL_SCANCODE_RIGHT: return 0xCD;
        case SDL_SCANCODE_END: return 0xCF;
        case SDL_SCANCODE_DOWN: return 0xD0;
        case SDL_SCANCODE_PAGEDOWN: return 0xD1;
        case SDL_SCANCODE_INSERT: return 0xD2;
        case SDL_SCANCODE_DELETE: return 0xD3;
        case SDL_SCANCODE_LGUI: return 0xDB;
        case SDL_SCANCODE_RGUI: return 0xDC;
        case SDL_SCANCODE_APPLICATION: return 0xDD;
        case SDL_SCANCODE_F13: return 0x64;
        case SDL_SCANCODE_F14: return 0x65;
        case SDL_SCANCODE_F15: return 0x66;
        case SDL_SCANCODE_INTERNATIONAL2: return 0x70;   // Katakana / Hiragana = DIK_KANA
        case SDL_SCANCODE_INTERNATIONAL4: return 0x79;   // Henkan = DIK_CONVERT
        case SDL_SCANCODE_KP_EQUALS: return 0x8D;
        case SDL_SCANCODE_SLEEP: return 0xDF;
        default: break;
    }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_0) return 0x02 + (s - SDL_SCANCODE_1);
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) return 0x3B + (s - SDL_SCANCODE_F1);
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) {
        const char letter = char('a' + (s - SDL_SCANCODE_A));
        return keyCodeForName(std::string(1, letter));
    }
    return 0;
}

void setEnabled(Widget* w, bool enabled) {
    if (w->enabled == enabled) return;
    if (!enabled) {
        releaseCapture(w);
        removeFocus(w);
    }
    w->enabled = enabled;
    sendMessage(w, 4, 1);
    if (isComposite(w->kind)) {
        std::vector<Widget*> children;
        for (const auto& c : w->children) children.push_back(c.get());
        for (Widget* c : children) setEnabled(c, enabled);
    }
    if (w->kind == Kind::ComboBox)
        if (Widget* drop = comboPart(w, "DropDown")) {
            releaseCapture(drop);
            setVisible(drop, false);
        }
    manager().dirty = true;
}
void layoutScrollThumb(Widget* bar) { thumbLayout(bar); }
// FUN_008f1320: the list's visibility kept across the rebuild; an index past the new strings selects 0.
void comboSetStringList(Widget* combo, std::vector<std::u16string> strings) {
    Widget* drop = comboPart(combo, "DropDown");
    const bool wasShown = drop && drop->visible;
    combo->stringList = std::move(strings);
    comboRebuild(combo);
    if (combo->stringList.size() <= size_t(std::max(combo->value, 0))) comboSelect(combo, 0);
    comboShow(combo, wasShown);
}
void comboSetValue(Widget* combo, int index) { comboSelect(combo, index); }
void setSliderValue(Widget* slider, int value) { sliderValue(slider, value); }
void setScrollValue(Widget* bar, int value) { scrollValue(bar, value); }
void scrollBarsFrame(int msElapsed) { repeatsFrame(msElapsed); }

int handleEvent(Widget* w, Event& e) {
    int result = kNotHandled;
    const bool dragDrop = e.type == 2 && e.message > 14 && e.message < 20;
    if (e.source == w || !e.source || dragDrop) {
        result = std::max(result, partCommands(w, e));
        if (result != kHandledStop) result = std::max(result, pythonHandlers(w, e));
    }
    if (result == kHandledStop) return result;
    result = std::max(result, builtIn(w, e));
    if (result != kHandledStop && layoutParentOf(w)) result = std::max(result, handleEvent(layoutParentOf(w), e));
    return result;
}

// The focus manager's hover messages (eMessageMouseEnter 5 / eMessageMouseExit 6: FUN_008aa920, FUN_008ab340) leave the event's source
// (+ 0xC) unset and name the widget at + 0x10, so the Python handlers of every widget they bubble through run - the radial menu's
// Context composite gets its MouseEnter while the hovered widget is its Bkgd or the headshot view.
static int hoverMessage(Widget* w, int message) {
    Event e;
    e.type = 2;
    e.dest = w;
    e.message = message;
    return handleEvent(w, e);
}

int sendMessage(Widget* w, int message, int phase, std::vector<std::pair<std::string, std::u16string>> data) {
    Event e;
    e.type = 2;
    e.phase = phase;
    e.source = w;
    e.message = message;
    e.data = std::move(data);
    return handleEvent(w, e);
}

void pushFocus(Widget* w) {
    Manager& m = manager();
    Widget* old = m.focus();
    if (old == w) return;
    m.focusStack.erase(std::remove(m.focusStack.begin(), m.focusStack.end(), w), m.focusStack.end());
    m.focusStack.push_back(w);
    if (old) sendMessage(old, 10);
    sendMessage(w, 9);
    m.dirty = true;
}
void removeFocus(Widget* w) {
    Manager& m = manager();
    const bool wasTop = m.focus() == w;
    m.focusStack.erase(std::remove(m.focusStack.begin(), m.focusStack.end(), w), m.focusStack.end());
    if (wasTop) {
        sendMessage(w, 10);
        if (Widget* top = m.focus()) sendMessage(top, 9);
    }
    m.dirty = true;
}
void pushCapture(Widget* w) {
    Manager& m = manager();
    Widget* old = m.capture();
    if (old == w) return;
    m.captureStack.erase(std::remove(m.captureStack.begin(), m.captureStack.end(), w), m.captureStack.end());
    m.captureStack.push_back(w);
    if (old) sendMessage(old, 8);
    sendMessage(w, 7);
}
void releaseCapture(Widget* w) {
    Manager& m = manager();
    const bool wasTop = m.capture() == w;
    m.captureStack.erase(std::remove(m.captureStack.begin(), m.captureStack.end(), w), m.captureStack.end());
    if (wasTop) {
        sendMessage(w, 8);
        if (Widget* top = m.capture()) sendMessage(top, 7);
    }
}

// The hit test (EventHandler slot 1; Widget FUN_0089de20, WidgetComposite FUN_008af240): enabled, visible and pickable; a composite
// that clips (the default) must contain the point; children last to first; a composite itself only with mouseCapture and a non-zero
// alpha. Leaves although they hold parts: EditBox / PasswordBox (Widget's test), CheckBox (FUN_008d5260), PushButton (FUN_008e42a0)
// and ToggleButton (FUN_008e8b30) - the buttons test the point against their Bitmap part (+ 0xC4 / + 0xC0).
static Widget* buttonFace(Widget* w) {
    for (const auto& c : w->children)
        if (c->kind == Kind::Bitmap) return c.get();
    return w;
}
// The point test (slot 103): a Bitmap's (FUN_008f5100) also needs a texel with alpha, unless opaqueClick; others the rectangle.
static bool pointIn(Widget* w, int x, int y) {
    return w->kind == Kind::Bitmap ? bitmapPointIn(w, x, y) : contains(w, x, y);
}
static Widget* hit(Widget* w, int x, int y) {
    if (w->kind == Kind::DNDWidget) return nullptr;   // its hit test (slot 1, FUN_008cc3e0) finds nothing
    if (!w->enabled || !w->visible || !w->pickable) return nullptr;
    const bool inside = contains(w, x, y);
    switch (w->kind) {
        case Kind::EditBox: case Kind::PasswordBox: case Kind::MultiLineEditBox: case Kind::CheckBox:
            return inside ? w : nullptr;
        case Kind::PushButton: case Kind::ToggleButton:
            return pointIn(buttonFace(w), x, y) ? w : nullptr;
        default:
            break;
    }
    if (!isComposite(w->kind)) return pointIn(w, x, y) ? w : nullptr;
    if (w->clipChildren && !inside) return nullptr;
    for (auto it = w->children.rbegin(); it != w->children.rend(); ++it)
        if (Widget* h = hit(it->get(), x, y)) return h;
    if (w->mouseCapture && inside && w->a != 0) return w;
    return nullptr;
}
Widget* hitTest(int x, int y) {
    Widget* root = manager().root();
    if (!root) return nullptr;
    for (auto it = root->children.rbegin(); it != root->children.rend(); ++it)
        if (Widget* h = hit(it->get(), x, y)) return h;
    return nullptr;
}

// The focus manager's mouse dispatch (FUN_008aa920): the event goes to the mouse capture first (when there is one); the widget under the
// cursor is ClickedWidget; on a move not stopped by the capture the hovered widget changes (MouseExit to the old, MouseEnter to the
// new); then - unless the capture stopped it - the widget under the cursor gets the event too, when it is not the capture. (Not here:
// the modal stack's check and the 3D world's input, FUN_008a66d0.)
// FUN_008b4f50: whether w's parents (from its parent up) reach top.
bool insideOf(const Widget* w, const Widget* top) {
    for (const Widget* p = w ? w->parent : nullptr; p; p = p->parent)
        if (p == top) return true;
    return false;
}

// SetModal(True) (FUN_008ab340), unless the widget already is modal: it takes the mouse capture when the capture is not inside it
// (FUN_008aac50), the keyboard focus likewise (FUN_008aafc0), the hovered widget gets eMessageMouseExit when it is another one, and it
// goes to the front of the modal stack (FUN_008ab730). SetModal(False) (FUN_008ab470): a modal widget loses the focus (FUN_008ab100)
// and the capture (FUN_008aadc0) and leaves the stack. (The stack's new top is then sent a virtual call - slot 2, empty on widgets.)
void setModal(Widget* w, bool modal) {
    Manager& m = manager();
    auto it = std::find(m.modalStack.begin(), m.modalStack.end(), w);
    if (modal) {
        if (it != m.modalStack.end()) return;
        if (!m.capture() || !insideOf(m.capture(), w)) pushCapture(w);
        if (!m.focus() || !insideOf(m.focus(), w)) pushFocus(w);
        if (m.hovered && m.hovered != w) hoverMessage(m.hovered, 6);
        m.modalStack.insert(m.modalStack.begin(), w);
    } else {
        if (it == m.modalStack.end()) return;
        removeFocus(w);
        releaseCapture(w);
        m.modalStack.erase(std::find(m.modalStack.begin(), m.modalStack.end(), w));
    }
    m.dirty = true;
}

// UIManager.CreateDNDWidget (FUN_008968b0): the template's widgets (unknown template: none) become the visual of a new DNDWidget
// (FUN_008cbe10: anchor offset = the given point, source = the widget given, state 4 dragging; FUN_008cbfc0: the visual added as its
// child, shown, the DND widget sized to it), armed unless fromDrag (+ 0xFD); then (FUN_008967a0) a child of the DND window (SetDNDWindow;
// else the root), on the DND list, and the mouse capture's top (FUN_008aac50).
Widget* createDndWidget(Widget* source, const std::string& templateName, int x, int y, bool fromDrag) {
    Manager& m = manager();
    Widget* parent = m.dndWindow ? m.dndWindow : m.root();
    if (!parent) return nullptr;
    auto d = std::make_unique<Widget>();
    d->kind = Kind::DNDWidget;
    d->tag = "DNDWidget";
    d->enabled = true;
    d->parent = parent;
    std::unique_ptr<Widget> visual = m.buildTemplate(templateName, d.get());
    if (!visual) return nullptr;
    d->dndVisual = visual.get();
    setVisible(visual.get(), true);
    d->width = visual->width;
    d->height = visual->height;
    d->children.push_back(std::move(visual));
    d->offsetX = x;
    d->offsetY = y;
    d->dndSource = source;
    d->dndArmed = !fromDrag;
    Widget* raw = d.get();
    parent->children.push_back(std::move(d));
    m.dndWidgets.push_back(raw);
    m.dirty = true;
    pushCapture(raw);
    return raw;
}

void finishDnd(Widget* d) {
    Manager& m = manager();
    if (d->dndSource) dndMessage(d, d->dndSource, 19);
    if (d->dndState == 4) audio::playAudioSet(10000026);
    // FUN_00896820: when still on the DND list - off the window (RemoveChild) and the list, the capture let go (FUN_008aadc0).
    auto it = std::find(m.dndWidgets.begin(), m.dndWidgets.end(), d);
    if (it == m.dndWidgets.end()) return;
    m.dndWidgets.erase(it);
    releaseCapture(d);
    m.detachLater(d);
}

// UIManager.DestroyDNDWidget (0x00896E10 -> FUN_00896960): every live DND widget rejected (state 3) and ended.
void destroyDndWidgets() {
    Manager& m = manager();
    while (!m.dndWidgets.empty()) {
        Widget* d = m.dndWidgets.front();
        d->dndState = 3;
        finishDnd(d);
    }
}

// SetVisualOffset (FUN_008cc030): the offset kept, and added to the current anchor offset at once.
void setDndVisualOffset(Widget* d, int x, int y) {
    d->dndOffsetX = x;
    d->dndOffsetY = y;
    d->offsetX += x;
    d->offsetY += y;
    manager().dirty = true;
}

void dispatchMouse(Event& e) {
    Manager& m = manager();
    int result = kNotHandled;
    Widget* capture = m.capture();
    if (capture) result = handleEvent(capture, e);
    Widget* under = hitTest(e.locX, e.locY);
    e.clicked = under;
    const bool move = e.code3 == 4;
    if (move && result != kHandledStop && under != m.hovered && m.hovered) {
        Widget* old = m.hovered;
        m.hovered = nullptr;
        hoverMessage(old, 6);
    }
    // A modal window (the modal stack's top) takes the event from widgets outside it (FUN_008b4f50 on the widget under the cursor:
    // its parents, not itself, must reach the top).
    if (!m.modalStack.empty() && under && !insideOf(under, m.modalStack.front())) return;
    if (move) {
        if (result == kHandledStop) return;
        if (under != m.hovered && under) {
            hoverMessage(under, 5);
            m.hovered = under;
        }
    }
    if (result != kHandledStop && under && under != capture) result = std::max(result, handleEvent(under, e));
    // Then the input mapper (FUN_008aa920 -> FUN_008a66d0), by the event code: a press only when neither the capture nor the widget
    // handled it; a release always; a wheel step (a release of 0x20 / 0x40) only when unhandled; moves, clicks and drags never.
    if (e.code3 == 0) {
        if (result == kNotHandled) mapKey(e);
    } else if (e.code3 == 1) {
        const bool wheel = e.code == 0x20 || e.code == 0x40;
        if (!wheel || result == kNotHandled) mapKey(e);
    }
}

// The click filter (FUN_00903800, an input filter after the window-message source FUN_00902870, which stamps each event with
// GetTickCount): from the mouse events it adds eMouseClicked, eMouseDoubleClicked and eMouseDrag events behind the originals.
//   down: remember its time, button and position (and its time as the drag start); a remembered click older than 200 ms is forgotten.
//   up within 200 ms of the down (unsigned difference < 201): with no remembered click -> eMouseClicked (the up's button, position and
//     modifiers), remembered as the last click, the drag start cleared; with one -> eMouseDoubleClicked, then everything is forgotten.
//     A later up forgets everything.
//   move with a drag start set: more than 3 units from the down's position (either axis) -> eMouseDrag with the down's button and
//     position; down, click and drag start forgotten.
// Positions here are logical UI units (the exe's are window pixels).
namespace {
struct ClickFilter {
    int64_t downTime = -1, lastClick = -1, dragStart = -1;
    int button = 0, downX = 0, downY = 0;
} clickFilter;

void synthesize(int code3, int button, int x, int y, int modifiers) {
    Event e;
    e.type = 1;
    e.phase = 1;
    e.code = button;
    e.code3 = code3;
    e.locX = x;
    e.locY = y;
    e.modifiers = modifiers;
    dispatchMouse(e);
}
}  // namespace

void scriptSetToggled(Widget* w, bool v) { setToggled(w, v); }
void scriptSetChecked(Widget* w, bool v) {
    if (checkBoxButton(w)) setChecked(w, v);
    else w->checked = v;
}
void scriptSetHighlighted(Widget* w, bool v) { setHighlighted(w, v); }

void inputMouseMove(int x, int y, int modifiers) {
    Manager& m = manager();
    const int dx = x - m.mouseX, dy = y - m.mouseY;
    m.mouseX = x;
    m.mouseY = y;
    m.dirty = true;
    Event e;
    e.type = 1;
    e.code3 = 4;   // eMouseMove
    // The cursor's position: the window-message path (FUN_00902870: WM_MOUSEMOVE, the buttons and the wheel carry the client position)
    // feeds the UI; the DirectInput path (FUN_00902640) carries movements, for mouse look. Logical units here - the conversion the UI
    // manager applies is NOT traced; the scripts (attributeswindow.py, inputhandlers.py) and the scroll bar's drag use it as a position.
    e.locX = x;
    e.locY = y;
    (void)dx;
    (void)dy;
    e.modifiers = modifiers;
    dispatchMouse(e);
    ClickFilter& f = clickFilter;
    if (f.dragStart != -1 && (std::abs(x - f.downX) > 3 || std::abs(y - f.downY) > 3)) {
        f.downTime = f.lastClick = f.dragStart = -1;
        synthesize(5, f.button, f.downX, f.downY, modifiers);
    }
}

void inputMouseButton(int button, bool down, int x, int y, int modifiers) {
    Manager& m = manager();
    m.mouseX = x;
    m.mouseY = y;
    m.dirty = true;
    Event e;
    e.type = 1;
    e.phase = down ? 1 : 2;   // FUN_00902640: down On, up Off
    e.code = button;
    e.code3 = down ? 0 : 1;
    e.locX = x;
    e.locY = y;
    e.modifiers = modifiers;
    dispatchMouse(e);
    ClickFilter& f = clickFilter;
    const int64_t now = int64_t(SDL_GetTicks());
    if (down) {
        f.downTime = f.dragStart = now;
        f.button = button;
        f.downX = x;
        f.downY = y;
        if (f.lastClick != -1 && f.downTime - f.lastClick > 200) f.lastClick = -1;
        return;
    }
    if (f.downTime != -1 && now - f.downTime < 201) {
        if (f.lastClick == -1) {
            f.lastClick = now;
            f.dragStart = -1;
            synthesize(2, button, x, y, modifiers);
            return;
        }
        synthesize(3, button, x, y, modifiers);
    }
    f.downTime = f.lastClick = f.dragStart = -1;
}

void inputMouseWheel(int notches, int x, int y, int modifiers) {
    Manager& m = manager();
    if (notches == 0) return;
    m.mouseX = x;
    m.mouseY = y;
    m.dirty = true;
    Event e;
    e.type = 1;
    e.phase = 1;
    e.scroll = -notches;
    e.code = e.scroll >= 0 ? 0x40 : 0x20;
    e.code3 = 1;
    e.locX = x;
    e.locY = y;
    e.modifiers = modifiers;
    dispatchMouse(e);
}

// OpenRasa: a tilting wheel's notch, sent as the wheel's is (phase 1, eMouseUp) with the codes kWheelLeft / kWheelRight - no widget
// acts on them; the input mapper and the key-binding window do.
void inputMouseTilt(int notches, int x, int y, int modifiers) {
    Manager& m = manager();
    if (notches == 0) return;
    m.mouseX = x;
    m.mouseY = y;
    m.dirty = true;
    Event e;
    e.type = 1;
    e.phase = 1;
    e.code = notches > 0 ? kWheelRight : kWheelLeft;
    e.code3 = 1;
    e.locX = x;
    e.locY = y;
    e.modifiers = modifiers;
    for (int i = 0; i < std::abs(notches); ++i) dispatchMouse(e);
}

// The event's modifiers as the exe's keyboard device gives them (the Win32 device the InputSystem constructor selects - keyboard type
// + 0x14 = 0; message handler FUN_00902f00): Shift / Ctrl / Alt send their own events with NO modifiers (they only update the device's
// state); any other key's down takes the modifiers held then and keeps them per key (+ 0x20 + 4 x key), and its up repeats those and
// clears them. So holding left Ctrl reads "keyboard.leftcontrol" (the radial menu's default binding), not "keyboard.ctrl-leftcontrol".
// Print Screen: Windows posts no WM_KEYDOWN for VK_SNAPSHOT, only WM_KEYUP, so the exe never makes a down event for it and its up finds
// no stored modifiers - "keyboard.printscreen" whatever is held (the screenshot binding fires on the up, inputhandlers.TakeScreenshot).
void setTestNumLock(int state) { testNumLock = state; }

void inputKey(int scanCode, bool down, int modifiers) {
    static std::map<int, int> downModifiers;
    if (scanCode == 0xB7 && down) return;
    const bool modifierKey = scanCode == 0x1D || scanCode == 0x9D || scanCode == 0x2A || scanCode == 0x36 || scanCode == 0x38 ||
                             scanCode == 0xB8;
    if (modifierKey) {
        modifiers = 0;
    } else if (down) {
        downModifiers[scanCode] = modifiers;
    } else {
        const auto it = downModifiers.find(scanCode);
        modifiers = it != downModifiers.end() ? it->second : 0;
        if (it != downModifiers.end()) downModifiers.erase(it);
    }
    Event e;
    e.type = 0;
    e.phase = down ? 1 : 2;
    e.code = scanCode;
    e.code2 = down ? 0 : 1;
    e.modifiers = modifiers;
    int result = kNotHandled;
    if (Widget* f = manager().focus()) result = handleEvent(f, e);
    // EventRouter::ProcessKeyboardEvent (FUN_008aa8a0): the input mapper only while NO modal widget is up (the router's modal list + 0xc
    // empty) - the vendor's quantity chooser is modal (vendorwindow.py SetModal(True)), so its digits do not arm abilities.
    if (result != kHandledStop && manager().modalStack.empty()) mapKey(e);
    manager().dirty = true;
}

// A held key's repeated WM_KEYDOWNs make no event in the exe's Win32 keyboard device (FUN_00902f00: lParam bit 30 -> no event; traced
// 2026-11-05), yet the user saw Backspace / Delete / the arrows repeat while held in a text box in retail 1.16.5 (tested by hand). The
// mechanism is NOT found (the router FUN_008aa7f0 / FUN_008aa8a0 and the edit box key handler FUN_008d0190 hold no repeat; WM_CHAR repeats
// only reach FUN_008d0070's character path). OpenRasa gives such repeats (the OS's own delay and rate, SDL's repeat flag) to a FOCUSED
// EDIT BOX only - not the input mapper, so held bindings still fire once. UNVERIFIED against retail's timing.
void inputKeyRepeat(int scanCode, int modifiers) {
    // Only the editing and caret keys repeat (Backspace, Delete, Home / End, the arrows and the numpad's 4 / 6 / 8 / 2 / 7 / 1): a held Enter,
    // Tab or Escape must not re-submit or re-fire.
    switch (scanCode) {
        case 0x0E: case 0xD3: case 0xC7: case 0xCF: case 0xC8: case 0xD0: case 0xCB: case 0xCD:
        case 0x4B: case 0x4D: case 0x48: case 0x50: case 0x47: case 0x4F: break;
        default: return;
    }
    Widget* f = manager().focus();
    if (!f || (f->kind != Kind::EditBox && f->kind != Kind::PasswordBox && f->kind != Kind::MultiLineEditBox)) return;
    Event e;
    e.type = 0;
    e.phase = 1;
    e.code = scanCode;
    e.code2 = 0;
    e.modifiers = modifiers;
    handleEvent(f, e);
    manager().dirty = true;
}

void imeEditing(const std::u16string& text, int cursor) {
    Manager& m = manager();
    if (!m.imeBox || !m.imeOpenStatus) return;     // the exe's context is open only while an allowIME box has the focus
    if (text.empty()) {
        if (!m.imeComposing) return;
        // Cancelled (no result): WM_IME_COMPOSITION with lParam 0 - cleared, 10 - then WM_IME_ENDCOMPOSITION: 11 with the (empty)
        // result, which replaces the composition shown in the box, and 9.
        imeReset();
        imeSend(10);
        m.imeResult.clear();
        imeSend(11);
        imeSend(9);
        return;
    }
    if (!m.imeComposing) {                         // WM_IME_STARTCOMPOSITION
        m.imeComposing = true;
        m.imeResult.clear();
        imeSend(6);
    }
    imeSend(5);                                    // WM_IME_COMPOSITION: typing, then the change
    m.imeComposition = text;
    m.imeCursor = cursor;
    imeSend(7);
}
bool imeCommit(const std::u16string& text) {
    Manager& m = manager();
    if (!m.imeBox || !m.imeComposing) return false;
    imeSend(5);                                    // WM_IME_COMPOSITION with GCS_RESULTSTR
    m.imeResult = text;
    imeSend(11);
    imeReset();                                    // WM_IME_ENDCOMPOSITION
    imeSend(9);
    return true;
}
void imeCandidates(const std::vector<std::u16string>& candidates, int selected) {
    Manager& m = manager();
    if (candidates.empty()) {
        if (!m.imeCandidatesOpen) return;
        imeSend(14);                               // IMN_CLOSECANDIDATE: the event, then the list back to its defaults
        m.imeCandidatesOpen = false;
        m.imeCandidates.clear();
        m.imeCandidateSelection = 0;
        return;
    }
    const bool opening = !m.imeCandidatesOpen;
    m.imeCandidatesOpen = true;
    m.imeCandidates = candidates;
    m.imeCandidateSelection = std::max(selected, 0);
    imeSend(opening ? 12 : 13);                    // IMN_OPENCANDIDATE / IMN_CHANGECANDIDATE
}
void imeLanguageChanged() { imeSend(2); }

void inputChar(char16_t c, int modifiers) {
    Event e;
    e.type = 0;
    e.phase = 1;
    e.code2 = 2;   // eKeyChar
    e.character = c;
    e.modifiers = modifiers;
    if (Widget* f = manager().focus()) handleEvent(f, e);
    manager().dirty = true;
}

PyObject* inputMapperObject() {
    static PyObject* mapper = nullptr;
    if (!mapper) {
        static PyMethodDef defs[] = {
            {const_cast<char*>("AddKeyMapping"), imAddKeyMapping, METH_VARARGS, nullptr},
            {const_cast<char*>("AddMapping"), imAddMapping, METH_VARARGS, nullptr},
            {const_cast<char*>("AddMouseMapping"), imAddMapping, METH_VARARGS, nullptr},
            {const_cast<char*>("RemoveKeyMapping"), imRemove, METH_VARARGS, nullptr},
            {const_cast<char*>("RemoveMapping"), imRemove, METH_VARARGS, nullptr},
            {const_cast<char*>("RemoveMouseMapping"), imRemove, METH_VARARGS, nullptr},
            {const_cast<char*>("Clear"), imClear, METH_VARARGS, nullptr},
            {const_cast<char*>("SwitchInputState"), imSwitchInputState, METH_VARARGS, nullptr},
            {nullptr, nullptr, 0, nullptr}};
        mapper = Py_InitModule(const_cast<char*>("openrasa_inputmapper"), defs);
        Py_INCREF(mapper);
    }
    Py_INCREF(mapper);
    return mapper;
}

void setInputRouter(PyObject* r) {
    Py_XINCREF(r);
    Py_XDECREF(router);
    router = r;
}

}  // namespace engine::ui
