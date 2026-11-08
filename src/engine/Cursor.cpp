// Cursor.cpp - see Cursor.h.
#include "Cursor.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

namespace engine::cursor {
namespace {

std::map<std::string, SDL_Cursor*> cursors;
std::deque<std::string> stack;

uint32_t u32(const std::vector<uint8_t>& d, size_t at) {
    return at + 4 <= d.size() ? uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24 : 0;
}
uint16_t u16(const std::vector<uint8_t>& d, size_t at) { return at + 2 <= d.size() ? uint16_t(d[at] | d[at + 1] << 8) : 0; }

// The PE file's resources: (type, id) -> data (the first language).
struct Resources {
    std::vector<uint8_t> file;
    std::map<std::pair<uint32_t, uint32_t>, std::pair<size_t, size_t>> entries;   // -> file offset, size

    bool read(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (file.size() < 0x40 || file[0] != 'M' || file[1] != 'Z') return false;
        const size_t pe = u32(file, 0x3c);
        if (u32(file, pe) != 0x4550) return false;
        const size_t sections = u16(file, pe + 6), optional = u16(file, pe + 20), opt = pe + 24;
        const bool pe32plus = u16(file, opt) == 0x20b;
        const uint32_t rsrcRva = u32(file, opt + (pe32plus ? 112 : 96) + 2 * 8);
        if (!rsrcRva) return false;
        auto offset = [&](uint32_t rva) -> size_t {
            for (size_t s = 0; s < sections; ++s) {
                const size_t h = opt + optional + 40 * s;
                const uint32_t va = u32(file, h + 12), vsize = std::max(u32(file, h + 8), u32(file, h + 16)), raw = u32(file, h + 20);
                if (rva >= va && rva < va + vsize) return raw + (rva - va);
            }
            return 0;
        };
        const size_t base = offset(rsrcRva);
        if (!base) return false;
        // type -> id -> language -> data entry.
        auto level = [&](size_t dir, auto&& each) {
            const size_t count = size_t(u16(file, base + dir + 12)) + u16(file, base + dir + 14);
            for (size_t i = 0; i < count && i < 4096; ++i) each(u32(file, base + dir + 16 + 8 * i), u32(file, base + dir + 20 + 8 * i));
        };
        level(0, [&](uint32_t type, uint32_t typeDir) {
            if (!(typeDir & 0x80000000) || (type & 0x80000000)) return;
            level(typeDir & 0x7fffffff, [&](uint32_t id, uint32_t idDir) {
                if (!(idDir & 0x80000000) || (id & 0x80000000)) return;
                level(idDir & 0x7fffffff, [&](uint32_t, uint32_t dataEntry) {
                    if (dataEntry & 0x80000000) return;
                    const size_t at = offset(u32(file, base + dataEntry)), size = u32(file, base + dataEntry + 4);
                    if (at && at + size <= file.size()) entries.emplace(std::make_pair(type, id), std::make_pair(at, size));
                });
            });
        });
        return true;
    }
    std::vector<uint8_t> get(uint32_t type, uint32_t id) const {
        auto it = entries.find({type, id});
        if (it == entries.end()) return {};
        return std::vector<uint8_t>(file.begin() + std::ptrdiff_t(it->second.first),
                                    file.begin() + std::ptrdiff_t(it->second.first + it->second.second));
    }
};

// A cursor group (RT_GROUP_CURSOR 12) -> its first cursor (RT_CURSOR 1): u16 hotspot x, y, then a DIB (BITMAPINFOHEADER, the height
// counting the colour and mask halves, bottom-up rows; 1 / 4 / 8-bit with a palette, 24 or 32-bit), then the 1-bit AND mask. A pixel is
// transparent where the mask is set (the screen-inverting case - mask set, colour not black - does not occur in the game's cursors).
SDL_Cursor* makeCursor(const Resources& r, uint32_t groupId) {
    const std::vector<uint8_t> group = r.get(12, groupId);
    if (group.size() < 6 + 14 || u16(group, 4) == 0) return nullptr;
    const std::vector<uint8_t> c = r.get(1, u16(group, 6 + 12));
    if (c.size() < 4 + 40) return nullptr;
    const int hotX = u16(c, 0), hotY = u16(c, 2);
    const size_t header = u32(c, 4);
    const int width = int(u32(c, 8)), height = int(u32(c, 12)) / 2, bits = u16(c, 18);
    if (width <= 0 || width > 256 || height <= 0 || height > 256) return nullptr;
    const size_t colours = bits <= 8 ? (u32(c, 4 + 32) ? u32(c, 4 + 32) : (1u << bits)) : 0;
    const size_t palette = 4 + header, pixels = palette + 4 * colours;
    const size_t row = size_t((width * bits + 31) / 32) * 4, maskRow = size_t((width + 31) / 32) * 4;
    const size_t mask = pixels + row * size_t(height);
    if (mask + maskRow * size_t(height) > c.size()) return nullptr;
    SDL_Surface* s = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (!s) return nullptr;
    for (int y = 0; y < height; ++y) {
        const size_t src = pixels + row * size_t(height - 1 - y), m = mask + maskRow * size_t(height - 1 - y);
        uint8_t* out = static_cast<uint8_t*>(s->pixels) + size_t(y) * size_t(s->pitch);
        for (int x = 0; x < width; ++x, out += 4) {
            uint8_t b = 0, g = 0, red = 0, a = 255;
            if (bits == 24 || bits == 32) {
                const size_t p = src + size_t(x) * size_t(bits / 8);
                b = c[p], g = c[p + 1], red = c[p + 2];
                if (bits == 32) a = c[p + 3];
            } else {
                const int index = (c[src + size_t(x * bits / 8)] >> (8 - bits - (x * bits) % 8)) & ((1 << bits) - 1);
                const size_t p = palette + 4 * size_t(index);
                b = c[p], g = c[p + 1], red = c[p + 2];
            }
            if ((c[m + size_t(x / 8)] >> (7 - x % 8)) & 1) a = 0;
            out[0] = red, out[1] = g, out[2] = b, out[3] = a;
        }
    }
    // OPENRASA_CURSORDUMP=<folder> (tests): each cursor's pixels as raw RGBA (<group id>.rgba, width x height).
    if (const char* dump = std::getenv("OPENRASA_CURSORDUMP"); dump && *dump) {
        const std::string file = (std::filesystem::u8path(dump) / (std::to_string(groupId) + ".rgba")).string();
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            for (int y = 0; y < height; ++y) std::fwrite(static_cast<uint8_t*>(s->pixels) + size_t(y) * size_t(s->pitch), 4, size_t(width), f);
            std::fclose(f);
        }
    }
    SDL_Cursor* cursor = SDL_CreateColorCursor(s, hotX, hotY);
    SDL_DestroySurface(s);
    return cursor;
}

// FUN_008a3cc0: every occurrence of the name except the back element, and none while fewer than two remain.
void remove(const std::string& name) {
    if (stack.size() < 2) return;
    for (size_t i = 0; i + 1 < stack.size();) {
        if (stack[i] == name) stack.erase(stack.begin() + std::ptrdiff_t(i));
        else ++i;
    }
}

// FUN_008a3c40: the front's cursor, when it has one.
void show() {
    if (stack.empty()) return;
    auto it = cursors.find(stack.front());
    if (it != cursors.end() && it->second) SDL_SetCursor(it->second);
}

}  // namespace

void load(const std::string& gameFolder) {
    Resources r;
    const std::filesystem::path exe = std::filesystem::u8path(gameFolder) / "tabula_rasa.exe";
    const bool haveExe = r.read(exe);
    if (!haveExe) std::fprintf(stderr, "[ui] cursors: %s not readable - the system's cursors stand in\n", exe.string().c_str());
    // A cursor the exe cannot give falls back to one of the system's (OpenRasa ships no copy of the game's art): the arrow, the
    // I-beam, and the hand for the four action cursors.
    const struct {
        const char* name;
        uint32_t id;
        SDL_SystemCursor fallback;
    } table[] = {{"Pointer", 115, SDL_SYSTEM_CURSOR_DEFAULT}, {"Use", 120, SDL_SYSTEM_CURSOR_POINTER},
                 {"TextEntry", 116, SDL_SYSTEM_CURSOR_TEXT},  {"Trade", 124, SDL_SYSTEM_CURSOR_POINTER},
                 {"Buy", 122, SDL_SYSTEM_CURSOR_POINTER},     {"Sell", 123, SDL_SYSTEM_CURSOR_POINTER}};
    for (const auto& t : table) {
        SDL_Cursor* c = haveExe ? makeCursor(r, t.id) : nullptr;
        if (haveExe && !c) std::fprintf(stderr, "[ui] cursor %s (resource %u) could not be made: %s\n", t.name, t.id, SDL_GetError());
        if (!c) c = SDL_CreateSystemCursor(t.fallback);
        cursors[t.name] = c;
    }
    push("Pointer");
}

void push(const std::string& name) {
    if (name.empty()) return;
    remove(name);
    if (stack.empty() || stack.back() != name) stack.push_front(name);
    show();
}

void pop(const std::string& name) {
    remove(name);
    show();
}

void setShown(bool shown) {
    if (shown) SDL_ShowCursor();
    else SDL_HideCursor();
}

void shutdown() {
    for (auto& [name, c] : cursors)
        if (c) SDL_DestroyCursor(c);
    cursors.clear();
    stack.clear();
}

}  // namespace engine::cursor
