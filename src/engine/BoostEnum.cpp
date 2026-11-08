// BoostEnum.cpp - see BoostEnum.h.
#include "BoostEnum.h"

#include <structmember.h>

#include <cstddef>
#include <string>

namespace engine {
namespace {

struct EnumObject {
    PyIntObject base;
    PyObject* name;
};

PyTypeObject enumBaseType = {PyObject_HEAD_INIT(nullptr)};

PyMemberDef enumMembers[] = {
    {const_cast<char*>("name"), T_OBJECT_EX, offsetof(EnumObject, name), READONLY, nullptr},
    {nullptr, 0, 0, 0, nullptr},
};

PyObject* enumRepr(PyObject* self) {
    EnumObject* e = reinterpret_cast<EnumObject*>(self);
    if (!e->name) return PyString_FromFormat("%s(%ld)", self->ob_type->tp_name, e->base.ob_ival);
    const char* name = PyString_AsString(e->name);
    if (!name) return nullptr;
    return PyString_FromFormat("%s.%s", self->ob_type->tp_name, name);
}

PyObject* enumStr(PyObject* self) {
    EnumObject* e = reinterpret_cast<EnumObject*>(self);
    if (!e->name) return PyInt_Type.tp_str(self);
    Py_INCREF(e->name);
    return e->name;
}

void enumDealloc(PyObject* self) {
    Py_XDECREF(reinterpret_cast<EnumObject*>(self)->name);
    self->ob_type->tp_free(self);
}

bool baseReady() {
    if (enumBaseType.tp_name) return true;
    enumBaseType.tp_name = const_cast<char*>("Boost.Python.enum");
    enumBaseType.tp_basicsize = sizeof(EnumObject);
    enumBaseType.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
    enumBaseType.tp_base = &PyInt_Type;
    enumBaseType.tp_repr = enumRepr;
    enumBaseType.tp_str = enumStr;
    enumBaseType.tp_members = enumMembers;
    enumBaseType.tp_dealloc = enumDealloc;
    if (PyType_Ready(&enumBaseType) < 0) {
        PyErr_Print();
        return false;
    }
    return true;
}

}  // namespace

PyObject* makeBoostEnum(const char* moduleName, const char* enumName, std::initializer_list<std::pair<const char*, long>> values) {
    if (!baseReady()) return nullptr;
    const std::string qualified = std::string(moduleName) + "." + enumName;
    PyObject* dict = PyDict_New();
    PyObject* slots = PyTuple_New(0);
    PyObject* valueDict = PyDict_New();
    PyDict_SetItemString(dict, "__slots__", slots);
    PyDict_SetItemString(dict, "values", valueDict);
    Py_DECREF(slots);
    PyObject* cls = PyObject_CallFunction(reinterpret_cast<PyObject*>(&PyType_Type), const_cast<char*>("s(O)O"), qualified.c_str(),
                                          reinterpret_cast<PyObject*>(&enumBaseType), dict);
    Py_DECREF(dict);
    if (!cls) {
        Py_DECREF(valueDict);
        PyErr_Print();
        return nullptr;
    }
    for (const auto& [name, v] : values) {
        PyObject* x = PyObject_CallFunction(cls, const_cast<char*>("l"), v);
        if (!x) {
            PyErr_Print();
            continue;
        }
        EnumObject* e = reinterpret_cast<EnumObject*>(x);
        Py_XDECREF(e->name);
        e->name = PyString_FromString(name);
        PyObject* key = PyInt_FromLong(v);
        PyDict_SetItem(valueDict, key, x);
        Py_DECREF(key);
        PyObject_SetAttrString(cls, const_cast<char*>(name), x);
        Py_DECREF(x);
    }
    Py_DECREF(valueDict);
    return cls;
}

PyObject* boostEnumValue(PyObject* enumClass, long value) {
    PyObject* values = PyObject_GetAttrString(enumClass, const_cast<char*>("values"));
    if (values && PyDict_Check(values)) {
        PyObject* key = PyInt_FromLong(value);
        PyObject* x = PyDict_GetItem(values, key);   // borrowed
        Py_DECREF(key);
        if (x) {
            Py_INCREF(x);
            Py_DECREF(values);
            return x;
        }
    }
    Py_XDECREF(values);
    PyErr_Clear();
    return PyObject_CallFunction(enumClass, const_cast<char*>("l"), value);
}

bool boostEnumArgument(PyObject* enumClass, PyObject* o, long& value) {
    if (PyObject_IsInstance(o, enumClass) != 1) {
        PyErr_Clear();
        return false;
    }
    value = PyInt_AsLong(o);
    return true;
}

}  // namespace engine
