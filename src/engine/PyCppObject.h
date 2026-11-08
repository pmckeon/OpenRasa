// PyCppObject.h - the exe's "PyCPPObject" Python type (type object 0x00d13130): a named object with a method table, used for the light,
// weather and region managers (gameclient.GetLightManager / GetWeatherManager / GetRegionManager).
//
// Instances (0x34 bytes): + 8 the C++ object, + 0xc the name (31 characters), + 0x2c the method table, + 0x30 a clean-up function.
// tp_getattr (0x00951960) = Py_FindMethod(methods, self, name) - methods only, no other attributes; tp_repr and tp_str (0x00951980)
// "<PyCPPObject type='%.64s' @ 0x%p>" with the name and the C++ object; tp_dealloc (0x00951930) runs the clean-up on the C++ object.
#pragma once

#include <Python.h>

namespace engine {

// A new reference. `methods` must live as long as the object.
PyObject* newPyCppObject(const char* name, PyMethodDef* methods);

}  // namespace engine
