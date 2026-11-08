// PyProfile.cpp - see PyProfile.h.
#include "PyProfile.h"

#include <Python.h>
#include <frameobject.h>

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace engine::pyprof {
namespace {
struct Entry {
    long calls = 0;
    double inclusive = 0, self = 0;   // ms
};
std::map<std::string, Entry> functions;
std::map<std::string, Entry> frameFunctions;   // this engine::frame's (slow-frame reports)
uint64_t frameStart = 0;
const double slowMs = 40.0;
std::map<std::string, Entry> sections;
struct Frame {
    std::string key;
    uint64_t start;
    double children = 0;
};
std::vector<Frame> stack;
uint64_t sectionMark = 0;
int frames = 0;

std::string keyOf(PyFrameObject* f) {
    PyCodeObject* c = f->f_code;
    std::string k = PyString_AsString(c->co_filename);
    k += ":";
    k += PyString_AsString(c->co_name);
    k += ":" + std::to_string(c->co_firstlineno);
    return k;
}

std::string keyOfC(PyObject* fn) {
    std::string k = "C ";
    if (fn && PyCFunction_Check(fn)) {
        PyCFunctionObject* c = reinterpret_cast<PyCFunctionObject*>(fn);
        if (c->m_self && !PyModule_Check(c->m_self)) {
            k += c->m_self->ob_type->tp_name;
            k += ".";
        } else if (c->m_module && PyString_Check(c->m_module)) {
            k += PyString_AsString(c->m_module);
            k += ".";
        }
        k += c->m_ml->ml_name;
    }
    return k;
}

int profiler(PyObject*, PyFrameObject* frame, int what, PyObject* arg) {
    if (what == PyTrace_CALL || what == PyTrace_C_CALL) {
        stack.push_back(Frame{what == PyTrace_CALL ? keyOf(frame) : keyOfC(arg), SDL_GetTicksNS(), 0});
    } else if (what == PyTrace_RETURN || what == PyTrace_C_RETURN || what == PyTrace_C_EXCEPTION) {
        if (stack.empty()) return 0;
        Frame f = std::move(stack.back());
        stack.pop_back();
        const double ms = double(SDL_GetTicksNS() - f.start) / 1e6;
        for (Entry* e : {&functions[f.key], &frameFunctions[f.key]}) {
            ++e->calls;
            e->inclusive += ms;
            e->self += ms - f.children;
        }
        if (!stack.empty()) stack.back().children += ms;
    }
    return 0;
}

void dump(const char* title, std::map<std::string, Entry>& table, bool bySelf) {
    std::vector<std::pair<std::string, Entry>> rows(table.begin(), table.end());
    std::sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
        return (bySelf ? a.second.self : a.second.inclusive) > (bySelf ? b.second.self : b.second.inclusive);
    });
    std::fprintf(stderr, "[pyprof] %s (%d frames; ms per frame)\n", title, frames);
    size_t n = 0;
    for (const auto& [k, e] : rows) {
        if (++n > 45) break;
        std::fprintf(stderr, "[pyprof]   self %7.3f  incl %7.3f  calls %7.1f  %s\n", e.self / std::max(1, frames),
                     e.inclusive / std::max(1, frames), double(e.calls) / std::max(1, frames), k.c_str());
    }
}
}  // namespace

bool enabled() {
    static const bool on = std::getenv("OPENRASA_PYPROF") != nullptr;
    return on;
}

void install() {
    if (!enabled()) return;
    PyEval_SetProfile(profiler, nullptr);
}

void frameBegin() {
    if (!enabled()) return;
    const uint64_t now = SDL_GetTicksNS();
    const double ms = frameStart ? double(now - frameStart) / 1e6 : 0;
    if (ms > slowMs) {   // the last main-loop frame was slow: what the scripts spent it in (self time, C calls included)
        std::vector<std::pair<std::string, Entry>> rows(frameFunctions.begin(), frameFunctions.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.self > b.second.self; });
        std::fprintf(stderr, "[pyprof] slow frame %.1f ms at %llu ms:\n", ms, static_cast<unsigned long long>(SDL_GetTicks()));
        for (size_t i = 0; i < rows.size() && i < 14; ++i)
            std::fprintf(stderr, "[pyprof]     self %7.2f  incl %7.2f  calls %6ld  %s\n", rows[i].second.self, rows[i].second.inclusive,
                         rows[i].second.calls, rows[i].first.c_str());
    }
    frameFunctions.clear();
    frameStart = now;
    ++frames;
    sectionMark = now;
}

void section(const char* name) {
    if (!enabled()) return;
    const uint64_t t = SDL_GetTicksNS();
    Entry& e = sections[name];
    ++e.calls;
    e.inclusive += double(t - sectionMark) / 1e6;
    sectionMark = t;
}

void report() {
    if (!enabled()) return;
    PyEval_SetProfile(nullptr, nullptr);
    dump("engine::frame sections", sections, false);
    dump("Python functions by self time", functions, true);
    dump("Python functions by inclusive time", functions, false);
}
}  // namespace engine::pyprof
