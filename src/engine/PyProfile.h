// PyProfile.h - OPENRASA_PYPROF=1: a profile of the scripts (per Python function: calls, inclusive and self time) and of
// engine::frame's sections, written to stderr at shutdown. A test tool, off by default.
#pragma once

namespace engine::pyprof {
bool enabled();
void install();                       // after Py_Initialize
void report();                        // before Py_Finalize
void section(const char* name);       // engine::frame: the time since the last section() call goes to `name`
void frameBegin();
}  // namespace engine::pyprof
