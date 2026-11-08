#include "PyMarshal.h"

#include <longintrepr.h>
#include <marshal.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace engine {

namespace {

bool quietErrors = false;   // the self-test's deliberate failures are not printed

void printError() {
    if (quietErrors) PyErr_Clear();
    else PyErr_Print();
}

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    std::string error;
    PyObject* table = nullptr;   // the 0x41 string table: none on the CallMethod path

    bool take(void* out, size_t n) {
        if (size_t(end - p) < n) {
            error = "truncated";
            return false;
        }
        std::memcpy(out, p, n);
        p += n;
        return true;
    }
    template <typename T>
    bool value(T& out) {
        return take(&out, sizeof out);   // little-endian, as the platforms built for
    }
    // A size: the low nibble 0..12 itself, 13 / 14 / 15 a following u8 / u16 / u32 (FUN_00952960).
    bool size(uint8_t low, uint32_t& n) {
        if (low <= 12) {
            n = low;
            return true;
        }
        if (low == 13) {
            uint8_t v;
            if (!value(v)) return false;
            n = v;
            return true;
        }
        if (low == 14) {
            uint16_t v;
            if (!value(v)) return false;
            n = v;
            return true;
        }
        return value(n);
    }
    // 0x4D-0x4F / 0x5D-0x5F: the length, then the bytes through BufPack::UnPackRawString (FUN_00492d20), which refuses 0x8000 and
    // more (skipping them).
    bool bytes(uint8_t low, const char*& s, uint32_t& n) {
        if (!size(low, n)) return false;
        if (n >= 0x8000) {
            p += std::min(size_t(n), size_t(end - p));
            error = "string length " + std::to_string(n) + " is too long";
            return false;
        }
        if (size_t(end - p) < n) {
            error = "truncated string";
            return false;
        }
        s = reinterpret_cast<const char*>(p);
        p += n;
        return true;
    }

    // 0x41: a u32 id looked up in the reader's string table (FUN_00951aa0 / FUN_009527f0: PyDict_GetItem(table, int(id)), an
    // exact str only). The CallMethod path gives the reader no table (FUN_00951e50 passes 0 at 0x00951ee3), so there it fails.
    PyObject* tableString() {
        uint32_t id;
        if (!value(id)) return nullptr;
        if (!table) {
            error = "string table reference " + std::to_string(id) + " without a string table";
            return nullptr;
        }
        PyObject* key = PyInt_FromLong(long(id));
        PyObject* s = key ? PyDict_GetItem(table, key) : nullptr;   // borrowed
        Py_XDECREF(key);
        if (!s || s->ob_type != &PyString_Type) {
            error = "string table has no string " + std::to_string(id);
            return nullptr;
        }
        Py_INCREF(s);
        return s;
    }

    // 0x42 / 0x52: a u32 length, then that many bytes copied into a buffer (FUN_00951c20) - which the exe accepts only when it
    // copied FEWER bytes than the length says (CMP copied, length; JNC fail at 0x00951c61): every complete one is refused, and a
    // truncated one has taken the rest of the arguments, end mark included. The buffer is a compression block (FUN_0095c920 ->
    // FUN_0095ccd0): a kind byte - 0 the bytes stored, 1 a u32 size and a zlib stream (FUN_0095cff0 -> FUN_0095cfa0, zlib 1.1's
    // uncompress, which must give exactly that size), 2 and up refused.
    bool compressed(std::vector<uint8_t>& out) {
        uint32_t n;
        if (!value(n)) return false;
        const size_t copied = std::min(size_t(n), size_t(end - p));
        const uint8_t* block = p;
        p += copied;
        if (copied >= n) {
            error = "compressed string refused (the exe takes only one shorter than its length)";
            return false;
        }
        if (copied < 1 || block[0] >= 2) {
            error = "bad compression block";
            return false;
        }
        if (block[0] == 0) {
            out.assign(block + 1, block + copied);
            return true;
        }
        if (copied < 5) {
            error = "bad compression block";
            return false;
        }
        uint32_t size;
        std::memcpy(&size, block + 1, 4);
        out.resize(size);
        uLongf produced = size;
        if (uncompress(out.data(), &produced, block + 5, uLong(copied - 5)) != Z_OK || produced != size) {
            error = "compressed string does not inflate";
            return false;
        }
        return true;
    }

    PyObject* read(int depth) {
        if (++depth > 1000) {
            error = "maximum recursion level exceeded";
            return nullptr;
        }
        uint8_t t;
        if (!value(t)) return nullptr;
        const uint8_t low = t & 0xF;
        switch (t >> 4) {
        case 0:
            if (low == 0) Py_RETURN_NONE;
            if (low == 1) Py_RETURN_TRUE;
            if (low == 2) Py_RETURN_FALSE;
            break;
        case 1: {
            if (low <= 12) return PyInt_FromLong(low);
            if (low == 13) {
                int8_t v;
                return value(v) ? PyInt_FromLong(v) : nullptr;
            }
            if (low == 14) {
                int16_t v;
                return value(v) ? PyInt_FromLong(v) : nullptr;
            }
            int32_t v;
            return value(v) ? PyInt_FromLong(v) : nullptr;
        }
        case 2: {
            if (low == 0) return PyLong_FromLongLong(0);
            if (low != 15) break;
            int64_t v;
            return value(v) ? PyLong_FromLongLong(v) : nullptr;
        }
        case 3: {
            if (low == 0) return PyFloat_FromDouble(0.0);
            if (low == 1) return PyFloat_FromDouble(1.0);
            if (low == 14) {
                double v;
                return value(v) ? PyFloat_FromDouble(v) : nullptr;
            }
            if (low == 15) {
                float v;
                return value(v) ? PyFloat_FromDouble(v) : nullptr;
            }
            break;
        }
        case 4: {
            if (low == 0) return PyString_FromString(const_cast<char*>(""));
            if (low == 1) return tableString();
            if (low == 2) {
                std::vector<uint8_t> v;
                if (!compressed(v)) return nullptr;
                return PyString_FromStringAndSize(reinterpret_cast<const char*>(v.data()), int(v.size()));
            }
            if (low < 13) break;
            const char* s;
            uint32_t n;
            return bytes(low, s, n) ? PyString_FromStringAndSize(s, int(n)) : nullptr;
        }
        case 5: {
            if (low == 0) return PyUnicode_DecodeUTF8("", 0, const_cast<char*>("strict"));
            std::vector<uint8_t> v;
            const char* s;
            uint32_t n;
            if (low == 2) {
                if (!compressed(v)) return nullptr;
                s = reinterpret_cast<const char*>(v.data());
                n = uint32_t(v.size());
            } else if (low < 13) {
                break;
            } else if (!bytes(low, s, n)) {
                return nullptr;
            }
            PyObject* u = PyUnicode_DecodeUTF8(s, int(n), const_cast<char*>("strict"));
            if (!u) {
                PyErr_Clear();
                error = "bad UTF-8";
            }
            return u;
        }
        case 6: {
            uint32_t n;
            if (!size(low, n)) return nullptr;
            PyObject* d = PyDict_New();
            for (uint32_t i = 0; i < n; ++i) {
                PyObject* k = read(depth);
                PyObject* v = k ? read(depth) : nullptr;
                if (!v || PyDict_SetItem(d, k, v) != 0) {
                    if (k && v && error.empty()) {
                        PyErr_Clear();
                        error = "unhashable dict key";
                    }
                    Py_XDECREF(k);
                    Py_XDECREF(v);
                    Py_DECREF(d);
                    return nullptr;
                }
                Py_DECREF(k);
                Py_DECREF(v);
            }
            return d;
        }
        case 7:
        case 8: {
            uint32_t n;
            if (!size(low, n)) return nullptr;
            if (n > size_t(end - p)) {   // every element takes a byte at least
                error = "count past the data";
                return nullptr;
            }
            const bool tuple = (t >> 4) == 8;
            PyObject* c = tuple ? PyTuple_New(int(n)) : PyList_New(int(n));
            for (uint32_t i = 0; i < n; ++i) {
                PyObject* v = read(depth);
                if (!v) {
                    Py_DECREF(c);
                    return nullptr;
                }
                if (tuple)
                    PyTuple_SET_ITEM(c, int(i), v);
                else
                    PyList_SET_ITEM(c, int(i), v);
            }
            return c;
        }
        default: break;
        }
        error = "unknown type " + std::to_string(t >> 4) + "." + std::to_string(low);
        return nullptr;
    }
};

// The 'F' format's reader (FUN_0095c270): a cut-down copy of Python 2.4's marshal reader with its own float form. Counts and
// ints are 4 bytes little-endian - fewer than 4 left reads 0 and consumes nothing (FUN_0095c1e0); long digits 2 bytes, likewise
// (FUN_0095c1a0). Codes: '(' tuple, '[' list (count < 0: "bad marshal data"), '{' dict - pairs until a key reads as null ('0',
// the end, an error; a null value skips the pair), '0' null without an error, 'F' False, 'N' None, 'T' True, 'f' a raw 8-byte
// double (fewer left: the bytes there are, the rest left as they were - zero here; stock marshal writes floats as text), 'i' int,
// 'l' long (signed digit count, then 15-bit digits as stored), 's' string, 'u' UTF-8 unicode (strict); the end gives EOFError
// "EOF read where object expected", any other code ValueError "bad marshal data". No recursion limit.
struct MarshalF {
    const uint8_t* p;
    const uint8_t* end;

    int32_t rlong() {
        if (end - p < 4) return 0;
        int32_t v;
        std::memcpy(&v, p, 4);
        p += 4;
        return v;
    }
    uint16_t rshort() {
        if (end - p < 2) return 0;
        const uint16_t v = uint16_t(p[0] | p[1] << 8);
        p += 2;
        return v;
    }
    size_t rbytes(char* out, size_t n) {
        n = std::min(n, size_t(end - p));
        std::memcpy(out, p, n);
        p += n;
        return n;
    }
    PyObject* bad() {
        PyErr_SetString(PyExc_ValueError, const_cast<char*>("bad marshal data"));
        return nullptr;
    }
    PyObject* eof() {
        PyErr_SetString(PyExc_EOFError, const_cast<char*>("EOF read where object expected"));
        return nullptr;
    }

    PyObject* read() {
        if (p == end) return eof();
        switch (*p++) {
        case '(':
        case '[': {
            const bool tuple = p[-1] == '(';
            const int32_t n = rlong();
            if (n < 0) return bad();
            PyObject* c = tuple ? PyTuple_New(n) : PyList_New(n);
            if (!c) return nullptr;
            for (int32_t i = 0; i < n; ++i) {
                PyObject* v = read();
                if (!v) {
                    Py_DECREF(c);
                    return nullptr;
                }
                if (tuple)
                    PyTuple_SET_ITEM(c, i, v);
                else
                    PyList_SET_ITEM(c, i, v);
            }
            return c;
        }
        case '{': {
            PyObject* d = PyDict_New();
            if (!d) return nullptr;
            for (PyObject* k = read(); k; k = read()) {
                PyObject* v = read();
                if (v) PyDict_SetItem(d, k, v);
                Py_DECREF(k);
                Py_XDECREF(v);
            }
            return d;
        }
        case '0': return nullptr;
        case 'F': Py_RETURN_FALSE;
        case 'N': Py_RETURN_NONE;
        case 'T': Py_RETURN_TRUE;
        case 'f': {
            double v = 0.0;
            rbytes(reinterpret_cast<char*>(&v), 8);
            return PyFloat_FromDouble(v);
        }
        case 'i': return PyInt_FromLong(rlong());
        case 'l': {
            const int32_t n = rlong();
            const int32_t size = n < 0 ? -n : n;
            PyLongObject* l = _PyLong_New(size);
            if (!l) return nullptr;
            l->ob_size = n;
            for (int32_t i = 0; i < size; ++i) l->ob_digit[i] = rshort();
            return reinterpret_cast<PyObject*>(l);
        }
        case 's': {
            const int32_t n = rlong();
            if (n < 0) return bad();
            PyObject* s = PyString_FromStringAndSize(nullptr, n);
            if (s && rbytes(PyString_AS_STRING(s), size_t(n)) != size_t(n)) {
                Py_DECREF(s);
                return eof();
            }
            return s;
        }
        case 'u': {
            const int32_t n = rlong();
            if (n < 0) return bad();
            if (end - p < n) {
                p = end;
                return eof();
            }
            PyObject* u = PyUnicode_DecodeUTF8(reinterpret_cast<const char*>(p), n, nullptr);
            p += n;
            return u;
        }
        default: return bad();
        }
    }
};

}  // namespace

PyObject* unmarshalArguments(const uint8_t* data, size_t length, std::string& error) {
    if (length < 2) {
        error = "empty";
        return nullptr;
    }
    const uint8_t* p = data + 1;
    const uint8_t* end = data + length;
    PyObject* v = nullptr;
    if (data[0] == 'O') {
        Reader r{p, end, {}};
        v = r.read(0);
        if (!v) error = r.error;
        p = r.p;
    } else if (data[0] == 'F' || data[0] == 'M') {
        // A raw string (TRasa::BufPack::UnPackRawString FUN_0048ad80 / FUN_00492d20): u32 length (little-endian), refused from
        // 0x8000 up, then the bytes.
        uint32_t n = 0;
        if (end - p < 4) {
            error = "truncated";
            return nullptr;
        }
        std::memcpy(&n, p, 4);
        p += 4;
        if (n >= 0x8000) {
            error = "buffer length too long";
            return nullptr;
        }
        if (uint32_t(end - p) < n) {
            error = "truncated";
            return nullptr;
        }
        if (data[0] == 'M') {
            // FUN_009525c0: Python's own marshal reader.
            v = PyMarshal_ReadObjectFromString(reinterpret_cast<char*>(const_cast<uint8_t*>(p)), int(n));
        } else if (!PyErr_Occurred()) {
            MarshalF m{p, p + n};
            v = m.read();
        }
        p += n;
        if (!v) {
            error = PyErr_Occurred() ? "Python error" : "no value";
            if (PyErr_Occurred()) printError();
            PyErr_Clear();
            return nullptr;
        }
    } else {
        error = std::string("unknown argument format '") + char(data[0]) + "'";
        return nullptr;
    }
    if (!v) return nullptr;
    // The exe reads one byte after the value and wants the end mark there (FUN_00951e50 at 0x00951f51); a Python error left
    // set by the reader fails the call too (printed and cleared).
    const bool marked = p < end && *p == 'f';
    if (PyErr_Occurred()) {
        printError();
        PyErr_Clear();
        Py_DECREF(v);
        error = "Python error";
        return nullptr;
    }
    if (!marked) {
        Py_DECREF(v);
        error = "no end mark";
        return nullptr;
    }
    return v;
}

// The exe's 'O' writer (FUN_00953080; nesting deeper than 1000 fails, "maximum recursion level (%d) exceeded"):
//   None 0x00, True 0x01, False 0x02 (by identity);
//   int (exact type): 0..12 as 0x1n, else the narrowest of 0x1D int8 / 0x1E int16 / 0x1F int32 the value fits as signed;
//   long (exact): PyLong_AsLongLong - 0 as 0x20, else 0x2F and 8 bytes (an overflow leaves a Python error: the send fails);
//   float (exact): f = (float)d; f == 0 -> 0x30, f == 1 -> 0x31 (1.0f at 0x00bc4a80), |d - f| >= 0.01 (0x00c2b800) -> 0x3E
//     and the double, else 0x3F and the float;
//   str (exact): empty 0x40, else 0x4D / 0x4E / 0x4F with a u8 / u16 / u32 length and the bytes (FUN_009529f0; its string-table
//     form 0x41 needs a table and its compressed form 0x42 a flag, 0x00d451fb, that nothing sets - neither is ever written);
//   unicode (exact): its UTF-8 as 0x50 / 0x5D / 0x5E / 0x5F;
//   dict or a subclass: 0x6n / 0x6D-F count (FUN_009528b0), then key, value in PyDict_Next order;
//   list (exact): 0x7.. count, the items; tuple (exact): 0x8.. count, the items; anything else fails.
namespace {

void putCount(std::vector<uint8_t>& out, uint8_t type, uint32_t n) {
    if (n < 13) {
        out.push_back(uint8_t(type << 4 | n));
    } else if (n <= 0xFF) {
        out.push_back(uint8_t(type << 4 | 0xD));
        out.push_back(uint8_t(n));
    } else if (n <= 0xFFFF) {
        out.push_back(uint8_t(type << 4 | 0xE));
        out.push_back(uint8_t(n));
        out.push_back(uint8_t(n >> 8));
    } else {
        out.push_back(uint8_t(type << 4 | 0xF));
        for (int i = 0; i < 4; ++i) out.push_back(uint8_t(n >> (8 * i)));
    }
}

template <typename T>
void putRaw(std::vector<uint8_t>& out, T v) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), b, b + sizeof v);
}

// A string: empty = the type byte alone; else the length form (never the inline 0..12 count) and the bytes.
void putBytes(std::vector<uint8_t>& out, uint8_t type, const char* s, size_t n) {
    if (n == 0) {
        out.push_back(uint8_t(type << 4));
        return;
    }
    if (n <= 0xFF) {
        out.push_back(uint8_t(type << 4 | 0xD));
        out.push_back(uint8_t(n));
    } else if (n <= 0xFFFF) {
        out.push_back(uint8_t(type << 4 | 0xE));
        out.push_back(uint8_t(n));
        out.push_back(uint8_t(n >> 8));
    } else {
        out.push_back(uint8_t(type << 4 | 0xF));
        for (int i = 0; i < 4; ++i) out.push_back(uint8_t(n >> (8 * i)));
    }
    out.insert(out.end(), s, s + n);
}

bool put(PyObject* v, std::vector<uint8_t>& out, int depth) {
    if (!v) return false;
    if (++depth > 1000) {
        std::fprintf(stderr, "[engine] maximum recursion level (%d) exceeded\n", depth);
        return false;
    }
    if (v == Py_None) {
        out.push_back(0x00);
        return true;
    }
    if (v == Py_True) {
        out.push_back(0x01);
        return true;
    }
    if (v == Py_False) {
        out.push_back(0x02);
        return true;
    }
    PyTypeObject* t = v->ob_type;
    if (t == &PyInt_Type) {
        const long i = PyInt_AS_LONG(v);
        if (static_cast<unsigned long>(i) <= 12) {
            out.push_back(uint8_t(0x10 | i));
        } else if (int8_t(i) == i) {
            out.push_back(0x1D);
            putRaw(out, int8_t(i));
        } else if (int16_t(i) == i) {
            out.push_back(0x1E);
            putRaw(out, int16_t(i));
        } else {
            out.push_back(0x1F);
            putRaw(out, int32_t(i));
        }
        return true;
    }
    if (t == &PyLong_Type) {
        const long long i = PyLong_AsLongLong(v);
        if (i == 0) {
            out.push_back(0x20);
        } else {
            out.push_back(0x2F);
            putRaw(out, int64_t(i));
        }
        return true;
    }
    if (t == &PyFloat_Type) {
        const double d = PyFloat_AS_DOUBLE(v);
        const float f = float(d);
        if (f == 0.0f) {
            out.push_back(0x30);
        } else if (f == 1.0f) {
            out.push_back(0x31);
        } else if (std::fabs(d - double(f)) >= 0.01) {
            out.push_back(0x3E);
            putRaw(out, d);
        } else {
            out.push_back(0x3F);
            putRaw(out, f);
        }
        return true;
    }
    if (t == &PyString_Type) {
        putBytes(out, 4, PyString_AS_STRING(v), size_t(PyString_GET_SIZE(v)));
        return true;
    }
    if (t == &PyUnicode_Type) {
        PyObject* u = PyUnicode_AsUTF8String(v);
        if (!u) return false;
        putBytes(out, 5, PyString_AS_STRING(u), size_t(PyString_GET_SIZE(u)));
        Py_DECREF(u);
        return true;
    }
    if (PyDict_Check(v)) {
        putCount(out, 6, uint32_t(PyDict_Size(v)));
        int pos = 0;
        PyObject *key, *value;
        bool ok = true;
        while (PyDict_Next(v, &pos, &key, &value)) ok = ok && put(key, out, depth) && put(value, out, depth);
        return ok;
    }
    if (t == &PyList_Type || t == &PyTuple_Type) {
        const bool tuple = t == &PyTuple_Type;
        const int n = tuple ? int(PyTuple_GET_SIZE(v)) : int(PyList_GET_SIZE(v));
        putCount(out, tuple ? 8 : 7, uint32_t(n));
        for (int i = 0; i < n; ++i)
            if (!put(tuple ? PyTuple_GET_ITEM(v, i) : PyList_GET_ITEM(v, i), out, depth)) return false;
        return true;
    }
    return false;
}

}  // namespace

bool marshalArguments(PyObject* value, std::vector<uint8_t>& out) {
    PyErr_Clear();
    std::vector<uint8_t> buffer = {'O'};
    const bool ok = put(value, buffer, 0);
    buffer.push_back('f');
    if (PyErr_Occurred()) {   // FUN_00951c80: an error left by the writer fails the send, printed and cleared
        printError();
        PyErr_Clear();
        return false;
    }
    if (!ok) return false;
    out = std::move(buffer);
    return true;
}

namespace {

bool expectRepr(const char* name, const std::vector<uint8_t>& payload, const char* expected) {
    std::string error;
    PyObject* v = unmarshalArguments(payload.data(), payload.size(), error);
    std::string got = "(null: " + error + ")";
    if (v) {
        PyObject* r = PyObject_Repr(v);
        if (r) got = PyString_AsString(r);
        Py_XDECREF(r);
        Py_DECREF(v);
    }
    PyErr_Clear();
    const bool ok = expected ? got == expected : got.rfind("(null", 0) == 0;
    if (!ok) std::fprintf(stderr, "[engine] marshal self-test %s: got %s, expected %s\n", name, got.c_str(), expected ? expected : "a refusal");
    return ok;
}

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}

}  // namespace

bool marshalSelfTest() {
    quietErrors = true;
    bool ok = true;
    ok &= expectRepr("O", {'O', 0x83, 0x15, 0x4D, 2, 'h', 'i', 0x01, 'f'}, "(5, 'hi', True)");
    ok &= expectRepr("O empty strings", {'O', 0x82, 0x40, 0x50, 'f'}, "('', u'')");
    ok &= expectRepr("O no end mark", {'O', 0x80}, nullptr);
    ok &= expectRepr("O string form 3", {'O', 0x43, 'f'}, nullptr);
    ok &= expectRepr("O table string", {'O', 0x41, 7, 0, 0, 0, 'f'}, nullptr);
    ok &= expectRepr("O compressed, complete", {'O', 0x42, 3, 0, 0, 0, 0, 'a', 'b', 'f'}, nullptr);
    ok &= expectRepr("O compressed, short", {'O', 0x42, 9, 0, 0, 0, 0, 'a', 'b', 'f'}, nullptr);
    {
        // The pieces the CallMethod path never lets succeed, read directly: a table string, and short compressed blocks.
        auto direct = [&](const char* name, std::vector<uint8_t> in, PyObject* table, const char* expected) {
            Reader r{in.data(), in.data() + in.size(), {}, table};
            PyObject* v = r.read(0);
            std::string got = "(null: " + r.error + ")";
            if (v) {
                PyObject* t = PyObject_Repr(v);
                if (t) got = PyString_AsString(t);
                Py_XDECREF(t);
                Py_DECREF(v);
            }
            PyErr_Clear();
            if (got != expected) {
                std::fprintf(stderr, "[engine] marshal self-test %s: got %s, expected %s\n", name, got.c_str(), expected);
                ok = false;
            }
        };
        PyObject* table = PyDict_New();
        PyObject* key = PyInt_FromLong(7);
        PyObject* value = PyString_FromString(const_cast<char*>("seven"));
        PyDict_SetItem(table, key, value);
        Py_DECREF(key);
        Py_DECREF(value);
        direct("table string", {0x41, 7, 0, 0, 0}, table, "'seven'");
        direct("table string missing", {0x41, 8, 0, 0, 0}, table, "(null: string table has no string 8)");
        Py_DECREF(table);
        direct("stored block", {0x42, 9, 0, 0, 0, 0, 'a', 'b'}, nullptr, "'ab'");
        const char text[] = "compressed text";
        uLongf packedSize = compressBound(sizeof text - 1);
        std::vector<uint8_t> packed(packedSize);
        compress(packed.data(), &packedSize, reinterpret_cast<const Bytef*>(text), sizeof text - 1);
        std::vector<uint8_t> in = {0x52};
        putU32(in, uint32_t(1 + 4 + packedSize + 1));   // one more than follows
        in.push_back(1);
        putU32(in, sizeof text - 1);
        in.insert(in.end(), packed.begin(), packed.begin() + packedSize);
        direct("zlib block", in, nullptr, "u'compressed text'");
    }

    // 'F': a tuple of an int, a raw double, a string, a dict ending in '0', and a long of two 15-bit digits.
    std::vector<uint8_t> f = {'('};
    putU32(f, 5);
    f.push_back('i');
    putU32(f, uint32_t(-7));
    f.push_back('f');
    const double d = 1.5;
    const uint8_t* db = reinterpret_cast<const uint8_t*>(&d);
    f.insert(f.end(), db, db + 8);
    f.push_back('s');
    putU32(f, 2);
    f.push_back('a');
    f.push_back('b');
    f.push_back('{');
    f.push_back('u');
    putU32(f, 2);
    f.push_back(0xC3);
    f.push_back(0xA9);
    f.push_back('N');
    f.push_back('0');
    f.push_back('l');
    putU32(f, uint32_t(-2));
    f.insert(f.end(), {1, 0, 1, 0});
    std::vector<uint8_t> fp = {'F'};
    putU32(fp, uint32_t(f.size()));
    fp.insert(fp.end(), f.begin(), f.end());
    fp.push_back('f');
    ok &= expectRepr("F", fp, "(-7, 1.5, 'ab', {u'\\xe9': None}, -32769L)");
    ok &= expectRepr("F null", {'F', 1, 0, 0, 0, '0', 'f'}, nullptr);
    ok &= expectRepr("F bad code", {'F', 1, 0, 0, 0, '?', 'f'}, nullptr);
    ok &= expectRepr("F too long", {'F', 0, 0x80, 0, 0, 'f'}, nullptr);

    // 'M': Python's own marshal of the same kind of value.
    PyObject* value = Py_BuildValue(const_cast<char*>("(isd)"), 3, "xyz", 2.25);
    PyObject* bytes = value ? PyMarshal_WriteObjectToString(value, 1) : nullptr;
    Py_XDECREF(value);
    if (bytes) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(PyString_AS_STRING(bytes));
        std::vector<uint8_t> mp = {'M'};
        putU32(mp, uint32_t(PyString_GET_SIZE(bytes)));
        mp.insert(mp.end(), b, b + PyString_GET_SIZE(bytes));
        mp.push_back('f');
        Py_DECREF(bytes);
        ok &= expectRepr("M", mp, "(3, 'xyz', 2.25)");
    } else {
        PyErr_Clear();
        std::fprintf(stderr, "[engine] marshal self-test: PyMarshal_WriteObjectToString failed\n");
        ok = false;
    }
    ok &= expectRepr("unknown format", {'X', 0, 'f'}, nullptr);
    {
        // The writer, read back: every form it writes.
        PyObject* value = Py_BuildValue(const_cast<char*>("(iiiiiLLdddds#s#[O]{s:O}O)"), 0, 12, -1, 300, 70000, 0LL,
                                        5000000000LL, 0.0, 1.0, 0.5, 1e40, "", 0, "ab", 2, Py_None, "k", Py_None, Py_True);
        std::vector<uint8_t> out;
        if (!value || !marshalArguments(value, out)) {
            std::fprintf(stderr, "[engine] marshal self-test: the writer refused a value it writes\n");
            ok = false;
        } else {
            ok &= expectRepr("writer round trip", out,
                             "(0, 12, -1, 300, 70000, 0L, 5000000000L, 0.0, 1.0, 0.5, 1e+40, '', 'ab', [None], {'k': None}, True)");
            const std::vector<uint8_t> head = {'O', 0x8D, 16, 0x10, 0x1C, 0x1D, 0xFF, 0x1E, 0x2C, 0x01, 0x1F};
            if (out.size() < head.size() || !std::equal(head.begin(), head.end(), out.begin())) {
                std::fprintf(stderr, "[engine] marshal self-test: the writer's int forms differ\n");
                ok = false;
            }
        }
        Py_XDECREF(value);
        PyErr_Clear();
        std::vector<uint8_t> unused;
        PyObject* bad = PyTuple_New(1);
        PyTuple_SET_ITEM(bad, 0, PyComplex_FromDoubles(1, 2));
        if (marshalArguments(bad, unused)) {
            std::fprintf(stderr, "[engine] marshal self-test: the writer took a complex number\n");
            ok = false;
        }
        Py_DECREF(bad);
    }
    quietErrors = false;
    return ok;
}

}  // namespace engine
