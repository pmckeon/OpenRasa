// FontMetrics.cpp - see FontMetrics.h.
#include "FontMetrics.h"

#include "UiManager.h"
#include "UiRender.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

namespace engine::ui {
namespace {

struct Face {
    std::string family;
    int unitsPerEm = 2048;
    int winAscent = 0, winDescent = 0;
    std::vector<uint16_t> advances;           // per glyph (hmtx; the last repeats)
    std::map<uint32_t, uint16_t> glyphOf;     // cmap (format 4, Unicode BMP)
    int advance(char16_t c) const {
        auto it = glyphOf.find(c);
        uint16_t g = it == glyphOf.end() ? 0 : it->second;
        if (advances.empty()) return unitsPerEm / 2;
        return advances[g < advances.size() ? g : advances.size() - 1];
    }
};

std::vector<Face> faces;
bool loaded = false;

uint16_t be16(const std::vector<uint8_t>& d, size_t o) { return o + 2 <= d.size() ? uint16_t(d[o] << 8 | d[o + 1]) : 0; }
uint32_t be32(const std::vector<uint8_t>& d, size_t o) { return uint32_t(be16(d, o)) << 16 | be16(d, o + 2); }

bool readFace(const std::vector<uint8_t>& d, Face& f) {
    std::map<std::string, size_t> tables;
    int n = be16(d, 4);
    for (int i = 0; i < n; ++i) {
        size_t r = 12 + size_t(i) * 16;
        tables[std::string(reinterpret_cast<const char*>(&d[r]), 4)] = be32(d, r + 8);
    }
    for (const char* t : {"head", "hhea", "hmtx", "cmap", "OS/2", "name"})
        if (!tables.count(t)) return false;
    f.unitsPerEm = be16(d, tables["head"] + 18);
    size_t os2 = tables["OS/2"];
    f.winAscent = be16(d, os2 + 74);
    f.winDescent = be16(d, os2 + 76);
    int metrics = be16(d, tables["hhea"] + 34);
    for (int i = 0; i < metrics; ++i) f.advances.push_back(be16(d, tables["hmtx"] + size_t(i) * 4));
    size_t cmap = tables["cmap"];
    for (int i = 0, count = be16(d, cmap + 2); i < count; ++i) {
        size_t r = cmap + 4 + size_t(i) * 8;
        size_t sub = cmap + be32(d, r + 4);
        if (be16(d, sub) != 4) continue;
        int segs = be16(d, sub + 6) / 2;
        size_t ends = sub + 14, starts = ends + segs * 2 + 2, deltas = starts + segs * 2, offsets = deltas + segs * 2;
        for (int s = 0; s < segs; ++s) {
            uint16_t end = be16(d, ends + s * 2), start = be16(d, starts + s * 2);
            int16_t delta = int16_t(be16(d, deltas + s * 2));
            uint16_t ro = be16(d, offsets + s * 2);
            for (uint32_t c = start; c <= end && c != 0xFFFF; ++c) {
                uint16_t g;
                if (ro == 0) {
                    g = uint16_t(c + delta);
                } else {
                    g = be16(d, offsets + s * 2 + ro + (c - start) * 2);
                    if (g) g = uint16_t(g + delta);
                }
                f.glyphOf.emplace(c, g);
            }
        }
        break;
    }
    // Family name (name id 1): Windows UTF-16 BE, else Macintosh Roman.
    size_t name = tables["name"];
    size_t strings = name + be16(d, name + 4);
    for (int i = 0, count = be16(d, name + 2); i < count; ++i) {
        size_t r = name + 6 + size_t(i) * 12;
        if (be16(d, r + 6) != 1) continue;
        int platform = be16(d, r), length = be16(d, r + 8), offset = be16(d, r + 10);
        std::string s;
        if (platform == 3)
            for (int k = 0; k + 1 < length; k += 2) s += char(be16(d, strings + offset + k));
        else
            for (int k = 0; k < length; ++k) s += char(d[strings + offset + k]);
        f.family = s;
        if (platform == 3) break;
    }
    return true;
}

const Face* faceFor(const std::string& family) {
    for (const Face& f : faces)
        if (f.family == family) return &f;
    return faces.empty() ? nullptr : &faces.front();
}

}  // namespace

void loadFontFiles(const std::vector<std::string>& files) {
    if (loaded) return;
    loaded = true;
    for (const std::string& file : files) {
        std::ifstream in("data/misc/" + file, std::ios::binary);
        if (!in) continue;
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Face f;
        if (bytes.size() > 12 && readFace(bytes, f)) faces.push_back(std::move(f));
    }
}

TextSize measureText(const std::string& fontId, const std::u16string& text, int wrapWidth) {
    const tr::UiExport& ex = manager().definitions();
    loadFontFiles(ex.loadedFontFiles);
    TextSize out;
    const tr::UiFont* font = ex.font(fontId);
    if (!font || font->ranges.empty()) return out;
    const tr::UiFontRange& range = font->ranges.front();
    const Face* face = faceFor(range.face);
    if (!face) return out;
    int em = int(std::trunc(range.pointSize * 4.0 / 3.0));
    double scale = double(em) / face->unitsPerEm;
    out.lineHeight = int(std::lround(face->winAscent * scale)) + int(std::lround(face->winDescent * scale));
    // No text, no lines: UIText::GetTextHeight is its laid-out lines x the line height (the 2007 build's 0x0088d080: (end - begin) /
    // 0x28 entries), and an empty text lays out none - the Debug Tracker's empty Name row collapses in retail.
    if (text.empty()) return out;
    auto width = [&](size_t a, size_t b) {
        int w = 0;
        float other = 0;   // characters of other ranges, logical units from screen pixels (rounded up at the end)
        for (size_t i = a; i < b; ++i) {
            // A character of another range (e.g. CJK in MS UI Gothic, the export's 592+ ranges): measured in that range's face by the
            // canvas (system fonts included).
            const tr::UiFontRange* r = font->rangeFor(text[i]);
            if (r && r != &range && r->face != range.face) {
                other += glyphAdvanceLogical(fontId, text[i]);
                continue;
            }
            w += int(std::lround(face->advance(text[i]) * scale)) + (range.bold ? 1 : 0);
        }
        return w + int(std::ceil(other - 0.001f));
    };
    // Lines: split at '\n', and when wrapping at the last space that fits.
    int lines = 0, widest = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(u'\n', start);
        if (end == std::u16string::npos) end = text.size();
        size_t a = start;
        do {
            size_t b = end;
            if (wrapWidth > 0) {
                while (b > a && width(a, b) > wrapWidth) {
                    size_t space = text.rfind(u' ', b - 1);
                    b = space == std::u16string::npos || space <= a ? b - 1 : space;
                }
                if (b == a) b = std::min(end, a + 1);
            }
            widest = std::max(widest, width(a, b));
            ++lines;
            a = b;
            while (a < end && text[a] == u' ' && wrapWidth > 0) ++a;
        } while (a < end);
        start = end + 1;
    }
    out.width = widest;
    out.height = lines * out.lineHeight;
    return out;
}

}  // namespace engine::ui
