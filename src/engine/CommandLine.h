// CommandLine.h - tabula_rasa.exe's command-line options (WinMain FUN_00402130, traced 2026-10-25), and OpenRasa's game folder argument.
//
// The original splits its command line at spaces (strtok, at most 1,023 characters, FUN_00401620) into "/key=value" or "/key" tokens,
// kept in a std::map: names are case-sensitive, the first "/key=value" of a key wins, a bare "/key" sets its value to "". It reads:
//   /NoPatch            required: without it the exe starts FindLauncher.exe /NoPatch /LauncherArgs=" /LaunchGame=<LauncherID>
//                       /StartLocation=\"{_stub_dir}\" " (the patcher, FUN_00401550) and quits. OpenRasa has no patcher: it accepts the
//                       option and always starts the game.
//   /AuthServer=<host>  written to login.cfg as [Auth] Host before the game starts; "Could not find login.cfg" / "Could not save
//                       login.cfg" quit it.
//   /NoEULA             option Client.EULA.Skip = true.
//   /autologin=<file>   an INI file read when it exists: [AutoLogin] User, Password, Character, Server (strtol base 0, default -1);
//                       "Error loading autologin file '<file>'" when it does not parse (the game goes on).
//   /user= /password= /character= /server=<int, atoi>   override the file's values.
//   The four values always become the options Client.AutoLogin.Username / Password / Character / Server ("" / -1 when not given);
//   when a user name, a password or a server is given, Client.Developer.SkipToLogin and Client.EULA.Skip are set too
//   (FUN_004017b0).
// Not an option: LauncherID.ini [Settings] LauncherID ("TabulaRasa" without the file; "Could not import LauncherID file" / "Could not
// read LauncherID from file" quit the game, FUN_004013b0) - Client.Info.IsPTS = (LauncherID == "TabulaRasaPTS").
// OpenRasa takes each command-line argument as one token (the operating system has already split and unquoted them). An argument whose
// key (before '=') is one of the options above is an option; else the first other argument is the game folder; anything else is
// stored like the original (and unused) with a warning.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace engine {

struct CommandLine {
    std::map<std::string, std::string> options;   // "/key" -> value
    std::string gameFolder;                        // OpenRasa's own argument ("" = not given)
    std::vector<std::string> unknown;              // arguments neither an option nor the folder

    bool has(const char* key) const { return options.count(key) != 0; }
    const std::string* value(const char* key) const {
        auto it = options.find(key);
        return it == options.end() ? nullptr : &it->second;
    }
};

CommandLine parseCommandLine(int argc, char** argv);

// What WinMain does before the engine starts, in its order: the LauncherID (into launcherId), /NoPatch, /AuthServer. False when the
// original would quit (its message box shown).
bool runStartupOptions(const std::string& gameFolder, const CommandLine& commandLine, std::string& launcherId);

}  // namespace engine
