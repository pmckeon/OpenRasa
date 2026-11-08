// CommandLine.cpp - see CommandLine.h.
#include "CommandLine.h"

#include "assetlibrary.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace engine {
namespace {

const char* const kOptionKeys[] = {"/NoPatch", "/AuthServer", "/NoEULA", "/autologin", "/user", "/password", "/character", "/server"};

bool isOptionKey(const std::string& key) {
    for (const char* k : kOptionKeys)
        if (key == k) return true;
    return false;
}

// The original's map insert (FUN_00401620): a bare key sets "", "key=value" only adds a key not yet present.
void addToken(CommandLine& cl, const std::string& token) {
    const size_t eq = token.find('=');
    if (eq == std::string::npos) cl.options[token] = "";
    else cl.options.emplace(token.substr(0, eq), token.substr(eq + 1));
}

void errorBox(const std::string& text) {
    std::fprintf(stderr, "Tabula Rasa Error: %s\n", text.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Tabula Rasa Error", text.c_str(), nullptr);
}

std::string lowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// login.cfg [Auth] Host = value (the original imports the file, sets the item and exports it - TRasa::Config FUN_00476160 /
// FUN_00478430 / FUN_00476780). Here the file is edited in place, every other line kept; the first change keeps
// login.cfg.openrasa-original.
bool setLoginHost(const std::filesystem::path& file, const std::string& host) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        errorBox("Could not find login.cfg");
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();
    in.close();
    const std::string newline = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> lines;
    for (size_t at = 0; at <= text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        at = end + 1;
    }
    if (!lines.empty() && lines.back().empty()) lines.pop_back();
    int section = -1, hostLine = -1, sectionEnd = -1;
    for (int i = 0; i < int(lines.size()); ++i) {
        const std::string t = trim(lines[size_t(i)]);
        if (!t.empty() && t.front() == '[') {
            if (section >= 0 && sectionEnd < 0) sectionEnd = i;
            if (lowerAscii(t) == "[auth]") section = i, sectionEnd = -1;
        } else if (section >= 0 && sectionEnd < 0) {
            const size_t eq = t.find('=');
            if (eq != std::string::npos && lowerAscii(trim(t.substr(0, eq))) == "host") hostLine = i;
        }
    }
    const std::string entry = "Host = " + host;
    if (hostLine >= 0) {
        if (lines[size_t(hostLine)] == entry) return true;
        lines[size_t(hostLine)] = entry;
    } else if (section >= 0) {
        lines.insert(lines.begin() + (sectionEnd >= 0 ? sectionEnd : int(lines.size())), entry);
    } else {
        lines.push_back("[Auth]");
        lines.push_back(entry);
    }
    std::error_code ec;
    const std::filesystem::path backup = file.parent_path() / "login.cfg.openrasa-original";
    if (!std::filesystem::exists(backup, ec)) std::filesystem::copy_file(file, backup, ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    for (const std::string& l : lines) out << l << newline;
    if (!out) {
        errorBox("Could not save login.cfg");
        return false;
    }
    std::fprintf(stderr, "[engine] login.cfg [Auth] Host = %s (/AuthServer)\n", host.c_str());
    return true;
}

}  // namespace

CommandLine parseCommandLine(int argc, char** argv) {
    CommandLine cl;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.empty()) continue;
        if (isOptionKey(arg.substr(0, arg.find('=')))) {
            addToken(cl, arg);
        } else if (cl.gameFolder.empty()) {
            cl.gameFolder = arg;
        } else {
            addToken(cl, arg);
            cl.unknown.push_back(arg);
            std::fprintf(stderr, "warning: command-line argument %s is not one the game reads\n", arg.c_str());
        }
    }
    return cl;
}

bool runStartupOptions(const std::string& gameFolder, const CommandLine& cl, std::string& launcherId) {
    const std::filesystem::path root = std::filesystem::u8path(gameFolder);
    // FUN_004013b0: LauncherID.ini [Settings] LauncherID, "TabulaRasa" without the file; an empty result quits.
    std::error_code ec;
    const std::filesystem::path idFile = root / "LauncherID.ini";
    launcherId = "TabulaRasa";
    if (std::filesystem::exists(idFile, ec)) {
        try {
            tr::ConfigFile cfg(idFile.string());
            launcherId = cfg.value("Settings", "LauncherID", "");
            if (launcherId.empty()) errorBox("Could not read LauncherID from file");
        } catch (const std::exception&) {
            errorBox("Could not import LauncherID file");
            launcherId.clear();
        }
        if (launcherId.empty()) return false;
    }
    if (!cl.has("/NoPatch"))
        std::fprintf(stderr, "[engine] no /NoPatch: tabula_rasa.exe would start FindLauncher.exe (the patcher) and quit; OpenRasa has "
                             "no patcher and starts the game\n");
    if (const std::string* host = cl.value("/AuthServer"))
        if (!setLoginHost(root / "login.cfg", *host)) return false;
    return true;
}

}  // namespace engine
