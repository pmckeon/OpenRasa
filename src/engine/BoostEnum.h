// BoostEnum.h - Boost.Python enum_ classes as tabula_rasa.exe makes them (Boost 1.33; traced 2026-10-28).
//
// The base type "Boost.Python.enum" (type object 0x00d02e90, made once by FUN_00903f00): a subclass of int whose instances carry a name
// (+ 0xc, member "name", T_OBJECT_EX read-only - unset: AttributeError); repr (0x00903e90) "%s.%s" % (type name, name), or
// "%s(%ld)" % (type name, value) without a name; str (0x00903ee0) the name, else int's. An enum class (FUN_00903f00) = the metatype called
// with "<module>.<Enum>" (the scope's __name__ + '.'), (base,) and {"__slots__": (), "values": {}}; each value (FUN_00904400 add_value):
// an instance with its name, values[int] = it, setattr(class, name, it). To Python: values[v], else class(v) (no name). From Python:
// only instances of the class (PyObject_IsInstance) - a plain int does not convert.
#pragma once

#include <Python.h>

#include <initializer_list>
#include <utility>

namespace engine {

// A new reference to the enum class.
PyObject* makeBoostEnum(const char* moduleName, const char* enumName, std::initializer_list<std::pair<const char*, long>> values);
// enum -> Python (a new reference).
PyObject* boostEnumValue(PyObject* enumClass, long value);
// Python -> enum: false (no error set) unless `o` is an instance of the class.
bool boostEnumArgument(PyObject* enumClass, PyObject* o, long& value);

}  // namespace engine
