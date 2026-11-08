// Stub.h - stand-ins for the engine's Python modules while the engine is being written.
//
// tabula_rasa.exe registered its own modules with Python (extern_trmain_trmain, extern_gameclient_gameclient, ..., uievent,
// uianchor, sysentity, scenebodydimension, debugmon_base). Until each is implemented, a stub stands in: any attribute is a
// child stub (cached, so a name is always the same object), any call is logged with its arguments and returns a stub, a stub
// is true, iterates as empty and converts to 0. Real functions and values put into a stub's attributes replace the stand-ins,
// so the engine can be filled in piece by piece. The log (call paths, counts, first arguments) is the list of what the
// scripts actually use.
#pragma once

#include <Python.h>

#include <string>

namespace engine {

void initStubType();
PyObject* newStub(const std::string& name);                         // new reference
// steals nothing (value is increfed). On a real object (not a stub) the value goes into its type's dictionary.
void setStubAttribute(PyObject* stub, const char* name, PyObject* value);
bool isStub(PyObject* o);

// The calls stubs received, one line per distinct call path: count and the first call's arguments.
std::string stubCallReport();
void setStubCallEcho(bool echo);     // print each new call path as it happens (stderr)

}  // namespace engine
