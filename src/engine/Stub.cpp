// Stub.cpp - see Stub.h.
#include "Stub.h"

#include <cstdio>
#include <algorithm>
#include <map>
#include <vector>

namespace engine {

namespace {

struct StubObject {
    PyObject_HEAD
    PyObject* name;      // str
    PyObject* attrs;     // dict: attribute name (or "()", "[key]") -> object
};

struct CallRecord {
    int count = 0;
    std::string firstArgs;
    int order = 0;
};
std::map<std::string, CallRecord> calls;
int callOrder = 0;
bool echo = true;

PyTypeObject StubType;

std::string nameOf(PyObject* self) { return PyString_AsString(reinterpret_cast<StubObject*>(self)->name); }

std::string reprText(PyObject* o) {
    PyObject* r = PyObject_Repr(o);
    if (!r) {
        PyErr_Clear();
        return "?";
    }
    std::string s = PyString_AsString(r);
    Py_DECREF(r);
    if (s.size() > 200) s = s.substr(0, 200) + "...";
    return s;
}

PyObject* child(PyObject* self, const std::string& key, const std::string& childName) {
    StubObject* s = reinterpret_cast<StubObject*>(self);
    PyObject* found = PyDict_GetItemString(s->attrs, const_cast<char*>(key.c_str()));
    if (found) {
        Py_INCREF(found);
        return found;
    }
    PyObject* c = newStub(childName);
    PyDict_SetItemString(s->attrs, const_cast<char*>(key.c_str()), c);
    return c;
}

void stubDealloc(PyObject* self) {
    StubObject* s = reinterpret_cast<StubObject*>(self);
    Py_XDECREF(s->name);
    Py_XDECREF(s->attrs);
    self->ob_type->tp_free(self);
}

PyObject* stubGetAttr(PyObject* self, PyObject* nameObj) {
    StubObject* s = reinterpret_cast<StubObject*>(self);
    const char* n = PyString_AsString(nameObj);
    if (!n) return nullptr;
    PyObject* found = PyDict_GetItem(s->attrs, nameObj);
    if (found) {
        Py_INCREF(found);
        return found;
    }
    std::string attr = n;
    if (attr.size() > 4 && attr.compare(0, 2, "__") == 0 && attr.compare(attr.size() - 2, 2, "__") == 0) {
        if (attr == "__name__") return PyString_FromString(nameOf(self).c_str());
        return PyObject_GenericGetAttr(self, nameObj);
    }
    return child(self, attr, nameOf(self) + "." + attr);
}

int stubSetAttr(PyObject* self, PyObject* nameObj, PyObject* value) {
    StubObject* s = reinterpret_cast<StubObject*>(self);
    if (!value) return PyDict_DelItem(s->attrs, nameObj);
    return PyDict_SetItem(s->attrs, nameObj, value);
}

PyObject* stubCall(PyObject* self, PyObject* args, PyObject* kwargs) {
    std::string n = nameOf(self);
    // A class statement deriving from an engine class (whose metaclass is then this stub): make a real class.
    if (PyTuple_Size(args) == 3 && PyString_Check(PyTuple_GET_ITEM(args, 0)) && PyTuple_Check(PyTuple_GET_ITEM(args, 1)) &&
        PyDict_Check(PyTuple_GET_ITEM(args, 2))) {
        PyObject* bases = PyTuple_GET_ITEM(args, 1);
        bool derives = false;
        for (int i = 0; i < PyTuple_Size(bases); ++i) derives = derives || PyTuple_GET_ITEM(bases, i) == self;
        if (derives) {
            std::vector<PyObject*> kept;
            for (int i = 0; i < PyTuple_Size(bases); ++i)
                if (!isStub(PyTuple_GET_ITEM(bases, i))) kept.push_back(PyTuple_GET_ITEM(bases, i));
            kept.push_back(reinterpret_cast<PyObject*>(&PyBaseObject_Type));
            PyObject* newBases = PyTuple_New(int(kept.size()));
            for (size_t i = 0; i < kept.size(); ++i) {
                Py_INCREF(kept[i]);
                PyTuple_SET_ITEM(newBases, int(i), kept[i]);
            }
            std::string key = "class " + std::string(PyString_AsString(PyTuple_GET_ITEM(args, 0))) + "(" + n + ")";
            CallRecord& r = calls[key];
            if (r.count++ == 0) {
                r.order = ++callOrder;
                if (echo) std::fprintf(stderr, "[engine stub] %s\n", key.c_str());
            }
            PyObject* cls = PyObject_CallFunction(reinterpret_cast<PyObject*>(&PyType_Type), const_cast<char*>("OOO"),
                                                  PyTuple_GET_ITEM(args, 0), newBases, PyTuple_GET_ITEM(args, 2));
            Py_DECREF(newBases);
            return cls;
        }
    }
    CallRecord& r = calls[n];
    if (r.count++ == 0) {
        r.order = ++callOrder;
        std::string text = reprText(args);
        if (kwargs && PyDict_Size(kwargs)) text += " " + reprText(kwargs);
        r.firstArgs = text;
        if (echo) std::fprintf(stderr, "[engine stub] %s%s\n", n.c_str(), text.c_str());
    }
    return child(self, "()", n + "()");
}

PyObject* stubRepr(PyObject* self) { return PyString_FromString(("<engine stub " + nameOf(self) + ">").c_str()); }
int stubNonzero(PyObject*) { return 1; }
PyObject* stubIter(PyObject*) {
    PyObject* empty = PyTuple_New(0);
    PyObject* it = PyObject_GetIter(empty);
    Py_DECREF(empty);
    return it;
}
PyObject* stubToInt(PyObject*) { return PyInt_FromLong(0); }
PyObject* stubToFloat(PyObject*) { return PyFloat_FromDouble(0.0); }
int stubLength(PyObject*) { return 0; }
PyObject* stubSubscript(PyObject* self, PyObject* key) {
    std::string k = "[" + reprText(key) + "]";
    return child(self, k, nameOf(self) + k);
}
int stubAssign(PyObject*, PyObject*, PyObject*) { return 0; }

PyNumberMethods stubNumber;
PyMappingMethods stubMapping;

}  // namespace

void initStubType() {
    static bool ready = false;
    if (ready) return;
    ready = true;
    stubNumber.nb_nonzero = stubNonzero;
    stubNumber.nb_int = stubToInt;
    stubNumber.nb_long = stubToInt;
    stubNumber.nb_float = stubToFloat;
    stubMapping.mp_length = stubLength;
    stubMapping.mp_subscript = stubSubscript;
    stubMapping.mp_ass_subscript = stubAssign;
    PyTypeObject t = {PyObject_HEAD_INIT(&PyType_Type)};
    StubType = t;
    StubType.tp_name = "openrasa.EngineStub";
    StubType.tp_basicsize = sizeof(StubObject);
    StubType.tp_dealloc = stubDealloc;
    StubType.tp_repr = stubRepr;
    StubType.tp_as_number = &stubNumber;
    StubType.tp_as_mapping = &stubMapping;
    StubType.tp_call = stubCall;
    StubType.tp_getattro = stubGetAttr;
    StubType.tp_setattro = stubSetAttr;
    StubType.tp_iter = stubIter;
    StubType.tp_flags = Py_TPFLAGS_DEFAULT;
    StubType.tp_free = PyObject_Del;
    PyType_Ready(&StubType);
}

PyObject* newStub(const std::string& name) {
    initStubType();
    StubObject* s = PyObject_New(StubObject, &StubType);
    s->name = PyString_FromString(name.c_str());
    s->attrs = PyDict_New();
    return reinterpret_cast<PyObject*>(s);
}

void setStubAttribute(PyObject* stub, const char* name, PyObject* value) {
    if (stub && stub->ob_type == &StubType) {
        PyDict_SetItemString(reinterpret_cast<StubObject*>(stub)->attrs, const_cast<char*>(name), value);
        return;
    }
    // A real object (gameclient's GameClient): into its type's dictionary - a built-in function there is not bound to the instance.
    PyTypeObject* type = PyType_Check(stub) ? reinterpret_cast<PyTypeObject*>(stub) : stub->ob_type;
    PyDict_SetItemString(type->tp_dict, const_cast<char*>(name), value);
}

bool isStub(PyObject* o) { return o && o->ob_type == &StubType; }

std::string stubCallReport() {
    std::vector<std::pair<int, std::string>> lines;
    for (auto& [name, r] : calls) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%5d x  ", r.count);
        lines.emplace_back(r.order, buf + name + r.firstArgs);
    }
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (auto& [order, line] : lines) out += line + "\n";
    return out;
}

void setStubCallEcho(bool e) { echo = e; }

}  // namespace engine
