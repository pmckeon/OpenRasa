// Config.h - extern_util_config: TRasa::Config, the exe's .cfg reader / writer and its Python class (registration FUN_00484a80:
// Config with a default constructor, CreateWrapper = the identity).
//
// The engine makes two at start-up (TRasa::GameClient::Initialize, FUN_004cd8c0): extern_util_config.Config() called from C++
// (FUN_00484410), ImportFile("tabula_rasa.cfg") into GameClient + 0x29c (gameclient.GetAppConfig, 0x004d4bb0) and
// ImportFile("login.cfg") into + 0x2a0 (GetLoginConfig, 0x004d4be0); either failing logs "******The application config file
// (tabula_rasa.cfg) could not be imported ..." / "... login config file (login.cfg) ..." and the client stops (Initialize returns -1).
#pragma once

#include <Python.h>

#include <string>

namespace engine::config {

// Registers the extern_util_config module and its Config type.
void initModule();

// A new Config object with `path` imported (ImportFile's rules); nullptr (no Python error set) when the import failed.
PyObject* newConfigFromFile(const char* path);

// GameClient::Initialize's two: tabula_rasa.cfg and login.cfg (relative to the current folder, the game's). False (the exe's
// message printed) when one could not be imported - the client does not start.
bool createEngineConfigs();
PyObject* appConfig();     // borrowed: gameclient.GetAppConfig's object (GameClient + 0x29c)
PyObject* loginConfig();   // borrowed: GetLoginConfig's (+ 0x2a0)
void releaseEngineConfigs();

// gameclient.GenerateBookmark (0x004d7530): see Config.cpp. `camera` false: no view (only the import happens); `rotation` x y z w;
// `lastMapTemplate` the option Client.Developer.LastMapTemplate's text (nullptr: unset).
void generateBookmark(bool camera, const float eye[3], const float rotation[4], const std::string* lastMapTemplate);

// Lookups on a Config object (as GetItemAsString / HasItem); `config` must be one.
bool hasItem(PyObject* config, const std::string& section, const std::string& key);
std::string itemAsString(PyObject* config, const std::string& section, const std::string& key, const std::string& fallback);

}  // namespace engine::config
