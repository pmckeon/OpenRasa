// pyrun - imports modules of the game's Python scripts with the embedded CPython 2.4.6 and reports what fails.
//
//   pyrun <game folder> <module> [@<module> ...]      (@module: also print the module's globals)
//
// As tabula_rasa.exe: the game folder is the working directory and sys.path is python_nca_internal, ./trpython.zip,
// ./trpython.zip/client/system; then the [Data] zips of tabula_rasa.cfg in the client's order (what clientbootstrap.boot()
// adds through trmain.GetDataPaths()).
#include <Python.h>

#include "assetlibrary.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: pyrun <game folder> <module> [@<module> ...]\n");
        return 2;
    }
    std::string root = argv[1];
    std::filesystem::current_path(std::filesystem::u8path(root));

    std::vector<std::string> path = {"python_nca_internal", "./trpython.zip", "./trpython.zip/client/system"};
    try {
        tr::AssetLibrary library(".");
        for (const std::string& p : library.gameDataPaths()) path.push_back(p);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "warning: %s\n", e.what());
    }

    Py_NoSiteFlag = 1;               // no site.py: the standard library comes from trpython.zip once sys.path is set
    Py_IgnoreEnvironmentFlag = 1;    // ignore PYTHONPATH / PYTHONHOME
    Py_FrozenFlag = 1;               // no warnings about a missing Lib folder
    Py_Initialize();
    // A list, not PySys_SetPath: that splits on the platform's separator (";" on Windows, ":" elsewhere).
    PyObject* sysPath = PyList_New(0);
    for (const std::string& p : path) {
        PyObject* item = PyString_FromString(p.c_str());
        PyList_Append(sysPath, item);
        Py_DECREF(item);
    }
    PySys_SetObject(const_cast<char*>("path"), sysPath);
    Py_DECREF(sysPath);

    int failures = 0;
    for (int i = 2; i < argc; ++i) {
        std::string name = argv[i];
        bool dump = !name.empty() && name[0] == '@';
        if (dump) name = name.substr(1);
        auto t0 = std::chrono::steady_clock::now();
        PyObject* m = PyImport_ImportModule(const_cast<char*>(name.c_str()));
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!m) {
            ++failures;
            std::printf("FAIL  %s\n", name.c_str());
            std::fflush(stdout);
            PyErr_Print();
            continue;
        }
        std::printf("ok    %s (%.1f ms)\n", name.c_str(), ms);
        if (dump) {
            PyObject* d = PyModule_GetDict(m);
            PyObject *key, *value;
            int pos = 0;   // Python 2.4: no Py_ssize_t
            while (PyDict_Next(d, &pos, &key, &value)) {
                PyObject* r = PyObject_Repr(value);
                std::printf("  %s = %.400s\n", PyString_AsString(key), r ? PyString_AsString(r) : "?");
                Py_XDECREF(r);
                PyErr_Clear();
            }
        }
        Py_DECREF(m);
    }
    std::fflush(stdout);
    Py_Finalize();
    return failures ? 1 : 0;
}
