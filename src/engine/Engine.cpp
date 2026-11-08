// Engine.cpp - see Engine.h.
#include "Engine.h"
#include "ClientVersion.h"
#include "Stub.h"
#include "PyCppObject.h"
#include "Audio.h"
#include "VoiceChat.h"
#include "Cursor.h"
#include "UiManager.h"
#include "UiRender.h"
#include "World.h"
#include "Catalogs.h"
#include "Player.h"
#include "../net/AuthClient.h"
#include "../net/AuthCrypt.h"
#include "../net/TrConnection.h"
#include "../net/GameProtocol.h"
#include "PyMarshal.h"
#include "EntityBody.h"
#include "PyProfile.h"
#include "Rqs.h"
#include "../scene/SceneView.h"

#include "assetlibrary.h"
#include "Config.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <initializer_list>
#include <list>
#include <map>
#include <memory>

namespace engine {

namespace {

Callbacks callbacks;
bool quit = false;
std::string gameRoot;

// ---- Small C API helpers ------------------------------------------------------------------------------------------------
using CFunction = PyObject* (*)(PyObject*, PyObject*);

PyObject* function(const char* name, CFunction fn) {
    // Method definitions must outlive the function objects: keep them for the life of the process.
    static std::vector<std::unique_ptr<PyMethodDef>> defs;
    defs.push_back(std::make_unique<PyMethodDef>(PyMethodDef{const_cast<char*>(name), fn, METH_VARARGS, nullptr}));
    return PyCFunction_New(defs.back().get(), nullptr);
}

void put(PyObject* stub, const char* name, CFunction fn) {
    PyObject* f = function(name, fn);
    setStubAttribute(stub, name, f);
    Py_DECREF(f);
}

PyObject* none() {
    Py_INCREF(Py_None);
    return Py_None;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](char c) { return char(tolower(uint8_t(c))); });
    return s;
}

// ---- Options manager (TRasa::OptionsManager, optionsmanager.cpp; traced 2026-10-28) ------------------------------------------------
// The options are a tree of TRasa::Option nodes (0x6c bytes, constructor FUN_004ef700): + 4 name, + 0x1c value, + 0x38 default (all
// text), + 0x54 persistent, + 0x58 parent, + 0x60 the children (a std::list, appended), + 0x64 their count, + 0x68 the node's Python
// object (made on first use through extern_gameclient_option_wrap.CreateWrapper, FUN_004efd80, and kept: the same object every time).
// The manager's root (+ 0xc) has the name "". A name is split at any of '.', '/', '\' (DAT_00bd8040, FUN_004813e0; empty parts
// skipped) and looked up part by part among the children with _stricmp (FUN_004efb60 / FUN_004efaf0): FUN_004943a0 / FUN_004942e0
// (no parts: the root). A node's path (FUN_004efc00) is its name with each ancestor's name and "." in front, up to the root.
// FUN_00494070 (BuildOptionHierarchy) makes the missing parts: value "", default "", persistent (FUN_004ef900(parent, part, "", 1)).
// The observers (+ 4, slots 1 added / 2 changed / 3 removed / 4 sync / 6 save) are OpenRasa's functions below; slots 1 and 3 do
// nothing here (the observers' bodies for them are not read).
struct OptionNode {
    std::string name, value, def;
    bool persistent = false;
    OptionNode* parent = nullptr;
    std::list<OptionNode*> children;
    PyObject* wrapper = nullptr;   // + 0x68 (a reference held)
};
OptionNode optionRoot;   // OptionsManager + 0xc

struct OptionObject {
    PyObject_HEAD
    OptionNode* node;   // null once the option is deleted (the exe's object would point at freed memory)
};
PyTypeObject optionType = {PyObject_HEAD_INIT(nullptr)};

// _stricmp in the C locale: A-Z folded to a-z.
int optionCompare(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        int x = uint8_t(a[i]), y = uint8_t(b[i]);
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return x - y;
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}
std::string asciiLower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c + 32);
    return s;
}

std::vector<std::string> optionParts(const std::string& name) {
    std::vector<std::string> parts;
    std::string part;
    for (char c : name) {
        if (c == '.' || c == '/' || c == '\\') {
            if (!part.empty()) parts.push_back(part);
            part.clear();
        } else {
            part += c;
        }
    }
    if (!part.empty()) parts.push_back(part);
    return parts;
}
OptionNode* optionChild(OptionNode* node, const std::string& name) {
    for (OptionNode* c : node->children)
        if (optionCompare(c->name, name) == 0) return c;
    return nullptr;
}
OptionNode* findOption(const std::string& name) {
    OptionNode* node = &optionRoot;
    for (const std::string& part : optionParts(name)) {
        node = optionChild(node, part);
        if (!node) return nullptr;
    }
    return node;
}
// FUN_004ef900: the child of that name, else a new one (value and default = value) appended.
OptionNode* addChildOption(OptionNode* parent, const std::string& name, const std::string& value, bool persistent) {
    if (OptionNode* c = optionChild(parent, name)) return c;
    OptionNode* c = new OptionNode();
    c->name = name;
    c->value = value;
    c->def = value;
    c->persistent = persistent;
    c->parent = parent;
    parent->children.push_back(c);
    return c;
}
OptionNode* buildOption(const std::string& name) {
    OptionNode* node = &optionRoot;
    for (const std::string& part : optionParts(name)) node = addChildOption(node, part, "", true);
    return node;
}
// FUN_004ef780 + delete: the children first (FUN_004efa90), the Python object let go.
void deleteOption(OptionNode* node) {
    for (OptionNode* c : node->children) deleteOption(c);
    node->children.clear();
    if (node->wrapper) {
        reinterpret_cast<OptionObject*>(node->wrapper)->node = nullptr;
        Py_DECREF(node->wrapper);
        node->wrapper = nullptr;
    }
    delete node;
}
std::string optionPath(const OptionNode* node) {
    std::string path = node->name;
    for (const OptionNode* p = node->parent; p && !p->name.empty(); p = p->parent) path = p->name + "." + path;
    return path;
}
// Pre-order, as TRasa::Option's visit (FUN_004efd40); TRasa::OptionVisitorEnumerate (FUN_004f3c90, slot 1 FUN_004f3e60) keeps the
// options whose lower-cased path CONTAINS the lower-cased text (all when it is empty).
void enumerateOptions(OptionNode* node, const std::string& lowerText, std::vector<OptionNode*>& out) {
    if (lowerText.empty() || asciiLower(optionPath(node)).find(lowerText) != std::string::npos) out.push_back(node);
    for (OptionNode* c : node->children) enumerateOptions(c, lowerText, out);
}
std::vector<OptionNode*> enumerateOptions(const std::string& text) {
    std::vector<OptionNode*> out;
    enumerateOptions(&optionRoot, asciiLower(text), out);
    return out;
}

PyObject* optionObject(OptionNode* node) {   // a new reference
    if (!node->wrapper) {
        OptionObject* o = PyObject_New(OptionObject, &optionType);
        if (!o) return nullptr;
        o->node = node;
        node->wrapper = reinterpret_cast<PyObject*>(o);
    }
    Py_INCREF(node->wrapper);
    return node->wrapper;
}

const std::string* optionValue(const char* name) {
    const OptionNode* node = findOption(name);
    return node ? &node->value : nullptr;
}

void configureAudio();
void optionChanged(const std::string& path, const std::string& value);
bool loadingOptions = false;   // client.cfg being read: no observers (OpenRasa's: its observers act only after the load)

// OptionsManager::SetOption (FUN_00494270): the option, made when missing (BuildOptionHierarchy: persistent), takes the value; only a
// different value is stored and told to the observers (slot 2, FUN_0049df40).
void setOption(const char* name, const std::string& value) {
    OptionNode* node = buildOption(name);
    if (node->value == value) return;
    node->value = value;
    if (!loadingOptions) optionChanged(optionPath(node), value);
}
// OptionsManager::AddOption (FUN_004941f0): an existing option is returned as it is; a new one (BuildOptionHierarchy) gets the value as
// its value and default and the persistence given; the observers' slot 1 (added).
OptionNode* addOption(const char* name, const std::string& value, bool persistent) {
    if (OptionNode* node = findOption(name)) return node;
    OptionNode* node = buildOption(name);
    node->def = value;
    node->value = value;
    node->persistent = persistent;
    return node;
}

// ---- Values as the manager converts them (the "usa" locale streams: FUN_004040c0 int, FUN_00496ea0 float, FUN_00496fd0 /
// FUN_00497090 reading) -----------------------------------------------------------------------------------------------------------
// An int written by an ostream with the "usa" locale: digits grouped by thousands with commas ("1,920", "-1").
std::string groupedInt(long v) {
    const unsigned long magnitude = v < 0 ? 0ul - static_cast<unsigned long>(v) : static_cast<unsigned long>(v);
    std::string digits = std::to_string(magnitude), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return v < 0 ? "-" + out : out;
}
// A float (promoted to double) through the same ostream: precision 6, %g; the integer digits grouped; MSVC's three-digit exponent.
// UNVERIFIED: the grouping of a number in exponent form (none in the shipped options).
std::string groupedFloat(float f) {
    char text[64];
    PyOS_snprintf(text, sizeof(text), "%g", double(f));
    std::string s = text;
    const size_t e = s.find_first_of("eE");
    std::string mantissa = e == std::string::npos ? s : s.substr(0, e), exponent = e == std::string::npos ? std::string() : s.substr(e);
    if (!exponent.empty()) {   // e+06 -> e+006
        std::string digits = exponent.substr(2);
        while (digits.size() < 3) digits = "0" + digits;
        exponent = exponent.substr(0, 2) + digits;
    }
    const size_t start = (mantissa[0] == '-' || mantissa[0] == '+') ? 1 : 0;
    size_t end = mantissa.find('.');
    if (end == std::string::npos) end = mantissa.size();
    std::string grouped;
    for (size_t i = start; i < end; ++i) {
        if (i > start && (end - i) % 3 == 0) grouped += ',';
        grouped += mantissa[i];
    }
    return mantissa.substr(0, start) + grouped + mantissa.substr(end) + exponent;
}
// istream >> int / double with the "usa" locale: leading white space skipped, a sign, digits (thousands commas allowed in the
// integer part), and for a double a fraction and exponent; the text after the number is ignored. False (the value untouched) when no
// number starts the text or an int overflows. UNVERIFIED: misplaced group separators (MSVC sets failbit after reading them).
bool readStreamNumber(const std::string& text, bool real, double& out) {
    size_t i = 0;
    while (i < text.size() && std::isspace(uint8_t(text[i]))) ++i;
    std::string number;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) number += text[i++];
    bool digits = false;
    while (i < text.size() && (std::isdigit(uint8_t(text[i])) || (text[i] == ',' && digits))) {
        if (text[i] != ',') {
            number += text[i];
            digits = true;
        }
        ++i;
    }
    if (real) {
        if (i < text.size() && text[i] == '.') {
            number += text[i++];
            while (i < text.size() && std::isdigit(uint8_t(text[i]))) {
                number += text[i++];
                digits = true;
            }
        }
        if (digits && i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
            size_t j = i + 1;
            std::string exponent = "e";
            if (j < text.size() && (text[j] == '+' || text[j] == '-')) exponent += text[j++];
            if (j < text.size() && std::isdigit(uint8_t(text[j]))) {
                while (j < text.size() && std::isdigit(uint8_t(text[j]))) exponent += text[j++];
                number += exponent;
            }
        }
    }
    if (!digits) return false;
    if (real) {
        out = std::strtod(number.c_str(), nullptr);
    } else {
        errno = 0;
        const long long v = std::strtoll(number.c_str(), nullptr, 10);
        if (errno == ERANGE || v > INT32_MAX || v < INT32_MIN) return false;
        out = double(v);
    }
    return true;
}
// FUN_0049dfb0: "True", "Yes", "On" or "1" (_stricmp) is true, anything else false.
bool optionBoolText(const std::string& v) {
    return optionCompare(v, "True") == 0 || optionCompare(v, "Yes") == 0 || optionCompare(v, "On") == 0 || optionCompare(v, "1") == 0;
}
// std::wstring -> std::string by ctype<wchar_t>::narrow in the classic locale ('@' for what does not fit: code points above 255),
// FUN_00481670; back by widen (bytes as code points 0 - 255), FUN_00481780.
std::string narrowUnicode(PyObject* u) {
    std::string out;
    const Py_UNICODE* p = PyUnicode_AS_UNICODE(u);
    for (int i = 0, n = PyUnicode_GET_SIZE(u); i < n; ++i) out += p[i] < 256 ? char(uint8_t(p[i])) : '@';
    return out;
}
PyObject* widenToUnicode(const std::string& s) {
    std::vector<Py_UNICODE> w(s.size() + 1);
    for (size_t i = 0; i < s.size(); ++i) w[i] = Py_UNICODE(uint8_t(s[i]));
    return PyUnicode_FromUnicode(w.data(), int(s.size()));
}

// ---- Boost.Python-like argument conversion (the exe's bindings take std::string / std::wstring / int / float / bool) --------------
// A wrong type raises TypeError (Boost.Python's ArgumentError is a TypeError; its message lists the C++ signatures - this one names
// the method and argument only, UNVERIFIED wording).
bool argError(const char* method, int index, const char* wanted) {
    char text[200];
    PyOS_snprintf(text, sizeof(text), "Python argument types in %s did not match C++ signature (argument %d: %s)", method, index + 1, wanted);
    PyErr_SetString(PyExc_TypeError, text);
    return false;
}
bool argCount(PyObject* args, int n, const char* method) {
    if (PyTuple_GET_SIZE(args) == n) return true;
    char text[200];
    PyOS_snprintf(text, sizeof(text), "Python argument types in %s did not match C++ signature (%d argument(s) expected)", method, int(n));
    PyErr_SetString(PyExc_TypeError, text);
    return false;
}
bool argString(PyObject* args, int i, std::string& out, const char* method) {
    PyObject* o = PyTuple_GET_ITEM(args, i);
    if (!PyString_Check(o)) return argError(method, i, "str");
    out.assign(PyString_AS_STRING(o), size_t(PyString_GET_SIZE(o)));
    return true;
}
bool argWide(PyObject* args, int i, std::string& out, const char* method) {   // std::wstring, narrowed as the manager does
    PyObject* o = PyTuple_GET_ITEM(args, i);
    if (!PyUnicode_Check(o)) return argError(method, i, "unicode");
    out = narrowUnicode(o);
    return true;
}
bool argInt(PyObject* args, int i, long& out, const char* method) {
    PyObject* o = PyTuple_GET_ITEM(args, i);
    if (!PyInt_Check(o) && !PyLong_Check(o)) return argError(method, i, "int");
    out = PyInt_AsLong(o);
    return !(out == -1 && PyErr_Occurred());
}
bool argFloat(PyObject* args, int i, double& out, const char* method) {
    PyObject* o = PyTuple_GET_ITEM(args, i);
    if (!PyFloat_Check(o) && !PyInt_Check(o) && !PyLong_Check(o)) return argError(method, i, "float");
    out = PyFloat_AsDouble(o);
    return !(out == -1.0 && PyErr_Occurred());
}
bool argBool(PyObject* args, int i, bool& out, const char* method) {
    PyObject* o = PyTuple_GET_ITEM(args, i);
    if (!PyInt_Check(o) && !PyLong_Check(o)) return argError(method, i, "bool");
    out = PyObject_IsTrue(o) != 0;
    return true;
}

// ---- extern_gameclient_option_wrap.Option (FUN_004ef460) -------------------------------------------------------------------------
OptionNode* liveOption(PyObject* self) {
    OptionNode* node = reinterpret_cast<OptionObject*>(self)->node;
    // OpenRasa's guard: the exe keeps no check, a deleted option's object points at freed memory.
    if (!node) PyErr_SetString(PyExc_RuntimeError, "the option has been removed");
    return node;
}
PyObject* textObject(const std::string& s) { return PyString_FromStringAndSize(s.data(), int(s.size())); }
PyObject* optionGetName(PyObject* self, PyObject*) {   // 0x004efeb0
    OptionNode* n = liveOption(self);
    return n ? textObject(n->name) : nullptr;
}
PyObject* optionGetValue(PyObject* self, PyObject*) {   // 0x004eff50
    OptionNode* n = liveOption(self);
    return n ? textObject(n->value) : nullptr;
}
PyObject* optionGetDefault(PyObject* self, PyObject*) {   // 0x004efff0
    OptionNode* n = liveOption(self);
    return n ? textObject(n->def) : nullptr;
}
// SetName / SetValue / SetDefault (0x004efea0 / 0x004eff40 / 0x004effe0): std::string assignments of + 4 / + 0x1c / + 0x38 - no
// observer is told (unlike the manager's SetOption*), and a renamed option is found under its new name.
PyObject* optionSetField(PyObject* self, PyObject* args, std::string OptionNode::*field, const char* method) {
    OptionNode* n = liveOption(self);
    std::string v;
    if (!n || !argCount(args, 1, method) || !argString(args, 0, v, method)) return nullptr;
    n->*field = v;
    return none();
}
PyObject* optionSetName(PyObject* self, PyObject* args) { return optionSetField(self, args, &OptionNode::name, "Option.SetName"); }
PyObject* optionSetValue(PyObject* self, PyObject* args) { return optionSetField(self, args, &OptionNode::value, "Option.SetValue"); }
PyObject* optionSetDefault(PyObject* self, PyObject* args) { return optionSetField(self, args, &OptionNode::def, "Option.SetDefault"); }
PyObject* optionIsPersistant(PyObject* self, PyObject*) {   // 0x004f0090: + 0x54
    OptionNode* n = liveOption(self);
    return n ? PyBool_FromLong(n->persistent) : nullptr;
}
PyObject* optionSetPersistant(PyObject* self, PyObject* args) {   // 0x004f0080
    OptionNode* n = liveOption(self);
    bool v = false;
    if (!n || !argCount(args, 1, "Option.SetPersistant") || !argBool(args, 0, v, "Option.SetPersistant")) return nullptr;
    n->persistent = v;
    return none();
}
PyObject* optionIsName(PyObject* self, PyObject* args) {   // 0x004f0120: _stricmp with the name
    OptionNode* n = liveOption(self);
    std::string v;
    if (!n || !argCount(args, 1, "Option.IsName") || !argString(args, 0, v, "Option.IsName")) return nullptr;
    return PyBool_FromLong(optionCompare(n->name, v) == 0);
}
PyObject* optionIsPath(PyObject* self, PyObject* args) {   // 0x004f0720 -> FUN_004efce0: _stricmp with the path
    OptionNode* n = liveOption(self);
    std::string v;
    if (!n || !argCount(args, 1, "Option.IsPath") || !argString(args, 0, v, "Option.IsPath")) return nullptr;
    return PyBool_FromLong(optionCompare(optionPath(n), v) == 0);
}
PyObject* optionGetPath(PyObject* self, PyObject*) {   // 0x004f0670
    OptionNode* n = liveOption(self);
    return n ? textObject(optionPath(n)) : nullptr;
}
PyObject* optionGetParent(PyObject* self, PyObject*) {   // 0x004f0590: None for the root
    OptionNode* n = liveOption(self);
    if (!n) return nullptr;
    return n->parent ? optionObject(n->parent) : none();
}
PyObject* optionGetChildren(PyObject* self, PyObject*) {   // 0x004f0490: a list, in the children's order
    OptionNode* n = liveOption(self);
    if (!n) return nullptr;
    PyObject* list = PyList_New(0);
    for (OptionNode* c : n->children) {
        PyObject* o = optionObject(c);
        PyList_Append(list, o);
        Py_DECREF(o);
    }
    return list;
}
PyObject* optionGetChild(PyObject* self, PyObject* args) {   // 0x004f03b0 -> FUN_004efb60: None when there is none
    OptionNode* n = liveOption(self);
    std::string v;
    if (!n || !argCount(args, 1, "Option.GetChild") || !argString(args, 0, v, "Option.GetChild")) return nullptr;
    OptionNode* c = optionChild(n, v);
    return c ? optionObject(c) : none();
}
PyObject* optionAddChild(PyObject* self, PyObject* args) {   // 0x004f01d0 -> FUN_004ef900(name, value, persistent)
    OptionNode* n = liveOption(self);
    std::string name, value;
    bool persistent = false;
    if (!n || !argCount(args, 3, "Option.AddChild") || !argString(args, 0, name, "Option.AddChild") ||
        !argString(args, 1, value, "Option.AddChild") || !argBool(args, 2, persistent, "Option.AddChild"))
        return nullptr;
    return optionObject(addChildOption(n, name, value, persistent));
}
PyObject* optionRemoveChild(PyObject* self, PyObject* args) {   // 0x004f0290 -> FUN_004efa00: the first match deleted; True / False
    OptionNode* n = liveOption(self);
    std::string v;
    if (!n || !argCount(args, 1, "Option.RemoveChild") || !argString(args, 0, v, "Option.RemoveChild")) return nullptr;
    for (auto it = n->children.begin(); it != n->children.end(); ++it)
        if (optionCompare((*it)->name, v) == 0) {
            OptionNode* c = *it;
            n->children.erase(it);
            deleteOption(c);
            Py_RETURN_TRUE;
        }
    Py_RETURN_FALSE;
}
PyObject* optionRemoveChildren(PyObject* self, PyObject*) {   // 0x004f0320 -> FUN_004efa90: True
    OptionNode* n = liveOption(self);
    if (!n) return nullptr;
    std::list<OptionNode*> children;
    children.swap(n->children);
    for (OptionNode* c : children) deleteOption(c);
    Py_RETURN_TRUE;
}
PyMethodDef optionMethods[] = {
    {const_cast<char*>("SetName"), optionSetName, METH_VARARGS, nullptr},
    {const_cast<char*>("GetName"), optionGetName, METH_NOARGS, nullptr},
    {const_cast<char*>("SetValue"), optionSetValue, METH_VARARGS, nullptr},
    {const_cast<char*>("GetValue"), optionGetValue, METH_NOARGS, nullptr},
    {const_cast<char*>("SetDefault"), optionSetDefault, METH_VARARGS, nullptr},
    {const_cast<char*>("GetDefault"), optionGetDefault, METH_NOARGS, nullptr},
    {const_cast<char*>("SetPersistant"), optionSetPersistant, METH_VARARGS, nullptr},
    {const_cast<char*>("IsPersistant"), optionIsPersistant, METH_NOARGS, nullptr},
    {const_cast<char*>("IsName"), optionIsName, METH_VARARGS, nullptr},
    {const_cast<char*>("AddChild"), optionAddChild, METH_VARARGS, nullptr},
    {const_cast<char*>("RemoveChild"), optionRemoveChild, METH_VARARGS, nullptr},
    {const_cast<char*>("RemoveChildren"), optionRemoveChildren, METH_NOARGS, nullptr},
    {const_cast<char*>("GetChild"), optionGetChild, METH_VARARGS, nullptr},
    {const_cast<char*>("GetChildren"), optionGetChildren, METH_NOARGS, nullptr},
    {const_cast<char*>("GetParent"), optionGetParent, METH_NOARGS, nullptr},
    {const_cast<char*>("GetPath"), optionGetPath, METH_NOARGS, nullptr},
    {const_cast<char*>("IsPath"), optionIsPath, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};
// The wrapped classes' __init__ (FUN_0046b5e0 puts the C function at 0x0046b5c0 there): RuntimeError "This class cannot be
// instantiated from Python".
PyObject* cannotInstantiate(PyTypeObject*, PyObject*, PyObject*) {
    PyErr_SetString(PyExc_RuntimeError, "This class cannot be instantiated from Python");
    return nullptr;
}
// CreateWrapper (0x004ef420 and its kin in every wrapped module: MOV EAX, [ESP + 4]; RET): returns its argument.
PyObject* createWrapper(PyObject*, PyObject* args) {
    if (PyTuple_GET_SIZE(args) != 1) {
        PyErr_SetString(PyExc_TypeError, "CreateWrapper takes exactly one argument");
        return nullptr;
    }
    PyObject* o = PyTuple_GET_ITEM(args, 0);
    Py_INCREF(o);
    return o;
}

// ---- The manager's Python methods (extern_gameclient_optionsmanager_wrap.OptionsManager, FUN_00493d50) ----------------------------
// Get<Type>(name, default) (FUN_00495590 bool, FUN_00495430 int, FUN_004954e0 float, FUN_00495200 string, FUN_004952e0 unicode): a
// missing option gives the default; else the value converted - a bool by optionBoolText (any other text False), an int / float by the
// stream (unreadable: the default), a string as it is, unicode widened.
PyObject* getOptionBool(PyObject*, PyObject* args) {
    std::string name;
    bool fallback = false;
    if (!argCount(args, 2, "OptionsManager.GetOptionBool") || !argString(args, 0, name, "OptionsManager.GetOptionBool") ||
        !argBool(args, 1, fallback, "OptionsManager.GetOptionBool"))
        return nullptr;
    const std::string* v = optionValue(name.c_str());
    return PyBool_FromLong(v ? optionBoolText(*v) : fallback);
}
PyObject* getOptionInt(PyObject*, PyObject* args) {
    std::string name;
    long fallback = 0;
    if (!argCount(args, 2, "OptionsManager.GetOptionInt") || !argString(args, 0, name, "OptionsManager.GetOptionInt") ||
        !argInt(args, 1, fallback, "OptionsManager.GetOptionInt"))
        return nullptr;
    double d = 0;
    if (const std::string* v = optionValue(name.c_str()); v && readStreamNumber(*v, false, d)) fallback = long(d);
    return PyInt_FromLong(fallback);
}
PyObject* getOptionFloat(PyObject*, PyObject* args) {
    std::string name;
    double fallback = 0;
    if (!argCount(args, 2, "OptionsManager.GetOptionFloat") || !argString(args, 0, name, "OptionsManager.GetOptionFloat") ||
        !argFloat(args, 1, fallback, "OptionsManager.GetOptionFloat"))
        return nullptr;
    double d = double(float(fallback));   // the default is a float argument, widened to the double read into
    if (const std::string* v = optionValue(name.c_str())) readStreamNumber(*v, true, d);
    return PyFloat_FromDouble(d);
}
PyObject* getOptionString(PyObject*, PyObject* args) {
    std::string name, fallback;
    if (!argCount(args, 2, "OptionsManager.GetOptionString") || !argString(args, 0, name, "OptionsManager.GetOptionString") ||
        !argString(args, 1, fallback, "OptionsManager.GetOptionString"))
        return nullptr;
    const std::string* v = optionValue(name.c_str());
    return textObject(v ? *v : fallback);
}
PyObject* getOptionUnicode(PyObject*, PyObject* args) {
    std::string name;
    if (!argCount(args, 2, "OptionsManager.GetOptionUnicode") || !argString(args, 0, name, "OptionsManager.GetOptionUnicode")) return nullptr;
    PyObject* fallback = PyTuple_GET_ITEM(args, 1);
    if (!PyUnicode_Check(fallback)) {
        argError("OptionsManager.GetOptionUnicode", 1, "unicode");
        return nullptr;
    }
    const std::string* v = optionValue(name.c_str());
    if (!v) {
        Py_INCREF(fallback);
        return fallback;
    }
    return widenToUnicode(*v);
}
// Set<Type>(name, value) (FUN_00495090 bool "True" / "False", FUN_00494f90 int, FUN_00495010 float, FUN_00494f00 string, FUN_00494f20
// unicode narrowed): SetOption, None.
bool optionArgText(PyObject* args, char type, std::string& out, const char* method) {
    switch (type) {
    case 'b': {
        bool b = false;
        if (!argBool(args, 1, b, method)) return false;
        out = b ? "True" : "False";
        return true;
    }
    case 'i': {
        long v = 0;
        if (!argInt(args, 1, v, method)) return false;
        out = groupedInt(v);
        return true;
    }
    case 'f': {
        double v = 0;
        if (!argFloat(args, 1, v, method)) return false;
        out = groupedFloat(float(v));
        return true;
    }
    case 's': return argString(args, 1, out, method);
    default: return argWide(args, 1, out, method);
    }
}
const char* optionTypeName(char type) {
    return type == 'b' ? "Bool" : type == 'i' ? "Int" : type == 'f' ? "Float" : type == 's' ? "String" : "Unicode";
}
template <char Type>
PyObject* setOptionTyped(PyObject*, PyObject* args) {
    static const std::string method = std::string("OptionsManager.SetOption") + optionTypeName(Type);
    std::string name, value;
    if (!argCount(args, 2, method.c_str()) || !argString(args, 0, name, method.c_str()) || !optionArgText(args, Type, value, method.c_str()))
        return nullptr;
    setOption(name.c_str(), value);
    return none();
}
// AddOption<Type>(name, value, persistent) (FUN_00494dc0 bool, FUN_00494b80 int, FUN_00494ca0 float, FUN_00494970 string,
// FUN_00494a60 unicode): AddOption; the option's object.
template <char Type>
PyObject* addOptionTyped(PyObject*, PyObject* args) {
    static const std::string method = std::string("OptionsManager.AddOption") + optionTypeName(Type);
    std::string name, value;
    bool persistent = false;
    if (!argCount(args, 3, method.c_str()) || !argString(args, 0, name, method.c_str()) || !optionArgText(args, Type, value, method.c_str()) ||
        !argBool(args, 2, persistent, method.c_str()))
        return nullptr;
    return optionObject(addOption(name.c_str(), value, persistent));
}
// RemoveOption(name) (0x00495650 -> FUN_00494540): an unknown name is logged ("%s failed: Option \"%s\" not registered.") and gives
// False; else the observers' slot 3, the option taken from its parent and deleted, True.
PyObject* removeOption(PyObject*, PyObject* args) {
    std::string name;
    if (!argCount(args, 1, "OptionsManager.RemoveOption") || !argString(args, 0, name, "OptionsManager.RemoveOption")) return nullptr;
    OptionNode* node = findOption(name);
    if (!node) {
        std::fprintf(stderr, "[options] RemoveOption failed: Option \"%s\" not registered.\n", name.c_str());
        Py_RETURN_FALSE;
    }
    if (node == &optionRoot) {   // the exe deletes its root object here (it has no parent); OpenRasa keeps it, emptied
        std::list<OptionNode*> children;
        children.swap(optionRoot.children);
        for (OptionNode* c : children) deleteOption(c);
        Py_RETURN_TRUE;
    }
    node->parent->children.remove(node);
    deleteOption(node);
    Py_RETURN_TRUE;
}
// RestoreDefault(name) (0x00495640 -> FUN_00494460): unknown -> logged, False; else the default back as the value when it differs
// (observers' slot 2), True.
PyObject* restoreDefault(PyObject*, PyObject* args) {
    std::string name;
    if (!argCount(args, 1, "OptionsManager.RestoreDefault") || !argString(args, 0, name, "OptionsManager.RestoreDefault")) return nullptr;
    OptionNode* node = findOption(name);
    if (!node) {
        std::fprintf(stderr, "[options] RestoreDefault failed: Option \"%s\" not registered.\n", name.c_str());
        Py_RETURN_FALSE;
    }
    if (node->value != node->def) {
        node->value = node->def;
        optionChanged(optionPath(node), node->value);
    }
    Py_RETURN_TRUE;
}
// ForceOptionChangeEvent(text) (0x00495660 -> FUN_004946d0): slot 2 for every option whose path contains the text (enumerated first).
PyObject* forceOptionChangeEvent(PyObject*, PyObject* args) {
    std::string text;
    if (!argCount(args, 1, "OptionsManager.ForceOptionChangeEvent") || !argString(args, 0, text, "OptionsManager.ForceOptionChangeEvent"))
        return nullptr;
    std::vector<std::pair<std::string, std::string>> changed;
    for (OptionNode* n : enumerateOptions(text)) changed.emplace_back(optionPath(n), n->value);
    for (const auto& [path, value] : changed) optionChanged(path, value);
    return none();
}

PyObject* noOp(PyObject*, PyObject*) { return none(); }

// ---- Saving client.cfg (TRasa::Config::ExportFile FUN_00476780, the client serializer FUN_005616f0, traced 2026-10-25) ------------
// The file: [ConfigInfo] Version = 2, then [Options] with the "Notes" comment (FUN_00478ba0) and every persistent option with a name,
// sorted by name (a std::map: byte order), each "name = value"; written in text mode ("\n" -> CRLF), a blank line after each section.
// A value is written bare when every character is alphanumeric or one of ".,_-@:/" and it does not start with "." (the empty value
// too), else quoted with \ and " escaped (FUN_00475d40). Only Client.* options are this file's - User.* and Character.* have their
// own serializers (TRasa::OptionSerializerUser / OptionSerializerCharacter, the SaveUserOptions / SaveCharacterOptions server calls) and
// are not saved here. The tree's inner options (made persistent with an empty value by BuildOptionHierarchy) are written too.
// The first save keeps the game's file as client.cfg.openrasa-original.
std::string configValue(const std::string& v) {
    bool bare = v.empty() || v[0] != '.';
    for (char c : v)
        if (!std::isalnum(static_cast<unsigned char>(c)) && !std::strchr(".,_-@:/", c)) bare = false;
    if (bare) return v;
    std::string out = "\"";
    for (char c : v) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out + "\"";
}
void saveClientConfig() {
    // The options whose path contains "client." (OptionVisitorEnumerate with the serializer's "Client.", + 0x20), persistent and named,
    // by path in a std::map (byte order).
    std::map<std::string, std::string> items;
    for (const OptionNode* n : enumerateOptions("Client."))
        if (n->persistent && !n->name.empty()) items[optionPath(n)] = n->value;
    // OPENRASA_CLIENTCFG_OUT=<file> (test runs): write there instead, the game's file untouched.
    const char* testOut = std::getenv("OPENRASA_CLIENTCFG_OUT");
    const std::filesystem::path path = testOut && *testOut ? std::filesystem::path(testOut) : std::filesystem::path(gameRoot) / "client.cfg";
    std::error_code ec;
    const std::filesystem::path backup = std::filesystem::path(gameRoot) / "client.cfg.openrasa-original";
    if (!(testOut && *testOut) && std::filesystem::exists(path, ec) && !std::filesystem::exists(backup, ec))
        std::filesystem::copy_file(path, backup, ec);
    std::string text = "[ConfigInfo]\r\nVersion = 2\r\n\r\n";
    const char* notes =
        "##################################################\n\n Notes:\n  1. Be sure the game isn't running before editing this file or\n"
        "     your changes will be lost.\n  2. Be sure to save any changes to this file before starting the client\n"
        "     or your changes won't take effect and could be lost.\n  3. To reset any option to its default value, delete the entire line\n"
        "     listing the option from this file and restart the game.\n  4. To reset graphics settings, find and delete the entire line\n"
        "     containing \"Client.Graphics.AutoConfigureID\".  This will have\n     the game prompt you to reconfigure graphics settings upon "
        "restart.\n  5. To reset all settings stored in this file (graphics, chat options,\n     audio), delete this file and restart the game.\n"
        "##################################################\n";
    bool lineStart = true;
    for (const char* c = notes; *c; ++c) {
        if (lineStart) text += '#';
        if (*c == '\n') text += "\r\n";
        else text += *c;
        lineStart = *c == '\n';
    }
    text += "[Options]\r\n";
    for (const auto& [name, value] : items) text += configValue(name) + " = " + configValue(value) + "\r\n";
    text += "\r\n";
    if (FILE* f = std::fopen(path.string().c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    } else {
        std::fprintf(stderr, "[engine] could not write %s\n", path.string().c_str());
    }
}
// The Serialize observer's save (TRasa::OptionObserverSerialize slot 6, FUN_005360c0, traced 2026-10-28): the User.* options to the
// server through the scripts' clientmethod.SaveUserOptions(names) (OptionSerializerUser FUN_00561c50), the Character.* options through
// clientmethod.SaveCharacterOptions(names) (OptionSerializerCharacter FUN_00561a60), then client.cfg. Each list names the persistent
// options of that branch (option + 0x54 set, a name); the scripts send only those that differ from the generated defaults and from what
// they last sent. Called at once (FUN_00536900 on the client method entity 5, delay -1).
bool scheduleMethodCall(unsigned long long entityId, const char* method, PyObject* args, int delayMs);
void saveServerOptions(const char* prefix, const char* method) {
    std::vector<std::string> names;   // the enumeration's order (pre-order): paths containing the prefix (FUN_00561c50)
    for (const OptionNode* n : enumerateOptions(prefix))
        if (n->persistent && !n->name.empty()) names.push_back(optionPath(n));
    PyObject* list = PyList_New(0);
    if (!list) {
        PyErr_Print();
        return;
    }
    for (const std::string& n : names) {
        PyObject* s = PyString_FromString(n.c_str());
        if (s) {
            PyList_Append(list, s);
            Py_DECREF(s);
        }
    }
    scheduleMethodCall(5, method, Py_BuildValue(const_cast<char*>("(N)"), list), -1);
}
PyObject* endEditing(PyObject*, PyObject*) {
    player::keepCameraOptionsForSave();   // OpenRasa deviation (retail saved the zoom one logout late): see Player.cpp
    saveServerOptions("User.", "SaveUserOptions");
    saveServerOptions("Character.", "SaveCharacterOptions");
    saveClientConfig();
    return none();
}

// ---- Option observers (TRasa::OptionObserver* - Audio, Graphics, Input, Serialize, UI; traced 2026-10-25) --------------------------
// The options manager keeps them in a list (+ 4); slot 2 is told of each changed option, slot 4 runs on BeginEditing (and after a
// video change), slot 6 on EndEditing / Export (the Serialize observer saves). The UI observer's slot 4 (FUN_00532c20 ->
// FUN_005202e0) calls the scripts' client.gameui.SyncUIToOptions (posting UI_SYNC_OPTIONS: the options window re-reads its controls).
void syncUiToOptions() {
    if (!Py_IsInitialized() || !ui::manager().root()) return;
    PyObject* m = PyImport_ImportModule("client.gameui");
    if (!m) {
        PyErr_Print();
        return;
    }
    PyObject* r = PyObject_CallMethod(m, const_cast<char*>("SyncUIToOptions"), nullptr);
    if (!r) PyErr_Print();
    Py_XDECREF(r);
    Py_DECREF(m);
}
void graphicsBeginEditing();
PyObject* beginEditing(PyObject*, PyObject*) {
    graphicsBeginEditing();
    syncUiToOptions();
    return none();
}

VideoRequest videoRequest;
bool videoRequested = false;

int intOptionValue(const char* name, int fallback) {
    const std::string* v = optionValue(name);
    if (!v || v->empty()) return fallback;
    std::string digits;
    for (char c : *v)
        if (c != ',') digits += c;
    return std::atoi(digits.c_str());
}
bool boolOptionValue(const char* name, bool fallback) {
    const std::string* v = optionValue(name);
    if (!v || v->empty()) return fallback;
    return lower(*v) == "true" || *v == "1";
}
// OptionObserverGraphics slot 4 (FUN_0053b400), the part about the options window's rows: it writes the renderer's settings back into
// the options (not done here: OpenRasa's options are the settings) and sets the ".DisableUI" flags the options window greys rows by
// (optionswindow.py _LoadOptions / _RefreshOptions): windowed -> FullScreenResolution, FullScreenRefreshRate, Gamma, GammaGradient;
// a Video Quality preset (not "Custom", render settings + 0xB8 != 6) -> GeometryQuality, TextureQuality, Flora, TerrainQuality,
// Refractions, FakeShadows, Animations.DetailLevel, DeferredLighting, and FloraDensity also without Flora; Reflections,
// SpecularLighting, StaticShadows, DynamicShadows also without deferred lighting (+ 0x2F, the device's deferred capability, taken as
// present); ShadowDistance and ShadowMaxSimultaneous also without static and dynamic shadows.
void graphicsBeginEditing() {
    auto flag = [](const char* name, bool disabled) { setOption((std::string(name) + ".DisableUI").c_str(), disabled ? "True" : "False"); };
    const bool windowed = !boolOptionValue("Client.Graphics.FullScreen", false);
    const std::string* quality = optionValue("Client.Graphics.VideoQuality");
    const bool preset = !(quality && lower(*quality) == "custom");
    const bool deferred = boolOptionValue("Client.Graphics.DeferredLighting", true);
    const bool shadows = boolOptionValue("Client.Graphics.StaticShadows", true) || boolOptionValue("Client.Graphics.DynamicShadows", true);
    for (const char* n : {"Client.Graphics.FullScreenResolution", "Client.Graphics.FullScreenRefreshRate", "Client.Graphics.Gamma",
                          "Client.Graphics.GammaGradient"})
        flag(n, windowed);
    for (const char* n : {"Client.Graphics.GeometryQuality", "Client.Graphics.TextureQuality", "Client.Graphics.Flora",
                          "Client.Graphics.TerrainQuality", "Client.Graphics.Refractions", "Client.Graphics.FakeShadows",
                          "Client.Graphics.Animations.DetailLevel", "Client.Graphics.DeferredLighting"})
        flag(n, preset);
    flag("Client.Graphics.FloraDensity", preset || !boolOptionValue("Client.Graphics.Flora", true));
    for (const char* n : {"Client.Graphics.Reflections", "Client.Graphics.SpecularLighting", "Client.Graphics.StaticShadows",
                          "Client.Graphics.DynamicShadows"})
        flag(n, preset || !deferred);
    flag("Client.Graphics.ShadowDistance", preset || !deferred || !shadows);
    flag("Client.Graphics.ShadowMaxSimultaneous", preset || !deferred || !shadows);
}

// FUN_007c0560: a rate the mode does not list becomes the mode's first; 0 (the default rate) stays.
int validRefreshRate(int width, int height, int rate);   // after videoModes
// Video Quality presets and Auto Configure (after the video lists).
extern const char* const kQualityNames[];
extern int videoQualityLevel;
int qualityLevelOf(const std::string& value);
void applyVideoQuality(int level);
void autoConfigure();
void autoConfigurePrompt();

// OptionObserverGraphics slot 2 (FUN_00539b50), the display-mode part:
// - FullScreenResolution "WxH": .Width / .Height set, the refresh rate checked against the mode (FullScreenRefreshRate set when it
//   changes), a fullscreen mode change of that size queued;
// - FullScreenRefreshRate: applied (a device reset) while fullscreen;
// - VSync: applied (a device reset);
// - WindowedResolution "WxH": .Width / .Height set, a windowed mode change queued while windowed (no UI sync);
// - FullScreen: a mode change to the fullscreen or windowed size (each falls back to 1024 x 768);
// then, after any but WindowedResolution, every observer's slot 4 (the UI re-syncs). The rest of the function (quality, lighting,
// shadow, flora, gamma, view distance settings) is not applied here.
void graphicsOptionChanged(const std::string& name, const std::string& value) {
    bool video = false;
    int w = 0, h = 0;
    const bool fullscreen = boolOptionValue("Client.Graphics.FullScreen", false);
    if (name == "client.graphics.fullscreenresolution" && std::sscanf(value.c_str(), "%dx%d", &w, &h) == 2) {
        setOption("Client.Graphics.FullScreenResolution.Width", groupedInt(w));
        setOption("Client.Graphics.FullScreenResolution.Height", groupedInt(h));
        const int rate = intOptionValue("Client.Graphics.FullScreenRefreshRate", 0);
        const int valid = validRefreshRate(w, h, rate);
        if (valid != rate) setOption("Client.Graphics.FullScreenRefreshRate", groupedInt(valid));
        videoRequest.mode = true;
        videoRequest.width = w;
        videoRequest.height = h;
        videoRequest.fullscreen = true;
        video = true;
    } else if (name == "client.graphics.fullscreenrefreshrate" && std::sscanf(value.c_str(), "%d", &w) == 1) {
        if (fullscreen) {
            videoRequest.refresh = true;
            videoRequest.refreshRate = validRefreshRate(intOptionValue("Client.Graphics.FullScreenResolution.Width", 1024),
                                                        intOptionValue("Client.Graphics.FullScreenResolution.Height", 768), w);
            video = true;
        }
    } else if (name == "client.graphics.vsync") {
        videoRequest.vsync = true;
        videoRequest.vsyncOn = boolOptionValue("Client.Graphics.VSync", true);
        video = true;
    } else if (name == "client.graphics.windowedresolution" && std::sscanf(value.c_str(), "%dx%d", &w, &h) == 2) {
        setOption("Client.Graphics.WindowedResolution.Width", groupedInt(w));
        setOption("Client.Graphics.WindowedResolution.Height", groupedInt(h));
        if (!fullscreen) {
            videoRequest.mode = true;
            videoRequest.width = w;
            videoRequest.height = h;
            videoRequest.fullscreen = false;
        }
    } else if (name == "client.graphics.videoquality") {
        const int level = qualityLevelOf(value);
        if (level < 0) setOption("Client.Graphics.VideoQuality", kQualityNames[videoQualityLevel]);
        else applyVideoQuality(level);
        video = true;
    } else if (name == "client.graphics.autoconfig") {
        autoConfigure();
        video = true;
    } else if (name == "client.graphics.fullscreen") {
        videoRequest.mode = true;
        videoRequest.fullscreen = fullscreen;
        videoRequest.width = intOptionValue(fullscreen ? "Client.Graphics.FullScreenResolution.Width" : "Client.Graphics.WindowedResolution.Width", 1024);
        videoRequest.height = intOptionValue(fullscreen ? "Client.Graphics.FullScreenResolution.Height" : "Client.Graphics.WindowedResolution.Height", 768);
        videoRequest.refreshRate = intOptionValue("Client.Graphics.FullScreenRefreshRate", 0);
        video = true;
    } else if (name == "client.graphics.deferredlighting" || name == "client.graphics.staticshadows" ||
               name == "client.graphics.dynamicshadows" || name == "client.graphics.fakeshadows") {
        // FUN_00539b50: these four set its "re-sync" flag (local_76) whether or not the render bit changed, so every observer's slot 4
        // runs afterwards - the options window's rows are greyed / ungreyed at once (Deferred Lighting greys Specular, Static, Dynamic,
        // Shadow Distance, Max Simultaneous). A change of Deferred Lighting at quality level 6 also sets + 0xC6, a device reset - the
        // window is recreated in retail; OpenRasa switches its renderer in the next frame without one.
        video = true;
    } else {
        return;
    }
    if (videoRequest.mode && videoRequest.fullscreen) videoRequest.refreshRate = intOptionValue("Client.Graphics.FullScreenRefreshRate", 0);
    videoRequested = videoRequested || videoRequest.mode || videoRequest.refresh || videoRequest.vsync;
    if (video) {   // every observer's slot 4
        graphicsBeginEditing();
        syncUiToOptions();
    }
}

// Slot 2 of the observers: Audio (FUN_00534ac0 - its body is not read; the volumes are applied on every change, which matches the
// game: a volume slider changes the sound while it is dragged) and Graphics.
void voiceOptionChanged(const std::string& lowerName);
void renderBitsFromOption(const std::string& lowerName);
void antiAliasingOptionChanged(const std::string& lowerName);
// The UI observer (TRasa::OptionObserverUI, vtable 0x00be03d4, registered in GameClient::Initialize at 0x004d09b4) slot 2 (FUN_00532b30):
// a dict {the option's path (FUN_004efc00, case as stored): its value} handed to the scripts' clientmethod.SendOptionsChangedNotification
// (AddMethodCall on entity 5, delay 0, format "(O)"), which posts OPTIONS_CHANGED - how the script-side options (animation detail level,
// targeting, text filter, the windows ...) hear of live changes.
void notifyScriptsOptionChanged(const std::string& path, const std::string& value) {
    if (!Py_IsInitialized()) return;
    PyObject* dict = PyDict_New();
    PyObject* v = PyString_FromStringAndSize(value.data(), int(value.size()));
    if (!dict || !v || PyDict_SetItemString(dict, path.c_str(), v) != 0) {
        PyErr_Print();
        Py_XDECREF(v);
        Py_XDECREF(dict);
        return;
    }
    Py_DECREF(v);
    scheduleMethodCall(5, "SendOptionsChangedNotification", Py_BuildValue(const_cast<char*>("(N)"), dict), 0);
}
// OpenRasa FIX (user, 2026-11-01): characteranimationmgr.OnOptionsChanged is a module-level function, so its
// `CharacterAnimationMgr.__detailLevel = ...` is not name-mangled (the shipped bytecode: STORE_ATTR __detailLevel) while the class
// reads _CharacterAnimationMgr__detailLevel - in retail animation diversity changes only after a restart. After the scripts' own
// handler, the value is also set where the class reads it, computed as the handler does (int, x 0.01, clamped 0 .. 1; nothing for an
// empty value).
void fixAnimationDetailLevel(const std::string& value) {
    if (!Py_IsInitialized() || value.empty()) return;
    char* end = nullptr;
    const long v = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str()) return;
    const double level = std::clamp(double(v) * 0.01, 0.0, 1.0);
    PyObject* m = PyImport_ImportModule("client.characteranimationmgr");
    PyObject* cls = m ? PyObject_GetAttrString(m, "CharacterAnimationMgr") : nullptr;
    PyObject* f = cls ? PyFloat_FromDouble(level) : nullptr;
    if (!f || PyObject_SetAttrString(cls, "_CharacterAnimationMgr__detailLevel", f) != 0) PyErr_Print();
    Py_XDECREF(f);
    Py_XDECREF(cls);
    Py_XDECREF(m);
}
void optionChanged(const std::string& path, const std::string& value) {
    const std::string lowerName = asciiLower(path);
    notifyScriptsOptionChanged(path, value);
    if (lowerName == "client.graphics.animations.detaillevel") fixAnimationDetailLevel(value);
    if (lowerName.rfind("client.audio.", 0) == 0) configureAudio();
    if (lowerName.rfind("client.voice.", 0) == 0) voiceOptionChanged(lowerName);
    if (lowerName.rfind("client.graphics.", 0) == 0) {
        renderBitsFromOption(lowerName);
        antiAliasingOptionChanged(lowerName);
        graphicsOptionChanged(lowerName, value);
    }
}

// The options manager's constructor (0x0054a7c0, traced 2026-11-01) registers these with AddOption before client.cfg is read: the
// default and whether it is saved. Every reader then sees the exe's default for an option client.cfg lacks (e.g. MusicVolume 50,
// FloraDensity 50, TerrainQuality 50, RealTimeQualityScaling 15). Strings through FUN_0054b9c0 .. FUN_0054bbd0 (their defaults from
// FUN_0054bc50 "English", FUN_0054bd80 "Normal", FUN_0054beb0 "1024x768", FUN_004e5d30 "Off", FUN_0054bfe0 "High"), ints through
// FUN_00403a60 (the "usa" stream), bools through FUN_00403af0 ("True" / "False"). The .DisableUI options (optionswindow.py greys an
// option out while its one is True) are registered False and not saved.
void registerDefaultOptions() {
    struct Entry {
        const char* name;
        const char* value;
        bool persistent;
    };
    static const Entry entries[] = {
        {"Client.UserInterface.Language", "English", true},
        {"Client.Audio.Enabled", "True", true},
        {"Client.Audio.MasterVolume", "100", true},
        {"Client.Audio.MasterVolume.DisableUI", "False", false},
        {"Client.Audio.MusicVolume", "50", true},
        {"Client.Audio.MusicVolume.DisableUI", "False", false},
        {"Client.Audio.EffectVolume", "100", true},
        {"Client.Audio.EffectVolume.DisableUI", "False", false},
        {"Client.Audio.DetailLevel", "100", true},
        {"Client.Audio.DetailLevel.DisableUI", "False", false},
        {"Client.Audio.UpdatePeriodMilliseconds", "500", true},   // DAT_00bc5750
        {"Client.Voice.Enabled", "True", true},
        {"Client.Voice.VoiceVolume", "75", true},
        {"Client.Voice.VoiceVolume.DisableUI", "False", false},
        {"Client.Graphics.AutoConfigureID", "0", true},
        {"Client.Graphics.VideoQuality", "Normal", true},
        {"Client.Graphics.FullScreen", "True", true},
        {"Client.Graphics.FullScreenResolution", "1024x768", true},
        {"Client.Graphics.FullScreenResolution.DisableUI", "False", false},
        {"Client.Graphics.FullScreenRefreshRate", "60", true},
        {"Client.Graphics.FullScreenRefreshRate.DisableUI", "False", false},
        {"Client.Graphics.Gamma", "100", true},
        {"Client.Graphics.Gamma.DisableUI", "False", false},
        {"Client.Graphics.GammaGradient.DisableUI", "False", false},
        {"Client.Graphics.HardwareFSAA", "Off", true},
        {"Client.Graphics.HardwareFSAA.DisableUI", "False", false},
        {"Client.Graphics.TextureDetail", "High", true},
        {"Client.Graphics.TextureDetail.DisableUI", "False", false},
        {"Client.Graphics.Reflections", "False", true},
        {"Client.Graphics.Reflections.DisableUI", "False", false},
        {"Client.Graphics.Refractions", "True", true},
        {"Client.Graphics.Refractions.DisableUI", "False", false},
        {"Client.Graphics.Flora", "True", true},
        {"Client.Graphics.Flora.DisableUI", "False", false},
        {"Client.Graphics.FloraDensity", "50", true},
        {"Client.Graphics.FloraDensity.DisableUI", "False", false},
        {"Client.Graphics.ViewDistance", "100", true},
        {"Client.Graphics.ViewDistance.DisableUI", "False", false},
        {"Client.Graphics.TerrainQuality", "50", true},
        {"Client.Graphics.TerrainQuality.DisableUI", "False", false},
        {"Client.Graphics.DeferredLighting", "True", true},
        {"Client.Graphics.DeferredLighting.DisableUI", "False", false},
        {"Client.Graphics.SpecularLighting", "True", true},
        {"Client.Graphics.SpecularLighting.DisableUI", "False", false},
        {"Client.Graphics.StaticShadows", "True", true},
        {"Client.Graphics.StaticShadows.DisableUI", "False", false},
        {"Client.Graphics.DynamicShadows", "True", true},
        {"Client.Graphics.DynamicShadows.DisableUI", "False", false},
        {"Client.Graphics.FakeShadows", "True", true},
        {"Client.Graphics.FakeShadows.DisableUI", "False", false},
        {"Client.Graphics.ShadowDistance", "100", true},
        {"Client.Graphics.ShadowDistance.DisableUI", "False", false},
        {"Client.Graphics.ShadowMaxSimultaneous", "100", true},
        {"Client.Graphics.ShadowMaxSimultaneous.DisableUI", "False", false},
        {"Client.Graphics.RealTimeQualityScaling", "15", true},
        {"Client.Graphics.RealTimeQualityScaling.DisableUI", "False", false},
        {"Client.Graphics.TextureQuality", "High", true},
        {"Client.Graphics.TextureQuality.DisableUI", "False", false},
        {"Client.Graphics.GeometryQuality", "High", true},
        {"Client.Graphics.GeometryQuality.DisableUI", "False", false},
        {"Client.Graphics.AutoConfig", "True", false},
        {"Client.Graphics.AutoConfig.DisableUI", "False", false},
        {"Client.Graphics.Animations.DetailLevel", "100", true},
        {"Client.Graphics.Animations.DetailLevel.DisableUI", "False", false},
        {"Client.Graphics.EnableMeshCompression", "True", true},
        {"Client.Graphics.EnableMeshCompression.DisableUI", "False", false},
        {"Client.Graphics.EnableOcclusionQueries", "True", true},
        {"Client.Graphics.EnableOcclusionQueries.DisableUI", "False", false},
        {"Client.Graphics.ForceSharedBuffers", "False", true},
        {"Client.Graphics.ForceSharedBuffers.DisableUI", "False", false},
        {"Client.Graphics.PreloadMapGeometry", "False", true},
        {"Client.Graphics.PreloadMapGeometry.DisableUI", "False", false},
        {"Client.Graphics.VSync", "False", true},
        {"Client.Graphics.VSync.DisableUI", "False", false},
        {"Client.Graphics.ScreenShotAsBMP", "False", true},
        {"Client.Graphics.ScreenShotAsBMP.DisableUI", "False", false},
    };
    for (const Entry& e : entries) addOption(e.name, e.value, e.persistent);
}

// Reading client.cfg (FUN_00561480, the client serializer's import): an old file's [Client] Language makes
// Client.UserInterface.Language (AddOption "English", persistent, then SetOption with that item's value); the [Options] items are set
// with SetOption (new ones persistent) - only when [ConfigInfo] Version is 2 (strtol base 0).
void loadOptions() {
    loadingOptions = true;
    struct Done {
        ~Done() { loadingOptions = false; }
    } done;
    registerDefaultOptions();   // the options manager's constructor, before the import (FUN_004cd8c0)
    try {
        tr::ConfigFile cfg((std::filesystem::path(gameRoot) / "client.cfg").string());
        if (const auto* client = cfg.section("Client")) {
            for (const auto& item : *client)
                if (optionCompare(item.key, "Language") == 0) {
                    addOption("Client.UserInterface.Language", "English", true);
                    setOption("Client.UserInterface.Language", item.value);
                }
        }
        if (std::strtol(cfg.value("ConfigInfo", "Version", "").c_str(), nullptr, 0) == 2)
            if (const auto* items = cfg.section("Options"))
                for (const auto& item : *items) setOption(item.key.c_str(), item.value);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[engine] client.cfg: %s\n", e.what());
    }
    if (const std::string* q = optionValue("Client.Graphics.VideoQuality"); q && qualityLevelOf(*q) >= 0) videoQualityLevel = qualityLevelOf(*q);
    renderBitsFromOption("");
    antiAliasingOptionChanged("client.graphics.antialiasing");   // the observer's order: AntiAliasing, then HardwareFSAA
    antiAliasingOptionChanged("client.graphics.hardwarefsaa");
    // GameClient::Initialize (FUN_004cd8c0, 0x004cf963) reads it once into DAT_00d07ab1, True when missing; no observer.
    setMeshCompression(boolOptionValue("Client.Graphics.EnableMeshCompression", true));
}

// The command line's options (tabula_rasa.exe WinMain FUN_00402130 and FUN_004017b0, see CommandLine.h), in the original's order:
// /NoEULA, the auto-login values, Client.Info.IsPTS - each registered with AddOption, not persistent (so never written to
// client.cfg), then set (FUN_004039e0 string / FUN_00403a60 int / FUN_00403af0 bool AddOption, FUN_00403b90 SetOptionBool).
void applyCommandLineOptions(const CommandLine& cl, const std::string& launcherId) {
    auto boolOption = [](const char* name, bool value) {
        addOption(name, "False", false);
        setOption(name, value ? "True" : "False");
    };
    if (cl.has("/NoEULA")) boolOption("Client.EULA.Skip", true);
    std::string user, password, character;
    long server = -1;
    if (const std::string* file = cl.value("/autologin"); file && !file->empty()) {
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::u8path(*file), ec)) {
            try {
                tr::ConfigFile cfg(*file);
                user = cfg.value("AutoLogin", "User");
                password = cfg.value("AutoLogin", "Password");
                character = cfg.value("AutoLogin", "Character");
                const std::string s = cfg.value("AutoLogin", "Server");
                if (!s.empty()) server = std::strtol(s.c_str(), nullptr, 0);
            } catch (const std::exception&) {
                const std::string text = "Error loading autologin file '" + *file + "'";
                std::fprintf(stderr, "Tabula Rasa Error: %s\n", text.c_str());
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Tabula Rasa Error", text.c_str(), nullptr);
            }
        }
    }
    if (const std::string* v = cl.value("/user")) user = *v;
    if (const std::string* v = cl.value("/password")) password = *v;
    if (const std::string* v = cl.value("/character")) character = *v;
    if (const std::string* v = cl.value("/server")) server = std::atoi(v->c_str());
    addOption("Client.AutoLogin.Username", user, false);
    addOption("Client.AutoLogin.Password", password, false);
    addOption("Client.AutoLogin.Character", character, false);
    addOption("Client.AutoLogin.Server", groupedInt(server), false);
    if (!user.empty() || !password.empty() || server != -1) {
        boolOption("Client.Developer.SkipToLogin", true);
        boolOption("Client.EULA.Skip", true);
    }
    boolOption("Client.Info.IsPTS", launcherId == "TabulaRasaPTS");
}

// extern_gameclient_optionsmanager_wrap.OptionsManager (FUN_00493d50): the game client's one manager (GetOptionsManager).
struct OptionsManagerObject {
    PyObject_HEAD
};
PyTypeObject optionsManagerType = {PyObject_HEAD_INIT(nullptr)};
PyMethodDef optionsManagerMethods[] = {
    {const_cast<char*>("AddOptionString"), addOptionTyped<'s'>, METH_VARARGS, nullptr},
    {const_cast<char*>("AddOptionUnicode"), addOptionTyped<'u'>, METH_VARARGS, nullptr},
    {const_cast<char*>("AddOptionInt"), addOptionTyped<'i'>, METH_VARARGS, nullptr},
    {const_cast<char*>("AddOptionFloat"), addOptionTyped<'f'>, METH_VARARGS, nullptr},
    {const_cast<char*>("AddOptionBool"), addOptionTyped<'b'>, METH_VARARGS, nullptr},
    {const_cast<char*>("SetOptionString"), setOptionTyped<'s'>, METH_VARARGS, nullptr},
    {const_cast<char*>("SetOptionUnicode"), setOptionTyped<'u'>, METH_VARARGS, nullptr},
    {const_cast<char*>("SetOptionInt"), setOptionTyped<'i'>, METH_VARARGS, nullptr},
    {const_cast<char*>("SetOptionFloat"), setOptionTyped<'f'>, METH_VARARGS, nullptr},
    {const_cast<char*>("SetOptionBool"), setOptionTyped<'b'>, METH_VARARGS, nullptr},
    {const_cast<char*>("GetOptionString"), getOptionString, METH_VARARGS, nullptr},
    {const_cast<char*>("GetOptionUnicode"), getOptionUnicode, METH_VARARGS, nullptr},
    {const_cast<char*>("GetOptionInt"), getOptionInt, METH_VARARGS, nullptr},
    {const_cast<char*>("GetOptionFloat"), getOptionFloat, METH_VARARGS, nullptr},
    {const_cast<char*>("GetOptionBool"), getOptionBool, METH_VARARGS, nullptr},
    {const_cast<char*>("ForceOptionChangeEvent"), forceOptionChangeEvent, METH_VARARGS, nullptr},
    {const_cast<char*>("RestoreDefault"), restoreDefault, METH_VARARGS, nullptr},
    {const_cast<char*>("RemoveOption"), removeOption, METH_VARARGS, nullptr},
    // UpdateFromEngine (0x00495670) and BeginEditing (0x004956B0) both run every observer's slot 4 (0x0049df10).
    {const_cast<char*>("UpdateFromEngine"), beginEditing, METH_NOARGS, nullptr},
    {const_cast<char*>("BeginEditing"), beginEditing, METH_NOARGS, nullptr},
    // EndEditing (0x004956F0) and Export (0x00495730) both run slot 6 (0x0049df50): saving.
    {const_cast<char*>("EndEditing"), endEditing, METH_NOARGS, nullptr},
    {const_cast<char*>("Export"), endEditing, METH_NOARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};
PyObject* makeOptionsManager() { return reinterpret_cast<PyObject*>(PyObject_New(OptionsManagerObject, &optionsManagerType)); }
// The two wrapped modules' classes (neither can be made from Python).
void initOptionTypes() {
    optionType.tp_name = const_cast<char*>("extern_gameclient_option_wrap.Option");
    optionType.tp_basicsize = sizeof(OptionObject);
    optionType.tp_flags = Py_TPFLAGS_DEFAULT;
    optionType.tp_methods = optionMethods;
    optionType.tp_new = cannotInstantiate;
    optionsManagerType.tp_name = const_cast<char*>("extern_gameclient_optionsmanager_wrap.OptionsManager");
    optionsManagerType.tp_basicsize = sizeof(OptionsManagerObject);
    optionsManagerType.tp_flags = Py_TPFLAGS_DEFAULT;
    optionsManagerType.tp_methods = optionsManagerMethods;
    optionsManagerType.tp_new = cannotInstantiate;
    if (PyType_Ready(&optionType) < 0 || PyType_Ready(&optionsManagerType) < 0) PyErr_Print();
}

// ---- extern_trmain_trmain and the game client ---------------------------------------------------------------------------------
PyObject* gameClient = nullptr;
PyObject* optionsManager = nullptr;

PyObject* trueFn(PyObject*, PyObject*) { Py_RETURN_TRUE; }

PyObject* inputStateRouter = nullptr;   // what client.inputstaterouter.InitializeRouter() returned (the engine keeps it)

// TRasa::EntityManager (FUN_005366c0, made by the game client's set-up right after client.main.Initialize()): the scripts'
// client.entitymanager.AddMethodCall and RunMethods. Every call the engine makes on a script entity goes through AddMethodCall
// with (entity id as a long, method name, argument tuple, delay ms) (FUN_00536870, Py_BuildValue "(LsOi)"); GameClient::PreRender
// (FUN_004d2000) calls RunMethods once a frame (FUN_00536970), after the network update.
PyObject* addMethodCall = nullptr;
PyObject* runMethods = nullptr;

bool createEntityManager() {
    PyObject* m = PyImport_ImportModule(const_cast<char*>("client.entitymanager"));
    if (!m) {
        std::fprintf(stderr, "[engine] Failed to import \"client.entitymanager\"!\n");
        PyErr_Print();
        return false;
    }
    PyObject* add = PyObject_GetAttrString(m, const_cast<char*>("AddMethodCall"));
    PyObject* run = PyObject_GetAttrString(m, const_cast<char*>("RunMethods"));
    Py_DECREF(m);
    if (!add || !PyCallable_Check(add) || !run || !PyCallable_Check(run)) {
        PyErr_Clear();
        std::fprintf(stderr, "[engine] %s attribute missing or not callable.\n", add && PyCallable_Check(add) ? "RunMethods" : "AddMethodCall");
        Py_XDECREF(add);
        Py_XDECREF(run);
        return false;
    }
    Py_XDECREF(addMethodCall);
    Py_XDECREF(runMethods);
    addMethodCall = add;
    runMethods = run;
    return true;
}

// Schedules a method call on a script entity; steals `args`. False (and the Python error printed) when it could not be scheduled.
bool scheduleMethodCall(unsigned long long entityId, const char* method, PyObject* args, int delayMs) {
    if (!args) {
        PyErr_Print();
        return false;
    }
    if (!addMethodCall) {
        Py_DECREF(args);
        return false;
    }
    PyObject* call = Py_BuildValue(const_cast<char*>("(LsOi)"), (long long)entityId, method, args, delayMs);
    Py_DECREF(args);
    PyObject* r = call ? PyObject_CallObject(addMethodCall, call) : nullptr;
    Py_XDECREF(call);
    if (!r) {
        PyErr_Print();
        return false;
    }
    Py_DECREF(r);
    return true;
}

void runScheduledMethods() {
    if (!runMethods) return;
    PyObject* r = PyObject_CallObject(runMethods, nullptr);
    if (!r) PyErr_Print();
    Py_XDECREF(r);
}

// gameclient.InitializeMethodNameLookup(generated.client.methodid.idtoname) (FUN_004be6d0: the dict copied into the game client's
// id -> name map, + 0x31c): names the server's CallMethod messages by id (FUN_004d74f0), and the client's own calls by name
// (FUN_004dd760 in FUN_004d4700).
std::map<uint32_t, std::string> methodNames;
std::map<std::string, uint32_t> methodIds;

PyObject* initializeMethodNameLookup(PyObject*, PyObject* args) {
    PyObject* dict;
    if (!PyArg_ParseTuple(args, "O!", &PyDict_Type, &dict)) return nullptr;
    methodNames.clear();
    methodIds.clear();
    int pos = 0;
    PyObject *key, *value;
    while (PyDict_Next(dict, &pos, &key, &value)) {
        const long id = PyInt_AsLong(key);
        const char* name = PyString_AsString(value);
        if (PyErr_Occurred() || !name) return nullptr;
        methodNames[uint32_t(id)] = name;
        methodIds[name] = uint32_t(id);
    }
    Py_RETURN_NONE;
}

void registerModule(const char* name, PyObject* module);

// A module of integer constants, replacing its stub.
void constantsModule(const char* name, std::initializer_list<std::pair<const char*, long>> constants) {
    PyObject* modules = PyImport_GetModuleDict();
    PyDict_DelItemString(modules, const_cast<char*>(name));
    PyErr_Clear();
    PyObject* m = PyImport_AddModule(const_cast<char*>(name));   // borrowed
    for (const auto& [n, v] : constants) PyModule_AddIntConstant(m, const_cast<char*>(n), v);
}

// trmain.InitGameClient(): the game client's set-up as tabula_rasa.exe does it (FUN_004d4c10, traced 2026-10-24): load the UI,
// register uianchor and scenebodydimension, import client.main, InitializeRouter() of client.inputstaterouter (the router is kept
// by the engine), then client.main.Initialize() - every manager's Initialize. False (and the traceback printed) when one fails.
PyObject* initGameClient(PyObject*, PyObject*) {
    // trmain.InitGameClient is FUN_004cd8c0: after the engine's set-up (the UI manager and its root window among it) FUN_004d4c10
    // below (the script package, uievent, scenebodydimension, client.main, the input router, Initialize()), then the UI layout -
    // whose windows import their scripts from the package as they load - and a MovieComposite "MoviePlayer" on the root.
    ui::manager().createRoot(double(callbacks.screenWidth) / double(std::max(1, callbacks.screenHeight)) > 1.5);
    ui::manager().setScriptPackage("client.ui");
    if (!ui::pythonInit()) Py_RETURN_FALSE;   // FUN_0051f2f0: uievent, uianchor, CPPLookup (a failure fails InitGameClient)
    constantsModule("scenebodydimension", {{"eBirthing", -2}, {"eAll", -1}, {"eDefault", 0}});
    PyObject* mainModule = PyImport_ImportModule(const_cast<char*>("client.main"));
    if (!mainModule) { PyErr_Print(); Py_RETURN_FALSE; }
    PyObject* initialize = PyObject_GetAttrString(mainModule, const_cast<char*>("Initialize"));
    Py_DECREF(mainModule);
    if (!initialize) { PyErr_Print(); Py_RETURN_FALSE; }
    PyObject* routerModule = PyImport_ImportModule(const_cast<char*>("client.inputstaterouter"));
    PyObject* initRouter = routerModule ? PyObject_GetAttrString(routerModule, const_cast<char*>("InitializeRouter")) : nullptr;
    Py_XDECREF(routerModule);
    PyObject* router = initRouter ? PyObject_CallFunction(initRouter, nullptr) : nullptr;
    Py_XDECREF(initRouter);
    if (!router) { Py_DECREF(initialize); PyErr_Print(); Py_RETURN_FALSE; }
    Py_XDECREF(inputStateRouter);
    inputStateRouter = router;
    ui::setInputRouter(router);   // the input mapper calls the router's methods
    PyObject* r = PyObject_CallFunction(initialize, nullptr);
    Py_DECREF(initialize);
    if (!r) { PyErr_Print(); Py_RETURN_FALSE; }
    Py_DECREF(r);
    if (!createEntityManager()) Py_RETURN_FALSE;
    if (!ui::manager().loadLayout()) Py_RETURN_FALSE;
    autoConfigurePrompt();   // Initialize's AutoConfigureID question (the observers are registered by now)
    Py_RETURN_TRUE;
}

PyObject* getDataPaths(PyObject*, PyObject*) {
    // The [Data] zips of tabula_rasa.cfg in the order the client puts them on sys.path (trmain.GetDataPaths, FUN_00405480).
    PyObject* list = PyList_New(0);
    try {
        tr::AssetLibrary library(".");
        for (const std::string& p : library.gameDataPaths()) {
            PyObject* s = PyString_FromString(p.c_str());
            PyList_Append(list, s);
            Py_DECREF(s);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[engine] GetDataPaths: %s\n", e.what());
    }
    return list;
}

PyObject* getGameClient(PyObject*, PyObject*) {
    Py_INCREF(gameClient);
    return gameClient;
}


// ---- Video lists (optionswindow.py) -------------------------------------------------------------------------------------------------
// The display's modes as (width, height, refresh rate) in pixels, from SDL (the exe enumerates the adapter's D3D modes, one record per
// resolution with its refresh rates; FUN_004db5c0 / FUN_007c0770), ascending - D3D9's enumeration order, UNVERIFIED for SDL's set.
struct VideoMode {
    int width = 0, height = 0;
    std::vector<int> rates;
};
std::vector<VideoMode> videoModes() {
    std::vector<VideoMode> modes;
    int count = 0;
    SDL_DisplayMode** list = SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &count);
    if (!list) return modes;
    for (int i = 0; i < count; ++i) {
        const SDL_DisplayMode* m = list[i];
        const float density = m->pixel_density > 0 ? m->pixel_density : 1.0f;
        const int w = int(std::lround(m->w * density)), h = int(std::lround(m->h * density));
        const int rate = int(std::lround(m->refresh_rate));
        auto it = std::find_if(modes.begin(), modes.end(), [&](const VideoMode& v) { return v.width == w && v.height == h; });
        if (it == modes.end()) {
            modes.push_back({w, h, {}});
            it = modes.end() - 1;
        }
        if (rate > 0 && std::find(it->rates.begin(), it->rates.end(), rate) == it->rates.end()) it->rates.push_back(rate);
    }
    SDL_free(list);
    std::sort(modes.begin(), modes.end(), [](const VideoMode& a, const VideoMode& b) {
        return a.width != b.width ? a.width < b.width : a.height < b.height;
    });
    for (VideoMode& v : modes) std::sort(v.rates.begin(), v.rates.end());
    return modes;
}
PyObject* stringList(const std::vector<std::string>& items) {
    PyObject* list = PyList_New(0);
    for (const std::string& i : items) {
        PyObject* s = PyString_FromString(i.c_str());
        PyList_Append(list, s);
        Py_DECREF(s);
    }
    return list;
}
int validRefreshRate(int width, int height, int rate) {
    if (rate == 0) return 0;
    for (const VideoMode& v : videoModes())
        if (v.width == width && v.height == height && !v.rates.empty())
            return std::find(v.rates.begin(), v.rates.end(), rate) != v.rates.end() ? rate : v.rates.front();
    return rate;
}
// GetVideoResolutionList (FUN_004d3520): "%dx%d", or "%dx%d (Wide)" when width / height >= 1.5 (0x00c2b570).
std::string resolutionName(int w, int h) {
    char text[64];
    std::sprintf(text, double(w) / double(h) < 1.5 ? "%dx%d" : "%dx%d (Wide)", w, h);
    return text;
}
// ---- Video Quality presets and Auto Configure (traced 2026-10-25, tabula_rasa.exe) ---------------------------------------------------
// The level names (FUN_007bde40's table): 0 Low, 1 Normal, 2 High, 3 VeryHigh, 4 Ultra, 5 Maximum, 6 Custom ("Lowest" above 6).
// FUN_007bd900 reads an option value lower-cased: low / l, normal / n / medium / med, high / hi / h, veryhigh / vh / v, ultra / u,
// max / maximum / m, custom / c; any other value keeps the current level (the option is set back to its name).
const char* const kQualityNames[] = {"Low", "Normal", "High", "VeryHigh", "Ultra", "Maximum", "Custom"};
int qualityLevelOf(const std::string& value) {
    static const std::pair<const char*, int> names[] = {
        {"low", 0},  {"l", 0},       {"normal", 1}, {"n", 1},        {"medium", 1}, {"med", 1}, {"high", 2},
        {"hi", 2},   {"h", 2},       {"veryhigh", 3}, {"vh", 3},     {"v", 3},      {"ultra", 4}, {"u", 4},
        {"max", 5},  {"maximum", 5}, {"m", 5},      {"custom", 6},   {"c", 6}};
    const std::string v = lower(value);
    for (const auto& [name, level] : names)
        if (v == name) return level;
    return -1;
}
int videoQualityLevel = 6;   // render settings + 0xB8 (the level the settings were last set to); from client.cfg at load

// FUN_007bcab0 (set a level; nothing when it is the current one), with the settings written back as options the way slot 4
// (FUN_0053b400) reads them: GeometryQuality "Low" when bit 15 (0x8000) is set, TextureQuality the names Low / Medium / High of + 0x2C,
// the booleans the inverse of their "off" bits, the 0..1 settings x 100 (FloraDensity + 0x1C, ViewDistance + 0x24, TerrainQuality
// + 0x28, ShadowDistance + 0x30, ShadowMaxSimultaneous + 0x34, Animations.DetailLevel + 0xDC). Before the level's own settings every
// change of level sets AntiAliasing off (bit 6), FakeShadows on (bit 23 cleared), Reflections off (bit 17; not written back by slot 4)
// and clears bit 13. Per level (the jump table at 0x007BD878; floats 0x00BC4A80 1.0, 0x00BC7768 0.5, 0x00BC79B8 0.25, 0x00C2B900 0.8;
// ViewDistance from FUN_007bfb10: 0.2 (0x00BC5438) for Low, 1.0 above):
//              Geometry Texture Refract Deferred Specular Static Dynamic Fake Flora FloraDens View Terrain ShadowDist ShadowMax Anim
//   Maximum    High     High    on      on       on       on     on      on   on    100       100  100     100        100       100
//   Ultra      High     Medium  on      on       on       on     on      on   on    100       100  100     50         50        100
//   VeryHigh   High     Medium  on      on       on       on     off     on   on    80        100  50      50         25        100
//   High       High     Medium  on      on       off      off    off     on   on    80        100  0       0          0         100
//   Normal     High     Medium  on      off      (kept)   off    off     on   on    80        100  0       0          0         100
//   Low        Low      Low     off     off      (kept)   off    off     off  off   0         20   0       0          0         0
//   Custom     (only the changes made before the switch)
// (64BitLighting, bit 20, is switched off by every level but Maximum; the options window does not show it.) The engine first lowers
// the level while the device lacks what it needs (FUN_007bde90: video memory and capability fields of the settings, not known here -
// every level is taken as supported, UNVERIFIED).
void applyVideoQuality(int level) {
    if (level < 0 || level > 6 || level == videoQualityLevel) return;
    auto setBool = [](const char* name, bool on) { setOption(name, on ? "True" : "False"); };
    auto setPercent = [](const char* name, int v) { setOption(name, groupedInt(v)); };
    setBool("Client.Graphics.AntiAliasing", false);
    setBool("Client.Graphics.FakeShadows", true);
    struct Preset {
        const char *geometry, *texture;
        bool refractions, deferred;
        int specular;   // -1 kept
        bool staticShadows, dynamicShadows, fake, flora;
        int floraDensity, view, terrain, shadowDistance, shadowMax, anim;
    };
    static const Preset presets[6] = {
        {"Low", "Low", false, false, -1, false, false, false, false, 0, 20, 0, 0, 0, 0},
        {"High", "Medium", true, false, -1, false, false, true, true, 80, 100, 0, 0, 0, 100},
        {"High", "Medium", true, true, 0, false, false, true, true, 80, 100, 0, 0, 0, 100},
        {"High", "Medium", true, true, 1, true, false, true, true, 80, 100, 50, 50, 25, 100},
        {"High", "Medium", true, true, 1, true, true, true, true, 100, 100, 100, 50, 50, 100},
        {"High", "High", true, true, 1, true, true, true, true, 100, 100, 100, 100, 100, 100},
    };
    if (level < 6) {
        const Preset& q = presets[level];
        setOption("Client.Graphics.GeometryQuality", q.geometry);
        setOption("Client.Graphics.TextureQuality", q.texture);
        setBool("Client.Graphics.Refractions", q.refractions);
        setBool("Client.Graphics.DeferredLighting", q.deferred);
        if (q.specular >= 0) setBool("Client.Graphics.SpecularLighting", q.specular != 0);
        setBool("Client.Graphics.StaticShadows", q.staticShadows);
        setBool("Client.Graphics.DynamicShadows", q.dynamicShadows);
        setBool("Client.Graphics.FakeShadows", q.fake);
        setBool("Client.Graphics.Flora", q.flora);
        setPercent("Client.Graphics.FloraDensity", q.floraDensity);
        setPercent("Client.Graphics.ViewDistance", q.view);
        setPercent("Client.Graphics.TerrainQuality", q.terrain);
        setPercent("Client.Graphics.ShadowDistance", q.shadowDistance);
        setPercent("Client.Graphics.ShadowMaxSimultaneous", q.shadowMax);
        setPercent("Client.Graphics.Animations.DetailLevel", q.anim);
    }
    videoQualityLevel = level;
}

// The adapter's description (D3DADAPTER_IDENTIFIER9.Description in the game; here the GPU device's name), for Auto Configure.
std::string adapterDescription;

// Auto Configure (the observer's Client.Graphics.AutoConfig branch: FUN_007bff70, then VideoQuality set to the level's name - which
// applies the preset - and FUN_0053b1e0 the resolution).
// - The level (FUN_007c0090 on the upper-cased description; run unless the settings' + 0x44 is 1..640 or + 0x48 1..255 - video memory
//   and the like, not known here: taken as passing): start 1; NVIDIA: "FX" anywhere -> 0, else "GEFORCE %d" < 8600 -> 1, < 8700 -> 2,
//   < 9000 -> 4, < 9600 -> 3, 9600 GSO -> 3, other -> 4; ATI / RADEON / ALL-IN-WONDER: (+ 0x48 > 511), then by model ("RADEON X%d",
//   "RADEON %d", "RADEON HD %d"); a 4 becomes 5 when + 0x48 > 511 (never here). Cards the table does not know - every card made since
//   2009 ("GEFORCE RTX 2070" does not read as "GEFORCE %d") - keep the start value: Normal for NVIDIA and others, Low for AMD (+ 0x48
//   unknown). Then lowered while the device lacks the level (FUN_007bde90; taken as supported).
// - The resolution (FUN_007c0400): the current one when fullscreen, the level not Low and the display lists it; else the display's
//   first mode of at least 1024 x 768 pixels (0xC0000); 800 x 600 without one. Over 1280 x 1024 pixels (0x140000) with a card the table
//   flags the level is capped: 4 and above -> 3, 2..3 -> 1. Refresh rate 60 (FUN_007c0560 checks it against the mode).
// - Fullscreen: FullScreenResolution "WxH[ (Wide)]", .Width, .Height, FullScreenRefreshRate; windowed: WindowedResolution, .Width,
//   .Height (FUN_0053b1e0).
struct AutoConfigResult {
    int level, width, height, rate;
};
// FUN_007bff70(&level, &width, &height, &rate, fullscreen): the level and a resolution for that kind of window.
AutoConfigResult autoConfigurePick(bool fullscreen) {
    std::string d = adapterDescription;
    for (char& c : d) c = char(std::toupper(static_cast<unsigned char>(c)));
    int level = 1, n = 0;
    bool capWide = false;
    if (std::strstr(d.c_str(), "NVIDIA")) {
        const char* g = std::strstr(d.c_str(), "GEFORCE");
        if (std::strstr(d.c_str(), "FX")) {
            level = 0;
        } else if (g && std::sscanf(g, "GEFORCE %d", &n) == 1) {
            if (n < 8600) level = 1;
            else if (n < 8700) level = 2, capWide = true;
            else if (n < 9000) level = 4;
            else if (n < 9600) level = 3, capWide = true;
            else if (n == 9600 && std::strstr(g, "9600 GSO")) level = 3, capWide = true;
            else level = 4;
        }
    } else if (std::strstr(d.c_str(), "ATI") || std::strstr(d.c_str(), "RADEON") || std::strstr(d.c_str(), "ALL-IN-WONDER")) {
        level = 0;   // + 0x48 > 511, unknown here
        if (const char* r = std::strstr(d.c_str(), "RADEON")) {
            if (std::sscanf(r, "RADEON X%d", &n) == 1) {
                if (n < 1600) level = n > 799 ? 1 : 0;   // 0 whenever + 0xBC is set (not known)
                else if (n < 1800) level = 1;
                else level = n < 1900 ? 3 : 4, capWide = true;
            } else if (std::sscanf(r, "RADEON %d", &n) == 1) {
                level = n > 9699 ? 1 : 0;
            } else if (std::sscanf(r, "RADEON HD %d", &n) == 1) {
                if (n < 2600) level = 2, capWide = true;
                else if (n < 2900) level = 3, capWide = true;
                else if (n < 3000) level = 4;
                else if (n < 3600) level = 0;
                else if (n < 3850) level = 1;
                else if (n < 3870) level = 2, capWide = true;
                else if (n < 3880) level = 4, capWide = true;
                else if (n < 4000) level = 4;
                else if (n < 4850) level = 1;
                else if (n < 5000) level = 4;
            }
        }
    }
    const int curW = intOptionValue("Client.Graphics.FullScreenResolution.Width", 0);
    const int curH = intOptionValue("Client.Graphics.FullScreenResolution.Height", 0);
    int w = 800, h = 600;
    bool listed = false, found = false;
    for (const VideoMode& v : videoModes()) {
        if (static_cast<long long>(v.width) * v.height < 0xC0000) continue;
        if (!found) w = v.width, h = v.height, found = true;
        if (v.width == curW && v.height == curH) listed = true;
    }
    if (listed && fullscreen && level != 0) w = curW, h = curH;
    if (static_cast<long long>(w) * h > 0x140000 && capWide) level = level >= 4 ? 3 : (level > 1 ? 1 : level);
    const int rate = validRefreshRate(w, h, 60);
    std::printf("[options] auto configure: adapter \"%s\" -> %s, %dx%d\n", adapterDescription.c_str(), kQualityNames[level], w, h);
    std::fflush(stdout);
    return {level, w, h, rate};
}
// FUN_0053b1e0(width, height, rate, fullscreen, windowed): "WxH[ (Wide)]" with .Width / .Height to the fullscreen and / or windowed
// options, the refresh rate with the fullscreen ones.
void writeResolutionOptions(int w, int h, int rate, bool fullscreen, bool windowed) {
    const std::string rez = resolutionName(w, h);
    if (windowed) {
        setOption("Client.Graphics.WindowedResolution", rez);
        setOption("Client.Graphics.WindowedResolution.Width", groupedInt(w));
        setOption("Client.Graphics.WindowedResolution.Height", groupedInt(h));
    }
    if (fullscreen) {
        setOption("Client.Graphics.FullScreenResolution", rez);
        setOption("Client.Graphics.FullScreenResolution.Width", groupedInt(w));
        setOption("Client.Graphics.FullScreenResolution.Height", groupedInt(h));
        setOption("Client.Graphics.FullScreenRefreshRate", groupedInt(rate));
    }
}
void autoConfigure() {
    const bool fullscreen = boolOptionValue("Client.Graphics.FullScreen", false);
    const AutoConfigResult r = autoConfigurePick(fullscreen);
    setOption("Client.Graphics.VideoQuality", kQualityNames[r.level]);
    writeResolutionOptions(r.width, r.height, r.rate, fullscreen, !fullscreen);
}

// GameClient::Initialize (0x004d0a01 ..), after the observers are registered: unless Client.Graphics.AutoConfigureID is 3 (absent: 0),
// MessageBoxW(the window, playermessagelanguage PM_SYSTEM_REQUEST_VIDEO_AUTOCONFIGURE through GetLanguageString (FUN_0051fa00), caption
// L"", MB_YESNOCANCEL | MB_ICONQUESTION). Yes: FUN_007bff70 for fullscreen (from 1024 x 768, rate 60), VisualQuality::SetMode(level),
// the fullscreen resolution and rate (FUN_0053b1e0(.., 1, 0)); FUN_007bff70 again for windowed, its resolution (FUN_0053b1e0(.., 0, 1));
// PalantirApp::SetResolution; UpdateFromEngine. Yes or No: FUN_004cd0a0(yes, the old ID) - FullScreenValidateSettings True on Yes,
// ForceSharedBuffers False when the old ID was 1 -, AutoConfigureID 3, Export (client.cfg saved). Cancel: nothing (asked again next
// time). The buttons are SDL's (Windows' own Yes / No / Cancel labels and question icon are not reproduced); GetLanguageString's last
// argument (GameClient + 0x22c) is not traced - 0 is passed, as for widget texts.
void autoConfigurePrompt() {
    const int id = intOptionValue("Client.Graphics.AutoConfigureID", 0);
    if (id == 3) return;
    std::string text = "PM_SYSTEM_REQUEST_VIDEO_AUTOCONFIGURE";
    if (PyObject* lookup = ui::cppLookupObject()) {
        PyObject* v = PyObject_CallFunction(lookup, const_cast<char*>("ssi"), "playermessagelanguage",
                                            "playermessage.PM_SYSTEM_REQUEST_VIDEO_AUTOCONFIGURE", 0);
        if (v && PyUnicode_Check(v)) {
            if (PyObject* u = PyUnicode_AsUTF8String(v)) {
                text = PyString_AsString(u);
                Py_DECREF(u);
            }
        }
        if (!v) PyErr_Clear();
        Py_XDECREF(v);
    }
    const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 6, "Yes"}, {0, 7, "No"},
                                                {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Cancel"}};
    const SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION, player::gameWindow(), "", text.c_str(), 3, buttons, nullptr};
    int answer = 2;
    if (!SDL_ShowMessageBox(&box, &answer)) answer = 2;
    if (answer != 6 && answer != 7) return;
    if (answer == 6) {
        const AutoConfigResult full = autoConfigurePick(true);
        setOption("Client.Graphics.VideoQuality", kQualityNames[full.level]);
        writeResolutionOptions(full.width, full.height, full.rate, true, false);
        const AutoConfigResult windowed = autoConfigurePick(false);
        writeResolutionOptions(windowed.width, windowed.height, full.rate, false, true);
        graphicsOptionChanged("client.graphics.fullscreen", "");   // PalantirApp::SetResolution: the window takes the new mode
        graphicsBeginEditing();                                     // UpdateFromEngine
        setOption("Client.Graphics.FullScreenValidateSettings", "True");
    }
    if (id == 1) setOption("Client.Graphics.ForceSharedBuffers", "False");
    setOption("Client.Graphics.AutoConfigureID", "3");
    Py_XDECREF(endEditing(nullptr, nullptr));   // Export: the Serialize observer saves
}

PyObject* getVideoResolutionList(PyObject*, PyObject*) {
    std::vector<std::string> names;
    for (const VideoMode& v : videoModes()) names.push_back(resolutionName(v.width, v.height));
    return stringList(names);
}
// GetVideoRefreshRateList(rez) (FUN_004d3700): sscanf("%dx%d") of the name, that mode's refresh rates as "%d" (none: an empty list).
PyObject* getVideoRefreshRateList(PyObject*, PyObject* args) {
    const char* rez;
    if (!PyArg_ParseTuple(args, "s", &rez)) return nullptr;
    std::vector<std::string> names;
    int w = 0, h = 0;
    if (std::sscanf(rez, "%dx%d", &w, &h) == 2)
        for (const VideoMode& v : videoModes())
            if (v.width == w && v.height == h)
                for (int r : v.rates) names.push_back(std::to_string(r));
    return stringList(names);
}
// GetFSAAList(fullscreen) (FUN_004d3850, traced 2026-11-01): with deferred lighting (render settings bit 0x80000 clear) the supersampling
// factors 1, 1.5, 2, 3, 4 (FUN_007c0a20; floats at 0x00bc4a80 / 0x00c2b500 / 0x00c2b5ac / 0x00c2b564 / 0x00bc5434), only when render
// settings + 0xBE is set and stopping at the first whose trunc(width x f) or trunc(height x f) exceeds + 0xD0 / + 0xD4. FUN_007be620
// fills those from the device's D3DCAPS9 (copied at gfxDevice + 0x14, so + 0x100 = DeclTypes as the mesh compression reads it): + 0xBE =
// DevCaps2 D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES (0x10) and StretchRectFilterCaps D3DPTFILTERCAPS_MAGFLINEAR (0x200) - the linear
// StretchRect the supersampled view is copied down with -, + 0xD0 / + 0xD4 = MaxTextureWidth / MaxTextureHeight. No factor -> "Off" alone
// (1.0). Named "Off" (f <= 1), "%1.0fX (Emulated)" (whole), "%1.1fX (Emulated)" (FUN_007c0ce0). Without deferred lighting the forward
// list (FUN_007c0830): gfxAdapter::ChooseMode for the windowed / fullscreen flag (the script's argument) and 1024 x 768 picks a display
// mode and depth format of the adapter's enumeration, and its multisample list is copied; gfxAdapter::FillModeInfo built that list per
// (mode, depth format) by CheckDeviceMultiSampleType for types 0 and 2 .. 16 (1 = NONMASKABLE skipped) on the colour AND the depth
// format, kept when both succeed with at least one quality level; empty -> 0. Named "%dX" for 1 .. 16, "Off" otherwise (FUN_007c0c50).
// Here: SDL GPU's counts 2 / 4 / 8 that the device supports for the forward pass's formats (Renderer::supportedSampleCounts; SDL GPU has
// no other counts) and, as no SDL GPU query gives it, 16384 for the maximum texture size (checked 2026-11-01 on the user's RTX 2070 SUPER: its
// D3D9 caps give MaxTextureWidth / Height 16384, DevCaps2 0x51 and StretchRectFilterCaps 0x03000300 - both bits the list needs set).
// The size is the screen's.
std::vector<uint32_t> deviceSampleCounts = {2, 4, 8};   // set by main.cpp from the device (setSupportedSampleCounts)
std::vector<float> fsaaFactors() {
    int w = 1920, h = 1080;
    if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay())) {
        w = int(std::lround(m->w * (m->pixel_density > 0 ? m->pixel_density : 1.0f)));
        h = int(std::lround(m->h * (m->pixel_density > 0 ? m->pixel_density : 1.0f)));
    }
    const int maxSize = 16384;
    std::vector<float> factors;
    for (float f : {1.0f, 1.5f, 2.0f, 3.0f, 4.0f}) {
        if (int(float(w) * f) > maxSize || int(float(h) * f) > maxSize) break;
        factors.push_back(f);
    }
    return factors;
}
PyObject* getFSAAList(PyObject*, PyObject* args) {
    PyObject* fullscreen = nullptr;
    if (!PyArg_ParseTuple(args, "|O", &fullscreen)) return nullptr;
    std::vector<std::string> names;
    const std::string* deferred = optionValue("Client.Graphics.DeferredLighting");
    if (!deferred || lower(*deferred) != "false") {
        for (float f : fsaaFactors()) {
            char text[64];
            if (f <= 1.0f) std::sprintf(text, "Off");
            else if (std::fmod(f, 1.0f) <= 0.001f) std::sprintf(text, "%1.0fX (Emulated)", double(f));
            else std::sprintf(text, "%1.1fX (Emulated)", double(f));
            names.push_back(text);
        }
        if (names.empty()) names.push_back("Off");
    } else {
        names = {"Off"};
        for (uint32_t n : deviceSampleCounts) names.push_back(std::to_string(n) + "X");
    }
    return stringList(names);
}

// gameclient.Time(): seconds as a float (the scripts multiply it by 1000 for milliseconds and add seconds to it for timers -
// messageboxtimer.py, actor.py). The engine's function is NOT traced (the API reference's 0x00B23E10 is an exit-time destructor), so
// its epoch is unknown; here the seconds since OpenRasa started.
PyObject* gameTime(PyObject*, PyObject*) { return PyFloat_FromDouble(double(SDL_GetTicksNS()) / 1e9); }

// PreRender's frame-rate average (FUN_004d2000): every call the milliseconds since the last TIMED update (+ 700, moved on only when
// more than 9 ms have passed), 33 when negative and 1000 when above 1000, go into a ring of 64 (+ 0x334, slot + 0x434, running sum
// + 0x438); the mean (sum >> 6, + 0x330) gives the frame rate 1000 / mean (+ 0x32c; 30 while the mean is 0). GetFramerate (0x004d9480)
// returns + 0x32c.
uint32_t clientClock = 0;
// Whether this frame's PreRender ran its timed step (more than 9 ms since the last). The special-FX products update there, and an SCRF
// screen filter is submitted to the PostEffect phase by its product's update (FUN_0077b330 -> the phase's slot 0x24) - so it is drawn only on
// those frames: at high frame rates every other frame, which is the stun's grey / colour flicker (tabula_rasa.16.trace: the filter on
// alternate frames, now and then two colour frames in a row).
bool timedStepThisFrame = true;
uint32_t timedStepMs = 0;               // the timed step's elapsed milliseconds (at most 1000)
struct FrameRate {
    uint64_t lastTimed = 0;
    uint32_t ring[64] = {};
    uint32_t slot = 0, sum = 0, mean = 0;
    float fps = 30.0f;
} frameRate;
void stepFrameRate() {
    FrameRate& f = frameRate;
    const uint64_t now = SDL_GetTicks();
    if (f.lastTimed == 0) f.lastTimed = now;
    const int64_t elapsed = int64_t(now - f.lastTimed);
    uint32_t ms = elapsed < 0 ? 33u : uint32_t(elapsed);
    if (ms > 1000) ms = 1000;
    f.sum += ms - f.ring[f.slot];
    f.ring[f.slot] = ms;
    f.slot = (f.slot + 1) & 0x3f;
    f.mean = f.sum >> 6;
    f.fps = f.mean == 0 ? 30.0f : float(1000.0 / double(f.mean));
    if (ms > 9) f.lastTimed = now;
    timedStepThisFrame = ms > 9;
    timedStepMs = ms;
    // + 0x2c0: the milliseconds since the client started (+ 0x2bc now - + 0x2b8 start), at each timed update (FUN_004d2000).
    static const uint64_t start = now;
    if (ms > 9) clientClock = uint32_t(now - start);
    if (ms > 9) world::clearLineOfSightCache();   // PreRender's timed step ends by clearing the walkable-surface LOS cache (0x004d237c)
}
PyObject* getFramerate(PyObject*, PyObject*) { return PyFloat_FromDouble(double(frameRate.fps)); }

// gameclient.GetPerformanceData() (FUN_004d94a0): (memory load, used virtual memory in MB, the RQS degradation) - FUN_004d7420's
// GlobalMemoryStatusEx dwMemoryLoad and (ullTotalVirtual - ullAvailVirtual) >> 20, and TRRQS's slot 4 (render settings + 0xe0).
// The exe's "Memory Usage" is the process's used virtual ADDRESS SPACE: in a 32-bit process that is a fair measure of its memory
// (about 1 GB in game), but a 64-bit process has 128 TB of address space and its drivers, runtimes and thread stacks reserve
// gigabytes they never touch (OpenRasa showed 8.6 GB there against 1.4 GB actually committed). So OpenRasa reports the committed
// private bytes (PROCESS_MEMORY_COUNTERS_EX PrivateUsage) - what the 32-bit figure measures in practice - a deliberate deviation.
// The third value is how far the real-time quality scaling has degraded, 0 .. 100 (engine/Rqs.h). Other systems: /proc (Linux), else 0.
PyObject* getPerformanceData(PyObject*, PyObject*) {
    unsigned long memoryLoad = 0, memoryUsage = 0;
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) memoryLoad = status.dwMemoryLoad;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        memoryUsage = (unsigned long)(pmc.PrivateUsage >> 20);
#elif defined(__linux__)
    if (FILE* f = std::fopen("/proc/meminfo", "r")) {
        unsigned long long total = 0, available = 0, v = 0;
        char key[64];
        while (std::fscanf(f, "%63s %llu kB", key, &v) == 2) {
            if (std::strcmp(key, "MemTotal:") == 0) total = v;
            else if (std::strcmp(key, "MemAvailable:") == 0) available = v;
        }
        std::fclose(f);
        if (total) memoryLoad = (unsigned long)((total - available) * 100 / total);
    }
    if (FILE* f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        unsigned long long kb = 0;
        while (std::fgets(line, sizeof line, f))
            if (std::sscanf(line, "VmSize: %llu kB", &kb) == 1) memoryUsage = (unsigned long)(kb >> 10);
        std::fclose(f);
    }
#endif
    return Py_BuildValue(const_cast<char*>("(kkd)"), memoryLoad, memoryUsage, double(rqs::percent()));
}

// gameclient.GetNetworkPing() (0x004d9560): GameClient + 0x444, written by FUN_004d73b0 - per reply to the client's timing message
// (net message type 8, FUN_00529870: the clock at + 0x2c0 less the message's time, less half the mean frame time when larger), the
// least of every 15, then published. 0 from the constructor (FUN_004cd8c0). OpenRasa does not send or answer that message yet, so it
// stays 0 (the tracker shows " ms"). SetNetworkResponseTime (0x004d9570) is an empty function in the exe (RET 4).
// RequestNetworkStats (0x004D7470) sets + 0x448; the next PreRender with a game connection (FUN_004d2000 at 0x004d2210) sends a
// ClientMsg_Ping (FUN_0052a150: type 8, no subtype, the clock + 0x2c0 as a uint; Rasa.NET PingMessage, which the server echoes) and
// clears it. A reply (OnReceiveMessage FUN_00529870 -> FUN_004d73b0) gives rtt = clock - its time, less half the mean frame time
// (+ 0x330) when larger, kept as + 0x44c; + 0x440 holds the least of the batch, and every 15th reply publishes it as + 0x444 (the
// ping) and restarts the batch at 0x7fffffff. inputstate/game.py asks every second.
struct Ping {
    bool requested = false;
    uint32_t least = 0x7fffffff, last = 0, published = 0;
    int32_t count = 0;
} ping;
PyObject* requestNetworkStats(PyObject*, PyObject*) {
    ping.requested = true;
    Py_RETURN_NONE;
}
PyObject* getNetworkPing(PyObject*, PyObject*) { return PyInt_FromLong(long(ping.published)); }
PyObject* setNetworkResponseTime(PyObject*, PyObject*) { Py_RETURN_NONE; }

// ---- The render settings' flag words (DAT_00d516d8 + 0 / + 4) --------------------------------------------------------------------
// A set bit = a feature off or a debug view on. Written by the Graphics option observer (FUN_00539b50: bit 0 Flora, 21 DynamicShadows,
// 22 StaticShadows, 24 SpecularLighting, each set when the option is off; bit 19 = deferred lighting off, 0x80000), by
// gameclient.CallDebugFunction (FUN_007be010) and ParseDLCommand (FUN_004d6580), and by ToggleWireframe (bit 13). A write raises the
// settings' change count (+ 0x14) and the settings are applied (FUN_007bca40) when it drops back to 0 - here they are read each frame.
// Bits CallDebugFunction may not change unless the quality level (+ 0xB8) is Custom (6): 15 .. 25.
uint32_t renderBits[2] = {0, 0};
bool renderBit(uint32_t index) { return index < 64 && (renderBits[index >> 5] >> (index & 31) & 1u) != 0; }
void setRenderBit(uint32_t index, bool on) {
    if (index >= 64) return;
    if (on) renderBits[index >> 5] |= 1u << (index & 31);
    else renderBits[index >> 5] &= ~(1u << (index & 31));
}
// Anti-aliasing (the Graphics observer FUN_00539b50, traced 2026-11-01). Client.Graphics.AntiAliasing = the SOFTWARE anti-aliasing:
// on clears render bit 6 and resets the supersampling factor (render settings + 0x38) to 1 and the device's multisample type (+ 0x2f0)
// to 0; off sets bit 6 (both only on a change of the bit). Client.Graphics.HardwareFSAA: with deferred lighting (bit 19 clear) a
// supersampling factor read with "%f" ("2X (Emulated)" = 2; 1 when unreadable) and kept only when it is one GetFSAAList offers
// (FUN_007c0b90; else 1), stored at + 0x38; without it a multisample count read with "%d" (+ 0x2f0) and a device reset; a factor > 1 /
// count > 0 sets bit 6 (the software pass off). So the two are exclusive. The per-frame view set-up (FUN_007be550) runs the software
// pass (PostRender_AntiAlias, FUN_00803340) while bit 6 is clear and the RQS flag "Do AntiAliasing" (+ 0x16a, on at full quality) is
// set; a debug view takes the same post-render slot after it. The supersampling (FUN_00801b80, deferred only): the view is rendered at
// clamp(sqrt(factor), 1, 2) per axis and the PostEffect phase's DeferredSuperSamplingPostEffect (vtable 0x00c0374c, FUN_007ff350 ->
// FUN_0059c300) copies it to the back buffer with StretchRect(..., D3DTEXF_LINEAR). The multisample count multisamples OpenRasa's
// forward colour pass (Renderer::setMultisample; the count the device supports up to it).
float superSamplingFactor = 1.0f;   // render settings + 0x38
int multiSampleCount = 0;           // device + 0x2f0
bool renderBit(uint32_t index);
void setRenderBit(uint32_t index, bool on);
std::vector<float> fsaaFactors();
void antiAliasingOptionChanged(const std::string& lowerName) {
    if (lowerName == "client.graphics.antialiasing") {
        const bool on = boolOptionValue("Client.Graphics.AntiAliasing", false);
        if (renderBit(6) == on) {
            if (on) {
                superSamplingFactor = 1.0f;
                multiSampleCount = 0;
            }
            setRenderBit(6, !on);
        }
    } else if (lowerName == "client.graphics.hardwarefsaa") {
        const std::string* v = optionValue("Client.Graphics.HardwareFSAA");
        const std::string text = v ? *v : std::string();
        if (!renderBit(19)) {
            float f = 1.0f;
            std::sscanf(text.c_str(), "%f", &f);
            bool offered = false;
            for (float k : fsaaFactors())
                if (k == f) offered = true;
            if (!offered) f = 1.0f;
            if (f > 1.0f && !renderBit(6)) setRenderBit(6, true);
            superSamplingFactor = f;
        } else {
            int n = 0;
            std::sscanf(text.c_str(), "%d", &n);
            if (n > 0 && !renderBit(6)) setRenderBit(6, true);
            multiSampleCount = n;
        }
    }
}

// The option -> bit part of the Graphics observer. Deferred lighting (bit 19) and the other names' bits are taken from the debug
// function names (FUN_004d8a40): Reflections 17, Refractions 18, DeferredLighting 19, FakeShadows 23 (UNVERIFIED that the observer
// writes those four; Flora / shadows / specular are traced).
void renderBitsFromOption(const std::string& lowerName) {
    struct Map {
        const char* name;
        uint32_t bit;
        bool fallback;
    };
    static const Map map[] = {{"client.graphics.flora", 0, true},           {"client.graphics.reflections", 17, true},
                              {"client.graphics.refractions", 18, true},    {"client.graphics.deferredlighting", 19, true},
                              {"client.graphics.dynamicshadows", 21, true}, {"client.graphics.staticshadows", 22, true},
                              {"client.graphics.fakeshadows", 23, true},    {"client.graphics.specularlighting", 24, true}};
    for (const Map& m : map)
        if (lowerName.empty() || lowerName == m.name) {
            std::string option = m.name;
            // the stored name's case does not matter to optionValue (the tree compares with _stricmp)
            setRenderBit(m.bit, !boolOptionValue(option.c_str(), m.fallback));
        }
    // FUN_00539b50: Client.Graphics.64BitLighting (FUN_0049dfb0: True / Yes / On / 1) sets render bit 20 - only at quality level 6
    // (Custom), and with it + 0xC6 (a device reset). Bit 20 picks the light buffer's 64-bit format; OpenRasa lights in one composite pass
    // with no separate light buffer, so it changes nothing drawn here.
    if ((lowerName.empty() || lowerName == "client.graphics.64bitlighting") && videoQualityLevel == 6)
        if (const std::string* v = optionValue("Client.Graphics.64BitLighting")) setRenderBit(20, optionBoolText(*v));
}

// ---- gameclient's debug functions (GetDebugSetting 0x004d9290 / CallDebugFunction 0x004d9070) ---------------------------------------
// FUN_004d8a40 fills three std::maps (name -> bit) once: the scene's (app + 0x240 -> + 0x10; bits at + 0x2dc), the terrain's (GameClient
// + 0x288; FUN_00739760 / FUN_007397a0) and the render settings'. GetDebugSetting(name) reads the bit; three more names are the game
// client's own: "Display Diagnostics" (nothing), "Bone Tracking" (the bone tracker manager's + 4, GameClient + 0x274) and "Track Memory
// Usage" (DAT_00d44afe); anything else OutputDebugString "Unrecognized debug function passed into GetDebugSetting" and False.
// CallDebugFunction(name, flag) acts only while the GM flag (+ 0x329) is set: the scene's / the terrain's bit, the render settings' bit
// (FUN_007be010: no change when it already has that value; bits 15 .. 25 need quality level 6 - the function first switches to 6,
// FUN_007bcab0, and bits 15 .. 25 are then allowed), "Bone Tracking" = the flag, "Track Memory Usage" nothing. OpenRasa draws no BSP / PVS /
// occlusion debug geometry: the scene's bits are kept, nothing more.
const std::map<std::string, uint32_t>& sceneDebugNames() {
    static const std::map<std::string, uint32_t> m = {{"Use PVS", 0},          {"Freeze PVS", 5},
                                                      {"Render BSP Nodes", 1},  {"Render Occluded BSP Nodes", 2},
                                                      {"Render Occlusion Queries", 6}, {"Disable Occlusion Queries", 7}};
    return m;
}
const std::map<std::string, uint32_t>& terrainDebugNames() {
    static const std::map<std::string, uint32_t> m = {{"Hide Terrain", 2}};
    return m;
}
const std::map<std::string, uint32_t>& renderDebugNames() {
    static const std::map<std::string, uint32_t> m = {
        {"Disable FSAA", 6},          {"Enable 64bit Lighting", 0x14},   {"Disable Flora", 0},
        {"Hide Actors", 1},           {"Hide Non-Actors", 2},           {"Hide VFX", 3},
        {"Disable Specular", 0x18},   {"Disable Reflections", 0x11},    {"Disable Refractions", 0x12},
        {"Disable Deferred Lighting", 0x13}, {"Disable Dynamic Shadows", 0x15}, {"Disable Fake Shadows", 0x17},
        {"Disable Static Shadows", 0x16}, {"Disable Dynamic PVS", 4},   {"Disable Object Occlusion", 5},
        {"Disable LODs", 7},          {"Disable Animation Throttling", 8}, {"Disable Animation Updates", 9},
        {"Disable Particles", 10},    {"Disable Relevance", 0xb},        {"Low Geometry Quality", 0xf},
        {"Disable Real-time Scaling", 0xc}};
    return m;
}
uint64_t sceneDebugBits = 0, terrainDebugBits = 0;
bool gmEnabled = false;          // GameClient + 0x329 (SetGMEnabled / IsGMEnabled; clientmethod.Recv_SetIsGM)
bool trackMemoryUsage = false;   // DAT_00d44afe
#ifdef _WIN32
void debugString(const char* text) { OutputDebugStringA(text); }
#else
void debugString(const char*) {}
#endif
PyObject* getDebugSetting(PyObject*, PyObject* args) {
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "s:GetDebugSetting", &name)) return nullptr;
    if (const auto it = sceneDebugNames().find(name); it != sceneDebugNames().end()) return PyBool_FromLong((sceneDebugBits >> it->second) & 1);
    if (const auto it = terrainDebugNames().find(name); it != terrainDebugNames().end())
        return PyBool_FromLong((terrainDebugBits >> it->second) & 1);
    if (const auto it = renderDebugNames().find(name); it != renderDebugNames().end()) return PyBool_FromLong(renderBit(it->second));
    if (optionCompare(name, "Display Diagnostics") == 0) Py_RETURN_FALSE;
    if (optionCompare(name, "Bone Tracking") == 0) return PyBool_FromLong(boneTrackerManagerEnabled());
    if (optionCompare(name, "Track Memory Usage") == 0) return PyBool_FromLong(trackMemoryUsage);
    debugString("Unrecognized debug function passed into GetDebugSetting");
    Py_RETURN_FALSE;
}
PyObject* callDebugFunction(PyObject*, PyObject* args) {
    const char* name = nullptr;
    PyObject* flagObject = nullptr;
    if (!PyArg_ParseTuple(args, "sO:CallDebugFunction", &name, &flagObject)) return nullptr;
    const bool flag = PyObject_IsTrue(flagObject) == 1;
    if (!gmEnabled) Py_RETURN_NONE;
    auto setBit = [&](uint64_t& bits, uint32_t i) { bits = flag ? bits | (1ull << i) : bits & ~(1ull << i); };
    if (const auto it = sceneDebugNames().find(name); it != sceneDebugNames().end()) {
        setBit(sceneDebugBits, it->second);
    } else if (const auto it2 = terrainDebugNames().find(name); it2 != terrainDebugNames().end()) {
        setBit(terrainDebugBits, it2->second);
    } else if (const auto it3 = renderDebugNames().find(name); it3 != renderDebugNames().end()) {
        const uint32_t i = it3->second;
        if (i > 0xe && i < 0x1a && videoQualityLevel != 6) videoQualityLevel = 6;   // FUN_007bcab0(6): the quality level Custom
        setRenderBit(i, flag);
    } else if (optionCompare(name, "Display Diagnostics") == 0) {
    } else if (optionCompare(name, "Bone Tracking") == 0) {
        setBoneTrackerManagerEnabled(flag);
    } else if (optionCompare(name, "Track Memory Usage") != 0) {
        debugString("Unrecognized debug function passed into CallDebugFunction");
    }
    Py_RETURN_NONE;
}
PyObject* setGMEnabled(PyObject*, PyObject* args) {   // 0x004d6220: + 0x329 = the byte
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, "O:SetGMEnabled", &o)) return nullptr;
    gmEnabled = PyObject_IsTrue(o) == 1;
    Py_RETURN_NONE;
}
PyObject* isGMEnabled(PyObject*, PyObject*) { return PyBool_FromLong(gmEnabled); }   // 0x004d6230
// EnableBoneTracking(flag) (0x004d4fb0): the bone tracker manager's + 4 (GameClient + 0x274); its update (FUN_00538420) runs only while
// it is set - the trackers keep their angles meanwhile.
PyObject* enableBoneTrackingGlobal(PyObject*, PyObject* args) {
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, "O:EnableBoneTracking", &o)) return nullptr;
    setBoneTrackerManagerEnabled(PyObject_IsTrue(o) == 1);
    Py_RETURN_NONE;
}

// ParseDLCommand(command, value) (FUN_004d6580; communicator.py passes "/dl_<name>" without the "/" - the handler skips 3 characters,
// "dl_"): first every debug view bit is cleared (word 0 bits 28 - 31, word 1 bits 0 - 5), then the command sets one: shownormals 29,
// showdepth 30, showdiffuse 28, showspecpower 31, showspecintensity w1 0, showglow w1 1, showedges w1 2, showdifflight w1 3, showspeclight
// w1 4, showlight w1 5; showresult only clears. Toggles: hqt (bit 16), overdraw (w1 6), overdrawexpert (w1 7), complexity (w1 8),
// fsaa (6); on / off clear / set bit 19 (deferred lighting), shadowon / off bit 21, fakeshadowon / off 23, specon / off 24, fsaaon / off
// 6 - on, off, hqt and the shadow / spec ones only at quality level 6; "32" / "64" set render bit 20 (64-bit lighting) off / on; "mip<n>"
// FUN_006a2340 (a texture mip bias - NOT traced); flush FUN_007ff850 (NOT traced); anything else FUN_007bd900 (a settings lookup by name
// - NOT traced). The views draw only on the deferred path (render mode 5). OpenRasa shows normals, diffuse, specular power / intensity
// and the light buffer (showlight and showdifflight alike); depth, glow, edges, specular light, overdraw and complexity are not drawn.
PyObject* parseDLCommand(PyObject*, PyObject* args) {
    const char* command = nullptr;
    PyObject* value = nullptr;
    if (!PyArg_ParseTuple(args, "s|O:ParseDLCommand", &command, &value)) return nullptr;
    const char* c = std::strlen(command) >= 3 ? command + 3 : "";
    renderBits[0] &= 0x0fffffffu;
    renderBits[1] &= ~0x3fu;
    auto set = [](uint32_t word, uint32_t bit) { renderBits[word] |= bit; };
    auto toggle = [](uint32_t word, uint32_t bit) { renderBits[word] ^= bit; };
    const bool custom = videoQualityLevel == 6;
    if (optionCompare(c, "on") == 0) {
        if (custom) renderBits[0] &= ~0x80000u;
    } else if (optionCompare(c, "off") == 0) {
        if (custom) set(0, 0x80000u);
    } else if (optionCompare(c, "shownormals") == 0) {
        set(0, 0x20000000u);
    } else if (optionCompare(c, "showdepth") == 0) {
        set(0, 0x40000000u);
    } else if (optionCompare(c, "showdiffuse") == 0) {
        set(0, 0x10000000u);
    } else if (optionCompare(c, "showresult") == 0) {
    } else if (optionCompare(c, "showspecpower") == 0) {
        set(0, 0x80000000u);
    } else if (optionCompare(c, "showspecintensity") == 0) {
        set(1, 1u);
    } else if (optionCompare(c, "showedges") == 0) {
        set(1, 4u);
    } else if (optionCompare(c, "showdifflight") == 0) {
        set(1, 8u);
    } else if (optionCompare(c, "showspeclight") == 0) {
        set(1, 0x10u);
    } else if (optionCompare(c, "showlight") == 0) {
        set(1, 0x20u);
    } else if (optionCompare(c, "showglow") == 0) {
        set(1, 2u);
    } else if (optionCompare(c, "hqt") == 0) {
        if (custom) toggle(0, 0x10000u);
    } else if (optionCompare(c, "overdraw") == 0) {
        toggle(1, 0x40u);
    } else if (optionCompare(c, "overdrawexpert") == 0) {
        toggle(1, 0x80u);
    } else if (optionCompare(c, "complexity") == 0) {
        toggle(1, 0x100u);
    } else if (optionCompare(c, "flush") == 0) {
    } else if (optionCompare(c, "shadowon") == 0) {
        if (custom) renderBits[0] &= ~0x200000u;
    } else if (optionCompare(c, "shadowoff") == 0) {
        if (custom) set(0, 0x200000u);
    } else if (optionCompare(c, "fakeshadowon") == 0) {
        if (custom) renderBits[0] &= ~0x800000u;
    } else if (optionCompare(c, "fakeshadowoff") == 0) {
        if (custom) set(0, 0x800000u);
    } else if (optionCompare(c, "specon") == 0) {
        if (custom) renderBits[0] &= ~0x1000000u;
    } else if (optionCompare(c, "specoff") == 0) {
        if (custom) set(0, 0x1000000u);
    } else if (optionCompare(c, "fsaa") == 0) {
        toggle(0, 0x40u);
    } else if (optionCompare(c, "fsaaon") == 0) {
        set(0, 0x40u);
    } else if (optionCompare(c, "fsaaoff") == 0) {
        renderBits[0] &= ~0x40u;
    } else if (optionCompare(c, "32") == 0) {
        setRenderBit(0x14, false);
    } else if (optionCompare(c, "64") == 0) {
        setRenderBit(0x14, true);
    }
    Py_RETURN_NONE;
}
// ParseClientCommand(command, arguments) (FUN_004d6270; communicator.py's "/cl_..."): the arguments split at "," (spaces, tabs and new
// lines trimmed). cl_hue2test: up to 8 integers / 255 (missing ones 0, but the 4th and 7th 1.0) to a Hue2Colorizer visitor over the
// main scene's bodies (FUN_007fe460) - OpenRasa does not draw Hue2, so it has no visible effect here. cl_reloadcameras: the user
// controller's camera profiles read again (FUN_00551300: cameraprofiles.xml, cameraprofiles_internal.xml, cameraprofiles_custom.xml).
PyObject* parseClientCommand(PyObject*, PyObject* args) {
    const char* command = nullptr;
    const char* arguments = nullptr;
    if (!PyArg_ParseTuple(args, "ss:ParseClientCommand", &command, &arguments)) return nullptr;
    if (SDL_strcasecmp(command, "cl_reloadcameras") == 0) player::reloadCameraProfiles();
    Py_RETURN_NONE;
}

// SaveScreenShot() (0x004d9d80): Client.Graphics.ScreenShotAsBMP (bool, absent false) picks .bmp, else .jpg (FUN_00801710: format 0 BMP,
// 1 JPG; 4 = .dds, unused here); the folder <working folder>\ScreenShots\<year>-<month>-<day> (GetSystemTime: UTC, no zero padding) made
// when missing (-1 when it cannot be); the name \%.8d<ext> from a counter (DAT_00d2cde8, 1 at start, kept for the session) raised past
// every name that exists as a file; D3DXSaveSurfaceToFile of the back buffer. True when that succeeded. OpenRasa saves the next frame
// drawn (main.cpp takeScreenshotRequest); a JPEG at quality 90 and a 32-bit BMP (D3DX's encoder settings NOT traced).
int screenshotCounter = 1;
std::string screenshotPath;
int screenshotFormat = 0;
PyObject* saveScreenShot(PyObject*, PyObject*) {
    const bool bmp = boolOptionValue("Client.Graphics.ScreenShotAsBMP", false);
    std::error_code error;
    const std::filesystem::path base = std::filesystem::current_path(error) / "ScreenShots";
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char day[64];
    PyOS_snprintf(day, sizeof day, "%d-%d-%d", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
    const std::filesystem::path folder = base / day;
    std::filesystem::create_directories(folder, error);
    if (!std::filesystem::is_directory(folder, error)) Py_RETURN_FALSE;
    std::filesystem::path file;
    do {
        char name[64];
        PyOS_snprintf(name, sizeof name, "%.8d%s", screenshotCounter, bmp ? ".bmp" : ".jpg");
        file = folder / name;
        ++screenshotCounter;
    } while (std::filesystem::is_regular_file(file, error));
    const std::u8string u8 = file.u8string();
    screenshotPath.assign(u8.begin(), u8.end());
    screenshotFormat = bmp ? 0 : 1;
    Py_RETURN_TRUE;
}

// MarkPyObjects() / PrintPyObjects() (0x004d9c20 / 0x004d9cd0): client.main.DEBUG_MarkPyObjects() / DEBUG_CollectPyObjects(), the result
// dropped; a Python error propagates.
PyObject* callClientMain(const char* function) {
    PyObject* module = PyImport_ImportModule(const_cast<char*>("client.main"));
    if (!module) return nullptr;
    PyObject* r = PyObject_CallMethod(module, const_cast<char*>(function), nullptr);
    Py_DECREF(module);
    if (!r) return nullptr;
    Py_DECREF(r);
    Py_RETURN_NONE;
}
PyObject* markPyObjects(PyObject*, PyObject*) { return callClientMain("DEBUG_MarkPyObjects"); }
PyObject* printPyObjects(PyObject*, PyObject*) { return callClientMain("DEBUG_CollectPyObjects"); }

// GetHeapUsage() (0x004d9750): unicode text "--- Memory Usage ---\n" + "Physical Avail: %.2f MB / %.2f MB\nVirtual Avail: %.2f MB /
// %.2f MB\n" (GlobalMemoryStatusEx: available / total physical, available / total virtual, x 1 / 2^20) + "Totals: %.2f MB (%.2f MB /
// %.2f MB)\n" (total - available virtual, then two values the release build leaves 0). Linux: /proc/meminfo for physical memory, the
// process's address space from /proc/self/status (VmSize) as used virtual memory, the total taken as 2^47 bytes (UNVERIFIED stand-in for
// a 64-bit process's user address space).
PyObject* getHeapUsage(PyObject*, PyObject*) {
    double physAvail = 0, physTotal = 0, virtAvail = 0, virtTotal = 0;
    constexpr double mb = 1.0 / 1048576.0;
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        physAvail = double(status.ullAvailPhys);
        physTotal = double(status.ullTotalPhys);
        virtAvail = double(status.ullAvailVirtual);
        virtTotal = double(status.ullTotalVirtual);
    }
#elif defined(__linux__)
    if (FILE* f = std::fopen("/proc/meminfo", "r")) {
        unsigned long long v = 0;
        char key[64];
        while (std::fscanf(f, "%63s %llu kB", key, &v) == 2) {
            if (std::strcmp(key, "MemTotal:") == 0) physTotal = double(v) * 1024.0;
            else if (std::strcmp(key, "MemAvailable:") == 0) physAvail = double(v) * 1024.0;
        }
        std::fclose(f);
    }
    virtTotal = 140737488355328.0;
    if (FILE* f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        unsigned long long kb = 0;
        while (std::fgets(line, sizeof line, f))
            if (std::sscanf(line, "VmSize: %llu kB", &kb) == 1) virtAvail = virtTotal - double(kb) * 1024.0;
        std::fclose(f);
    }
#endif
    const float pa = float(physAvail * mb), pt = float(physTotal * mb), va = float(virtAvail * mb), vt = float(virtTotal * mb);
    char text[512];
    PyOS_snprintf(text, sizeof text,
                  "--- Memory Usage ---\nPhysical Avail: %.2f MB / %.2f MB\nVirtual Avail: %.2f MB / %.2f MB\nTotals: %.2f MB (%.2f MB / %.2f MB)\n",
                  double(pa), double(pt), double(va), double(vt), double(vt - va), 0.0, 0.0);
    return PyUnicode_DecodeASCII(text, int(std::strlen(text)), nullptr);
}

// IsGeometryBuilding() (0x004d9650): with the terrain (GameClient + 0x288), its loader (+ 0x10) in state 2 (+ 0x78; building); False
// without a terrain. OpenRasa builds terrain chunks between frames: true while chunks are still due (world::terrainBuilding).
PyObject* isGeometryBuilding(PyObject*, PyObject*) { return PyBool_FromLong(world::terrainBuilding()); }
// GetPerfStatus() (FUN_004d7d80): in the release build always (0, 0, 0, 0, "") - the bytes of 0xFF000000 (DAT_00d08b38) b, g, r, then 0
// and an empty text.
PyObject* getPerfStatus(PyObject*, PyObject*) { return Py_BuildValue(const_cast<char*>("(iiiis)"), 0, 0, 0, 0, ""); }
// AppTime() (0x004d3510): GameClient + 0x2c0, the milliseconds since the client started at the last timed update.
PyObject* appTime(PyObject*, PyObject*) { return PyLong_FromUnsignedLong(clientClock); }
// GetRQSEnabled(name) (FUN_004d3b80 -> FUN_007e3a10): the TRRQS item of that name (exact), its slot 6 (enabled); False when none.
// EnableRQSValue(name, flag) (0x004d3b40 -> FUN_007e3940): that item's slot 5 (enable / disable); then, with flag True, TRRQS's slot 3
// (a reset of the items) - engine/Rqs.h.
PyObject* getRQSEnabled(PyObject*, PyObject* args) {
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "s:GetRQSEnabled", &name)) return nullptr;
    return PyBool_FromLong(rqs::enabled(name));
}
PyObject* enableRQSValue(PyObject*, PyObject* args) {
    const char* name = nullptr;
    PyObject* flag = nullptr;
    if (!PyArg_ParseTuple(args, "sO:EnableRQSValue", &name, &flag)) return nullptr;
    rqs::enable(name, PyObject_IsTrue(flag) == 1);
    Py_RETURN_NONE;
}

// ToggleWireframe() (0x004d4fc0): the device settings' wireframe flag (DAT_00d51694 + 0x19) flipped and copied to render bit 13.
// TogglePerfStats() (0x004d50a0): the performance display (+ 0x208): off -> on at its current page; on -> the next page (+ 0x134), off
// after the last (page 0 again); slot 2 told the state; on, its graph (+ 0x60) restarted. DisplaySkeleton(body, bone) (0x004d5020): the
// debug renderer (DAT_00d516a8) flag 0x80 set, its skeleton = the body's, with a bone name the bone's index; DisableDisplaySkeleton
// (0x004d5090) clears 0x80. EnableMemoryLog() (FUN_004daaa0): + 0x4b4 flipped; off closes the log file (+ 0x4b0). OpenRasa has none of
// these overlays: the state is kept, nothing is drawn or logged.
bool wireframe = false;
PyObject* toggleWireframe(PyObject*, PyObject*) {
    wireframe = !wireframe;
    setRenderBit(13, wireframe);
    Py_RETURN_NONE;
}
bool perfStatsShown = false;
PyObject* togglePerfStats(PyObject*, PyObject*) {
    perfStatsShown = !perfStatsShown;   // OpenRasa's display has one page (UNVERIFIED: the exe's page count)
    Py_RETURN_NONE;
}
bool skeletonShown = false;
PyObject* displaySkeleton(PyObject*, PyObject*) {
    skeletonShown = true;
    Py_RETURN_NONE;
}
PyObject* disableDisplaySkeleton(PyObject*, PyObject*) {
    skeletonShown = false;
    Py_RETURN_NONE;
}
bool memoryLog = false;
PyObject* enableMemoryLog(PyObject*, PyObject*) {
    memoryLog = !memoryLog;
    Py_RETURN_NONE;
}
// StartClientPerformanceProfile(a, name) (FUN_004d5140): once (+ 0x324): a profiler (FUN_00534eb0) whose report file opens at once -
// "PerfReport_<name>_%2d.csv" in the working folder, the first number that opens (" 0" first) - and that starts only when the map's
// pathfinding data loads from the developers' tree ("..//..//..//..//assets//maps//<map>//p%08x_pathfind.dat", FUN_00535090), which a
// game folder never has. So: an empty report file, nothing recorded.
bool profileStarted = false;
PyObject* startClientPerformanceProfile(PyObject*, PyObject* args) {
    PyObject *first = nullptr, *second = nullptr;
    if (!PyArg_ParseTuple(args, "OO:StartClientPerformanceProfile", &first, &second)) return nullptr;
    if (profileStarted) Py_RETURN_NONE;
    profileStarted = true;
    PyObject* s = PyObject_Str(second);
    if (!s) return nullptr;
    char name[300];
    PyOS_snprintf(name, sizeof name, "PerfReport_%s_%2d.csv", PyString_AsString(s), 0);
    Py_DECREF(s);
    if (FILE* f = std::fopen(name, "w")) std::fclose(f);
    Py_RETURN_NONE;
}
// TakeScreenMetric(a, b, c, d) (0x004d7d50): with the metrics object (DAT_00d516cc) and its + 0x14 set, FUN_0053ed30 - a metric record
// and, when asked, a screenshot through D3DXSaveSurfaceToFile. The metrics object is not created in a game run (NOT traced further):
// nothing.
PyObject* takeScreenMetric(PyObject*, PyObject*) { Py_RETURN_NONE; }
// ForceCameraPositionUpdate(x, y, z) (0x004d9600): with the terrain, FUN_0072a580 - waits until the terrain loader has finished the
// geometry around the camera (spinning while its + 0x600 is set). OpenRasa: its terrain chunks for the camera are built at once.
PyObject* forceCameraPositionUpdate(PyObject*, PyObject* args) {
    float x = 0, y = 0, z = 0;
    if (!PyArg_ParseTuple(args, "fff:ForceCameraPositionUpdate", &x, &y, &z)) return nullptr;
    world::buildTerrainAround(x, y, z);
    Py_RETURN_NONE;
}
// FlushAssets(age) (0x004d9bf0): the asset manager's flush (FUN_00594ea0) of assets unlocked and unused for more than `age`
// (an unsigned integer as a float). OpenRasa's asset cache keeps loaded files: nothing (UNVERIFIED effect on memory only).
PyObject* flushAssets(PyObject*, PyObject* args) {
    unsigned int age = 0;
    if (!PyArg_ParseTuple(args, "I:FlushAssets", &age)) return nullptr;
    Py_RETURN_NONE;
}
// GetLightManager() (0x004d4f80): the light manager's Python object (FUN_0053c4c0, made once and kept at GameClient + 0x250): a
// PyCPPObject named "LightManager" whose methods CreatePointLight, CreateDirLight, AddLight, RemoveLight, RemoveAllLights,
// AttachLightToBody, SnapLightToBody (0x0053c510 ..) all return None at once - decoration.py's light code then fails on the None.
PyObject* lightManagerObject = nullptr;
PyObject* returnNone(PyObject*, PyObject*) { Py_RETURN_NONE; }
PyObject* getLightManager(PyObject*, PyObject*) {
    if (!lightManagerObject) {
        static PyMethodDef methods[] = {
            {const_cast<char*>("CreatePointLight"), returnNone, METH_VARARGS, nullptr},
            {const_cast<char*>("CreateDirLight"), returnNone, METH_VARARGS, nullptr},
            {const_cast<char*>("AddLight"), returnNone, METH_VARARGS, nullptr},
            {const_cast<char*>("RemoveLight"), returnNone, METH_VARARGS, nullptr},
            {const_cast<char*>("RemoveAllLights"), returnNone, METH_VARARGS, nullptr},
            {const_cast<char*>("AttachLightToBody"), returnNone, METH_VARARGS, nullptr},
            {const_cast<char*>("SnapLightToBody"), returnNone, METH_VARARGS, nullptr},
            {nullptr, nullptr, 0, nullptr},
        };
        lightManagerObject = newPyCppObject("LightManager", methods);
    }
    Py_INCREF(lightManagerObject);
    return lightManagerObject;
}

// gameclient.GetRQSValuesList() (FUN_004d3a60): the names of the RQS items, in the order the TRRQS constructor added them (the list
// at render settings + 0x110; rqswindow.py shows them in a debug window).
PyObject* getRQSValuesList(PyObject*, PyObject*) {
    PyObject* list = PyList_New(0);
    for (const std::string& n : rqs::itemNames()) {
        PyObject* s = PyString_FromString(n.c_str());
        PyList_Append(list, s);
        Py_DECREF(s);
    }
    return list;
}

// ---- The authentication server (gameclient.Auth*, TRasa::GameClient's ncAuthClient; see net/AuthClient.h) -----------------------
// The bound functions return as the exe's do: AuthConnect / AuthLogin / AuthRequestServerList / AuthSelectServer true when the request
// was made (pyAuthLogin FUN_004d40d0: only while connected, else "LoginToAuthServer - not connected" and False). What the server
// answers reaches the scripts' current input state by name, as the exe's listener does (FUN_00536900 with entity 6, the input state
// router - client.inputstaterouter's InputStateRedirector).
net::AuthClient authClient;

PyObject* authConnect(PyObject*, PyObject* args) {
    const char* host;
    int port;
    if (!PyArg_ParseTuple(args, "si", &host, &port)) return nullptr;
    return PyBool_FromLong(authClient.connect(host, port));
}
PyObject* authLogin(PyObject*, PyObject* args) {
    const char *user, *password;
    int gameId = 0, cdKey = 0;
    if (!PyArg_ParseTuple(args, "ss|ii", &user, &password, &gameId, &cdKey)) return nullptr;
    const bool ok = authClient.login(user, password, uint32_t(gameId), uint16_t(cdKey));
    if (!ok && !authClient.connected()) std::fprintf(stderr, "[net] LoginToAuthServer - not connected\n");
    return PyBool_FromLong(ok);
}
PyObject* authRequestServerList(PyObject*, PyObject* args) {
    int kind = 1;
    if (!PyArg_ParseTuple(args, "|i", &kind)) return nullptr;
    return PyBool_FromLong(authClient.requestServerList(kind));
}
PyObject* authSelectServer(PyObject*, PyObject* args) {
    int server;
    if (!PyArg_ParseTuple(args, "i", &server)) return nullptr;
    return PyBool_FromLong(authClient.selectServer(server));
}
PyObject* authLogout(PyObject*, PyObject*) { return PyBool_FromLong(authClient.logout()); }
PyObject* authClose(PyObject*, PyObject*) {
    authClient.close();
    return none();
}
PyObject* isAuthConnected(PyObject*, PyObject*) { return PyBool_FromLong(authClient.connected()); }

void callInputState(const char* name, PyObject* args);

// ---- The login queue (gameclient.LoginQueueConnect / LoginQueueLogin / DisconnectFromLoginQueue; net/TrConnection.h) ------------
// Messages (Rasa.NET Rasa.Game/Packets/Queue): client 0x07 QueueLogin (u32 user id, u32 one-time key); server 0x0D PositionInQueue
// (i32 position, i32 estimated time), 0x0E HandoffToGame (4 address bytes, i32 port, u32 user id, u32 one-time key). The scripts hear
// AuthOnConnectedToQueue "(ii)" (the address as an in_addr int and the port, FUN_005430f0) once the negotiation is done,
// AuthRecvPositionInQueueMsg "(ii)", AuthRecvHandoffToGameMsg "(iiii)" (address int, port, user id, key, FUN_005431e0) and
// AuthOnDisconnectedFromQueue "()" (FUN_00543150). A connection that fails is reported as AuthOnConnectionFailed (UNVERIFIED: which
// callback the exe uses then is not traced).
net::TrConnection queueConnection;

// "a.b.c.d" as an in_addr read as a little-endian int (the scripts take it apart with & 255, >> 8 ...).
int addressInt(const std::string& text) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    std::sscanf(text.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d);
    return int(a | b << 8 | c << 16 | d << 24);
}

PyObject* loginQueueConnect(PyObject*, PyObject* args) {
    const char* host;
    int port;
    if (!PyArg_ParseTuple(args, "si", &host, &port)) return nullptr;
    return PyBool_FromLong(queueConnection.connect(host, port));
}
PyObject* loginQueueLogin(PyObject*, PyObject* args) {
    unsigned long user, key;
    if (!PyArg_ParseTuple(args, "kk", &user, &key)) return nullptr;
    if (!queueConnection.connected()) return PyBool_FromLong(0);
    std::vector<uint8_t> m{0x07};
    for (unsigned long v : {user, key})
        for (int i = 0; i < 4; ++i) m.push_back(uint8_t(v >> (8 * i)));
    queueConnection.send(m);
    return PyBool_FromLong(1);
}
PyObject* disconnectFromLoginQueue(PyObject*, PyObject*) {
    queueConnection.close();
    return none();
}

void pumpQueue() {
    queueConnection.poll();
    std::vector<uint8_t> m;
    while (queueConnection.popMessage(m)) {
        auto u32 = [&](size_t at) {
            return m.size() >= at + 4 ? uint32_t(m[at]) | uint32_t(m[at + 1]) << 8 | uint32_t(m[at + 2]) << 16 | uint32_t(m[at + 3]) << 24 : 0u;
        };
        if (m.empty()) continue;
        if (m[0] == 0x0D && m.size() >= 9) {
            callInputState("AuthRecvPositionInQueueMsg", Py_BuildValue("(ii)", int(u32(1)), int(u32(5))));
        } else if (m[0] == 0x0E && m.size() >= 17) {
            callInputState("AuthRecvHandoffToGameMsg", Py_BuildValue("(iiii)", int(u32(1)), int(u32(5)), int(u32(9)), int(u32(13))));
        } else {
            std::fprintf(stderr, "[net] queue: message 0x%02X (%zu bytes) not handled\n", m[0], m.size());
        }
    }
    net::TrConnection::Event e;
    while (queueConnection.popEvent(e)) {
        switch (e) {
        case net::TrConnection::Event::Connected:
            callInputState("AuthOnConnectedToQueue", Py_BuildValue("(ii)", addressInt(queueConnection.host()), queueConnection.port()));
            break;
        case net::TrConnection::Event::ConnectionFailed: callInputState("AuthOnConnectionFailed", PyTuple_New(0)); break;
        case net::TrConnection::Event::Disconnected: callInputState("AuthOnDisconnectedFromQueue", PyTuple_New(0)); break;
        }
    }
}

// ---- The game server (gameclient.ConnectToGameServer / DisconnectFromGameServer; net/TrConnection.h with the DH exchange) -----
// The scripts hear GameOnConnect when the key negotiation is done, GameOnConnectFail when the connection or the negotiation fails,
// GameOnDisconnect when it ends (login.py; the exe's own calls and formats are not traced - "()" taken from the scripts' signatures).
// What the server sends after "ENC OK" is not decoded yet.
net::TrConnection gameConnection;
net::GameStream gameStream;

bool netLogging() {
    static const bool on = [] {
        const char* v = std::getenv("OPENRASA_NETLOG");
        return v && *v == '1';
    }();
    return on;
}

// gameclient.LoginToGameServer(loginServer, userId, oneTimeKey) (TRasa::GameClient::LoginToGameServer FUN_004d4500): a
// ClientMsg_Login, subtype 1 when the first argument is true, else 2 (when the message allows it), with the user id and key as uints
// and the version OPENRASA_CLIENT_VERSION (ClientMsg_Login write, vtable 0x00bffc4c slot 4); without a connection "LoginToGameServer - not
// connected" and False.
PyObject* loginToGameServer(PyObject*, PyObject* args) {
    PyObject* loginServer;
    unsigned long user, key;
    if (!PyArg_ParseTuple(args, "Okk", &loginServer, &user, &key)) return nullptr;
    if (!gameConnection.connected()) {
        std::fprintf(stderr, "[net] LoginToGameServer - not connected\n");
        return PyBool_FromLong(0);
    }
    net::ProtocolWriter w;
    w.uint(uint32_t(user));
    w.uint(uint32_t(key));
    w.string(OPENRASA_CLIENT_VERSION);
    gameConnection.send(net::encodeGameMessage(1, PyObject_IsTrue(loginServer) ? 1 : 2, w.out));
    return PyBool_FromLong(1);
}

std::wstring serverVersion;   // GameClient + 0x300 (see getServerVersionString)

// ClientMsg_LoginResponse (read: vtable 0x00bffcc4 slot 3) to the scripts as OnReceiveMessage (FUN_00529870) does: subtype 1 Handoff
// (uint user, uint key, net address) -> GameRecvLoginHandoff "(iis)", 2 HandoffFailed (uint) -> GameRecvLoginHandoffFailed "(i)",
// 3 WaitForLogout (uint ms) -> GameRecvWaitForLogout "(i)", 4 Success (uint, uint) -> GameRecvLoginSuccess "()", 5 Failed (uint) ->
// GameRecvLoginFailed "(i)"; then a string.
void handleLoginResponse(const net::GameMessage& m) {
    net::ProtocolReader r(m.body.data(), m.body.size(), m.flags);
    const uint8_t subtype = r.byte();
    switch (subtype) {
    case 1: {
        const uint32_t user = r.uint(), key = r.uint();
        r.debugByte(41);
        const uint32_t address = r.uint();
        const int32_t port = r.int32() & 0xFFFF;
        r.uint();
        r.debugByte(42);
        char hostPort[40];
        std::sprintf(hostPort, "%u.%u.%u.%u:%d", address & 0xFF, (address >> 8) & 0xFF, (address >> 16) & 0xFF,
                      address >> 24, port);   // UNVERIFIED: the address's byte order and the exe's text (FUN_00529530)
        callInputState("GameRecvLoginHandoff", Py_BuildValue("(iis)", int(user), int(key), hostPort));
        break;
    }
    case 2: callInputState("GameRecvLoginHandoffFailed", Py_BuildValue("(i)", int(r.uint()))); break;
    case 3: callInputState("GameRecvWaitForLogout", Py_BuildValue("(i)", int(r.uint()))); break;
    case 4:
        r.uint();
        r.uint();
        callInputState("GameRecvLoginSuccess", PyTuple_New(0));
        break;
    case 5: callInputState("GameRecvLoginFailed", Py_BuildValue("(i)", int(r.uint()))); break;
    default: std::fprintf(stderr, "[net] game: LoginResponse subtype %u not known\n", subtype); break;
    }
    {
        const std::string trailing = r.string();      // message + 0x28 -> FUN_004d4ab0 (the exe stores it before the subtype switch)
        if (r.ok()) serverVersion.assign(trailing.begin(), trailing.end());   // widened byte for byte (Latin-1)
    }
    if (netLogging()) std::printf("[net] game: LoginResponse subtype %u\n", subtype);
}

PyObject* connectToGameServer(PyObject*, PyObject* args) {
    const char* host;
    int port;
    if (!PyArg_ParseTuple(args, "si", &host, &port)) return nullptr;
    gameStream = net::GameStream();
    return PyBool_FromLong(gameConnection.connect(host, port, net::TrConnection::Exchange::DiffieHellman));
}
PyObject* disconnectFromGameServer(PyObject*, PyObject*) {
    gameConnection.close();
    return none();
}

// ClientMsg_CallMethod (read: vtable 0x00bffcdc; Rasa.NET CallMethodMessage): subtype, u64 entity id, subtype 1 / 3 a method id
// (uint) - named through InitializeMethodNameLookup's table -, 2 / 4 a method name (string), the marshalled arguments as an array
// (see PyMarshal.h), subtypes 3 / 4 then a uint, the delay. Dispatch (FUN_00529bc0 -> FUN_00529c40): "Recv_" + the name, scheduled
// on the entity through AddMethodCall with that delay (0 for subtypes 1 / 2).
// gameclient.SendCallUserMethod / SendCallSysUserMethod / SendCallActorMethod(method, args) (thunks 0x004d4a10 / 30 / 50 ->
// FUN_004d48e0): args must be a tuple (or a subclass) and method an int or a str (or subclasses), else RuntimeError "The second
// argument must be a tuple" / "The first argument must be an int or string". An int is sent as a method id (FUN_004d4820); a name
// that the method table knows with a non-zero id is sent as that id, otherwise as the name (FUN_004d4700). Nothing is sent
// without a game connection. The message is a ClientMsg_CallServerMethod (type 6; Rasa.NET CallServerMethodMessage): subtype -
// by id 1 user / 3 system user / 5 actor, by name 2 / 4 / 6 - then the uint id or the string name, then the arguments written
// by the exe's 'O' writer (FUN_00951c80) as an array; a value it cannot write sends nothing. Returns None.
PyObject* sendCallServerMethod(PyObject* args, uint8_t byIdSubtype) {
    PyObject *method, *arguments;
    if (!PyArg_ParseTuple(args, "OO", &method, &arguments)) return nullptr;
    if (!PyTuple_Check(arguments)) {
        PyErr_SetString(PyExc_RuntimeError, const_cast<char*>("The second argument must be a tuple"));
        return nullptr;
    }
    uint32_t id = 0;
    std::string name;
    if (PyInt_Check(method)) {
        id = uint32_t(PyInt_AS_LONG(method));
    } else if (PyString_Check(method)) {
        name = PyString_AS_STRING(method);
        const auto it = methodIds.find(name);
        if (it != methodIds.end() && it->second != 0) {
            id = it->second;
            name.clear();
        }
    } else {
        PyErr_SetString(PyExc_RuntimeError, const_cast<char*>("The first argument must be an int or string"));
        return nullptr;
    }
    if (!gameConnection.connected()) Py_RETURN_NONE;
    std::vector<uint8_t> payload;
    if (!marshalArguments(arguments, payload)) {
        std::fprintf(stderr, "[net] game: the arguments of %s could not be written; not sent\n",
                     name.empty() ? std::to_string(id).c_str() : name.c_str());
        Py_RETURN_NONE;
    }
    net::ProtocolWriter w;
    if (name.empty())
        w.uint(id);
    else
        w.string(name);
    w.array(payload);
    gameConnection.send(net::encodeGameMessage(6, name.empty() ? byIdSubtype : byIdSubtype + 1, w.out));
    if (netLogging()) {
        PyObject* text = PyObject_Repr(arguments);
        const char* t = text ? PyString_AsString(text) : nullptr;
        const auto known = methodNames.find(id);
        std::printf("[net] game: sent %s%s%.300s\n", name.empty() ? (known != methodNames.end() ? known->second.c_str() : "?") : name.c_str(),
                    name.empty() ? "" : " (by name)", t ? t : "?");
        Py_XDECREF(text);
        PyErr_Clear();
    }
    Py_RETURN_NONE;
}
PyObject* sendCallUserMethod(PyObject*, PyObject* args) { return sendCallServerMethod(args, 1); }
PyObject* sendCallSysUserMethod(PyObject*, PyObject* args) { return sendCallServerMethod(args, 3); }
PyObject* sendCallActorMethod(PyObject*, PyObject* args) { return sendCallServerMethod(args, 5); }
// SendChatMsg / SendWorldMsg (0x004d4a70 / 0x004d4a90): the same sender with subtypes 7 / 8 and 9 / 10 (by id / by name).
PyObject* sendChatMsg(PyObject*, PyObject* args) { return sendCallServerMethod(args, 7); }
PyObject* sendWorldMsg(PyObject*, PyObject* args) { return sendCallServerMethod(args, 9); }
// SetCurrentUserId (0x004D4B30): GameClient + 0x2d8 (a u64), kept; its readers are not traced.
uint64_t currentUserId = 0;
PyObject* setCurrentUserId(PyObject*, PyObject* args) {
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, "O:SetCurrentUserId", &o)) return nullptr;
    currentUserId = PyLong_Check(o) ? PyLong_AsUnsignedLongLong(o) : (uint64_t)PyInt_AsLong(o);
    PyErr_Clear();
    Py_RETURN_NONE;
}
// GetCurrentUserId (0x004D4B60): GameClient + 0x2d8 / + 0x2dc as a u64 - what SetCurrentUserId stored (0 before).
PyObject* getCurrentUserId(PyObject*, PyObject*) { return PyLong_FromUnsignedLongLong(currentUserId); }
// The server version (GameClient + 0x300, a wide string; empty from the start - UNVERIFIED: the constructor's value is not read): every
// LoginResponse (OnReceiveMessage FUN_00529870, before its subtype switch) passes the message's trailing string (+ 0x28) to FUN_004d4ab0,
// which widens it byte for byte in the C locale (FUN_00481780: ctype<wchar_t>::widen, i.e. Latin-1) and stores it. GetServerVersionString
// (0x004D2CC0) returns it as unicode. Rasa.NET sends "" there.
PyObject* getServerVersionString(PyObject*, PyObject*) {
    return PyUnicode_FromWideChar(serverVersion.data(), int(serverVersion.size()));
}
// GetClientVersionString (FUN_004d2c20 -> FUN_004d2b50): the literal OPENRASA_CLIENT_VERSION_STRING as unicode.
PyObject* getClientVersionString(PyObject*, PyObject*) {
    static const char text[] = OPENRASA_CLIENT_VERSION_STRING;
    return PyUnicode_DecodeASCII(text, int(sizeof text - 1), nullptr);
}
// The diagnostics bindings the retail build left empty (verified 2026-10-28 in the bytes): SetServerPerfMetrics(float, float) 0x004d74a0 =
// "ret 8"; EnableDiagnostics 0x004d7480, DisplayNetworkRTT 0x004d74b0, DisplayNetworkVariance 0x004d74c0 = "ret 4" (one argument, its type
// UNVERIFIED); DiagnosticsEnabled 0x004d7490 = "xor al, al; ret" - always False. (Rasa.NET's ServerPerformanceMetricsPacket notes the same.)
PyObject* setServerPerfMetrics(PyObject*, PyObject* args) {
    float loops = 0, peak = 0;
    if (!PyArg_ParseTuple(args, "ff:SetServerPerfMetrics", &loops, &peak)) return nullptr;
    Py_RETURN_NONE;
}
PyObject* takesOneDoesNothing(PyObject*, PyObject* args) {
    PyObject* o = nullptr;
    if (!PyArg_ParseTuple(args, "O", &o)) return nullptr;
    Py_RETURN_NONE;
}
PyObject* diagnosticsEnabled(PyObject*, PyObject*) { Py_RETURN_FALSE; }
// ---- Voice chat (VoiceChat.h) -------------------------------------------------------------------------------------------------
// The options read as the engine's option getters do (FUN_004e35e0 bool, FUN_004e3620 int: the stored value, else the default).
bool voiceOptionBool(const char* name, bool fallback) {
    const std::string* v = optionValue(name);
    if (!v) return fallback;
    const std::string l = lower(*v);
    if (l == "true" || l == "1") return true;
    if (l == "false" || l == "0") return false;
    return fallback;
}
int voiceOptionInt(const char* name, int fallback) {
    const std::string* v = optionValue(name);
    return v && !v->empty() ? std::atoi(v->c_str()) : fallback;
}
// Client.Voice.VoiceVolume as InitVoiceChat and the options observer read it: default 75, at most 100 (0x004d55e1).
int voiceVolumeOption() { return std::min(voiceOptionInt("Client.Voice.VoiceVolume", 75), 100); }
// The Audio options observer (FUN_00534ac0): Client.Voice.Enabled -> party.OnVoiceChatEnabled(value, default True) (FUN_004d5b60,
// entity 0xb, delay 0); Client.Voice.VoiceVolume -> the voice volume (FUN_004d5af0).
void voiceOptionChanged(const std::string& lowerName) {
    if (lowerName == "client.voice.enabled")
        scheduleMethodCall(0xb, "OnVoiceChatEnabled", Py_BuildValue(const_cast<char*>("(b)"), voiceOptionBool("Client.Voice.Enabled", true) ? 1 : 0), 0);
    else if (lowerName == "client.voice.voicevolume")
        voice::setVolume(voiceVolumeOption());
}
// InitVoiceChat (FUN_004d52c0): True at once when the client exists; needs the audio manager running. Voice activation =
// Client.Voice.Activated and not ListenOnly (both default False); ActivationLevel (default 190) and CutoffDelay (default 300) at
// least 100 (0x004d5512 / 0x004d556b); then the volume.
PyObject* initVoiceChat(PyObject*, PyObject*) {
    if (voice::exists()) Py_RETURN_TRUE;
    if (!audio::running()) Py_RETURN_FALSE;
    const bool activation = voiceOptionBool("Client.Voice.Activated", false) && !voiceOptionBool("Client.Voice.ListenOnly", false);
    const int level = std::max(voiceOptionInt("Client.Voice.ActivationLevel", 190), 100);
    const int cutoff = std::max(voiceOptionInt("Client.Voice.CutoffDelay", 300), 100);
    return PyBool_FromLong(voice::init(activation, level, cutoff, voiceVolumeOption()));
}
// ConnectToVoiceServer("host:port") (FUN_004d56b0): only when the client exists and is not connected.
PyObject* connectToVoiceServer(PyObject*, PyObject* args) {
    const char* address;
    if (!PyArg_ParseTuple(args, "s", &address)) return nullptr;
    return PyBool_FromLong(voice::exists() && !voice::connected() && voice::connect(address));
}
// LoginToVoiceGroup(group, player, token) (FUN_004d5770): connected and not yet logged in; the codec asked for is 5.
PyObject* loginToVoiceGroup(PyObject*, PyObject* args) {
    PyObject *g, *p, *t;
    if (!PyArg_ParseTuple(args, "OOO", &g, &p, &t)) return nullptr;
    const unsigned long long player = PyInt_Check(p) ? (unsigned long long)PyInt_AsLong(p) : PyLong_AsUnsignedLongLong(p);
    const unsigned long group = PyInt_Check(g) ? (unsigned long)PyInt_AsLong(g) : PyLong_AsUnsignedLong(g);
    const unsigned long token = PyInt_Check(t) ? (unsigned long)PyInt_AsLong(t) : PyLong_AsUnsignedLong(t);
    if (PyErr_Occurred()) return nullptr;
    return PyBool_FromLong(voice::exists() && voice::connected() && !voice::loggedIn() && voice::login(uint32_t(group), player, uint32_t(token)));
}
// DisconnectFromVoiceServer (FUN_004d5880): stops talking, then disconnects.
PyObject* disconnectFromVoiceServer(PyObject*, PyObject*) {
    if (voice::exists()) {
        if (voice::talking()) voice::stopTalking();
        if (voice::connected()) voice::disconnect();
    }
    Py_RETURN_NONE;
}
// ShutdownVoiceChat (FUN_004d58c0).
PyObject* shutdownVoiceChat(PyObject*, PyObject*) {
    voice::shutdown();
    Py_RETURN_NONE;
}
// BeginSpeaking (0x004D5900): push-to-talk - only without voice activation, not listen-only, and not already talking.
PyObject* beginSpeaking(PyObject*, PyObject*) {
    if (voice::exists() && voice::connected() && !voiceOptionBool("Client.Voice.Activated", false) &&
        !voiceOptionBool("Client.Voice.ListenOnly", false) && !voice::talking())
        voice::startTalking();
    Py_RETURN_NONE;
}
// EndSpeaking (0x004D5A30).
PyObject* endSpeaking(PyObject*, PyObject*) {
    if (voice::exists() && voice::talking() && !voiceOptionBool("Client.Voice.Activated", false)) voice::stopTalking();
    Py_RETURN_NONE;
}

// LogClientInformation (FUN_004d9f00): writes the graphics settings (HardwareFSAA, VSync, ...) to the client's log - nothing else.
PyObject* doesNothingHere(PyObject*, PyObject*) { Py_RETURN_NONE; }

void handleCallMethod(const net::GameMessage& m) {
    net::ProtocolReader r(m.body.data(), m.body.size(), m.flags);
    const uint8_t subtype = r.byte();
    const uint64_t entity = r.ulong();
    std::string name;
    uint32_t methodId = 0;
    if (subtype == 1 || subtype == 3) {
        methodId = r.uint();
        const auto it = methodNames.find(methodId);
        if (it != methodNames.end()) name = it->second;
    } else if (subtype == 2 || subtype == 4) {
        name = r.string();
    } else {
        std::fprintf(stderr, "[net] game: CallMethod subtype %u not known\n", subtype);
        return;
    }
    const std::vector<uint8_t> payload = r.array();
    const uint32_t delay = (subtype == 3 || subtype == 4) ? r.uint() : 0;
    if (!r.ok()) {
        std::fprintf(stderr, "[net] game: CallMethod message malformed\n");
        return;
    }
    if (name.empty()) {
        std::fprintf(stderr, "[net] game: CallMethod method id %u has no name\n", methodId);
        return;
    }
    std::string error;
    PyObject* args = unmarshalArguments(payload.data(), payload.size(), error);
    if (!args) {
        std::fprintf(stderr, "[net] Unable to unpack args for methodName=%s (%s)\n", name.c_str(), error.c_str());
        return;
    }
    if (netLogging()) {
        PyObject* text = PyObject_Repr(args);
        const char* t = text ? PyString_AsString(text) : nullptr;
        std::printf("[net] game: entity %llu Recv_%s%.300s\n", (unsigned long long)entity, name.c_str(), t ? t : "?");
        Py_XDECREF(text);
        PyErr_Clear();
    }
    if (!scheduleMethodCall(entity, ("Recv_" + name).c_str(), args, int(delay)))
        std::fprintf(stderr, "[net] Unable to schedule method call\n");
}

void sendPingIfRequested() {
    if (!ping.requested || !gameConnection.connected()) return;
    net::ProtocolWriter w;
    w.uint(clientClock);
    gameConnection.send(net::encodeGameMessage(8, -1, w.out));
    ping.requested = false;
}
void handlePing(const net::GameMessage& m) {
    net::ProtocolReader r(m.body.data(), m.body.size(), m.flags);
    const uint32_t sent = r.uint();
    if (!r.ok()) return;
    uint32_t rtt = clientClock - sent;
    const uint32_t half = frameRate.mean >> 1;
    if (rtt > half) rtt -= half;
    ping.last = rtt;
    if (netLogging()) std::printf("[net] game: ping reply %u ms (sent at %u)\n", rtt, sent);
    ping.least = std::min(ping.least, rtt);
    if (++ping.count >= 15) {
        ping.published = ping.least;
        ping.least = 0x7fffffff;
        ping.count = 0;
    }
}
void pumpGame() {
    gameConnection.poll();
    std::vector<uint8_t> m;
    while (gameConnection.popMessage(m)) gameStream.append(m);
    net::GameMessage message;
    while (gameStream.next(message)) {
        if (message.channel == 0xFF) continue;
        if (message.type == 2) {
            handleLoginResponse(message);
            continue;
        }
        if (message.type == 7) {
            handleCallMethod(message);
            continue;
        }
        if (message.type == 8) {
            handlePing(message);
            continue;
        }
        if (message.type == 4) {      // MoveObject: subtype, then (2) the entity id and its movement
            net::ProtocolReader r(message.body.data(), message.body.size(), message.flags);
            const uint8_t subtype = r.byte();
            player::moveObject(subtype, r);
            continue;
        }
        if (netLogging()) {
            std::printf("[net] game: message type %u, channel %u, %zu bytes:", message.type, message.channel, message.body.size());
            for (size_t i = 0; i < message.body.size() && i < 40; ++i) std::printf(" %02X", message.body[i]);
            std::printf("\n");
        }
    }
    if (gameStream.error()) {
        std::fprintf(stderr, "[net] game: %s; disconnecting\n", gameStream.errorText().c_str());
        gameStream = net::GameStream();
        gameConnection.close();
    }
    net::TrConnection::Event e;
    while (gameConnection.popEvent(e)) {
        switch (e) {
        case net::TrConnection::Event::Connected: callInputState("GameOnConnect", PyTuple_New(0)); break;
        case net::TrConnection::Event::ConnectionFailed: callInputState("GameOnConnectFail", PyTuple_New(0)); break;
        case net::TrConnection::Event::Disconnected: callInputState("GameOnDisconnect", PyTuple_New(0)); break;
        }
    }
}

void callInputState(const char* name, PyObject* args) { scheduleMethodCall(6, name, args, 0); }

void pumpAuth() {
    authClient.poll();
    net::AuthEvent e;
    while (authClient.popEvent(e)) {
        using K = net::AuthEvent::Kind;
        auto one = [&](const char* name) { callInputState(name, Py_BuildValue("(i)", int(e.values.at(0)))); };
        switch (e.kind) {
        case K::Connected: callInputState("AuthOnConnected", PyTuple_New(0)); break;
        case K::ConnectionFailed: callInputState("AuthOnConnectionFailed", PyTuple_New(0)); break;
        case K::Disconnected: callInputState("AuthOnDisconnected", PyTuple_New(0)); break;
        case K::LoginFail: one("AuthRecvLoginFail"); break;
        case K::BlockedAccount: one("AuthRecvBlockedAccount"); break;
        case K::ServerFail: one("AuthRecvServerFail"); break;
        case K::PlayFail: one("AuthRecvPlayFail"); break;
        case K::AccountKicked: one("AuthRecvAccountKicked"); break;
        case K::LoginOk:
            callInputState("AuthRecvLoginOk", Py_BuildValue("(iiiii)", int(e.values[0]), int(e.values[1]), int(e.values[2]),
                                                            int(e.values[3]), int(e.values[4])));
            break;
        case K::PlayOk:
            callInputState("AuthRecvPlayOk", Py_BuildValue("(iii)", int(e.values[0]), int(e.values[1]), int(e.values[2])));
            break;
        case K::HandoffToQueue:
            callInputState("AuthRecvHandoffToQueueMsg", Py_BuildValue("(iii)", int(e.values[0]), int(e.values[1]), int(e.values[2])));
            break;
        case K::ServerList: {
            PyObject* list = PyList_New(0);
            for (const net::ServerEntry& s : e.servers) {
                PyObject* item = Py_BuildValue("(isi(iiiii))", s.id, s.address.c_str(), s.port, s.ageLimit, s.pkFlag, s.players,
                                               s.maxPlayers, s.status);
                PyList_Append(list, item);
                Py_DECREF(item);
            }
            callInputState("AuthRecvServerListEx", Py_BuildValue("(Oi)", list, int(e.values.at(0))));
            Py_DECREF(list);
            break;
        }
        }
    }
}

PyObject* postQuitRequest(PyObject*, PyObject*) {
    quit = true;
    return none();
}

PyObject* getOptionsManager(PyObject*, PyObject*) {
    Py_INCREF(optionsManager);
    return optionsManager;
}

PyObject* getUIManager(PyObject*, PyObject*) { return ui::uiManagerObject(); }
PyObject* getInputMapper(PyObject*, PyObject*) { return ui::inputMapperObject(); }
PyObject* getGameUIManager(PyObject*, PyObject*) { return ui::gameUiManagerObject(); }
PyObject* getAnimManager(PyObject*, PyObject*) { return ui::animManagerObject(); }
PyObject* getIMEManager(PyObject*, PyObject*) { return ui::imeManagerObject(); }
PyObject* getAudioManager(PyObject*, PyObject*) { return audio::managerObject(); }

// The audio settings the engine reads (strings in the exe: Client.Audio.Enabled, MasterVolume, MusicVolume, EffectVolume; volumes
// 0 - 100).
void configureAudio() {
    auto number = [](const char* name, float fallback) {
        const std::string* v = optionValue(name);
        return v && !v->empty() ? float(std::atof(v->c_str())) : fallback;
    };
    const std::string* on = optionValue("Client.Audio.Enabled");
    const bool enabled = !on || on->empty() || lower(*on) == "true" || *on == "1";
    // MaxHandles is read as a byte (FUN_004e3840, default 0x40); UpdatePeriodMilliseconds as an integer (default 500).
    audio::configure(enabled, number("Client.Audio.MasterVolume", 100) / 100, number("Client.Audio.MusicVolume", 100) / 100,
                     number("Client.Audio.EffectVolume", 100) / 100, unsigned(number("Client.Audio.MaxHandles", 64)) & 0xff,
                     unsigned(number("Client.Audio.UpdatePeriodMilliseconds", 500)),
                     unsigned(number("Client.Audio.PlaybackFrequency", 44100)), unsigned(number("Client.Audio.PlaybackBits", 16)),
                     number("Client.Audio.DetailLevel", 100) / 100);
    // Also read by GameClient::Initialize: PlaybackChannels (into the settings, never passed to Miles: the system's speaker set-up decides)
    // and SampleCacheSize (x 1024 into the audio manager + 0x10; no reader traced).
}

// GenerateBookmark (0x004d7530): TRBookmarks.bmf gets the camera (config::generateBookmark).
PyObject* generateBookmark(PyObject*, PyObject* args) {
    if (!argCount(args, 0, "GameClient.GenerateBookmark")) return nullptr;
    float eye[3] = {}, rotation[4] = {0, 0, 0, 1}, yaw = 0, pitch = 0, fov = 0;
    const bool camera = player::camera(eye, yaw, pitch, fov) && player::cameraRotation(rotation);
    config::generateBookmark(camera, eye, rotation, optionValue("Client.Developer.LastMapTemplate"));
    return none();
}

// GetLoginConfig (0x004d4be0): GameClient + 0x2a0, login.cfg (config::createEngineConfigs).
PyObject* getLoginConfig(PyObject*, PyObject*) {
    PyObject* c = config::loginConfig();
    Py_INCREF(c);
    return c;
}

PyObject* run(PyObject*, PyObject*) {
    // OPENRASA_PYTEST=<file.py> (tests): runs that script in __main__ once the client scripts have initialised (with
    // OPENRASA_EXIT_AT_RUN=1 the program then ends).
    if (const char* test = std::getenv("OPENRASA_PYTEST"); test && *test) {
        if (FILE* f = std::fopen(test, "r")) {
            PyRun_SimpleFile(f, test);
            std::fclose(f);
        } else {
            std::fprintf(stderr, "[engine] OPENRASA_PYTEST: cannot open %s\n", test);
        }
    }
    if (callbacks.run) callbacks.run();
    if (PyErr_Occurred()) return nullptr;
    return none();
}

std::string utf8Text(PyObject* o) {
    PyObject* s = PyUnicode_Check(o) ? PyUnicode_AsUTF8String(o) : PyObject_Str(o);
    if (!s) {
        PyErr_Clear();
        return "?";
    }
    std::string r = PyString_AsString(s);
    Py_DECREF(s);
    return r;
}

// debugmon_base (the exe's table 0x00d03010): the release client's debug monitor does nothing. GetChannelId(module, channel)
// (0x00903AB0, "ss:GetChannelId") returns 0; ShouldLog(id) (0x00903B90, "i:ShouldLog") 0; WriteLine(id, file, line, text) (0x00903BF0,
// "isis:WriteLine") None; IsAvailable() (0x00903C80) 0, so shared/debugmon.py binds every channel to _DoNothing. A failed parse - or a
// NULL name, a channel id 0 (ShouldLog, WriteLine) - raises RuntimeError with the method's own text in place of the parse error.
PyObject* debugmonFailed(const char* text) {
    PyErr_Clear();
    PyErr_SetString(PyExc_RuntimeError, text);
    return nullptr;
}
PyObject* debugmonGetChannelId(PyObject*, PyObject* args) {
    const char* module = nullptr;
    const char* channel = nullptr;
    if (!PyArg_ParseTuple(args, "ss:GetChannelId", &module, &channel) || !module || !channel)
        return debugmonFailed("illegal module or channel name");
    return PyInt_FromLong(0);
}
PyObject* debugmonShouldLog(PyObject*, PyObject* args) {
    int id = 0;
    if (!PyArg_ParseTuple(args, "i:ShouldLog", &id) || id == 0) return debugmonFailed("illegal channel id");
    return PyInt_FromLong(0);
}
PyObject* debugmonWriteLine(PyObject*, PyObject* args) {
    int id = 0, line = 0;
    const char* file = nullptr;
    const char* text = nullptr;
    if (!PyArg_ParseTuple(args, "isis:WriteLine", &id, &file, &line, &text) || id == 0 || !file || !text)
        return debugmonFailed("illegal arguments");
    return none();
}
PyObject* debugmonIsAvailable(PyObject*, PyObject* args) {
    if (!PyArg_ParseTuple(args, ":IsAvailable")) return nullptr;
    return PyInt_FromLong(0);
}
// GetAllChannelInfo() (0x00903CB0; its format string is ":IsAvailable" too): an empty list.
PyObject* debugmonGetAllChannelInfo(PyObject*, PyObject* args) {
    if (!PyArg_ParseTuple(args, ":IsAvailable")) return nullptr;
    return PyList_New(0);
}

// The log (FUN_00404ac0, InitApp): made when client.cfg's [Logging] Enabled is not "false" (FUN_00478160, default true), writing
// [Logging] File (default tabula_rasa.log). The gate (FUN_00405070 / FUN_004050b0): the log exists, the level >= DAT_00d44d9c (never
// written: 0) and channel bit 0x80000000 of DAT_00d08410 (0xFFFFFFFF, never written). OpenRasa writes to stderr, not the file (the game
// folder's tabula_rasa.log is the retail client's).
bool loggingEnabled() {
    static int enabled = -1;
    if (enabled < 0) {
        enabled = 1;
        try {
            tr::ConfigFile cfg((std::filesystem::path(gameRoot) / "client.cfg").string());
            if (const auto* items = cfg.section("Logging"))
                for (const auto& item : *items)
                    if (optionCompare(item.key, "Enabled") == 0) enabled = optionCompare(item.value, "false") == 0 ? 0 : 1;
        } catch (const std::exception&) {
        }
    }
    return enabled == 1;
}
// trmain._ShouldLog(level) (FUN_00405070).
PyObject* shouldLog(PyObject*, PyObject* args) {
    long level = 0;
    if (!argCount(args, 1, "_ShouldLog") || !argInt(args, 0, level, "_ShouldLog")) return nullptr;
    return PyBool_FromLong(loggingEnabled() && level >= 0);
}
// trmain._WriteLog(level, fmt, args) (FUN_004050b0; client/log.py): nothing unless _ShouldLog(level). The text: fmt when args is None,
// else fmt % args - failing that "Format Error: <repr(fmt)> % <repr(args)>" (a repr that fails is "?"); unicode as ASCII, else its repr;
// a non-string "[Non-string object could not be translated to ASCII for logging]". Levels 0 - 2 go to the log with the name TRACE,
// DEBUG, INFO (WARNING, ERROR, ASSERT, FATAL for 3 - 6, "none" past them) through FUN_00705e70; 3 and above to the error handler
// FUN_007066d0 (which writes the same "LEVEL: text" line to the log - its dialog / break behaviour for ASSERT and FATAL is not
// reproduced).
PyObject* writeLog(PyObject*, PyObject* args) {
    long level = 0;
    if (!argCount(args, 3, "_WriteLog") || !argInt(args, 0, level, "_WriteLog")) return nullptr;
    if (!loggingEnabled() || level < 0) return none();
    PyObject* fmt = PyTuple_GET_ITEM(args, 1);
    PyObject* fmtArgs = PyTuple_GET_ITEM(args, 2);
    PyObject* text = nullptr;
    if (fmtArgs == Py_None) {
        Py_INCREF(fmt);
        text = fmt;
    } else {
        text = PyNumber_Remainder(fmt, fmtArgs);
        if (!text) {
            PyErr_Clear();
            PyObject* r1 = PyObject_Repr(fmt);
            if (!r1) PyErr_Clear();
            PyObject* r2 = PyObject_Repr(fmtArgs);
            if (!r2) PyErr_Clear();
            std::string error = std::string("Format Error: ") + (r1 ? PyString_AsString(r1) : "?") + " % " + (r2 ? PyString_AsString(r2) : "?");
            Py_XDECREF(r1);
            Py_XDECREF(r2);
            text = PyString_FromString(error.c_str());
        }
    }
    if (text && PyUnicode_Check(text)) {
        PyObject* ascii = PyUnicode_AsASCIIString(text);
        if (!ascii) {
            PyErr_Clear();
            ascii = PyObject_Repr(text);
        }
        Py_DECREF(text);
        text = ascii;
    }
    const char* line = text ? PyString_AsString(text) : nullptr;
    if (!line) {
        PyErr_Clear();
        line = "[Non-string object could not be translated to ASCII for logging]";
    }
    static const char* levels[] = {"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "ASSERT", "FATAL"};
    std::fprintf(stderr, "%s: %s\n", level < 7 ? levels[level] : "none", line);
    Py_XDECREF(text);
    return none();
}

PyObject* outputDebugString(PyObject*, PyObject* args) {
    PyObject* text = nullptr;
    if (!PyArg_ParseTuple(args, "O", &text)) return nullptr;
    std::fprintf(stderr, "%s", utf8Text(text).c_str());
    return none();
}

void registerModule(const char* name, PyObject* module) {
    PyObject* modules = PyImport_GetModuleDict();
    PyDict_SetItemString(modules, const_cast<char*>(name), module);
}

// A wrapped module as the exe registers it (Py_InitModule4, then its init function): the class and CreateWrapper. A new reference.
// extern_gameclient_gameclient (init 0x004bc100): the GameClient class (cannot be instantiated from Python), CreateWrapper; its one
// object is trmain.GetGameClient()'s. The methods are put into the class (`put`, Stub.cpp setStubAttribute).
struct GameClientObject {
    PyObject_HEAD
};
PyTypeObject gameClientType = {PyObject_HEAD_INIT(nullptr)};
PyObject* gameClientCannotInstantiate(PyTypeObject*, PyObject*, PyObject*) {
    PyErr_SetString(PyExc_RuntimeError, "This class cannot be instantiated from Python");
    return nullptr;
}
PyObject* makeGameClient() {
    gameClientType.tp_name = const_cast<char*>("extern_gameclient_gameclient.GameClient");
    gameClientType.tp_basicsize = sizeof(GameClientObject);
    gameClientType.tp_flags = Py_TPFLAGS_DEFAULT;
    gameClientType.tp_new = gameClientCannotInstantiate;
    if (PyType_Ready(&gameClientType) < 0) PyErr_Print();
    return reinterpret_cast<PyObject*>(PyObject_New(GameClientObject, &gameClientType));
}

PyObject* wrappedModule(const char* name, const char* className, PyTypeObject* type) {
    static PyMethodDef methods[] = {
        {const_cast<char*>("CreateWrapper"), createWrapper, METH_VARARGS, nullptr},
        {nullptr, nullptr, 0, nullptr},
    };
    PyObject* m = Py_InitModule4(const_cast<char*>(name), methods, nullptr, nullptr, PYTHON_API_VERSION);   // borrowed
    Py_INCREF(type);
    PyModule_AddObject(m, const_cast<char*>(className), reinterpret_cast<PyObject*>(type));
    Py_INCREF(m);
    return m;
}

}  // namespace

WorldGraphics worldGraphics() {
    WorldGraphics g;
    auto percent = [](const char* name) { return std::clamp(float(intOptionValue(name, 100)) / 100.0f, 0.0f, 1.0f); };
    g.flora = !renderBit(0);
    g.rqs = rqs::values();
    // The drawn share of a tribble container's instances = (TRRQS + 0x8c) x (0.67 x FloraDensity / 100 + 0.33) (FUN_0073b930). The
    // RQS scalar is 0.5 at full quality, not 1: "Flora Density I" (normal 1.0) and "Flora Density II" (normal 0.5) both write it and
    // the root's reset runs them in order, so II's 0.5 wins (TRRQS constructor FUN_00846c90; traced 2026-10-25) - the game draws half
    // of each container's instances (the retail frame at Alia Das draws 730 clutter instances where the full set is ~1,550). Degrading
    // takes it to 0 (II) and restoring back to 0.5 / 1.0 (engine/Rqs.h).
    g.floraFactor = g.rqs.flora * (0.67f * float(intOptionValue("Client.Graphics.FloraDensity", 100)) / 100.0f + 0.33f);
    g.shadowDistance = percent("Client.Graphics.ShadowDistance");
    g.shadowMaxSimultaneous = percent("Client.Graphics.ShadowMaxSimultaneous");
    // FUN_00806f00: a static light's shadow map needs bit 22 clear AND the RQS flag + 0x165, any other light's bit 21 clear AND + 0x166;
    // FUN_00807640: the specular buffer needs bit 24 clear AND + 0x168.
    g.staticShadows = !renderBit(22) && g.rqs.staticShadows;
    g.dynamicShadows = !renderBit(21) && g.rqs.dynamicShadows;
    g.specular = !renderBit(24) && g.rqs.specular;
    g.deferredLighting = !renderBit(19);
    g.refractions = !renderBit(18) && g.rqs.refractions;   // RefractionPhase / water: also the RQS flag + 0x169
    // OpenRasa's extra: the game's unused reflective water technique, driven by the game's own Client.Graphics.Reflections (bit 17
    // clear). Retail never reflects water (the piece's reflect flag is only ever cleared) and its options window has no Reflections
    // control; every Video Quality level switches it off, so it is on only when client.cfg says True (or lacks the key - bit 17
    // starts clear; the game writes the key on every save).
    g.waterReflections = !renderBit(17);
    g.fakeShadows = !renderBit(23) && g.rqs.fakeShadows;
    // TerrainVisualizerClient::Submit (0x0072a7b0): the terrain effect's TechniqueMode = 1 (Terrain_Deferred_NormalGlowSpec) when the
    // view renders deferred (mode 5) and render settings bit 16 is set, else 0. Only /dl_hqt (quality level 6) writes bit 16.
    g.terrainNormalGlowSpec = renderBit(16) && g.deferredLighting;   // FUN_007f0120: also the RQS flag + 0x167
    g.disableLods = renderBit(7);
    // ParseDLCommand's debug views (one at a time) and the debug functions' render / terrain bits.
    if (renderBits[0] & 0x20000000u) g.debugView = 2;
    else if (renderBits[1] & 1u) g.debugView = 3;
    else if (renderBits[0] & 0x10000000u) g.debugView = 4;
    else if (renderBits[0] & 0x80000000u) g.debugView = 5;
    g.showLight = (renderBits[1] & 0x28u) != 0;
    g.hideNonActors = renderBit(2);
    g.hideVfx = renderBit(3);
    g.noParticles = renderBit(10);
    g.hideTerrain = (terrainDebugBits >> 2) & 1;
    g.viewDistance = percent("Client.Graphics.ViewDistance");
    g.terrainQuality = percent("Client.Graphics.TerrainQuality");
    // Anti-aliasing (see antiAliasingOptionChanged): the software pass while bit 6 is clear and no debug view holds the post-render slot
    // (word 0 bits 28 - 31, word 1 bits 0 - 8); the supersampling scale per axis with deferred lighting.
    g.softwareAntiAlias = !renderBit(6) && g.rqs.antiAlias && (renderBits[0] & 0xF0000000u) == 0 && (renderBits[1] & 0x1FFu) == 0;
    g.multiSample = g.deferredLighting ? 0u : uint32_t(std::max(multiSampleCount, 0));
    g.superSampling = g.deferredLighting && superSamplingFactor > 1.0f ? std::clamp(std::sqrt(superSamplingFactor), 1.0f, 2.0f) : 1.0f;
    // FUN_007be1f0: the names Low / Medium / High (0x00d041ec) compared ignoring case; anything else is Medium (1).
    g.textureQuality = 1;
    if (const std::string* q = optionValue("Client.Graphics.TextureQuality")) {
        const std::string lower = asciiLower(*q);
        if (lower == "low") g.textureQuality = 0;
        else if (lower == "high") g.textureQuality = 2;
    } else {
        g.textureQuality = 2;   // no option: the factory's start-up flags (4 = High)
    }
    return g;
}

void setAdapterDescription(const std::string& description) { adapterDescription = description; }

void runPythonFile(const std::string& path) {
    if (FILE* f = std::fopen(path.c_str(), "r")) {
        PyRun_SimpleFile(f, path.c_str());
        std::fclose(f);
    } else {
        std::fprintf(stderr, "[engine] cannot open %s\n", path.c_str());
    }
}

void startPython(const std::string& gameFolder, const Callbacks& cb) {
    callbacks = cb;
    gameRoot = gameFolder;
    std::filesystem::current_path(std::filesystem::u8path(gameFolder));
    loadOptions();
    {   // GameClient::Initialize (0x004cdbd8 ..): when the option exists its float (the "usa" stream; unreadable keeps 55) replaces 55.
        float degrees = 55.0f;
        if (const std::string* v = optionValue("Client.Developer.MaxAngleToClimb")) {
            char* end = nullptr;
            const float f = std::strtof(v->c_str(), &end);
            if (end != v->c_str()) degrees = f;
        }
        player::setMaxAngleToClimb(degrees);
    }
    // GameClient::Initialize (just before the climb limit): Client.Developer.EnableAsyncLoading through FUN_0049dfb0 ("True" / "Yes" /
    // "On" / "1"; registered True) into the loader's async flag (FUN_0058b790).
    if (const std::string* v = optionValue("Client.Developer.EnableAsyncLoading")) world::setAssetsAsync(optionBoolText(*v));
    applyCommandLineOptions(cb.commandLine, cb.launcherId);
    cursor::load(gameFolder);   // the GameUIManager's cursors (FUN_00520e90), "Pointer" shown
    configureAudio();

    Py_NoSiteFlag = 1;             // the standard library comes from trpython.zip once sys.path is set
    Py_IgnoreEnvironmentFlag = 1;
    Py_FrozenFlag = 1;
    Py_Initialize();
    // WinMain (FUN_00402130): sys.path = [], then these appended. Built as a list: PySys_SetPath splits on the platform's path
    // separator (";" on Windows, ":" elsewhere).
    if (PyObject* path = Py_BuildValue("[sss]", "python_nca_internal", "./trpython.zip", "./trpython.zip/client/system")) {
        PySys_SetObject(const_cast<char*>("path"), path);
        Py_DECREF(path);
    }
    pyprof::install();

    initOptionTypes();
    optionsManager = makeOptionsManager();
    // TRasa::GameClient::Initialize (FUN_004cd8c0): extern_util_config.Config() imports tabula_rasa.cfg and login.cfg; a failure
    // stops the client.
    config::initModule();
    if (!config::createEngineConfigs()) std::exit(1);
    gameClient = makeGameClient();
    put(gameClient, "GetOptionsManager", getOptionsManager);
    put(gameClient, "GetLoginConfig", getLoginConfig);
    put(gameClient, "GenerateBookmark", generateBookmark);
    put(gameClient, "Run", run);
    put(gameClient, "GetUIManager", getUIManager);
    put(gameClient, "GetGameUIManager", getGameUIManager);
    put(gameClient, "GetAnimManager", getAnimManager);
    put(gameClient, "GetIMEManager", getIMEManager);
    put(gameClient, "GetRQSValuesList", getRQSValuesList);
    put(gameClient, "SaveScreenShot", saveScreenShot);
    put(gameClient, "PrintPyObjects", printPyObjects);
    put(gameClient, "MarkPyObjects", markPyObjects);
    put(gameClient, "GetHeapUsage", getHeapUsage);
    put(gameClient, "IsGeometryBuilding", isGeometryBuilding);
    put(gameClient, "GetDebugSetting", getDebugSetting);
    put(gameClient, "CallDebugFunction", callDebugFunction);
    put(gameClient, "IsGMEnabled", isGMEnabled);
    put(gameClient, "SetGMEnabled", setGMEnabled);
    put(gameClient, "GetPerfStatus", getPerfStatus);
    put(gameClient, "TogglePerfStats", togglePerfStats);
    put(gameClient, "DisplaySkeleton", displaySkeleton);
    put(gameClient, "DisableDisplaySkeleton", disableDisplaySkeleton);
    put(gameClient, "EnableMemoryLog", enableMemoryLog);
    put(gameClient, "ToggleWireframe", toggleWireframe);
    put(gameClient, "EnableBoneTracking", enableBoneTrackingGlobal);
    put(gameClient, "GetLightManager", getLightManager);
    put(gameClient, "AppTime", appTime);
    put(gameClient, "GetRQSEnabled", getRQSEnabled);
    put(gameClient, "EnableRQSValue", enableRQSValue);
    put(gameClient, "StartClientPerformanceProfile", startClientPerformanceProfile);
    put(gameClient, "TakeScreenMetric", takeScreenMetric);
    put(gameClient, "ForceCameraPositionUpdate", forceCameraPositionUpdate);
    put(gameClient, "ParseClientCommand", parseClientCommand);
    put(gameClient, "ParseDLCommand", parseDLCommand);
    put(gameClient, "FlushAssets", flushAssets);
    put(gameClient, "GetFramerate", getFramerate);
    put(gameClient, "GetPerformanceData", getPerformanceData);
    put(gameClient, "GetNetworkPing", getNetworkPing);
    put(gameClient, "SetNetworkResponseTime", setNetworkResponseTime);
    put(gameClient, "Time", gameTime);
    put(gameClient, "GetVideoResolutionList", getVideoResolutionList);
    put(gameClient, "GetVideoRefreshRateList", getVideoRefreshRateList);
    put(gameClient, "GetFSAAList", getFSAAList);
    put(gameClient, "GetInputMapper", getInputMapper);
    put(gameClient, "GetAudioManager", getAudioManager);
    put(gameClient, "AuthConnect", authConnect);
    put(gameClient, "AuthLogin", authLogin);
    put(gameClient, "AuthRequestServerList", authRequestServerList);
    put(gameClient, "AuthSelectServer", authSelectServer);
    put(gameClient, "AuthLogout", authLogout);
    put(gameClient, "AuthClose", authClose);
    put(gameClient, "IsAuthConnected", isAuthConnected);
    put(gameClient, "LoginQueueConnect", loginQueueConnect);
    put(gameClient, "LoginQueueLogin", loginQueueLogin);
    put(gameClient, "DisconnectFromLoginQueue", disconnectFromLoginQueue);
    put(gameClient, "ConnectToGameServer", connectToGameServer);
    put(gameClient, "DisconnectFromGameServer", disconnectFromGameServer);
    put(gameClient, "LoginToGameServer", loginToGameServer);
    put(gameClient, "InitializeMethodNameLookup", initializeMethodNameLookup);
    put(gameClient, "SendCallUserMethod", sendCallUserMethod);
    put(gameClient, "SendCallSysUserMethod", sendCallSysUserMethod);
    put(gameClient, "SendCallActorMethod", sendCallActorMethod);
    put(gameClient, "SendChatMsg", sendChatMsg);
    put(gameClient, "SendWorldMsg", sendWorldMsg);
    put(gameClient, "SetCurrentUserId", setCurrentUserId);
    put(gameClient, "RequestNetworkStats", requestNetworkStats);
    put(gameClient, "LogClientInformation", doesNothingHere);
    put(gameClient, "GetCurrentUserId", getCurrentUserId);
    put(gameClient, "GetServerVersionString", getServerVersionString);
    put(gameClient, "GetClientVersionString", getClientVersionString);
    put(gameClient, "SetServerPerfMetrics", setServerPerfMetrics);
    put(gameClient, "EnableDiagnostics", takesOneDoesNothing);
    put(gameClient, "DisplayNetworkRTT", takesOneDoesNothing);
    put(gameClient, "DisplayNetworkVariance", takesOneDoesNothing);
    put(gameClient, "DiagnosticsEnabled", diagnosticsEnabled);
    put(gameClient, "InitVoiceChat", initVoiceChat);
    put(gameClient, "ConnectToVoiceServer", connectToVoiceServer);
    put(gameClient, "LoginToVoiceGroup", loginToVoiceGroup);
    put(gameClient, "DisconnectFromVoiceServer", disconnectFromVoiceServer);
    put(gameClient, "ShutdownVoiceChat", shutdownVoiceChat);
    put(gameClient, "BeginSpeaking", beginSpeaking);
    put(gameClient, "EndSpeaking", endSpeaking);
    world::install(gameClient);   // the map load (gamemap.py) and the world's picture
    catalogs::install(gameClient);   // PreloadAssets, PreloadAssetsForClassId, ... (Catalogs.h)
    if (!net::gameCryptSelfTest()) std::fprintf(stderr, "[net] the game ciphers failed their self-test\n");
    if (!net::authCryptSelfTest()) std::fprintf(stderr, "[net] the auth ciphers failed their self-test\n");
    if (!marshalSelfTest())
        std::fprintf(stderr, "[net] the method argument readers failed their self-test\n");
    else if (netLogging())
        std::printf("[net] method argument readers ('O', 'F', 'M'): self-test passed\n");

    // extern_trmain_trmain (a Boost.Python module, init 0x00404680 - plain functions, no class): InitApp 0x00404f20 (-> FUN_00404ac0, the
    // application's set-up; OpenRasa's is done before Python starts), InitGameClient 0x00404f30, ShutdownApp 0x00404f40 (-> 0x00404e90:
    // the game client and the application shut down and deleted, the log closed - OpenRasa shuts down when the scripts return, so it
    // does nothing here), PostQuitRequest 0x00404f50, GetGameClient 0x00405620, GetDataPaths, _ShouldLog, _WriteLog, OutputDebugString.
    static PyMethodDef trmainMethods[] = {
        {const_cast<char*>("InitApp"), noOp, METH_VARARGS, nullptr},
        {const_cast<char*>("InitGameClient"), initGameClient, METH_VARARGS, nullptr},
        {const_cast<char*>("ShutdownApp"), noOp, METH_VARARGS, nullptr},
        {const_cast<char*>("PostQuitRequest"), postQuitRequest, METH_VARARGS, nullptr},
        {const_cast<char*>("OutputDebugString"), outputDebugString, METH_VARARGS, nullptr},
        {const_cast<char*>("GetGameClient"), getGameClient, METH_VARARGS, nullptr},
        {const_cast<char*>("_ShouldLog"), shouldLog, METH_VARARGS, nullptr},
        {const_cast<char*>("_WriteLog"), writeLog, METH_VARARGS, nullptr},
        {const_cast<char*>("GetDataPaths"), getDataPaths, METH_VARARGS, nullptr},
        {nullptr, nullptr, 0, nullptr},
    };
    Py_InitModule4(const_cast<char*>("extern_trmain_trmain"), trmainMethods, nullptr, nullptr, PYTHON_API_VERSION);

    // The other modules tabula_rasa.exe registered (names from its string table): stubs for now.
    Py_DECREF(wrappedModule("extern_gameclient_option_wrap", "Option", &optionType));
    Py_DECREF(wrappedModule("extern_gameclient_gameclient", "GameClient", &gameClientType));
    world::initModule();
    player::install(gameClient);
    Py_DECREF(wrappedModule("extern_gameclient_optionsmanager_wrap", "OptionsManager", &optionsManagerType));
    initEntityBody();
    // debugmon_base (Py_InitModule4 with table 0x00d03010, registered by the game client's set-up just before FUN_004d4c10).
    static PyMethodDef debugmonMethods[] = {
        {const_cast<char*>("GetChannelId"), debugmonGetChannelId, METH_VARARGS, nullptr},
        {const_cast<char*>("ShouldLog"), debugmonShouldLog, METH_VARARGS, nullptr},
        {const_cast<char*>("WriteLine"), debugmonWriteLine, METH_VARARGS, nullptr},
        {const_cast<char*>("IsAvailable"), debugmonIsAvailable, METH_VARARGS, nullptr},
        {const_cast<char*>("GetAllChannelInfo"), debugmonGetAllChannelInfo, METH_VARARGS, nullptr},
        {nullptr, nullptr, 0, nullptr},
    };
    Py_InitModule4(const_cast<char*>("debugmon_base"), debugmonMethods, nullptr, nullptr, PYTHON_API_VERSION);
    // The movement controller calls the scripts at once (FUN_00536900 with delay -1: TransitionMovement, OnFacingChange, OnStartJump,
    // OnStartFalling, OnStopFalling - entitymanager.AddMethodCall runs a negative delay right away).
    player::setMethodCaller([](uint64_t entity, const char* method, PyObject* args) { scheduleMethodCall(entity, method, args, -1); });
    // The voice client calls party.py on the client party manager, entity 0xb (FUN_00536900 with 0xb).
    voice::setScriptCaller([](const char* method, PyObject* args, int delayMs) { scheduleMethodCall(0xb, method, args, delayMs); });
    setEntityBodyMethodCaller([](uint64_t entity, const char* method, PyObject* args, int delayMs) { scheduleMethodCall(entity, method, args, delayMs); });
    player::setOptionReader([](const char* name) { const std::string* v = optionValue(name); return v ? *v : std::string(); });
    player::setOptionWriter([](const char* name, const std::string& value) { setOption(name, value); });
    // The player's Move messages (type 3, no subtype) on the game connection.
    player::setSender([](const std::vector<uint8_t>& fields) {
        gameConnection.send(net::encodeGameMessage(3, -1, fields));
        if (netLogging()) {
            std::printf("[net] game: sent Move");
            for (uint8_t b : fields) std::printf(" %02X", b);
            std::printf("\n");
        }
    });
    {
        // sysentity (FUN_009121e0): the ids the scripts register their system entities under, as Python longs.
        PyObject* m = PyImport_AddModule(const_cast<char*>("sysentity"));   // borrowed
        const std::pair<const char*, long> ids[] = {
            {"ClientInputStateRouterId", 1}, {"AreaServerId", 2}, {"AreaServerMapManagerId", 3}, {"AreaServerUserManagerId", 4},
            {"ClientMethodId", 5}, {"CurrentInputStateId", 6}, {"ClientGameMapId", 7}, {"CommunicatorId", 8},
            {"ClientInventoryManagerId", 9}, {"ClientServerFlagManagerId", 10}, {"ClientPartyManagerId", 11},
            {"ClientGameUIManagerId", 12}, {"ClientAudioManagerId", 13}, {"ClientObjectAnimationManagerId", 15},
            {"ClientTradeManagerId", 16}, {"ClientAdventureLauncherId", 17}, {"ClientGuildManagerId", 19},
            {"ClientPetitionManagerId", 22}, {"ClientWargameManagerId", 23}, {"ClientClanManagerId", 24},
            {"ClientSocialManagerId", 25}, {"ClientMapStateId", 26}, {"ClientAuctionHouseManagerId", 14},
            {"ClientPrestigeSystemId", 27}, {"ClientControlPointManagerId", 28}, {"ClientLookingForGroupManagerId", 29},
            {"ClientTeamManagerId", 30}};
        for (const auto& [n, v] : ids) PyModule_AddObject(m, const_cast<char*>(n), PyLong_FromLong(v));
    }
    ui::initPython();
}

bool boot() {
    PyObject* bootstrap = PyImport_ImportModule(const_cast<char*>("clientbootstrap"));
    if (!bootstrap) {
        std::fprintf(stderr, "[engine] Error importing clientbootstrap\n");
        PyErr_Print();
        return false;
    }
    PyObject* r = PyObject_CallMethod(bootstrap, const_cast<char*>("boot"), nullptr);
    Py_DECREF(bootstrap);
    if (!r) {
        std::fprintf(stderr, "[engine] Error running clientbootstrap.boot()\n");
        PyErr_Print();
        return false;
    }
    Py_DECREF(r);
    return true;
}

void stopPython() {
    Py_XDECREF(gameClient);
    Py_XDECREF(optionsManager);
    config::releaseEngineConfigs();
    gameClient = optionsManager = nullptr;
    if (const char* dump = std::getenv("OPENRASA_UIDUMP"); dump && *dump == '1')
        std::fprintf(stderr, "Visible windows:\n%s", ui::manager().visibleWindows().c_str());
    ui::manager().unload();   // widgets hold Python objects
    world::shutdown();
    catalogs::shutdown();
    player::shutdown();
    cursor::shutdown();
    voice::setScriptCaller(nullptr);
    voice::shutdown();
    audio::shutdown();
    pyprof::report();
    Py_Finalize();
}

// Test hook OPENRASA_VOICETEST=host:port[,talk ms]: 3 s after the world has a manifestation, party.Recv_VoiceChatConnectInfo(
// "host:port", group 1, player 1, token 1) as a VoiceChatConnectInfo from the server would arrive; 3 s after that BeginSpeaking
// (push-to-talk pressed) and EndSpeaking the talk time later (default 2000 ms). For a voice server without a squad behind it.
void voiceTestStep() {
    static const char* spec = std::getenv("OPENRASA_VOICETEST");
    if (!spec) return;
    static uint64_t worldAt = 0;
    static int step = 0;
    if (!player::currentManifestation()) return;
    const uint64_t now = SDL_GetTicks();
    if (!worldAt) worldAt = now;
    const std::string s(spec);
    const size_t comma = s.find(',');
    const int talkMs = comma == std::string::npos ? 2000 : std::atoi(s.c_str() + comma + 1);
    if (step == 0 && now - worldAt > 3000) {
        step = 1;
        scheduleMethodCall(0xb, "Recv_VoiceChatConnectInfo",
                           Py_BuildValue(const_cast<char*>("(siLi)"), s.substr(0, comma).c_str(), 1, 1LL, 1), 0);
    } else if (step == 1 && now - worldAt > 6000) {
        step = 2;
        if (PyObject* r = beginSpeaking(nullptr, nullptr)) Py_DECREF(r);
    } else if (step == 2 && now - worldAt > 6000 + uint64_t(talkMs)) {
        step = 3;
        if (PyObject* r = endSpeaking(nullptr, nullptr)) Py_DECREF(r);
    }
}

bool quitRequested() { return quit; }

// The last frame()'s section times (always kept, cheap): main.cpp prints them for slow frames.
namespace {
struct SlowNote {
    uint64_t mark = 0;
    std::string text;
} slowNote;
void noteSection(const char* name) {
    const uint64_t now = SDL_GetPerformanceCounter();
    const double ms = double(now - slowNote.mark) * 1000.0 / double(SDL_GetPerformanceFrequency());
    slowNote.mark = now;
    if (ms >= 1.0) {
        char buf[96];
        SDL_snprintf(buf, sizeof buf, "%s %.1f ", name, ms);
        slowNote.text += buf;
    }
    pyprof::section(name);
}
}  // namespace
std::string slowFrameNote() { return slowNote.text; }

void frame(int msElapsed) {
    slowNote.text.clear();
    setStaticModeAllowed(!boolOptionValue("Client.Graphics.PreloadMapGeometry", false));
    slowNote.mark = SDL_GetPerformanceCounter();
    pyprof::frameBegin();
    voiceTestStep();
    pumpAuth();
    pumpQueue();
    pumpGame();
    noteSection("net");
    stepFrameRate();
    sendPingIfRequested();
    advanceEntityBodyMarkers();   // animation markers -> OnMarker_* (queued for this run)
    advanceEntityBodyBoneTrackers();
    noteSection("markers+trackers");
    runScheduledMethods();
    noteSection("scheduled methods");
    catalogs::frame();
    noteSection("catalogs");
    player::deliverCursorMove();
    ui::frame(msElapsed);
    noteSection("ui::frame");
    audio::update(msElapsed);
    voice::update();   // UNVERIFIED: where in the exe's frame the voice client's update runs is not traced
    noteSection("audio+voice");
}
// GameClient::PostRender (0x004d2580) calls TRRQS's update (FUN_00847790) with GameClient + 0x32c, the frame rate PreRender's ring
// gives (GetFramerate); render settings bit 12 ("Disable Real-time Scaling") stops it. The threshold (TRRQS + 0x50) is
// Client.Graphics.RealTimeQualityScaling as a float (FUN_004e61d0), read at start-up (default 15, FUN_004cd8c0) and by the Graphics
// observer on a change (FUN_00539b50) - read here each frame, which comes to the same.
void postRender() {
    float threshold = 15.0f;
    if (const std::string* v = optionValue("Client.Graphics.RealTimeQualityScaling"); v && !v->empty()) {
        std::string digits;
        for (char c : *v)
            if (c != ',') digits += c;
        threshold = float(std::atof(digits.c_str()));
    }
    rqs::setThreshold(threshold);
    rqs::update(frameRate.fps, renderBit(12));
}
bool renderUi(int pixelWidth, int pixelHeight, std::vector<UiCanvas::Quad>& quads) { return ui::renderUi(pixelWidth, pixelHeight, quads); }
void renderUiCpu(int pixelWidth, int pixelHeight, std::vector<uint8_t>& rgba) { ui::renderUiCpu(pixelWidth, pixelHeight, rgba); }
void renderScenes(SDL_GPUCommandBuffer* commands, SDL_GPUDevice* device, std::vector<UiCanvas::Quad>& quads, float dt) {
    ui::renderScenes(commands, device, quads, dt);
}
// Logical units: the root's size over the screen's (the screen scale, FUN_00897190).
static bool logical(float x, float y, int pixelWidth, int pixelHeight, int& lx, int& ly) {
    const ui::Widget* root = ui::manager().root();
    if (!root || pixelWidth <= 0 || pixelHeight <= 0) return false;
    lx = int(x * root->width / pixelWidth);
    ly = int(y * root->height / pixelHeight);
    return true;
}
void mouseMoved(float x, float y, int pixelWidth, int pixelHeight, int modifiers) {
    int lx, ly;
    if (logical(x, y, pixelWidth, pixelHeight, lx, ly)) ui::inputMouseMove(lx, ly, modifiers);
}
float displayGamma() {
    const std::string* v = optionValue("Client.Graphics.Gamma");
    std::string digits;
    if (v)
        for (char c : *v)
            if (c != ',') digits += c;
    const float value = digits.empty() ? 100.0f : float(std::atof(digits.c_str()));
    const float g = value * 0.5f * 0.01f + 0.5f;
    return g <= 0.0f ? 1.0f : g;
}
bool takeVideoRequest(VideoRequest& out) {
    if (!videoRequested) return false;
    out = videoRequest;
    videoRequest = VideoRequest();
    videoRequested = false;
    return true;
}
VideoRequest startupVideoMode(const std::string& root) {
    VideoRequest r;
    try {
        tr::ConfigFile cfg((std::filesystem::path(root) / "client.cfg").string());
        auto number = [&](const char* key, int fallback) {
            std::string v = cfg.value("Options", key, "");
            v.erase(std::remove(v.begin(), v.end(), ','), v.end());
            return v.empty() ? fallback : std::atoi(v.c_str());
        };
        auto flag = [&](const char* key, bool fallback) {
            const std::string v = lower(cfg.value("Options", key, ""));
            return v.empty() ? fallback : v == "true" || v == "1";
        };
        r.mode = true;
        r.fullscreen = flag("Client.Graphics.FullScreen", false);
        r.width = number(r.fullscreen ? "Client.Graphics.FullScreenResolution.Width" : "Client.Graphics.WindowedResolution.Width", 1024);
        r.height = number(r.fullscreen ? "Client.Graphics.FullScreenResolution.Height" : "Client.Graphics.WindowedResolution.Height", 768);
        r.refreshRate = number("Client.Graphics.FullScreenRefreshRate", 0);
        r.vsync = true;
        r.vsyncOn = flag("Client.Graphics.VSync", true);
        r.borderless = flag("Client.OpenRasa.BorderlessFullscreen", false);
        r.windowedX = number("Client.Graphics.WindowedX", 0);
        r.windowedY = number("Client.Graphics.WindowedY", 0);
    } catch (const std::exception&) {
        r.mode = false;
    }
    return r;
}
bool closeBlocked() {
    const std::string* v = optionValue("Client.Developer.KioskMode");
    return v && optionBoolText(*v);   // a missing option: not blocked
}

void windowMoved(int outerLeft, int outerTop) {
    setOption("Client.Graphics.WindowedX", groupedInt(outerLeft));
    setOption("Client.Graphics.WindowedY", groupedInt(outerTop));
}
void setSupportedSampleCounts(const std::vector<uint32_t>& counts) { deviceSampleCounts = counts; }
void screenResized(int pixelWidth, int pixelHeight) { ui::setScreenSize(pixelWidth, pixelHeight); }
void mouseLookMotion(float dx, float dy, int pixelWidth, int pixelHeight) {
    // The UI's mouse positions are logical units (the root's size over the screen's); MouseLookHandler takes their differences.
    const ui::Widget* root = ui::manager().root();
    if (!root || pixelWidth <= 0 || pixelHeight <= 0) return;
    player::mouseDelta(dx * float(root->width) / float(pixelWidth), dy * float(root->height) / float(pixelHeight));
}
// During mouse look the cursor sits at the root window's centre: StartMouseLook (0x004F5570) puts it there and MouseLookHandler
// (FUN_004f5950) re-centres it after each motion, so the window messages (WM_MOUSEWHEEL, buttons) carry the centre. SDL's relative
// mode keeps reporting the position from before the lock instead.
static void mouseLookCentre(float& x, float& y, int pixelWidth, int pixelHeight) {
    if (!player::mouseLook()) return;
    x = float(pixelWidth) * 0.5f;
    y = float(pixelHeight) * 0.5f;
}
void mouseWheel(int notches, float x, float y, int pixelWidth, int pixelHeight, int modifiers) {
    mouseLookCentre(x, y, pixelWidth, pixelHeight);
    if (std::getenv("OPENRASA_WHEELLOG")) std::printf("[wheel] %d at %.0f %.0f mouse look %d\n", notches, x, y, int(player::mouseLook()));
    int lx, ly;
    if (logical(x, y, pixelWidth, pixelHeight, lx, ly)) ui::inputMouseWheel(notches, lx, ly, modifiers);
}
void mouseButton(int button, bool down, float x, float y, int pixelWidth, int pixelHeight, int modifiers) {
    mouseLookCentre(x, y, pixelWidth, pixelHeight);
    int lx, ly;
    if (!logical(x, y, pixelWidth, pixelHeight, lx, ly)) return;
    // uievent.mouse.eButton1 / 2 / 3 = 1 / 2 / 4 (FUN_00902640: DirectInput buttons 0 / 1 / 2 / 3 -> 1 / 2 / 4 / 8). The UI's events
    // come from the Win32 mouse device (FUN_00902870): WM_XBUTTONDOWN / UP / DBLCLK with XBUTTON1 -> 8 ("mouse.button4"), XBUTTON2 ->
    // 0x10 ("mouse.button5") - SDL's X1 / X2.
    // OpenRasa: further buttons (SDL 6 and up - gaming mice, macro pads) as 0x10000 + n, "mouse.button<n>" (UiInput.cpp mouseString).
    const int id = button == 1 ? 1 : button == 3 ? 2 : button == 2 ? 4 : button == 4 ? 8 : button == 5 ? 0x10
                 : button >= 6 && button < 256 ? 0x10000 + button : 0;
    if (id) ui::inputMouseButton(id, down, lx, ly, modifiers);
}
bool timedStep() { return timedStepThisFrame; }
uint32_t timedStepElapsedMs() { return timedStepMs; }
void keyRepeated(int sdlScancode, int modifiers, int raw) {
    if (int code = ui::keyCodeForSdl(sdlScancode, raw)) ui::inputKeyRepeat(code, modifiers);
}
void keyPressed(int sdlScancode, bool down, int modifiers, int raw) {
    // The exe's keys by their DirectInput codes; OpenRasa also passes every other key SDL reports, with an extended code and name, so
    // any key can be bound (ui::keyCodeForSdl).
    if (int code = ui::keyCodeForSdl(sdlScancode, raw)) ui::inputKey(code, down, modifiers);
}
void mouseTilt(int notches, float x, float y, int pixelWidth, int pixelHeight, int modifiers) {
    int lx, ly;
    if (logical(x, y, pixelWidth, pixelHeight, lx, ly)) ui::inputMouseTilt(notches, lx, ly, modifiers);
}
void textEntered(const char* utf8, int modifiers) {
    // UTF-8 -> UTF-16 code units, one eKeyChar event each.
    const unsigned char* s = reinterpret_cast<const unsigned char*>(utf8);
    while (*s) {
        uint32_t c = *s++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= 0x3F >> extra;
        for (; extra > 0 && *s; --extra) c = c << 6 | (*s++ & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            ui::inputChar(char16_t(0xD800 + (c >> 10)), modifiers);
            ui::inputChar(char16_t(0xDC00 + (c & 0x3FF)), modifiers);
        } else {
            ui::inputChar(char16_t(c), modifiers);
        }
    }
}
namespace {
std::u16string utf16Of(const char* utf8) {
    std::u16string out;
    const unsigned char* s = reinterpret_cast<const unsigned char*>(utf8 ? utf8 : "");
    while (*s) {
        uint32_t c = *s++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= 0x3F >> extra;
        for (; extra > 0 && *s; --extra) c = c << 6 | (*s++ & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            out += char16_t(0xD800 + (c >> 10));
            out += char16_t(0xDC00 + (c & 0x3FF));
        } else {
            out += char16_t(c);
        }
    }
    return out;
}
}  // namespace
void textEditing(const char* utf8, int cursor) { ui::imeEditing(utf16Of(utf8), cursor); }
bool textCommitted(const char* utf8) { return ui::imeCommit(utf16Of(utf8)); }
void textCandidates(const char* const* candidates, int count, int selected) {
    std::vector<std::u16string> list;
    for (int i = 0; i < count; ++i) list.push_back(utf16Of(candidates[i]));
    ui::imeCandidates(list, selected);
}
void keyboardLayoutChanged() { ui::imeLanguageChanged(); }
std::string stubReport() {
    const std::string stubs = stubCallReport(), uiCalls = ui::unimplementedCallReport();
    if (stubs.empty() && uiCalls.empty()) return {};
    return stubs + "\n-- UI methods not implemented yet --\n" + uiCalls;
}

int videoQuality() { return videoQualityLevel; }

bool takeScreenshotRequest(std::string& path, int& format) {
    if (screenshotPath.empty()) return false;
    path = screenshotPath;
    format = screenshotFormat;
    screenshotPath.clear();
    return true;
}

bool renderSettingBit(uint32_t index) { return renderBit(index); }

}  // namespace engine
