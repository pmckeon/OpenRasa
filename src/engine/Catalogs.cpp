// Catalogs.cpp - see Catalogs.h.
#include "Catalogs.h"

#include "Assets.h"
#include "Stub.h"
#include "World.h"

#include "trformats.h"

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace engine::catalogs {
namespace {

struct Callback {
    PyObject* callable = nullptr;   // owned
    PyObject* args = nullptr;       // owned (a tuple or None)
};

struct Entry {
    int references = 0;
    std::vector<std::string> assets;   // the load list
    std::vector<Callback> callbacks;
};

struct Asset {
    int references = 0;
    bool loaded = false;
};

std::map<std::string, Entry> entries;
std::map<std::string, Asset> loadedAssets;

// The background reader.
std::mutex mutex;
std::condition_variable wake;
std::deque<std::string> queue;          // to read
std::vector<std::string> finished;      // read, not yet taken by frame()
std::thread worker;
bool stopping = false;

void workerLoop() {
    for (;;) {
        std::string name;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [] { return stopping || !queue.empty(); });
            if (stopping) return;
            name = queue.front();
            queue.pop_front();
        }
        tr::Bytes bytes;
        assets().tryRead(name, bytes);
        std::lock_guard<std::mutex> lock(mutex);
        finished.push_back(name);
    }
}

void startWorker() {
    if (!worker.joinable()) worker = std::thread(workerLoop);
}

PyObject* none() {
    Py_INCREF(Py_None);
    return Py_None;
}

void run(Callback& c) {
    if (c.callable && c.callable != Py_None) {
        PyObject* r = PyObject_CallObject(c.callable, c.args && PyTuple_Check(c.args) ? c.args : nullptr);
        if (!r) PyErr_Print();
        Py_XDECREF(r);
    }
    Py_XDECREF(c.callable);
    Py_XDECREF(c.args);
    c.callable = c.args = nullptr;
}

bool complete(const Entry& e) {
    for (const std::string& a : e.assets) {
        const auto it = loadedAssets.find(a);
        if (it == loadedAssets.end() || !it->second.loaded) return false;
    }
    return true;
}

// The finished reads taken in; catalogs now complete run their callbacks.
void takeFinished() {
    std::vector<std::string> done;
    {
        std::lock_guard<std::mutex> lock(mutex);
        done.swap(finished);
    }
    for (const std::string& name : done) {
        const auto it = loadedAssets.find(name);
        if (it != loadedAssets.end()) it->second.loaded = true;   // a freed asset's read is dropped
    }
    // Callbacks may request or release catalogs: collect first.
    std::vector<Callback> due;
    for (auto& [name, e] : entries)
        if (!e.callbacks.empty() && complete(e)) {
            for (Callback& c : e.callbacks) due.push_back(c);
            e.callbacks.clear();
        }
    for (Callback& c : due) run(c);
}

// FUN_006ae120: the catalog's assets requested - each not held is read (now, or queued while async).
void requestAssets(const std::vector<std::string>& names) {
    for (const std::string& n : names) {
        Asset& a = loadedAssets[n];
        ++a.references;
        if (a.references > 1) continue;
        if (world::assetsAsync()) {
            startWorker();
            std::lock_guard<std::mutex> lock(mutex);
            queue.push_back(n);
            wake.notify_one();
        } else {
            tr::Bytes bytes;
            assets().tryRead(n, bytes);
            a.loaded = true;
        }
    }
}

// FUN_005a3400.
void request(const std::string& catalog, PyObject* callable, PyObject* args) {
    Callback c;
    if (callable && callable != Py_None) {
        Py_INCREF(callable);
        c.callable = callable;
        if (args) Py_INCREF(args);
        c.args = args;
    }
    auto it = entries.find(catalog);
    if (it != entries.end()) {
        Entry& e = it->second;
        ++e.references;
        if (complete(e)) run(c);
        else if (c.callable) e.callbacks.push_back(c);
        return;
    }
    Entry& e = entries[catalog];
    e.references = 1;
    tr::Bytes bytes;
    if (assets().tryRead(catalog, bytes)) {
        try {
            const tr::Catalog cat = tr::readCatalog(bytes);
            for (const tr::CatalogRecord& r : cat.records)
                if (r.owner.empty()) {
                    e.assets = r.assets;   // the load list (the first record)
                    break;
                }
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "[catalogs] %s: %s\n", catalog.c_str(), ex.what());
        }
    }
    requestAssets(e.assets);
    if (complete(e)) run(c);
    else if (c.callable) e.callbacks.push_back(c);
}

// FUN_005a3590.
bool release(const std::string& catalog) {
    auto it = entries.find(catalog);
    if (it == entries.end()) return false;
    Entry& e = it->second;
    if (e.references > 1) {
        --e.references;
        return true;
    }
    for (const std::string& n : e.assets) {
        auto a = loadedAssets.find(n);
        if (a != loadedAssets.end() && --a->second.references <= 0) loadedAssets.erase(a);
    }
    for (Callback& c : e.callbacks) {
        Py_XDECREF(c.callable);
        Py_XDECREF(c.args);
    }
    entries.erase(it);
    return true;
}

PyObject* boolOf(bool b) {
    PyObject* r = b ? Py_True : Py_False;
    Py_INCREF(r);
    return r;
}

PyObject* preloadAssets(PyObject*, PyObject* args) {
    const char* name = nullptr;
    PyObject* callable = Py_None;
    PyObject* callArgs = Py_None;
    if (!PyArg_ParseTuple(args, "s|OO:PreloadAssets", &name, &callable, &callArgs)) return nullptr;
    if (callable != Py_None && !PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "PreloadAssets: the callback must be callable");
        return boolOf(false);
    }
    request(name, callable, callArgs);
    return boolOf(true);
}
PyObject* releaseAssets(PyObject*, PyObject* args) {
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "s:ReleaseAssets", &name)) return nullptr;
    return boolOf(release(name));
}
std::string classCatalog(long id) { return "class" + std::to_string(id) + ".cat"; }
std::string abilityCatalog(long id, long level) { return "ability" + std::to_string(id) + "+" + std::to_string(level) + ".cat"; }
PyObject* preloadForClassId(PyObject*, PyObject* args) {
    long id = 0;
    if (!PyArg_ParseTuple(args, "l:PreloadAssetsForClassId", &id)) return nullptr;
    request(classCatalog(id), nullptr, nullptr);
    return boolOf(true);
}
PyObject* releaseForClassId(PyObject*, PyObject* args) {
    long id = 0;
    if (!PyArg_ParseTuple(args, "l:ReleaseAssetsForClassId", &id)) return nullptr;
    return boolOf(release(classCatalog(id)));
}
PyObject* preloadForAbility(PyObject*, PyObject* args) {
    long id = 0, level = 0;
    if (!PyArg_ParseTuple(args, "ll:PreloadAssetsForAbility", &id, &level)) return nullptr;
    request(abilityCatalog(id, level), nullptr, nullptr);
    return boolOf(true);
}
PyObject* releaseForAbility(PyObject*, PyObject* args) {
    long id = 0, level = 0;
    if (!PyArg_ParseTuple(args, "ll:ReleaseAssetsForAbility", &id, &level)) return nullptr;
    return boolOf(release(abilityCatalog(id, level)));
}

void put(PyObject* target, const char* name, PyCFunction fn) {
    static std::vector<std::unique_ptr<PyMethodDef>> defs;
    defs.push_back(std::make_unique<PyMethodDef>(PyMethodDef{const_cast<char*>(name), fn, METH_VARARGS, nullptr}));
    PyObject* f = PyCFunction_New(defs.back().get(), nullptr);
    setStubAttribute(target, name, f);
    Py_DECREF(f);
}

}  // namespace

void install(PyObject* gameClient) {
    put(gameClient, "PreloadAssets", preloadAssets);
    put(gameClient, "ReleaseAssets", releaseAssets);
    put(gameClient, "PreloadAssetsForClassId", preloadForClassId);
    put(gameClient, "ReleaseAssetsForClassId", releaseForClassId);
    put(gameClient, "PreloadAssetsForAbility", preloadForAbility);
    put(gameClient, "ReleaseAssetsForAbility", releaseForAbility);
}

void frame() { takeFinished(); }

void shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
    }
    wake.notify_all();
    if (worker.joinable()) worker.join();
}

long pending() {
    long n = 0;
    for (const auto& [name, a] : loadedAssets)
        if (!a.loaded) ++n;
    return n;
}

void finishAll() {
    // Every queued read done here (the worker's own reads finish too), then the completions.
    std::deque<std::string> rest;
    {
        std::lock_guard<std::mutex> lock(mutex);
        rest.swap(queue);
    }
    for (const std::string& n : rest) {
        tr::Bytes bytes;
        assets().tryRead(n, bytes);
        std::lock_guard<std::mutex> lock(mutex);
        finished.push_back(n);
    }
    for (;;) {
        takeFinished();
        if (pending() == 0) break;
        std::this_thread::yield();
    }
}

}  // namespace engine::catalogs
