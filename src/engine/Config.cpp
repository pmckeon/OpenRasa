// Config.cpp - TRasa::Config (traced 2026-10-28 in tabula_rasa.exe; see Config.h).
//
// Storage (the object): + 4 a stdext hash_map of sections (each: its items, another hash_map; + 0x4c the item list), + 0x2c the
// .define variables (the same kind of table), + 0x54 / + 0x58 the include count / limit (20), + 0x60 search paths (empty for the
// configs the engine makes: nothing adds one between their creation and import), + 0x6c the current section's name, + 0x8c the
// export order (0x2c-byte records: section name, the keys in the order SetItem added them). Lookups ignore case (djb2 of the
// lower-cased name, buckets sorted with _stricmp); a section or key keeps the spelling it was first added with. GetAllSections /
// GetAllItems list the tables in their own order (tr::configKeyOrder, the order traced for [Paths] in tabula_rasa.cfg).
#include "Config.h"

#include "assetlibrary.h"

#include <cctype>
#include <cmath>
#include <ctime>
#include <sstream>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace engine::config {
namespace {

bool equalIgnoringCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(uint8_t(a[i])) != std::tolower(uint8_t(b[i]))) return false;
    return true;
}

// The CRT's character classes for a char the parser passes sign-extended: bytes above 0x7F come in negative, which the exe's
// isalnum / islower index outside their table (UNVERIFIED what they read there) - taken as no class.
bool isAlnum(int c) { return c >= 0 && c < 0x80 && std::isalnum(c); }
bool isLower(int c) { return c >= 0 && c < 0x80 && std::islower(c); }
bool isWordChar(int c) { return c != 0 && std::strchr(".,_-@:/", c) != nullptr; }   // 0x00bd7dc0

// The bare-word terminators (std::string at 0x00d46c54, from 0x00bd7dc8).
const char* const kWordEnd = " \t\r\n[]%#;=";

struct Item {
    std::string key, value;
};
struct Section {
    std::string name;
    std::vector<Item> items;   // as added (the table's order: tr::configKeyOrder)
};
struct ExportRecord {
    std::string name;
    std::vector<std::string> keys;
};

void log(const std::string& text) { std::fprintf(stderr, "[config] %s\n", text.c_str()); }

std::string utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// fopen(path, "r") in the exe: Windows names ignore case - on other systems each part is matched without case when the name as
// given does not exist.
std::filesystem::path findPath(const std::string& name) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::u8path(name);
    if (std::filesystem::exists(p, ec)) return p;
    std::filesystem::path out = p.is_absolute() ? p.root_path() : std::filesystem::path(".");
    for (const auto& part : p.relative_path()) {
        const std::string want = utf8(part);
        if (want == "." || want == "..") {
            out /= part;
            continue;
        }
        std::filesystem::path found;
        for (const auto& e : std::filesystem::directory_iterator(out, ec))
            if (equalIgnoringCase(utf8(e.path().filename()), want)) {
                found = e.path();
                break;
            }
        if (found.empty()) return p;
        out = found;
    }
    return out;
}

// The file as fopen(..., "r") (text mode, the Microsoft CRT) reads it: CR LF as LF, a Ctrl+Z ends it.
bool readTextFile(const std::string& name, std::string& out) {
    std::ifstream in(findPath(name), std::ios::binary);
    if (!in) return false;
    const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out.clear();
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == 0x1a) break;
        if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') continue;
        out += raw[i];
    }
    return true;
}

class Config;

// The lexer (FUN_0048cc50, state FUN_0048cb70: + 0 the file, + 4 the line, + 8 characters pushed back, + 0x24 the token's text).
// Tokens: 0 end of file, 0x100 end of line, 0x101 a string, 0x102 a $variable, 0x103 a %section%key reference, 0x104 .include,
// 0x105 .includeoptional, 0x106 .define, 0x107 .section, 0x108 .importenv; '=', '[', ']' as themselves; anything else it could
// not read as a token comes back as that character.
struct Lexer {
    const std::string& data;
    size_t pos = 0;
    int line = 1;
    std::string pushback;
    std::string text;

    explicit Lexer(const std::string& d) : data(d) {}

    // FUN_0048ce30: a pushed-back character first (sign-extended), else fgetc; end of file and a NUL byte are 0.
    int get() {
        int c;
        if (!pushback.empty()) {
            c = int(static_cast<signed char>(pushback[0]));
            pushback.erase(0, 1);
        } else {
            if (pos >= data.size()) return 0;
            c = int(uint8_t(data[pos++]));
        }
        if (c == '\n') ++line;
        return c;
    }
    void unget(int c) {
        if (c == '\n' && line > 1) --line;
        pushback.push_back(char(c));
    }
    // FUN_0048d120: characters up to one of `end` (pushed back; none: up to the end of the file); a backslash takes the next
    // character as it is.
    int word(const char* end) {
        bool escaped = false;
        for (;;) {
            const int c = get();
            if (c == 0) return 0x101;
            if (escaped) {
                text += char(c);
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (*end && std::strchr(end, c) && c != 0) {
                unget(c);
                return 0x101;
            }
            text += char(c);
        }
    }
    // FUN_0048d1b0 ('"') / FUN_0048d2e0 ('`'): the quote, a word up to the same quote, the closing quote (anything else is put back).
    int quoted(char q) {
        const int c = get();
        if (c != q) return c;
        const char end[2] = {q, 0};
        word(end);
        const int close = get();
        if (close != q) unget(close);
        return 0x101;
    }
    // FUN_0048d250: a variable's name - quoted, or a word starting with a letter, digit or one of ".,_-@:/".
    int variableName() {
        const int c = get();
        if (c == '"') {
            pushback.push_back('"');
            return quoted('"');
        }
        if (!isAlnum(c) && !isWordChar(c)) {
            unget(c);
            return c;
        }
        if (c != 0) text += char(c);
        return word(kWordEnd);
    }
    int next() {
        text.clear();
        for (;;) {
            const int c = get();
            switch (c) {
                case 0: return 0;
                case '\t':
                case '\r':
                case ' ': continue;
                case '\n': return 0x100;
                case '"':
                    pushback.push_back('"');
                    return quoted('"');
                case '`':
                    pushback.push_back('`');
                    return quoted('`');
                case '#':
                case ';':
                    // A comment to the end of the line, read from the file itself (fgetc); its newline is pushed back.
                    while (pos < data.size()) {
                        const int d = uint8_t(data[pos++]);
                        if (d == '\n') {
                            unget('\n');
                            break;
                        }
                    }
                    continue;
                case '$': {   // FUN_0048d380
                    pushback.push_back('$');
                    get();
                    if (variableName() != 0x101) {
                        pushback += text;
                        text.clear();
                        return '$';
                    }
                    return 0x102;
                }
                case '%': {   // FUN_0048d3c0: %section%key -> "section%key"
                    pushback.push_back('%');
                    get();
                    word(kWordEnd);
                    if (get() != '%') {
                        pushback.push_back('%');
                        pushback += text;
                        text.clear();
                        return '%';
                    }
                    text += '%';
                    word(kWordEnd);
                    return 0x103;
                }
                case '.': {   // FUN_0048cef0: '.' and lower-case letters name a directive; anything else goes back unread
                    pushback.push_back('.');
                    get();
                    text += '.';
                    int d = get();
                    while (isLower(d)) {
                        if (d != 0) text += char(d);
                        d = get();
                    }
                    unget(d);
                    if (text == ".include") return 0x104;
                    if (text == ".includeoptional") return 0x105;
                    if (text == ".define") return 0x106;
                    if (text == ".section") return 0x107;
                    if (text == ".importenv") return 0x108;
                    pushback += text;
                    text.clear();
                    return get();
                }
                case '=':
                case '[':
                case ']': return c;
                default:
                    unget(c);
                    return word(kWordEnd);
            }
        }
    }
};

class Config {
public:
    std::vector<Section> sections;
    std::vector<ExportRecord> exportOrder;
    std::vector<Item> variables;
    std::string current;      // + 0x6c
    int includes = 0;         // + 0x54, never lowered
    int includeLimit = 20;    // + 0x58

    Section* section(const std::string& name) {
        for (Section& s : sections)
            if (equalIgnoringCase(s.name, name)) return &s;
        return nullptr;
    }
    Item* item(const std::string& sectionName, const std::string& key) {   // FUN_00478050 / FUN_00477fc0
        Section* s = section(sectionName);
        if (!s) return nullptr;
        for (Item& i : s->items)
            if (equalIgnoringCase(i.key, key)) return &i;
        return nullptr;
    }
    Section& addSection(const std::string& name) {
        if (Section* s = section(name)) return *s;
        sections.push_back(Section{name, {}});
        return sections.back();
    }
    // FUN_00478430: an existing item gets the value; a new one goes into its section (made when missing) and the export order
    // (the section's record found with _stricmp, else appended).
    void set(const std::string& sectionName, const std::string& key, const std::string& value) {
        if (Item* i = item(sectionName, key)) {
            i->value = value;
            return;
        }
        addSection(sectionName).items.push_back(Item{key, value});
        ExportRecord* record = nullptr;
        for (ExportRecord& r : exportOrder)
            if (equalIgnoringCase(r.name, sectionName)) record = &r;
        if (!record) {
            exportOrder.push_back(ExportRecord{sectionName, {}});
            record = &exportOrder.back();
        }
        record->keys.push_back(key);
    }
    // FUN_00477710: the source's items inserted into the target (made when missing); keys the target has are kept; nothing when
    // the source does not exist. The export order is not touched (the items go straight into the table).
    void copySection(const std::string& source, const std::string& target) {
        if (!section(source)) return;
        addSection(target);
        const std::vector<Item> items = section(source)->items;
        Section& t = *section(target);
        for (const Item& i : items) {
            bool have = false;
            for (const Item& k : t.items) have = have || equalIgnoringCase(k.key, i.key);
            if (!have) t.items.push_back(i);
        }
    }
    const Item* variable(const std::string& name) const {
        for (const Item& v : variables)
            if (equalIgnoringCase(v.key, name)) return &v;
        return nullptr;
    }
    void define(const std::string& name, const std::string& value) {
        for (Item& v : variables)
            if (equalIgnoringCase(v.key, name)) {
                v.value = value;
                return;
            }
        variables.push_back(Item{name, value});
    }

    bool importFile(const std::string& path, bool optional);
    bool exportFile(const std::string& path) const;

private:
    bool keyValue(Lexer& lex);
    bool sectionStatement(Lexer& lex);
};

// FUN_00476b10 FinishKeyValueStmt: key '=' (string | $variable | %section%key | nothing) then end of line or file.
bool Config::keyValue(Lexer& lex) {
    const std::string key = lex.text;
    int t = lex.next();
    if (t != '=') {
        log("Expected token '=', got token '" + lex.text + "'!");
        return false;
    }
    std::string value;
    t = lex.next();
    if (t != 0x100 && t != 0) {
        if (t < 0x101 || t > 0x103) {
            log("Expecting string, variable, reference, or EOL/EOF, got '" + lex.text + "'!");
            return false;
        }
        const std::string text = lex.text;
        const int after = lex.next();
        if (after != 0x100 && after != 0) return false;
        if (t == 0x102) {
            const Item* v = variable(text);
            if (!v) {
                log("Undefined variable (" + text + ") used by key (" + key + ")");
                return false;
            }
            value = v->value;
        } else if (t == 0x103) {
            const size_t split = text.find('%');
            if (split == std::string::npos) {
                log("Parse error: invalid reference (" + text + ")");
                return false;
            }
            const Item* i = item(text.substr(0, split), text.substr(split + 1));
            if (!i) {
                log("Undefined reference (" + text + ") used by key (" + key + ")");
                return false;
            }
            value = i->value;
        } else {
            value = text;
        }
    }
    set(current, key, value);
    return true;
}

// FUN_00477ca0 FinishSectionStmt: '[' name ['=' source] ']' - the source's items copied in, the section made, the current section.
bool Config::sectionStatement(Lexer& lex) {
    if (lex.next() != 0x101) {
        log("Expected TOK_STRING for section name!");
        return false;
    }
    const std::string name = lex.text;
    int t = lex.next();
    if (t == '=') {
        if (lex.next() != 0x101) {
            log("Expected TOK_STRING for source section name!");
            return false;
        }
        copySection(lex.text, name);
        t = lex.next();
    }
    if (t != ']') {
        log("Expected ']' to end section definition!");
        return false;
    }
    addSection(name);
    current = name;
    return true;
}

// FUN_00476160 ImportFile(path, optional): a missing file (also through the search paths - none here) is success only when
// optional. A failed statement logs and ends the import (what was read so far stays).
bool Config::importFile(const std::string& path, bool optional) {
    std::string data;
    if (!readTextFile(path, data)) return optional;
    Lexer lex(data);
    auto fail = [&](const char* what) {
        char text[512];
        std::snprintf(text, sizeof(text), what, path.c_str(), lex.line);
        log(text);
        return false;
    };
    for (;;) {
        const int t = lex.next();
        switch (t) {
            case 0: return true;
            case 0x100: continue;
            case '[':
                if (!sectionStatement(lex)) return fail("In \"%s:%d\" problem with section definition!");
                continue;
            case 0x101:
                if (current.empty()) return fail("In \"%s:%d\" (key, value) pair appeared before a section definition!");
                if (!keyValue(lex)) return fail("In \"%s:%d\" problem with (key,value) statement!");
                continue;
            case 0x102: return fail("In %s:%d variable outside of a .define or (key,value) pair!");
            case 0x103: return fail("In %s:%d item reference outside of a .define or (key,value) pair!");
            case 0x104:
            case 0x105: {   // FUN_00477250: the count goes up per include and never down
                const bool includeOptional = t == 0x105;
                bool ok = false;
                if (lex.next() != 0x101) {
                    log("Expected TOK_STRING for .include directive");
                } else if (includes + 1 > includeLimit) {
                    log("Maximum include depth exceeded!  Not including \"" + lex.text + "\"");
                } else {
                    ++includes;
                    ok = importFile(lex.text, includeOptional);
                }
                if (!ok)
                    return fail(includeOptional ? "In \"%s:%d\" problem with .includeoptional statement!"
                                                : "In \"%s:%d\" problem with .include statement!");
                continue;
            }
            case 0x106: {   // FUN_004773b0: .define name value
                bool ok = false;
                if (lex.next() != 0x101) {
                    log("Expected TOK_STRING for .define variable name");
                } else {
                    const std::string name = lex.text;
                    if (lex.next() != 0x101) {
                        log("Expected TOK_STRING for .define variable value");
                    } else {
                        define(name, lex.text);
                        ok = true;
                    }
                }
                if (!ok) return fail("In \"%s:%d\" problem with .define statment!");
                continue;
            }
            case 0x107: {   // FUN_00477a70: .section target = source
                bool ok = false;
                if (lex.next() != 0x101) {
                    log("Expected TOK_STRING for .section target name");
                } else {
                    const std::string target = lex.text;
                    if (lex.next() != '=') {
                        log("Expected '=' in .section statement");
                    } else if (lex.next() != 0x101) {
                        log("Expected TOK_STRING for .section source name");
                    } else {
                        copySection(lex.text, target);
                        ok = true;
                    }
                }
                if (!ok) return fail("In \"%s:%d\" problem with .section statment!");
                continue;
            }
            case 0x108: {   // FUN_004775d0: .importenv NAME -> the variable NAME = getenv(NAME), "" when unset
                if (lex.next() != 0x101) {
                    log("Expected TOK_STRING for .importenv directive");
                    return fail("In \"%s:%d\" problem with .importenv statment!");
                }
                const char* env = std::getenv(lex.text.c_str());
                define(lex.text, env ? env : "");
                continue;
            }
            default: {
                char text[512];
                std::snprintf(text, sizeof(text), "In %s:%d unexpected token '%s'!", path.c_str(), lex.line, lex.text.c_str());
                log(text);
                return false;
            }
        }
    }
}

// FUN_00475d40: as it is when every character is a letter, digit or one of ".,_-@:/" and it does not start with '.', else in
// double quotes with '\' and '"' escaped; an empty string writes nothing.
std::string quote(const std::string& s) {
    bool bare = true;
    for (char c : s) bare = bare && (isAlnum(int(static_cast<signed char>(c))) || isWordChar(int(static_cast<signed char>(c))));
    if (bare && (s.empty() || s[0] != '.')) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out + "\"";
}

// FUN_00476780 ExportFile: the export order's sections (each "[name]", its keys "key = value", an empty line), written in text mode
// (LF as CR LF). Section comments (a map at + 0x94 written as '#' lines) are not set by anything traced.
bool Config::exportFile(const std::string& path) const {
    std::string out;
    for (const ExportRecord& r : exportOrder) {
        out += "[" + quote(r.name) + "]\n";
        for (const std::string& key : r.keys) {
            const Item* i = const_cast<Config*>(this)->item(r.name, key);
            if (!i) {
                log("Failed to find item: " + r.name + "." + key);
                continue;
            }
            out += quote(key) + " = " + quote(i->value) + "\n";
        }
        out += "\n";
    }
    std::ofstream f(findPath(path), std::ios::binary | std::ios::trunc);
    if (!f) {
        log("Failed to open file \"" + path + "\" for export!");
        return false;
    }
    for (char c : out) {
        if (c == '\n') f.put('\r');
        f.put(c);
    }
    return bool(f);
}

// ---- The Python class -------------------------------------------------------------------------------------------------------------
struct ConfigObject {
    PyObject_HEAD
    Config* config;
};
PyTypeObject configType = {PyObject_HEAD_INIT(nullptr)};

Config& of(PyObject* self) { return *reinterpret_cast<ConfigObject*>(self)->config; }

// Boost.Python's argument check (UNVERIFIED wording, as Engine.cpp's argCount / argError).
bool argumentsAre(PyObject* args, const char* method, const char* kinds) {
    const int n = int(std::strlen(kinds));
    char text[256];
    if (PyTuple_GET_SIZE(args) != n) {
        PyOS_snprintf(text, sizeof(text), "Python argument types in Config.%s did not match C++ signature (%d argument(s) expected)",
                      method, n + 1);
        PyErr_SetString(PyExc_TypeError, text);
        return false;
    }
    for (int i = 0; i < n; ++i) {
        PyObject* o = PyTuple_GET_ITEM(args, i);
        bool ok = true;
        const char* wanted = "std::string";
        switch (kinds[i]) {
            case 's': ok = PyString_Check(o); break;
            case 'i': ok = PyInt_Check(o) || PyLong_Check(o); wanted = "int"; break;
            case 'f': ok = PyFloat_Check(o) || PyInt_Check(o) || PyLong_Check(o); wanted = "float"; break;
            case 'b': ok = PyInt_Check(o) || PyLong_Check(o); wanted = "bool"; break;
        }
        if (!ok) {
            PyOS_snprintf(text, sizeof(text), "Python argument types in Config.%s did not match C++ signature (argument %d: %s)",
                          method, i + 2, wanted);
            PyErr_SetString(PyExc_TypeError, text);
            return false;
        }
    }
    return true;
}
std::string str(PyObject* args, int i) {
    PyObject* o = PyTuple_GET_ITEM(args, i);
    return std::string(PyString_AS_STRING(o), size_t(PyString_GET_SIZE(o)));
}

PyObject* configNew(PyTypeObject* type, PyObject*, PyObject*) {   // the default constructor (FUN_00475e10): empty
    PyObject* self = type->tp_alloc(type, 0);
    if (self) reinterpret_cast<ConfigObject*>(self)->config = new Config;
    return self;
}
void configDealloc(PyObject* self) {
    delete reinterpret_cast<ConfigObject*>(self)->config;
    self->ob_type->tp_free(self);
}

// GetItemAsString(section, key, default) (0x00484640 -> FUN_004780e0): the value, else the default.
PyObject* getItemAsString(PyObject* self, PyObject* args) {
    if (!argumentsAre(args, "GetItemAsString", "sss")) return nullptr;
    const Item* i = of(self).item(str(args, 0), str(args, 1));
    const std::string v = i ? i->value : str(args, 2);
    return PyString_FromStringAndSize(v.data(), int(v.size()));
}
// GetItemAsInt (FUN_00484670): strtol(value, 0, base 0) - a 32-bit long (out of range: LONG_MAX / LONG_MIN), else the default.
PyObject* getItemAsInt(PyObject* self, PyObject* args) {
    if (!argumentsAre(args, "GetItemAsInt", "ssi")) return nullptr;
    const Item* i = of(self).item(str(args, 0), str(args, 1));
    if (!i) {
        const long fallback = PyInt_AsLong(PyTuple_GET_ITEM(args, 2));
        if (fallback == -1 && PyErr_Occurred()) return nullptr;
        return PyInt_FromLong(fallback);
    }
    long long v = std::strtoll(i->value.c_str(), nullptr, 0);
    if (v > 2147483647LL) v = 2147483647LL;
    if (v < -2147483648LL) v = -2147483648LL;
    return PyInt_FromLong(long(v));
}
// GetItemAsFloat (FUN_004846f0): strtod as a float, else the default.
PyObject* getItemAsFloat(PyObject* self, PyObject* args) {
    if (!argumentsAre(args, "GetItemAsFloat", "ssf")) return nullptr;
    const Item* i = of(self).item(str(args, 0), str(args, 1));
    const float v = i ? float(std::strtod(i->value.c_str(), nullptr)) : float(PyFloat_AsDouble(PyTuple_GET_ITEM(args, 2)));
    return PyFloat_FromDouble(double(v));
}
// GetItemAsBool (FUN_00478160): the lower-cased value is "true", "yes", "on" or "1"; else the default.
PyObject* getItemAsBool(PyObject* self, PyObject* args) {
    if (!argumentsAre(args, "GetItemAsBool", "ssb")) return nullptr;
    const Item* i = of(self).item(str(args, 0), str(args, 1));
    if (!i) return PyBool_FromLong(PyObject_IsTrue(PyTuple_GET_ITEM(args, 2)));
    std::string v = i->value;
    for (char& c : v) c = char(std::tolower(uint8_t(c)));
    return PyBool_FromLong(v == "true" || v == "yes" || v == "on" || v == "1");
}
PyObject* hasItemMethod(PyObject* self, PyObject* args) {
    if (!argumentsAre(args, "HasItem", "ss")) return nullptr;
    return PyBool_FromLong(of(self).item(str(args, 0), str(args, 1)) != nullptr);
}
PyObject* setItem(PyObject* self, PyObject* args) {
    if (!argumentsAre(args, "SetItem", "sss")) return nullptr;
    of(self).set(str(args, 0), str(args, 1), str(args, 2));
    Py_RETURN_NONE;
}
PyObject* setItemFromInt(PyObject* self, PyObject* args) {   // "%d"
    if (!argumentsAre(args, "SetItemFromInt", "ssi")) return nullptr;
    const long v = PyInt_AsLong(PyTuple_GET_ITEM(args, 2));
    if (v == -1 && PyErr_Occurred()) return nullptr;
    char text[32];
    std::snprintf(text, sizeof(text), "%d", int(v));
    of(self).set(str(args, 0), str(args, 1), text);
    Py_RETURN_NONE;
}
PyObject* setItemFromBool(PyObject* self, PyObject* args) {   // "true" / "false"
    if (!argumentsAre(args, "SetItemFromBool", "ssb")) return nullptr;
    of(self).set(str(args, 0), str(args, 1), PyObject_IsTrue(PyTuple_GET_ITEM(args, 2)) ? "true" : "false");
    Py_RETURN_NONE;
}
// SetItemFromFloat (FUN_00484830): "%g" of the float - the exe's CRT writes a three-digit exponent (1e+006).
PyObject* setItemFromFloat(PyObject* self, PyObject* args) {
    if (!argumentsAre(args, "SetItemFromFloat", "ssf")) return nullptr;
    const float v = float(PyFloat_AsDouble(PyTuple_GET_ITEM(args, 2)));
    char text[64];
    std::snprintf(text, sizeof(text), "%g", double(v));
    std::string s = text;
    const size_t e = s.find_first_of("eE");
    if (e != std::string::npos && e + 1 < s.size()) {
        const size_t digits = e + 2;
        while (s.size() - digits < 3) s.insert(digits, "0");
    }
    of(self).set(str(args, 0), str(args, 1), s);
    Py_RETURN_NONE;
}
PyObject* nameList(const std::vector<std::string>& names) {
    PyObject* list = PyList_New(0);
    for (const std::string& n : tr::configKeyOrder(names)) {
        PyObject* s = PyString_FromStringAndSize(n.data(), int(n.size()));
        PyList_Append(list, s);
        Py_DECREF(s);
    }
    return list;
}
PyObject* getAllSections(PyObject* self, PyObject* args) {   // FUN_00484880
    if (!argumentsAre(args, "GetAllSections", "")) return nullptr;
    std::vector<std::string> names;
    for (const Section& s : of(self).sections) names.push_back(s.name);
    return nameList(names);
}
PyObject* getAllItems(PyObject* self, PyObject* args) {   // FUN_00484940: the section's keys (an unknown section: [])
    if (!argumentsAre(args, "GetAllItems", "s")) return nullptr;
    std::vector<std::string> names;
    if (const Section* s = of(self).section(str(args, 0)))
        for (const Item& i : s->items) names.push_back(i.key);
    return nameList(names);
}
PyObject* importFileMethod(PyObject* self, PyObject* args) {   // FUN_00484560
    if (!argumentsAre(args, "ImportFile", "s")) return nullptr;
    if (!of(self).importFile(str(args, 0), false)) {
        PyErr_Format(PyExc_RuntimeError, "Unable to import file: %s", str(args, 0).c_str());
        return nullptr;
    }
    Py_RETURN_NONE;
}
PyObject* exportFileMethod(PyObject* self, PyObject* args) {   // FUN_004845d0
    if (!argumentsAre(args, "ExportFile", "s")) return nullptr;
    if (!of(self).exportFile(str(args, 0))) {
        PyErr_Format(PyExc_RuntimeError, "Unable to export file: %s", str(args, 0).c_str());
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyMethodDef configMethods[] = {
    {const_cast<char*>("ImportFile"), importFileMethod, METH_VARARGS, nullptr},
    {const_cast<char*>("ExportFile"), exportFileMethod, METH_VARARGS, nullptr},
    {const_cast<char*>("GetItemAsString"), getItemAsString, METH_VARARGS, nullptr},
    {const_cast<char*>("GetItemAsInt"), getItemAsInt, METH_VARARGS, nullptr},
    {const_cast<char*>("GetItemAsBool"), getItemAsBool, METH_VARARGS, nullptr},
    {const_cast<char*>("GetItemAsFloat"), getItemAsFloat, METH_VARARGS, nullptr},
    {const_cast<char*>("HasItem"), hasItemMethod, METH_VARARGS, nullptr},
    {const_cast<char*>("SetItem"), setItem, METH_VARARGS, nullptr},
    {const_cast<char*>("SetItemFromInt"), setItemFromInt, METH_VARARGS, nullptr},
    {const_cast<char*>("SetItemFromBool"), setItemFromBool, METH_VARARGS, nullptr},
    {const_cast<char*>("SetItemFromFloat"), setItemFromFloat, METH_VARARGS, nullptr},
    {const_cast<char*>("GetAllSections"), getAllSections, METH_VARARGS, nullptr},
    {const_cast<char*>("GetAllItems"), getAllItems, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

// CreateWrapper (0x00484a40): its argument.
PyObject* createWrapper(PyObject*, PyObject* args) {
    if (PyTuple_GET_SIZE(args) != 1) {
        PyErr_SetString(PyExc_TypeError, "CreateWrapper takes exactly one argument");
        return nullptr;
    }
    PyObject* o = PyTuple_GET_ITEM(args, 0);
    Py_INCREF(o);
    return o;
}
PyMethodDef moduleMethods[] = {
    {const_cast<char*>("CreateWrapper"), createWrapper, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

bool ready() {
    if (configType.tp_name) return true;
    configType.tp_name = const_cast<char*>("extern_util_config.Config");
    configType.tp_basicsize = sizeof(ConfigObject);
    configType.tp_flags = Py_TPFLAGS_DEFAULT;
    configType.tp_methods = configMethods;
    configType.tp_new = configNew;
    configType.tp_dealloc = configDealloc;
    if (PyType_Ready(&configType) < 0) {
        PyErr_Print();
        return false;
    }
    return true;
}

}  // namespace

void initModule() {
    if (!ready()) return;
    PyObject* m = Py_InitModule4(const_cast<char*>("extern_util_config"), moduleMethods, nullptr, nullptr, PYTHON_API_VERSION);
    Py_INCREF(&configType);
    PyModule_AddObject(m, const_cast<char*>("Config"), reinterpret_cast<PyObject*>(&configType));
}

PyObject* newConfigFromFile(const char* path) {
    if (!ready()) return nullptr;
    PyObject* self = configNew(&configType, nullptr, nullptr);
    if (!self) {
        PyErr_Clear();
        return nullptr;
    }
    if (!of(self).importFile(path, false)) {
        Py_DECREF(self);
        return nullptr;
    }
    return self;
}

namespace {
PyObject* appConfigObject = nullptr;
PyObject* loginConfigObject = nullptr;
}  // namespace

bool createEngineConfigs() {
    appConfigObject = newConfigFromFile("tabula_rasa.cfg");
    if (!appConfigObject) {
        std::fprintf(stderr, "\n******The application config file (tabula_rasa.cfg) could not be imported - its probably missing from the "
                             "directory this is running out of\n\n");
        return false;
    }
    loginConfigObject = newConfigFromFile("login.cfg");
    if (!loginConfigObject) {
        std::fprintf(stderr, "\n******The login config file (login.cfg) could not be imported - its probably missing from the directory "
                             "this is running out of\n\n");
        return false;
    }
    return true;
}
PyObject* appConfig() { return appConfigObject; }
PyObject* loginConfig() { return loginConfigObject; }
void releaseEngineConfigs() {
    Py_XDECREF(appConfigObject);
    Py_XDECREF(loginConfigObject);
    appConfigObject = loginConfigObject = nullptr;
}

// GenerateBookmark (0x004d7530): OutputDebugString "Generating Bookmark...", a new Config with TRBookmarks.bmf (in the current folder)
// imported ("TRBookmarks.bmf not found, it'll be created" when that fails); n = strtol(Bookmarks.NumBookmarks, 0, 0) or 0. Then,
// only when GameUIManager + 8 (the scene's view) exists: the view node (+ 0xb0) gives LocX..Z = its position (+ 0xa4) and RotW..Z =
// D3DXQuaternionRotationMatrix of its matrix; in [Bookmarks] "Book<n>-Name" = "<localtime %d-%02d-%02d %02d:%02d:%02d> AutoBookmark",
// "-MapId" = Client.Developer.LastMapTemplate read by an istream (unset or unreadable: -1) as "%d", "-LocX", "-LocY", "-LocZ", "-RotW",
// "-RotX", "-RotY", "-RotZ" as "%g" (FUN_004788c0), NumBookmarks = n + 1, ExportFile; "Failed to Export TRBookmarks.bmf" / "<name>
// Created" as debug output.
void generateBookmark(bool camera, const float eye[3], const float rotation[4], const std::string* lastMapTemplate) {
    std::fprintf(stderr, "[config] Generating Bookmark...\n");
    Config c;
    if (!c.importFile("TRBookmarks.bmf", false)) std::fprintf(stderr, "[config] TRBookmarks.bmf not found, it'll be created\n");
    long count = 0;
    if (const Item* i = c.item("Bookmarks", "NumBookmarks")) {
        long long v = std::strtoll(i->value.c_str(), nullptr, 0);
        count = long(v > 2147483647LL ? 2147483647LL : v < -2147483648LL ? -2147483648LL : v);
    }
    if (!camera) return;
    // D3DXMatrixRotationQuaternion, then D3DXQuaternionRotationMatrix (the largest-diagonal branches as Wine's d3dx9 writes them -
    // UNVERIFIED against the native D3DX).
    const float x = rotation[0], y = rotation[1], z = rotation[2], w = rotation[3];
    const float m11 = 1 - 2 * (y * y + z * z), m12 = 2 * (x * y + z * w), m13 = 2 * (x * z - y * w);
    const float m21 = 2 * (x * y - z * w), m22 = 1 - 2 * (x * x + z * z), m23 = 2 * (y * z + x * w);
    const float m31 = 2 * (x * z + y * w), m32 = 2 * (y * z - x * w), m33 = 1 - 2 * (x * x + y * y);
    float q[4];   // x y z w
    const float trace = m11 + m22 + m33 + 1.0f;
    if (trace > 1.0f) {
        const float r = 2.0f * std::sqrt(trace);
        q[0] = (m23 - m32) / r;
        q[1] = (m31 - m13) / r;
        q[2] = (m12 - m21) / r;
        q[3] = 0.25f * r;
    } else if (m11 >= m22 && m11 >= m33) {
        const float r = 2.0f * std::sqrt(1.0f + m11 - m22 - m33);
        q[0] = 0.25f * r;
        q[1] = (m12 + m21) / r;
        q[2] = (m13 + m31) / r;
        q[3] = (m23 - m32) / r;
    } else if (m22 >= m33) {
        const float r = 2.0f * std::sqrt(1.0f + m22 - m11 - m33);
        q[0] = (m12 + m21) / r;
        q[1] = 0.25f * r;
        q[2] = (m23 + m32) / r;
        q[3] = (m31 - m13) / r;
    } else {
        const float r = 2.0f * std::sqrt(1.0f + m33 - m11 - m22);
        q[0] = (m13 + m31) / r;
        q[1] = (m23 + m32) / r;
        q[2] = 0.25f * r;
        q[3] = (m12 - m21) / r;
    }
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "Book%ld-", count);
    const std::time_t now = std::time(nullptr);
    std::tm t{};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    char name[96];
    std::snprintf(name, sizeof(name), "%d-%02d-%02d %02d:%02d:%02d AutoBookmark", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                  t.tm_min, t.tm_sec);
    const std::string p = prefix;
    c.set("Bookmarks", p + "Name", name);
    int mapId = -1;
    if (lastMapTemplate) {
        std::istringstream in(*lastMapTemplate);
        int v;
        if (in >> v) mapId = v;
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%d", mapId);
    c.set("Bookmarks", p + "MapId", text);
    auto number = [&](const char* key, float v) {
        std::snprintf(text, sizeof(text), "%g", double(v));
        std::string s = text;
        const size_t e = s.find_first_of("eE");
        if (e != std::string::npos && e + 1 < s.size())
            while (s.size() - (e + 2) < 3) s.insert(e + 2, "0");   // the exe's CRT: a three-digit exponent
        c.set("Bookmarks", p + key, s);
    };
    number("LocX", eye[0]);
    number("LocY", eye[1]);
    number("LocZ", eye[2]);
    number("RotW", q[3]);
    number("RotX", q[0]);
    number("RotY", q[1]);
    number("RotZ", q[2]);
    std::snprintf(text, sizeof(text), "%ld", count + 1);
    c.set("Bookmarks", "NumBookmarks", text);
    if (c.exportFile("TRBookmarks.bmf"))
        std::fprintf(stderr, "[config] %s Created\n", name);
    else
        std::fprintf(stderr, "[config] Failed to Export TRBookmarks.bmf\n");
}

bool hasItem(PyObject* config, const std::string& section, const std::string& key) { return of(config).item(section, key) != nullptr; }

std::string itemAsString(PyObject* config, const std::string& section, const std::string& key, const std::string& fallback) {
    const Item* i = of(config).item(section, key);
    return i ? i->value : fallback;
}

}  // namespace engine::config
