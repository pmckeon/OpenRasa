// PyCppObject.cpp - see PyCppObject.h.
#include "PyCppObject.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace engine {
namespace {

struct PyCppObject {
    PyObject_HEAD
    char name[32];
    PyMethodDef* methods;
};

PyObject* pyCppGetAttr(PyObject* self, char* name) {
    return Py_FindMethod(reinterpret_cast<PyCppObject*>(self)->methods, self, name);
}
// The exe prints its C++ object's address (MSVC %p: 8 hex digits); OpenRasa has none behind it and prints the Python object's low 32 bits.
PyObject* pyCppRepr(PyObject* self) {
    char text[128];
    std::snprintf(text, sizeof text, "<PyCPPObject type='%.64s' @ 0x%08X>", reinterpret_cast<PyCppObject*>(self)->name,
                  unsigned(reinterpret_cast<uintptr_t>(self)));
    return PyString_FromString(text);
}
void pyCppDealloc(PyObject* self) { PyObject_Del(self); }

PyTypeObject pyCppType = {PyObject_HEAD_INIT(nullptr)};

}  // namespace

PyObject* newPyCppObject(const char* name, PyMethodDef* methods) {
    if (!pyCppType.tp_name) {
        pyCppType.tp_name = const_cast<char*>("PyCPPObject");
        pyCppType.tp_basicsize = sizeof(PyCppObject);
        pyCppType.tp_flags = Py_TPFLAGS_DEFAULT;
        pyCppType.tp_dealloc = pyCppDealloc;
        pyCppType.tp_getattr = pyCppGetAttr;
        pyCppType.tp_repr = pyCppRepr;
        pyCppType.tp_str = pyCppRepr;
        PyType_Ready(&pyCppType);
    }
    PyCppObject* o = PyObject_New(PyCppObject, &pyCppType);
    std::strncpy(o->name, name, 31);
    o->name[31] = 0;
    o->methods = methods;
    return reinterpret_cast<PyObject*>(o);
}

}  // namespace engine
