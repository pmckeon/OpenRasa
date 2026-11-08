#include "UiCanvas.h"

#include "dds.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <tuple>

// FreeType (vcpkg "freetype"): hinted glyphs like the game's GDI ones, on every platform.
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_DRIVER_H
#include FT_MODULE_H
#include FT_TRUETYPE_TABLES_H
#include FT_GASP_H

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int32_t intAttribute(const tr::XmlNode& n, const char* name, int32_t fallback = 0) {
    const std::string& v = n.attribute(name);
    return v.empty() ? fallback : int32_t(std::strtol(v.c_str(), nullptr, 10));
}

bool attributeIsTrue(const tr::XmlNode& n, const char* name) { return n.attribute(name) == "true"; }

// The engine's anchor point of a w x h rectangle (tabula_rasa.exe FUN_008b7060; integer halves).
void anchorPoint(int32_t anchor, int32_t w, int32_t h, int32_t& x, int32_t& y) {
    switch (anchor) {
        case 1: x = w / 2; y = 0; break;
        case 2: x = w; y = 0; break;
        case 3: x = w; y = h / 2; break;
        case 4: x = w; y = h; break;
        case 5: x = w / 2; y = h; break;
        case 6: x = 0; y = h; break;
        case 7: x = 0; y = h / 2; break;
        case 8: x = w / 2; y = h / 2; break;
        default: x = 0; y = 0; break;
    }
}

// Colour modulation (FUN_008b7280, per byte): 0 if either is 0, the other if one is 255, else round(a x b / 255).
uint8_t multiplyColour(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    if (a == 255) return b;
    if (b == 255) return a;
    return uint8_t(std::lround(double(a) * double(b) / 255.0));
}

UiCanvas::Rect intersect(const UiCanvas::Rect& a, const UiCanvas::Rect& b) {
    return UiCanvas::Rect{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

std::vector<uint32_t> codepoints(const std::string& s) {     // UTF-8 (invalid bytes taken as Latin-1)
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        uint32_t cp = extra == 3 ? c & 0x7 : extra == 2 ? c & 0xF : extra == 1 ? c & 0x1F : c;
        bool ok = true;
        for (int k = 1; k <= extra && ok; ++k) {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        if (!ok) { cp = c; extra = 0; }
        out.push_back(cp);
        i += size_t(extra) + 1;
    }
    return out;
}

}  // namespace

// ------------------------------------------------------------------------------------------
// Fonts
// ------------------------------------------------------------------------------------------
// The game's text (tabula_rasa.exe gfxFontImpl.cpp, decompiled 2026-10-15): FUN_006ae9e0 creates a GDI font (CreateFontIndirectA, weight
// 400 or 700 for bold - a synthetic bold on these regular faces -, quality 4 = ANTIALIASED_QUALITY: hinted grayscale) and a 32-bit DIB;
// FUN_006af200 draws a glyph white on transparent: the shadow (flag 4) in 0x00FFFF at (x + 1, 1), the outline (flag 8, no shipped font)
// four times in 0x00FFC0, the glyph in 0xFF00FF at (x, 0), x = -abcA when negative, and packs each pixel into an A4R4G4B4 texel:
// alpha = blue's high nibble (every layer), grey = table[red's high nibble][alpha], table[m][a] = a ? min(15, round(15 m / a)) : m
// (FUN_006ae5a0; 15.0 at 0x00c2b770) - the shadow comes out black, alpha has 16 levels. Cell abcB + max(abcA, 0) + max(abcC, 0) (+ 1
// shadow) by tmHeight; advance abcA + abcB + abcC; kerning pairs from the font.
// Here the glyphs come from FreeType with its TrueType bytecode interpreter in version 35 (the classic, GDI-like hinting), the layers
// combined as GDI blends them into the DIB (blue = main + (1 - main) x shadow, red = main) and packed the game's way.
struct UiCanvas::FontLibrary {
    FT_Library library = nullptr;
    FontLibrary() {
        if (FT_Init_FreeType(&library) != 0) {
            library = nullptr;
            return;
        }
        FT_UInt interpreter = TT_INTERPRETER_VERSION_35;
        FT_Property_Set(library, "truetype", "interpreter-version", &interpreter);
    }
    ~FontLibrary() {
        if (library) FT_Done_FreeType(library);
    }
};

struct UiCanvas::Face {
    FT_Face face = nullptr;
    std::shared_ptr<std::vector<uint8_t>> data;    // FreeType reads the font from this memory
    float winAscent = 0, winDescent = 0;            // OS/2 usWinAscent / usWinDescent in font units (GDI's tmAscent / tmDescent)
    // GDI takes tmAscent / tmDescent from the font's VDMX table at the sizes it lists (yMax, -yMin of the 1:1 ratio group) and scales the
    // OS/2 win metrics only at other sizes: Conduit ITC Light (f1.lff) at em 20 gives 18 / 5 from VDMX where the win metrics round to
    // 17 / 5 (checked against this machine's GDI over em 14 - 24 for f1 / f2.lff). tmHeight feeds the line height (GetLineHeight).
    std::map<int32_t, std::pair<int32_t, int32_t>> vdmx;   // ppem -> (ascent, descent)
    int32_t currentEm = -1;
    struct Glyph {
        int32_t width = 0, height = 0;
        int32_t originX = 0;                        // texel column 0 relative to the pen
        int32_t top = 0;                            // texel row 0 relative to the cell's top
        int32_t advance = 0;
        std::vector<uint8_t> alpha, grey;           // 4-bit values 0..15 (the game's A4R4G4B4 texel)
    };
    std::map<std::tuple<int32_t, uint32_t, bool, bool>, Glyph> glyphs;     // (em, code point, bold, shadow)
    ~Face() {
        if (face) FT_Done_Face(face);
    }
    float units(int32_t em) const { return float(em) / float(face->units_per_EM); }
    // GDI's TEXTMETRIC at an em (integers): tmAscent, tmDescent, tmHeight = their sum, tmInternalLeading = tmHeight - em.
    int32_t ascent(int32_t em) const {
        if (const auto v = vdmx.find(em); v != vdmx.end()) return v->second.first;
        return int32_t(std::lround(winAscent * units(em)));
    }
    int32_t descent(int32_t em) const {
        if (const auto v = vdmx.find(em); v != vdmx.end()) return v->second.second;
        return int32_t(std::lround(winDescent * units(em)));
    }
    void readMetrics() {
        const TT_OS2* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
        winAscent = os2 ? float(os2->usWinAscent) : float(face->ascender);
        winDescent = os2 ? float(os2->usWinDescent) : float(-face->descender);
        FT_ULong length = 0;
        const FT_ULong tag = FT_MAKE_TAG('V', 'D', 'M', 'X');
        if (FT_Load_Sfnt_Table(face, tag, 0, nullptr, &length) != 0 || length < 6) return;
        std::vector<uint8_t> t(length);
        if (FT_Load_Sfnt_Table(face, tag, 0, t.data(), &length) != 0) return;
        auto u16 = [&](size_t o) { return o + 2 <= t.size() ? int32_t(t[o] << 8 | t[o + 1]) : 0; };
        const int32_t ratios = u16(4);
        size_t group = 0;
        for (int32_t i = 0; i < ratios && !group; ++i) {   // first ratio record that is 1:1 or "any" (0:0)
            const size_t r = 6 + 4 * size_t(i);
            if (r + 4 > t.size()) return;
            const uint8_t x = t[r + 1], y0 = t[r + 2], y1 = t[r + 3];
            if ((x == 0 && y0 == 0 && y1 == 0) || (x != 0 && y0 <= x && x <= y1)) group = size_t(u16(6 + 4 * size_t(ratios) + 2 * size_t(i)));
        }
        if (!group) return;
        const int32_t records = u16(group);
        for (int32_t i = 0; i < records; ++i) {
            const size_t o = group + 4 + 6 * size_t(i);
            if (o + 6 > t.size()) break;
            vdmx[u16(o)] = {int32_t(int16_t(u16(o + 2))), -int32_t(int16_t(u16(o + 4)))};
        }
    }
    int32_t cellHeight(int32_t em) const { return ascent(em) + descent(em); }
    int32_t internalLeading(int32_t em) const { return cellHeight(em) - em; }
    void setSize(int32_t em) {
        if (em == currentEm) return;
        FT_Set_Pixel_Sizes(face, 0, FT_UInt(em));
        currentEm = em;
    }
    const Glyph& glyph(int32_t em, uint32_t cp, bool bold, bool shadow) {
        const auto key = std::make_tuple(em, cp, bold, shadow);
        auto it = glyphs.find(key);
        if (it != glyphs.end()) return it->second;
        static uint8_t table[16][16];
        static bool tableBuilt = false;
        if (!tableBuilt) {
            for (int m = 0; m < 16; ++m)
                for (int a = 0; a < 16; ++a) table[m][a] = uint8_t(a < 1 ? m : std::clamp(int(std::lround(double(m) * 15.0 / double(a))), 0, 15));
            tableBuilt = true;
        }
        Glyph g;
        setSize(em);
        std::vector<uint8_t> cover;                 // the main glyph's coverage
        int32_t bw = 0, bh = 0, left = 0, top = 0;
        // GDI's ANTIALIASED_QUALITY follows the font's gasp table: at sizes whose range lacks GASP_DOGRAY it draws hinted, without
        // antialiasing. The game's f1 / f2.lff have gasp (8: gray, 16: gridfit, above: both), so text of 9 - 16 pixels per em (the
        // chat entry, small labels) is solid 1-bit - drawn antialiased its periods and i-dots came out as single faint pixels.
        const FT_Int gasp = FT_Get_Gasp(face, FT_UInt(em));
        const bool mono = gasp != FT_GASP_NO_TABLE && !(gasp & FT_GASP_DO_GRAY);
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER | (mono ? FT_LOAD_TARGET_MONO : FT_LOAD_TARGET_NORMAL)) == 0) {
            const FT_GlyphSlot slot = face->glyph;
            const FT_Bitmap& bm = slot->bitmap;
            bw = int32_t(bm.width);
            bh = int32_t(bm.rows);
            left = slot->bitmap_left;
            top = slot->bitmap_top;
            g.advance = int32_t((slot->advance.x + 32) >> 6);
            cover.assign(size_t(bw) * size_t(bh), 0);
            // GDI's antialiased text is brighter at partial pixels than FreeType's coverage: fitted on this machine's GDI
            // (ANTIALIASED_QUALITY, Conduit ITC Light at em 18 - 28, 4,000+ pixels) as coverage^0.55 (RMS 37 -> 21 levels; the rest is
            // outline differences). UNVERIFIED which GDI step does it (a text gamma); the mono path (gasp) matched GDI exactly.
            static uint8_t gdiLevel[256];
            static bool gdiLevelBuilt = false;
            if (!gdiLevelBuilt) {
                for (int c = 0; c < 256; ++c) gdiLevel[c] = uint8_t(std::lround(255.0 * std::pow(double(c) / 255.0, 0.55)));
                gdiLevelBuilt = true;
            }
            for (int32_t y = 0; y < bh; ++y)
                for (int32_t x = 0; x < bw; ++x)
                    // An embedded bitmap (e.g. MS UI Gothic's at small sizes, which GDI draws too) comes as 1 bit a pixel.
                    cover[size_t(y) * size_t(bw) + size_t(x)] = bm.pixel_mode == FT_PIXEL_MODE_MONO
                                                                    ? ((bm.buffer[y * bm.pitch + (x >> 3)] >> (7 - (x & 7))) & 1 ? 255 : 0)
                                                                    : gdiLevel[bm.buffer[y * bm.pitch + x]];
        }
        if (bold) {                                 // synthetic bold: the coverage spread one pixel right, one pixel wider advance
            std::vector<uint8_t> wide(size_t(bw + 1) * size_t(bh), 0);
            for (int32_t y = 0; y < bh; ++y)
                for (int32_t x = 0; x <= bw; ++x) {
                    const uint8_t here = x < bw ? cover[size_t(y) * size_t(bw) + size_t(x)] : 0;
                    const uint8_t prev = x > 0 ? cover[size_t(y) * size_t(bw) + size_t(x - 1)] : 0;
                    wide[size_t(y) * size_t(bw + 1) + size_t(x)] = std::max(here, prev);
                }
            cover.swap(wide);
            bw += 1;
            g.advance += 1;
        }
        // The cell: the glyph bitmap plus the shadow one pixel right and down.
        const int32_t pad = shadow ? 1 : 0;
        g.width = bw + pad;
        g.height = bh + pad;
        g.originX = left;
        g.top = ascent(em) - top;
        g.alpha.assign(size_t(g.width) * size_t(g.height), 0);
        g.grey.assign(g.alpha.size(), 0);
        auto at = [&](int32_t x, int32_t y) -> float {
            return x >= 0 && y >= 0 && x < bw && y < bh ? float(cover[size_t(y) * size_t(bw) + size_t(x)]) / 255.0f : 0.0f;
        };
        for (int32_t y = 0; y < g.height; ++y)
            for (int32_t x = 0; x < g.width; ++x) {
                const float main = at(x, y), shade = shadow ? at(x - 1, y - 1) : 0.0f;
                const uint32_t blue = uint32_t(std::lround((main + (1.0f - main) * shade) * 255.0f)), red = uint32_t(std::lround(main * 255.0f));
                const uint32_t alpha = blue >> 4, mainNibble = red >> 4;
                g.alpha[size_t(y) * size_t(g.width) + size_t(x)] = uint8_t(alpha);
                g.grey[size_t(y) * size_t(g.width) + size_t(x)] = table[mainNibble][alpha];
            }
        return glyphs.emplace(key, std::move(g)).first->second;
    }
    int32_t kern(int32_t em, uint32_t first, uint32_t second) {
        if (!FT_HAS_KERNING(face)) return 0;
        setSize(em);
        FT_Vector delta{};
        // GDI's GetKerningPairs (the exe's FUN_006af120 reads them): the font's value x em / unitsPerEm rounded half up - matches 939 of
        // EurostileTRegExt's 943 pairs at em 16 / 20 and all at 14 on this machine's GDI. FreeType's grid-fitted kerning was a pixel off
        // in many (W-a 0 for GDI's -1, T-a -1 for -2), enough to wrap titles retail keeps on one line.
        FT_Get_Kerning(face, FT_Get_Char_Index(face, first), FT_Get_Char_Index(face, second), FT_KERNING_UNSCALED, &delta);
        return int32_t(std::floor(double(delta.x) * double(em) / double(face->units_per_EM) + 0.5));
    }
};

UiCanvas::UiCanvas() = default;
UiCanvas::~UiCanvas() {
    faces_.clear();                                  // before the library
    fontLibrary_.reset();
}

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::filesystem::path> systemFontDirectories() {
    std::vector<std::filesystem::path> dirs;
#ifdef _WIN32
    const char* windir = std::getenv("WINDIR");
    dirs.emplace_back(std::filesystem::path(windir ? windir : "C:\\Windows") / "Fonts");
#elif defined(__APPLE__)
    dirs.emplace_back("/System/Library/Fonts");
    dirs.emplace_back("/Library/Fonts");
    if (const char* home = std::getenv("HOME")) dirs.emplace_back(std::filesystem::path(home) / "Library" / "Fonts");
#else
    dirs.emplace_back("/usr/share/fonts");
    dirs.emplace_back("/usr/local/share/fonts");
    if (const char* home = std::getenv("HOME")) {
        dirs.emplace_back(std::filesystem::path(home) / ".fonts");
        dirs.emplace_back(std::filesystem::path(home) / ".local" / "share" / "fonts");
    }
#endif
    return dirs;
}

// The system's fonts by family: every regular (not bold, not italic) face of every .ttf / .ttc / .otf in the system font folders, the
// first in folder order for each family - the face the old file-by-file search found. Built once per process on its own thread and
// FreeType library (opening each face from its file reads only its tables), started by the first UiCanvas that needs fonts; reading
// whole files one after another until one matched took ~0.9 s to reach msgothic.ttc ("MS UI Gothic", the hint text's range font) on
// the loading screen's first draw.
struct SystemFontEntry {
    std::string path;
    FT_Long index = 0;
};
std::shared_future<std::map<std::string, SystemFontEntry>>& systemFontIndexFuture() {
    static std::shared_future<std::map<std::string, SystemFontEntry>> future;
    return future;
}
void startSystemFontIndex() {
    static std::once_flag once;
    std::call_once(once, [] {
        systemFontIndexFuture() = std::async(std::launch::async, [] {
            std::map<std::string, SystemFontEntry> index;
            FT_Library library = nullptr;
            if (FT_Init_FreeType(&library) != 0) return index;
            for (const auto& dir : systemFontDirectories()) {
                std::error_code ec;
                for (auto entry = std::filesystem::recursive_directory_iterator(dir, ec);
                     !ec && entry != std::filesystem::recursive_directory_iterator(); entry.increment(ec)) {
                    const std::string ext = lowerAscii(entry->path().extension().string());
                    if (ext != ".ttf" && ext != ".ttc" && ext != ".otf") continue;
                    const std::string path = entry->path().string();
                    FT_Long faces = 1;
                    for (FT_Long k = 0; k < faces; ++k) {
                        FT_Face face = nullptr;
                        if (FT_New_Face(library, path.c_str(), k, &face) != 0) break;
                        faces = face->num_faces;
                        if (face->family_name && (face->style_flags & (FT_STYLE_FLAG_BOLD | FT_STYLE_FLAG_ITALIC)) == 0)
                            index.emplace(lowerAscii(face->family_name), SystemFontEntry{path, k});
                        FT_Done_Face(face);
                    }
                }
            }
            FT_Done_FreeType(library);
            return index;
        }).share();
    });
}

}  // namespace

UiCanvas::Face* UiCanvas::face(const std::string& family) {
    auto it = faces_.find(family);
    if (it != faces_.end()) return it->second.get();
    if (!fontLibrary_) fontLibrary_ = std::make_unique<FontLibrary>();
    if (!fontLibrary_->library) return (faces_[family] = nullptr).get();
    if (!fontFilesListed_) {
        fontFilesListed_ = true;
        // The game's own fonts first (Definitions/Fonts LoadedFontFile: TrueType files in data\misc); the system's come from systemFontIndex below.
        if (ui_)
            for (const std::string& file : ui_->loadedFontFiles) {
                tr::Bytes bytes;
                auto data = std::make_shared<std::vector<uint8_t>>();
                if (assets_ && assets_->tryRead(file, bytes)) data->assign(bytes.begin(), bytes.end());
                else {
                    std::ifstream in(std::filesystem::path(gameRoot_) / "data" / "misc" / file, std::ios::binary);
                    if (in) data->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                }
                if (!data->empty()) fontFiles_.emplace_back(file, data);
            }
    }
    const std::string wanted = lowerAscii(family);
    std::unique_ptr<Face> found;
    for (auto& file : fontFiles_) {
        std::shared_ptr<std::vector<uint8_t>> data = file.second;
        if (!data) {
            data = std::make_shared<std::vector<uint8_t>>();
            std::ifstream in(file.first, std::ios::binary);
            if (in) data->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        if (data->empty()) continue;
        // Every face of the file (collections hold several); a regular face of the family.
        FT_Long faces = 1;
        for (FT_Long index = 0; index < faces && !found; ++index) {
            FT_Face ftFace = nullptr;
            if (FT_New_Memory_Face(fontLibrary_->library, data->data(), FT_Long(data->size()), index, &ftFace) != 0) break;
            faces = ftFace->num_faces;
            const bool match = ftFace->family_name && lowerAscii(ftFace->family_name) == wanted &&
                               (ftFace->style_flags & (FT_STYLE_FLAG_BOLD | FT_STYLE_FLAG_ITALIC)) == 0;
            if (!match) {
                FT_Done_Face(ftFace);
                continue;
            }
            found = std::make_unique<Face>();
            found->face = ftFace;
            found->data = data;
            found->readMetrics();
        }
        if (found) {
            file.second = data;                    // keep files that supplied a face
            break;
        }
    }
    // Else the system's face of that family (the index above). The two Windows families the UI export names (Arial: the login's password
    // dots; MS UI Gothic: the CJK ranges) are missing on most other systems; then a substitute with the same metrics or coverage, as
    // fontconfig picks them (Liberation Sans is metric-compatible with Arial). On Windows the families are always there.
    if (!found) {
        startSystemFontIndex();
        const auto& index = systemFontIndexFuture().get();
        auto it = index.find(wanted);
        static const std::map<std::string, std::vector<std::string>> substitutes = {
            {"arial", {"liberation sans", "arimo", "nimbus sans", "dejavu sans"}},
            {"ms ui gothic", {"noto sans cjk jp", "source han sans jp", "ipagothic", "droid sans fallback"}},
        };
        if (it == index.end())
            if (const auto sub = substitutes.find(wanted); sub != substitutes.end())
                for (const std::string& name : sub->second)
                    if ((it = index.find(name)) != index.end()) break;
        if (it != index.end()) {
            auto data = std::make_shared<std::vector<uint8_t>>();
            std::ifstream in(std::filesystem::u8path(it->second.path), std::ios::binary);
            if (in) data->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            FT_Face ftFace = nullptr;
            if (!data->empty() && FT_New_Memory_Face(fontLibrary_->library, data->data(), FT_Long(data->size()), it->second.index, &ftFace) == 0) {
                found = std::make_unique<Face>();
                found->face = ftFace;
                found->data = data;
                found->readMetrics();
            }
        }
    }
    if (!found) missingFonts_.insert(family);
    return (faces_[family] = std::move(found)).get();
}

// ------------------------------------------------------------------------------------------
// Sources and layout
// ------------------------------------------------------------------------------------------
void UiCanvas::setSources(const tr::UiExport* ui, const tr::AssetLibrary* assets, tr::GameData* game, const std::string& gameRoot) {
    nodeCache_.clear();
    ui_ = ui;
    if (ui_) {
        width_ = 1280;
        height_ = 960;
    }
    assets_ = assets;
    game_ = game;
    gameRoot_ = gameRoot;
    startSystemFontIndex();
    images_.clear();
    faces_.clear();
    fontFiles_.clear();
    loadAnimations();
    fontFilesListed_ = false;
    drawn_.clear();
    editLayouts_.clear();
    missingTextures_.clear();
    missingFonts_.clear();
    missingTextIds_.clear();
}

void UiCanvas::setExport(const tr::UiExport* ui) {
    ui_ = ui;
    loadAnimations();
}

void UiCanvas::loadAnimations() {
    animations_.clear();
    if (ui_) {
        for (const tr::XmlNode& a : ui_->animations.children) {
            if (a.tag != "Animation") continue;
            const tr::XmlNode* id = a.child("id");
            if (!id || id->text.empty()) continue;
            Animation anim;
            const tr::XmlNode* type = a.child("animType");
            anim.bitmap = type && type->text == "Bitmap";
            const tr::XmlNode* playback = a.child("playbackType");
            if (playback) anim.playback = playback->text == "pingpong" ? 2 : playback->text == "repeat" ? 1 : 0;
            auto readTrack = [&](const char* tag, AnimTrack& track, std::initializer_list<const char*> names) {
                const tr::XmlNode* t = a.child(tag);
                if (!t) return;
                track.discrete = t->attribute("discrete") == "true";
                for (const tr::XmlNode& f : t->children) {
                    AnimKey key;
                    key.time = uint32_t(std::strtoul(f.attribute("time").c_str(), nullptr, 10));
                    int k = 0;
                    for (const char* n : names) key.v[k++] = std::strtof(f.attribute(n).c_str(), nullptr);
                    track.keys.push_back(key);
                }
                std::stable_sort(track.keys.begin(), track.keys.end(), [](const AnimKey& x, const AnimKey& y) { return x.time < y.time; });
                if (!track.keys.empty()) anim.length = std::max(anim.length, track.keys.back().time);
            };
            if (anim.bitmap) {
                readTrack("uvTrack", anim.uv, {"X", "Y", "Z", "W"});
                readTrack("rotTrack", anim.rot, {"rot"});
                if (const tr::XmlNode* r = a.child("rotTrack")) {
                    if (r->hasAttribute("anchorX")) anim.anchorX = std::strtof(r->attribute("anchorX").c_str(), nullptr);
                    if (r->hasAttribute("anchorY")) anim.anchorY = std::strtof(r->attribute("anchorY").c_str(), nullptr);
                }
            } else {
                readTrack("scaleTrack", anim.scale, {"X", "Y"});
                readTrack("transTrack", anim.trans, {"X", "Y"});
                readTrack("colorTrack", anim.colour, {"R", "G", "B", "A"});
            }
            animations_[id->text] = std::move(anim);
        }
    }
}

const tr::XmlNode* UiCanvas::node(const std::vector<int32_t>& path) const {
    const tr::XmlNode* n = detachedRoot_ ? detachedRoot_ : root();
    if (!n) return nullptr;
    for (int32_t i : path) {
        if (i < 0 || size_t(i) >= n->children.size()) return nullptr;
        n = &n->children[size_t(i)];
    }
    return n;
}

// Animations (tabula_rasa.exe client\tr\ui\anim, decompiled 2026-10-15). Loaders: AnimData FUN_008bf0a0 (id; playbackType pingpong
// 2, repeat 1, else - "stall" - 0), WidgetAnimData FUN_008ba880 (scaleTrack / transTrack: (time, X, Y); colorTrack: (time, R, G, B, A)),
// BitmapAnimData FUN_008c0250 (uvTrack (time, X, Y, Z, W), rotTrack (time, rot) with anchorX / anchorY, default 0.5; textureTrack -
// empty in the export). Widgets name theirs with widgetAnimName (Widget loader FUN_0089c630 -> slot 88), bitmaps with bitmapAnimName
// (FUN_008f4070 -> slot 135); setting one starts its player at once (slot 91: AnimManager FUN_0088d100 makes a WidgetAnimPlayer /
// BitmapAnimPlayer, FUN_0088d3a0 starts it at time 0). Per frame (FUN_008bf780) the player's time t (ms, x speed 1.0) is wrapped
// (FUN_008bf440): length = the longest track's last key (FUN_008bba90); length 0 = nothing applied; repeat t mod length; pingpong t mod
// 2 length, mirrored past length; stall unwrapped. Each track is sampled (FUN_008bc1c0 / FUN_008bbdc0 / FUN_008c2190; keys sorted by
// time on first use): no keys = not applied; one key, or t at or before the first: the first; at or after the last: the last; else
// between the keys around t with f = (t - t0) / (t1 - t0): linear (vectors FUN_008bd310 / FUN_008c3fd0; colours per channel, rounded
// and clamped, FUN_004445c0 / FUN_00444390), or for a discrete track the earlier key while f < 1. UNVERIFIED: no in-game animation was
// captured; the clock here is one shared time (the game starts each widget's at load, and autoRestartAnims - Widget + 0x98 - restarts
// it, presumably when shown; that call was not traced).
bool UiCanvas::sampleAnimation(const std::string& name, bool bitmap, AnimFrame& frame, const tr::XmlNode* widget) const {
    float time = options_.animTime;
    if (const auto* times = bitmap ? options_.bitmapAnimTimes : options_.widgetAnimTimes; times && widget)
        if (const auto it = times->find(widget); it != times->end()) time = it->second;
    if (time < 0 || name.empty()) return false;
    const auto it = animations_.find(name);
    if (it == animations_.end() || it->second.bitmap != bitmap || it->second.length == 0) return false;
    const Animation& a = it->second;
    uint32_t t = uint32_t(time);
    if (a.playback != 0 || t < a.length) animating_ = true;
    if (a.playback == 1) t %= a.length;
    else if (a.playback == 2) {
        t %= a.length * 2;
        if (t > a.length) t = a.length * 2 - t;
    }
    auto sampleTrack = [&](const AnimTrack& track, int components, bool colour, float out[4]) {
        if (track.keys.empty()) return false;
        const std::vector<AnimKey>& k = track.keys;
        if (k.size() == 1 || t <= k.front().time) {
            for (int c = 0; c < components; ++c) out[c] = k.front().v[c];
            return true;
        }
        if (t >= k.back().time) {
            for (int c = 0; c < components; ++c) out[c] = k.back().v[c];
            return true;
        }
        size_t i = 1;
        while (i + 1 < k.size() && k[i].time < t) ++i;      // k[i - 1].time < t <= k[i].time
        const AnimKey& p = k[i - 1];
        const AnimKey& n = k[i];
        const float f = n.time != p.time ? float(t - p.time) / float(n.time - p.time) : 0.0f;
        for (int c = 0; c < components; ++c) {
            if (colour) {
                const float v = f <= 0 ? p.v[c] : f >= 1 ? n.v[c] : float(std::lround(p.v[c] + f * (n.v[c] - p.v[c])));
                out[c] = std::clamp(v, 0.0f, 255.0f);
            } else if (track.discrete) {
                out[c] = f < 1 ? p.v[c] : n.v[c];
            } else {
                out[c] = f <= 0 ? p.v[c] : f >= 1 ? n.v[c] : p.v[c] + f * (n.v[c] - p.v[c]);
            }
        }
        return true;
    };
    float v[4];
    if (bitmap) {
        if ((frame.uv = sampleTrack(a.uv, 4, false, v))) std::copy(v, v + 4, frame.uvRect);
        if ((frame.rot = sampleTrack(a.rot, 1, false, v))) {
            frame.rotation = v[0];
            frame.anchorX = a.anchorX;
            frame.anchorY = a.anchorY;
        }
        return frame.uv || frame.rot;
    }
    if ((frame.scale = sampleTrack(a.scale, 2, false, v))) {
        frame.scaleX = v[0];
        frame.scaleY = v[1];
    }
    if ((frame.trans = sampleTrack(a.trans, 2, false, v))) {
        frame.transX = v[0];
        frame.transY = v[1];
    }
    if ((frame.colour = sampleTrack(a.colour, 4, true, v)))
        for (int c = 0; c < 4; ++c) frame.rgba[c] = uint8_t(v[c]);
    return frame.scale || frame.trans || frame.colour;
}

// Layout (Widget slot 105, FUN_0089d740, read in the assembly 2026-10-15): size w x h (sizeW / sizeH, or the parent's size with
// resizeToParent); a widget animation's scale makes it trunc(w x sx) x trunc(h x sy), centred: offset ((w - w') / 2, (h - h') / 2)
// (integer halves towards 0); its translation adds (trunc(tx), trunc(ty)). With a parent: left = parent left + trunc((anchor position +
// offset) x S), width trunc(w' x S), S = the parent's scale (slot 107: its own animation scale x its parent's, so children shrink with
// it); the anchor position (FUN_008b6fb0) uses the unscaled sizes. Without animations this is the plain anchor layout.
// A Frame's pieces (tabula_rasa.exe Frame::vftable slot 74 SetSize FUN_008d61c0 -> FUN_008d62c0, run on every SetSize; the
// pieces' fields from the loader FUN_008d59a0: + 0xc0 Center, + 0xc4 TopLeftHoriz ... + 0x100 Bottom): each is placed (SetAnchorOffset,
// slot 82) and the edges / centre sized (SetSize, slot 74) from the frame's size W x H and the pieces' own sizes - corners at the
// frame's corners, the Horiz / Vert pieces beside them, Top / Bottom between the corners (and their Horiz pieces when those are
// visible), Left / Right between the corners (and their Vert pieces when visible), Center (only while visible) inside Left / Right /
// Top / Bottom; stretched sizes clamped at 0. So a frame widened by resizeToParent keeps its border at its edges. Checked: the rule
// gives the stored offsets and sizes of every piece of 292 of the export's 294 frames; the other two differ only in a hidden Center.
// False when the widget is not a laid-out piece (then its own attributes stand).
static bool framePiece(const tr::XmlNode& frame, int32_t W, int32_t H, const tr::XmlNode& piece, int32_t& x, int32_t& y, int32_t& w,
                       int32_t& h) {
    auto find = [&](const char* tag) -> const tr::XmlNode* {
        for (const tr::XmlNode& c : frame.children)
            if (c.tag == tag) return &c;
        return nullptr;
    };
    auto pw = [&](const char* tag) { const tr::XmlNode* n = find(tag); return n ? intAttribute(*n, "sizeW") : 0; };
    auto ph = [&](const char* tag) { const tr::XmlNode* n = find(tag); return n ? intAttribute(*n, "sizeH") : 0; };
    auto shown = [&](const char* tag) { const tr::XmlNode* n = find(tag); return !n || n->attribute("visible") != "false"; };
    const std::string& t = piece.tag;
    w = intAttribute(piece, "sizeW");
    h = intAttribute(piece, "sizeH");
    if (t == "TopLeft") { x = 0; y = 0; }
    else if (t == "TopLeftHoriz") { x = pw("TopLeft"); y = 0; }
    else if (t == "TopLeftVert") { x = 0; y = ph("TopLeft"); }
    else if (t == "TopRight") { x = W - w; y = 0; }
    else if (t == "TopRightHoriz") { x = W - pw("TopRight") - w; y = 0; }
    else if (t == "TopRightVert") { x = W - w; y = ph("TopRight"); }
    else if (t == "BottomLeft") { x = 0; y = H - h; }
    else if (t == "BottomLeftHoriz") { x = pw("BottomLeft"); y = H - h; }
    else if (t == "BottomLeftVert") { x = 0; y = H - ph("BottomLeft") - h; }
    else if (t == "BottomRight") { x = W - w; y = H - h; }
    else if (t == "BottomRightHoriz") { x = W - pw("BottomRight") - w; y = H - h; }
    else if (t == "BottomRightVert") { x = W - w; y = H - ph("BottomRight") - h; }
    else if (t == "Top" || t == "Bottom") {
        const bool top = t == "Top";
        const int32_t left = pw(top ? "TopLeft" : "BottomLeft") + (shown(top ? "TopLeftHoriz" : "BottomLeftHoriz") ? pw(top ? "TopLeftHoriz" : "BottomLeftHoriz") : 0);
        const int32_t right = pw(top ? "TopRight" : "BottomRight") + (shown(top ? "TopRightHoriz" : "BottomRightHoriz") ? pw(top ? "TopRightHoriz" : "BottomRightHoriz") : 0);
        x = left;
        y = top ? 0 : H - h;
        w = std::max(0, W - right - left);
    } else if (t == "Left" || t == "Right") {
        const bool left = t == "Left";
        const int32_t top = ph(left ? "TopLeft" : "TopRight") + (shown(left ? "TopLeftVert" : "TopRightVert") ? ph(left ? "TopLeftVert" : "TopRightVert") : 0);
        const int32_t bottom = ph(left ? "BottomLeft" : "BottomRight") + (shown(left ? "BottomLeftVert" : "BottomRightVert") ? ph(left ? "BottomLeftVert" : "BottomRightVert") : 0);
        x = left ? 0 : W - w;
        y = top;
        h = std::max(0, H - bottom - top);
    } else if (t == "Center" && piece.attribute("visible") != "false") {
        x = pw("Left");
        y = ph("Top");
        w = std::max(0, W - pw("Right") - pw("Left"));
        h = std::max(0, H - ph("Top") - ph("Bottom"));
    } else
        return false;
    return true;
}

UiCanvas::Placed UiCanvas::place(const tr::XmlNode& widget, const Placed& parent) const {
    const NodeInfo& in = info(widget);
    int32_t w = in.sizeW, h = in.sizeH;
    int32_t pieceX = 0, pieceY = 0;
    const bool piece = parent.frame && framePiece(*parent.frame, parent.w, parent.h, widget, pieceX, pieceY, w, h);
    if (in.resizeToParent) {
        w = parent.w;
        h = parent.h;
    }
    if (const auto so = buttonOverrides_.find(&widget); so != buttonOverrides_.end() && so->second.size) {
        w = so->second.width;
        h = so->second.height;
    }
    if (&widget == options_.dragNode) {
        w += options_.dragW;
        h += options_.dragH;
    }
    // Full-screen windows (declared at the export's logical size) take a wider screen's width (see fitToScreen).
    // OpenRasa runs the scripts, which do this themselves (frontend.py, legalwindow.py, hud.py, ingame.py: ResizeToParent(True)) -
    // and not for every such window (LoginWindow keeps 1280 and hangs from the top right): Options::scriptedWidening turns it off.
    if (!options_.scriptedWidening && width_ != 1280 && w == 1280 && h == 960 && parent.w == width_) w = width_;
    int32_t px = 0, py = 0, cx = 0, cy = 0;
    anchorPoint(in.parentAnchor, parent.w, parent.h, px, py);
    anchorPoint(in.childAnchor, w, h, cx, cy);
    int32_t offsetX = piece ? pieceX : in.offsetX, offsetY = piece ? pieceY : in.offsetY;
    if (&widget == options_.dragNode) {
        offsetX += options_.dragX;
        offsetY += options_.dragY;
    }
    const auto o = buttonOverrides_.find(&widget);
    if (o != buttonOverrides_.end() && o->second.offset) {
        offsetX = o->second.offsetX;
        offsetY = o->second.offsetY;
    }
    int32_t dx = 0, dy = 0, drawnW = w, drawnH = h;
    Placed out;
    out.w = w;
    out.h = h;
    if (widget.tag == "Frame") out.frame = &widget;
    out.sx = parent.sx;
    out.sy = parent.sy;
    AnimFrame frame;
    if (sampleAnimation(in.widgetAnimName, false, frame, &widget)) {
        if (frame.scale) {
            drawnW = int32_t(float(w) * frame.scaleX);
            drawnH = int32_t(float(h) * frame.scaleY);
            dx = (w - drawnW) / 2;
            dy = (h - drawnH) / 2;
            out.sx *= frame.scaleX;
            out.sy *= frame.scaleY;
        }
        if (frame.trans) {
            dx += int32_t(frame.transX);
            dy += int32_t(frame.transY);
        }
    }
    const int32_t x = parent.rect.x0 + int32_t(float(px + offsetX - cx + dx) * parent.sx);
    const int32_t y = parent.rect.y0 + int32_t(float(py + offsetY - cy + dy) * parent.sy);
    out.rect = Rect{x, y, x + int32_t(float(drawnW) * parent.sx), y + int32_t(float(drawnH) * parent.sy)};
    // The render context colour the widget's children draw with: its colour x its parent's context colour (FUN_008aee70).
    uint8_t own[4];
    ownColour(widget, own);
    for (int c = 0; c < 4; ++c) out.colour[c] = multiplyColour(own[c], parent.colour[c]);
    return out;
}

UiCanvas::Placed UiCanvas::placedAt(const std::vector<int32_t>& path) const {
    Placed placed;
    placed.rect = Rect{0, 0, width_, height_};
    placed.w = width_;
    placed.h = height_;
    if (!ui_) return placed;
    const tr::XmlNode* n = root();
    for (int32_t i : path) {
        if (i < 0 || size_t(i) >= n->children.size()) break;
        n = &n->children[size_t(i)];
        placed = place(*n, placed);
    }
    return placed;
}

UiCanvas::Rect UiCanvas::layout(const std::vector<int32_t>& path) const { return placedAt(path).rect; }

void UiCanvas::offsetsFor(const std::vector<int32_t>& path, const Rect& rect, int32_t& offsetX, int32_t& offsetY) const {
    offsetX = offsetY = 0;
    const tr::XmlNode* n = node(path);
    if (!n || path.empty()) return;
    const Placed parent = placedAt(std::vector<int32_t>(path.begin(), path.end() - 1));
    int32_t px = 0, py = 0, cx = 0, cy = 0;
    anchorPoint(intAttribute(*n, "parentAnchor"), parent.w, parent.h, px, py);
    anchorPoint(intAttribute(*n, "childAnchor"), rect.width(), rect.height(), cx, cy);
    offsetX = rect.x0 - parent.rect.x0 - px + cx;
    offsetY = rect.y0 - parent.rect.y0 - py + cy;
}

// ------------------------------------------------------------------------------------------
// Drawing
// ------------------------------------------------------------------------------------------
struct UiCanvas::Target {
    int32_t width = 0, height = 0;             // pixels
    float sx = 1, sy = 1;                      // pixels per logical unit
    std::vector<float> rgb;                    // 0..1 as the 8-bit frame buffer holds it
    enum class Blend : uint8_t { Alpha, Additive, One };
    // A logical rectangle in pixels (edges rounded).
    Rect pixels(const Rect& r) const {
        return Rect{int32_t(std::lround(float(r.x0) * sx)), int32_t(std::lround(float(r.y0) * sy)), int32_t(std::lround(float(r.x1) * sx)),
                    int32_t(std::lround(float(r.y1) * sy))};
    }
    void blend(int32_t x, int32_t y, float r, float g, float b, float a, Blend mode) {
        float* p = &rgb[(size_t(y) * size_t(width) + size_t(x)) * 3];
        switch (mode) {
            case Blend::Alpha:                 // SRCALPHA / INVSRCALPHA
                p[0] = r * a + p[0] * (1 - a); p[1] = g * a + p[1] * (1 - a); p[2] = b * a + p[2] * (1 - a);
                break;
            case Blend::Additive:              // SRCALPHA / ONE
                p[0] = std::min(1.0f, p[0] + r * a); p[1] = std::min(1.0f, p[1] + g * a); p[2] = std::min(1.0f, p[2] + b * a);
                break;
            case Blend::One:                   // ONE / ONE
                p[0] = std::min(1.0f, p[0] + r); p[1] = std::min(1.0f, p[1] + g); p[2] = std::min(1.0f, p[2] + b);
                break;
        }
    }
};

const UiCanvas::Image* UiCanvas::image(const std::string& name) {
    const std::string key = lower(name);
    auto it = images_.find(key);
    if (it != images_.end()) return it->second.get();
    std::unique_ptr<Image> img;
    tr::Bytes bytes;
    if (assets_ && assets_->tryRead(name, bytes)) {
        try {
            tr::DdsHeader header;
            const std::vector<tr::Rgba8> pixels = tr::decodeDds(bytes, header);
            img = std::make_unique<Image>();
            img->serial = nextSerial_++;
            img->width = int32_t(header.width);
            img->height = int32_t(header.height);
            img->rgba.resize(pixels.size() * 4);
            for (size_t k = 0; k < pixels.size(); ++k) {
                img->rgba[k * 4 + 0] = pixels[k].red;
                img->rgba[k * 4 + 1] = pixels[k].green;
                img->rgba[k * 4 + 2] = pixels[k].blue;
                img->rgba[k * 4 + 3] = pixels[k].alpha;
            }
        } catch (const std::exception&) {
            img.reset();
        }
    }
    if (!img) missingTextures_.insert(name);
    return (images_[key] = std::move(img)).get();
}

namespace {

// Bilinear sample at texel coordinates (u, v) (texel centres at .5), clamped to the image; out = rgba 0..1.
void sample(const std::vector<uint8_t>& rgba, int32_t w, int32_t h, float u, float v, float out[4]) {
    const float fx = u - 0.5f, fy = v - 0.5f;
    const int32_t x0 = int32_t(std::floor(fx)), y0 = int32_t(std::floor(fy));
    const float tx = fx - float(x0), ty = fy - float(y0);
    auto texel = [&](int32_t x, int32_t y, int c) {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        return float(rgba[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(c)]) / 255.0f;
    };
    for (int c = 0; c < 4; ++c) {
        const float a = texel(x0, y0, c) * (1 - tx) + texel(x0 + 1, y0, c) * tx;
        const float b = texel(x0, y0 + 1, c) * (1 - tx) + texel(x0 + 1, y0 + 1, c) * tx;
        out[c] = a * (1 - ty) + b * ty;
    }
}

void widgetColour(const tr::XmlNode& w, float c[4]) {
    c[0] = float(intAttribute(w, "colorR", 255)) / 255.0f;
    c[1] = float(intAttribute(w, "colorG", 255)) / 255.0f;
    c[2] = float(intAttribute(w, "colorB", 255)) / 255.0f;
    c[3] = float(intAttribute(w, "colorA", 255)) / 255.0f;
}

}  // namespace

const UiCanvas::NodeInfo& UiCanvas::info(const tr::XmlNode& n) const {
    NodeInfo* out = &temporaryInfo_;
    if (noCacheDepth_ == 0) {
        auto [it, made] = nodeCache_.try_emplace(&n);
        if (!made) return it->second;
        out = &it->second;
    }
    NodeInfo& i = *out;
    i = NodeInfo();
    i.sizeW = intAttribute(n, "sizeW");
    i.sizeH = intAttribute(n, "sizeH");
    i.parentAnchor = intAttribute(n, "parentAnchor");
    i.childAnchor = intAttribute(n, "childAnchor");
    i.offsetX = intAttribute(n, "anchorOffsetX");
    i.offsetY = intAttribute(n, "anchorOffsetY");
    i.resizeToParent = attributeIsTrue(n, "resizeToParent");
    i.hidden = n.attribute("visible") == "false";
    i.clipChildren = n.attribute("clipChildren") != "false";
    i.hasTextureDef = n.hasAttribute("textureDef");
    i.hasTextureFile = !n.attribute("textureFile").empty();
    i.wrapText = attributeIsTrue(n, "wrapText");
    i.scaleToFit = attributeIsTrue(n, "scaleToFit");
    widgetColour(n, i.colour);
    i.widgetAnimName = n.attribute("widgetAnimName");
    i.bitmapAnimName = n.attribute("bitmapAnimName");
    i.effect = lower(n.attribute("effect"));
    i.textureDef = n.attribute("textureDef");
    i.fontId = n.attribute("fontId");
    i.horizAlign = n.attribute("horizAlign");
    i.vertAlign = n.attribute("vertAlign");
    return i;
}

void UiCanvas::drawTexture(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target) {
    const NodeInfo& in = info(widget);
    const auto o = buttonOverrides_.find(&widget);
    const tr::UiTextureDef* def = ui_->texture(o != buttonOverrides_.end() && o->second.texture ? o->second.textureDef : in.textureDef);
    // A bitmap whose texture or region a script set (Bitmap.SetTexture / SetTextureRegion; OpenRasa's "textureFile"): that texture,
    // region and mode bits (1 tile U, 4 tile V).
    tr::UiTextureDef fileDef;
    if (const std::string file = widget.attribute("textureFile"); !file.empty()) {
        fileDef.name = file;
        auto number = [&](const char* name, int fallback) {
            return widget.hasAttribute(name) ? std::atoi(widget.attribute(name).c_str()) : fallback;
        };
        fileDef.left = number("textureRegionLeft", -1);
        fileDef.top = number("textureRegionTop", -1);
        fileDef.right = number("textureRegionRight", -1);
        fileDef.bottom = number("textureRegionBottom", -1);
        const int tile = number("textureTile", 0);
        fileDef.tileU = (tile & 0x1) != 0;
        fileDef.tileV = (tile & 0x4) != 0;
        def = &fileDef;
    }
    if (!def) return;
    const std::string& effect = in.effect;
    Target::Blend blend = Target::Blend::Alpha;
    bool useColour = true;
    if (effect == "truiadditivealpha.fx" || effect == "paluimultitexadd.fx") blend = Target::Blend::Additive;
    // TRUIBackgroundMask (traced 2026-10-15 in the fountain trace, call 37335934): pixel shader rgb = texture x UIMouseMaskSampler, alpha
    // 1, blended ONE / ONE, no vertex colour; the vertex shader's mask coordinate = MouseUVXForm x the vertex's screen position (NDC) -
    // (3.332, -1.875) at 1920 x 1080 = 1/288 per pixel on both axes, i.e. the 256 x 256 ui_mouse_mask.dds (the UI manager loads it,
    // FUN_00895430; a radial white spot, 1/255 at the edges) spread over 256 logical units centred on the mouse, address CLAMP.
    const Image* mouseMask = nullptr;
    if (effect == "truibackgroundmask.fx") {
        if (!options_.mouseHighlight) return;  // the clamped edge: 1/255, nothing visible
        mouseMask = image("ui_mouse_mask.dds");
        if (!mouseMask) return;
        blend = Target::Blend::One;
        useColour = false;
    }
    const Image* img = image(def->name);
    if (!img || img->width <= 0 || img->height <= 0) return;
    const Image* mask = nullptr;
    if (effect == "paluimultitex.fx" || effect == "paluimultitexadd.fx") {
        const std::string maskName = widget.attribute("maskTextureName");
        if (!maskName.empty()) mask = image(maskName);
    }
    // TRUIRadarMask (the radar's map tiles; TRUIRadarMaskSP_2.fxc UIRadarMaskDefault, disassembled 2026-10-25): rgba = texture x
    // UIRadarMaskTexture x colour, SRCALPHA / INVSRCALPHA, alpha test > 0. The mask is the UI manager's (InitRadarMask, 0x00520740 ->
    // FUN_0051fb30: the radar window loads ui_hud_radar_mask_01.dds, 256 x 256, white with a round alpha), set on each widget by
    // SetupRadarMaskWidget(widget, (x, y)) (0x00520880 -> FUN_0051fcc0) with the transform Scaling(0.5, -0.5) Translation(0.5, 0.5)
    // Translation(-x / root width, -y / root height) Scaling(root width / mask width, root height / mask height) Translation(0.5, 0.5):
    // the mask spans its own size in UI logical units, centred on (x, y) - the map group's centre (radarwindow.py _SetupRadarMaskImages),
    // so all nine tiles share one round window. Centre: OpenRasa's "radarMaskX" / "radarMaskY" (what the script passed), else the
    // parent's centre (the map group, as the script computes it). Out of the mask: clamped (its border is alpha 0).
    const Image* radarMask = nullptr;
    float radarX = 0, radarY = 0;
    if (effect == "truiradarmask.fx") {
        const std::string maskName = widget.attribute("radarMask").empty() ? widget.attribute("maskTextureName") : widget.attribute("radarMask");
        if (!maskName.empty()) radarMask = image(maskName);
        radarX = widget.hasAttribute("radarMaskX") ? float(intAttribute(widget, "radarMaskX")) : float(parentRect_.x0 + parentRect_.x1) / 2.0f;
        radarY = widget.hasAttribute("radarMaskY") ? float(intAttribute(widget, "radarMaskY")) : float(parentRect_.y0 + parentRect_.y1) / 2.0f;
    }
    float colour[4];
    colourOf(widget, colour);
    if (!useColour) colour[0] = colour[1] = colour[2] = colour[3] = 1.0f;
    const Rect px = target.pixels(rect);
    const Rect area = intersect(px, target.pixels(clip));
    // A bitmap animation (Bitmap render FUN_008f5300 -> FUN_008f5550, read in the assembly 2026-10-15): a uv key replaces the texture region
    // by (trunc(width x X), trunc(height x Y), trunc(width x Z), trunc(height x W)) of the whole texture (an empty one widened by a pixel,
    // both axes against the width - as the code does); a rotation key gives the quad's texture coordinates the matrix translate(-anchor)
    // x RotationZ(rot) x translate(anchor) (D3DX, row vectors; rot in radians). The draw (FUN_005adc60) takes a region with right <= left
    // or bottom <= top as the whole texture. UNVERIFIED: that the rotation applies to the quad's 0..1 coordinates before they are mapped
    // into the region (the draw's matrix chain decompiles poorly), and the sampler's wrap addressing for animated regions outside the
    // texture (16Frame_512x256's first row is at Y 1..1.25).
    int32_t left = def->left, top = def->top, right = def->right, bottom = def->bottom;
    AnimFrame anim;
    const bool animated = sampleAnimation(in.bitmapAnimName, true, anim, &widget);
    if (animated && anim.uv) {
        left = int32_t(float(img->width) * anim.uvRect[0]);
        top = int32_t(float(img->height) * anim.uvRect[1]);
        right = int32_t(float(img->width) * anim.uvRect[2]);
        bottom = int32_t(float(img->height) * anim.uvRect[3]);
        if (left == right) {
            if (right < img->width) ++right;
            else --left;
        }
        if (top == bottom) {
            if (bottom < img->width) ++bottom;
            else --top;
        }
    }
    if (right <= left || bottom <= top) {
        left = 0;
        top = 0;
        right = img->width;
        bottom = img->height;
    }
    const bool rotate = animated && anim.rot;
    const float rc = std::cos(anim.rotation), rs = std::sin(anim.rotation);
    const float regionW = float(right - left), regionH = float(bottom - top);
    const float w = float(std::max(1, px.width())), h = float(std::max(1, px.height()));
    if (options_.quads) {
        if (area.width() <= 0 || area.height() <= 0) return;
        Quad q;
        q.image = img;
        q.mask = mask;
        q.mouseMask = mouseMask;
        q.blend = blend == Target::Blend::Alpha ? 0 : blend == Target::Blend::Additive ? 1 : 2;
        std::copy(colour, colour + 4, q.colour);
        q.rect = px;
        q.clip = area;
        q.region[0] = float(left) / float(img->width);
        q.region[1] = float(top) / float(img->height);
        q.region[2] = float(right) / float(img->width);
        q.region[3] = float(bottom) / float(img->height);
        if (def->tileU && regionW > 0) q.tileU = w / target.sx / regionW;
        if (def->tileV && regionH > 0) q.tileV = h / target.sy / regionH;
        q.wrap = animated;
        q.rotate = rotate;
        if (rotate) {
            q.rotation[0] = rc;
            q.rotation[1] = rs;
            q.rotation[2] = anim.anchorX;
            q.rotation[3] = anim.anchorY;
        }
        q.mouseX = options_.mouseX * target.sx;
        q.mouseY = options_.mouseY * target.sy;
        q.mouseSize = 256.0f * target.sy;
        if (radarMask) {
            q.mouseMask = radarMask;
            q.radarMask = true;
            q.mouseX = radarX * target.sx;
            q.mouseY = radarY * target.sy;
            q.mouseSize = float(radarMask->width) * target.sy;
        }
        options_.quads->push_back(q);
        return;
    }
    for (int32_t y = std::max(area.y0, 0); y < std::min(area.y1, target.height); ++y) {
        const float dy = float(y - px.y0) + 0.5f;
        // Tiled: the region repeats at its own size in logical units.
        float v = def->tileV && regionH > 0 ? float(top) + std::fmod(dy / target.sy, regionH) : float(top) + dy / h * regionH;
        for (int32_t x = std::max(area.x0, 0); x < std::min(area.x1, target.width); ++x) {
            const float dx = float(x - px.x0) + 0.5f;
            float u = def->tileU && regionW > 0 ? float(left) + std::fmod(dx / target.sx, regionW) : float(left) + dx / w * regionW;
            if (rotate) {
                const float qx = dx / w - anim.anchorX, qy = dy / h - anim.anchorY;
                u = float(left) + (qx * rc - qy * rs + anim.anchorX) * regionW;
                v = float(top) + (qx * rs + qy * rc + anim.anchorY) * regionH;
            }
            float t[4];
            if (animated) {
                const float fw = float(img->width), fh = float(img->height);
                sample(img->rgba, img->width, img->height, u - std::floor(u / fw) * fw, v - std::floor(v / fh) * fh, t);
            } else {
                sample(img->rgba, img->width, img->height, u, v, t);
            }
            if (mask) {
                float m[4];
                sample(mask->rgba, mask->width, mask->height, dx / w * float(mask->width), dy / h * float(mask->height), m);
                for (int c = 0; c < 4; ++c) t[c] *= m[c];
            }
            if (radarMask) {
                const float mu = ((float(x) + 0.5f) / target.sx - radarX) / float(radarMask->width) + 0.5f;
                const float mv = ((float(y) + 0.5f) / target.sy - radarY) / float(radarMask->height) + 0.5f;
                float m[4];
                sample(radarMask->rgba, radarMask->width, radarMask->height, std::clamp(mu, 0.0f, 1.0f) * float(radarMask->width),
                       std::clamp(mv, 0.0f, 1.0f) * float(radarMask->height), m);
                for (int c = 0; c < 4; ++c) t[c] *= m[c];
            }
            if (mouseMask) {
                const float size = 256.0f * target.sy;     // pixels
                const float mu = (float(x) + 0.5f - options_.mouseX * target.sx) / size + 0.5f;
                const float mv = (float(y) + 0.5f - options_.mouseY * target.sy) / size + 0.5f;
                float m[4];
                sample(mouseMask->rgba, mouseMask->width, mouseMask->height, std::clamp(mu, 0.0f, 1.0f) * float(mouseMask->width),
                       std::clamp(mv, 0.0f, 1.0f) * float(mouseMask->height), m);
                for (int c = 0; c < 3; ++c) t[c] *= m[c];
            }
            target.blend(x, y, t[0] * colour[0], t[1] * colour[1], t[2] * colour[2], blend == Target::Blend::One ? 1.0f : t[3] * colour[3], blend);
        }
    }
}

void UiCanvas::drawColour(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target) {
    float colour[4];
    colourOf(widget, colour);
    const Target::Blend blend = lower(widget.attribute("effect")) == "truiadditivealpha.fx" ? Target::Blend::Additive : Target::Blend::Alpha;
    const Rect area = intersect(target.pixels(rect), target.pixels(clip));
    if (options_.quads) {
        if (area.width() <= 0 || area.height() <= 0) return;
        Quad q;
        q.blend = blend == Target::Blend::Alpha ? 0 : 1;
        std::copy(colour, colour + 4, q.colour);
        q.rect = target.pixels(rect);
        q.clip = area;
        options_.quads->push_back(q);
        return;
    }
    for (int32_t y = std::max(area.y0, 0); y < std::min(area.y1, target.height); ++y)
        for (int32_t x = std::max(area.x0, 0); x < std::min(area.x1, target.width); ++x) target.blend(x, y, colour[0], colour[1], colour[2], colour[3], blend);
}

std::string UiCanvas::text(const tr::XmlNode& widget) {
    if (&widget == sampleNode_) return sampleText_;          // a Definitions preview's sample
    if (const auto o = textOverrides_.find(&widget); o != textOverrides_.end()) return o->second;   // an edit box's typed text
    if (options_.texts)
        if (const auto t = options_.texts->find(&widget); t != options_.texts->end()) return t->second;
    const std::string id = widget.attribute("textId");
    if (id.empty() || !game_) return "";
    std::string s;
    if (!game_->uiTextExists(id, 1, &s)) missingTextIds_.insert(id);   // an id whose text is "" (ID_DEFAULT_TEXT) is not missing
    return s;
}

// Text, as the game draws it (see "Fonts" above): glyph em = UiCanvas::fontEm (trunc(pointSize x scale), MulDiv x 96 / 72) (the integer point step:
// Euro_9 14, A_Conduit_14 20, A_Conduit_16 24 pixels at 1920 x 1080 - measured on session image 270 and the fountain trace's final
// frame); lines GDI tmHeight apart, the first baseline tmAscent - tmInternalLeading below the rect's top (the em box at the top),
// vertAlign centre on the block's em height (fitted to the fountain trace's Quit dialog, FrontEnd/LogOutWindow). Alignment is in pixels
// (within 1 - 2 pixels of the game there; its exact rule is not traced).
void UiCanvas::drawText(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target) {
    const std::string s = text(widget);
    if (s.empty()) return;
    const NodeInfo& in = info(widget);
    const tr::UiFont* font = ui_->font(in.fontId);
    if (!font) font = ui_->font("Default");
    if (!font && !ui_->fonts.empty()) font = &ui_->fonts.begin()->second;    // UNVERIFIED stand-in for unknown font ids
    if (!font || font->ranges.empty()) return;
    auto emOf = [&](const tr::UiFontRange& r) { return UiCanvas::fontEm(r.pointSize, target.sy); };
    struct Item {
        uint32_t cp;
        Face* face;
        int32_t em;
        bool bold, shadow;
    };
    std::vector<Item> items;
    const tr::UiFontRange* basePtr = nullptr;
    Face* baseFace = nullptr;
    int32_t baseEm = 0;
    const std::vector<uint32_t> cps = codepoints(s);
    auto setFont = [&](const tr::UiFont& f) {
        basePtr = &f.ranges.front();
        baseFace = face(basePtr->face);
        baseEm = emOf(*basePtr);
        items.clear();
        if (!baseFace) return;
        for (uint32_t cp : cps) {
            const tr::UiFontRange* range = f.rangeFor(cp);
            Face* rf = range ? face(range->face) : nullptr;
            if (!rf) { rf = baseFace; range = basePtr; }
            items.push_back(Item{cp, rf, emOf(*range), range->bold, range->shadow});
        }
    };
    setFont(*font);
    if (!baseFace) return;
    Rect px = target.pixels(rect);
    px.x0 += textShiftX_;
    px.x1 += textShiftX_;
    auto advance = [&](size_t i) {
        int32_t a = items[i].face->glyph(items[i].em, items[i].cp, items[i].bold, items[i].shadow).advance;
        if (i + 1 < items.size() && items[i + 1].face == items[i].face) a += items[i].face->kern(items[i].em, items[i].cp, items[i + 1].cp);
        return a;
    };
    const bool wrap = in.wrapText;
    // Text that does not fit an unwrapped widget (uitext.cpp FUN_008b1c00, decompiled 2026-10-15):
    // - scaleToFit: another of the UI's fonts - FUN_00896a20 -> FUN_008ea910 -> FUN_006ab090 takes the width ratio (widget / text),
    //   s = trunc(-em x ratio) (GDI lfHeight = -em) and walks s towards 0 until a created font of the same face has that size (first
    //   with the same style flags, then plain); the defined font owning it is used, else "DefaultSmallest". Fonts are created for
    //   every defined range, so: the largest defined font of the same face no bigger than trunc(em x ratio).
    // - otherwise FUN_008fa210 cuts it to fit and appends "..." (three periods).
    if (!wrap && !items.empty()) {
        int32_t total = 0;
        for (size_t i = 0; i < items.size(); ++i) total += advance(i);
        if (total > px.width() && px.width() > 0) {
            if (in.scaleToFit) {
                const int32_t targetEm = int32_t(float(baseEm) * float(px.width()) / float(total));
                const tr::UiFont* best = nullptr;
                int32_t bestEm = 0;
                for (int pass = 0; pass < 2 && !best; ++pass)
                    for (const auto& entry : ui_->fonts) {
                        const tr::UiFontRange& r = entry.second.ranges.empty() ? *basePtr : entry.second.ranges.front();
                        if (entry.second.ranges.empty() || r.face != basePtr->face) continue;
                        if (pass == 0 && (r.bold != basePtr->bold || r.italic != basePtr->italic || r.shadow != basePtr->shadow)) continue;
                        if (pass == 1 && (r.bold || r.italic || r.shadow)) continue;
                        const int32_t em = emOf(r);
                        if (em <= targetEm && em > bestEm) {
                            best = &entry.second;
                            bestEm = em;
                        }
                    }
                if (!best) best = ui_->font("DefaultSmallest");
                if (best && !best->ranges.empty()) {
                    setFont(*best);
                    if (!baseFace) return;
                }
            } else if (scrollTail_) {
                // An edit box keeps its caret (at the end) in view: the leading characters scroll out (UNVERIFIED rule - the game
                // moves its visible window, + 0x184 / + 0x188, only as far as the caret needs; FUN_008d2710).
                int32_t used = total;
                size_t drop = 0;
                while (drop < items.size() && used > px.width()) used -= advance(drop++);
                items.erase(items.begin(), items.begin() + std::ptrdiff_t(drop));
            } else {
                // Keep the characters that fit, then drop more until "..." fits too.
                int32_t used = 0;
                size_t keep = 0;
                while (keep < items.size() && used + advance(keep) <= px.width()) used += advance(keep++);
                const Face::Glyph& dot = baseFace->glyph(baseEm, '.', basePtr->bold, basePtr->shadow);
                const int32_t dots = dot.advance * 3;
                while (keep > 0 && used + dots > px.width()) used -= advance(--keep);
                items.resize(keep);
                for (int k = 0; k < 3; ++k) items.push_back(Item{'.', baseFace, baseEm, basePtr->bold, basePtr->shadow});
            }
        }
    }
    std::vector<std::pair<size_t, size_t>> lines;
    if (wrap) {
        // The exe's wrap (UIText::SetText -> FUN_008f9860, wrapLines): the same lines UIText::GetTextHeight counts, at the widget's width
        // ROUND(logical width x screen scale). An own simpler rule here (break when the advances pass the width) put "Waypoint: Tampeii
        // Settlement" (315 px of ink in a 315 px title) on two lines while the height was taken for one - the map tooltip's title clipped.
        std::u16string t;
        for (const Item& it : items) t.push_back(char16_t(it.cp <= 0xFFFF ? it.cp : 0xFFFD));
        const std::string fontId = ui_->font(in.fontId) ? in.fontId : std::string("Default");
        const int wrapWidth = int(std::lround(float(rect.x1 - rect.x0) * target.sx));
        for (const auto& [start, length] : wrapLines(fontId, t, wrapWidth, target.sy)) lines.emplace_back(size_t(start), size_t(start + length));
    } else {
        size_t lineStart = 0;
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].cp == 10) {   // LF
                lines.emplace_back(lineStart, i);
                lineStart = i + 1;
            }
        lines.emplace_back(lineStart, items.size());
    }

    float colour[4];
    colourOf(widget, colour);
    const Target::Blend blend = in.effect == "truiadditivealpha.fx" ? Target::Blend::Additive : Target::Blend::Alpha;
    const std::string& horiz = in.horizAlign;
    const std::string& vert = in.vertAlign;
    const int32_t lineHeight = baseFace->cellHeight(baseEm), leading = baseFace->internalLeading(baseEm);
    const int32_t blockHeight = int32_t(lines.size()) * lineHeight - leading;
    int32_t y = px.y0;
    if (vert == "center") y += (px.height() - blockHeight) / 2;
    else if (vert == "bottom") y += px.height() - blockHeight;
    const Rect area = intersect(px, target.pixels(clip));
    measuredEm_ = baseEm;
    measuredWidth_ = 0;
    // GPU: the glyphs go into an image of the widget's visible pixel rectangle (its rectangle clipped: rgb = grey, a = alpha, layered as
    // the CPU blends them); the quad draws it with the colour. The image is kept for the same text, font, size, clip and layout.
    Image* textImage = nullptr;
    bool fillTextImage = false;
    if (options_.quads && !measureOnly_) {
        if (area.width() <= 0 || area.height() <= 0 || px.width() <= 0 || px.height() <= 0) return;
        std::string key = s;
        key += '\x01';
        key += in.fontId + "|" + std::to_string(px.width()) + "x" + std::to_string(px.height()) + "|" +
               std::to_string(area.x0 - px.x0) + "," + std::to_string(area.y0 - px.y0) + "," + std::to_string(area.width()) + "x" +
               std::to_string(area.height()) + "|" + horiz + "|" + vert +
               "|" + std::to_string(wrap) + std::to_string(in.scaleToFit) + std::to_string(scrollTail_) + "|" +
               std::to_string(baseEm);
        TextImage& cached = textImages_[key];
        cached.lastUsed = renderCount_;
        if (!cached.image) {
            cached.image = std::make_unique<Image>();
            cached.image->serial = nextSerial_++;
            cached.image->width = area.width();
            cached.image->height = area.height();
            cached.image->rgba.assign(size_t(area.width()) * size_t(area.height()) * 4, 0);
            fillTextImage = true;
        }
        textImage = cached.image.get();
    }
    for (const auto& line : lines) {
        int32_t lineWidth = 0;
        for (size_t i = line.first; i < line.second; ++i) lineWidth += advance(i);
        measuredWidth_ = std::max(measuredWidth_, lineWidth);
        if (measureOnly_ || (textImage && !fillTextImage)) continue;
        int32_t x = px.x0;
        if (horiz == "center") x += (px.width() - lineWidth) / 2;
        else if (horiz == "right") x += px.width() - lineWidth;
        const int32_t cellTop = y - leading;              // the glyph cell's top: the em box starts at the line's top
        for (size_t i = line.first; i < line.second; ++i) {
            const Face::Glyph& g = items[i].face->glyph(items[i].em, items[i].cp, items[i].bold, items[i].shadow);
            // Another range's face keeps the base line's baseline.
            const int32_t gx = x + g.originX, gy = cellTop + baseFace->ascent(baseEm) - items[i].face->ascent(items[i].em) + g.top;
            if (textImage) {
                for (int32_t yy = 0; yy < g.height; ++yy) {
                    const int32_t iy = gy + yy - area.y0;
                    if (iy < 0 || iy >= textImage->height) continue;
                    for (int32_t xx = 0; xx < g.width; ++xx) {
                        const int32_t ix = gx + xx - area.x0;
                        if (ix < 0 || ix >= textImage->width) continue;
                        const size_t k = size_t(yy) * size_t(g.width) + size_t(xx);
                        if (g.alpha[k] == 0) continue;
                        const float grey = float(g.grey[k]) / 15.0f, a = float(g.alpha[k]) / 15.0f;
                        uint8_t* p = &textImage->rgba[(size_t(iy) * size_t(textImage->width) + size_t(ix)) * 4];
                        const float oldA = float(p[3]) / 255.0f, old = float(p[0]) / 255.0f;
                        const float outA = a + oldA * (1 - a);
                        const float outGrey = outA > 0 ? (grey * a + old * oldA * (1 - a)) / outA : 0;
                        p[0] = p[1] = p[2] = uint8_t(std::lround(std::clamp(outGrey, 0.0f, 1.0f) * 255.0f));
                        p[3] = uint8_t(std::lround(std::clamp(outA, 0.0f, 1.0f) * 255.0f));
                    }
                }
                x += advance(i);
                continue;
            }
            for (int32_t yy = 0; yy < g.height; ++yy) {
                const int32_t py = gy + yy;
                if (py < std::max(area.y0, 0) || py >= std::min(area.y1, target.height)) continue;
                for (int32_t xx = 0; xx < g.width; ++xx) {
                    const int32_t ppx = gx + xx;
                    if (ppx < std::max(area.x0, 0) || ppx >= std::min(area.x1, target.width)) continue;
                    const size_t k = size_t(yy) * size_t(g.width) + size_t(xx);
                    if (g.alpha[k] == 0) continue;
                    const float grey = float(g.grey[k]) / 15.0f, a = float(g.alpha[k]) / 15.0f;
                    target.blend(ppx, py, colour[0] * grey, colour[1] * grey, colour[2] * grey, colour[3] * a, blend);
                }
            }
            x += advance(i);
        }
        y += lineHeight;
    }
    if (textImage) {
        Quad q;
        q.image = textImage;
        q.blend = blend == Target::Blend::Alpha ? 0 : 1;
        std::copy(colour, colour + 4, q.colour);
        q.rect = area;
        q.clip = area;
        options_.quads->push_back(q);
    }
}

// RichText metrics (RichText.h) from the faces drawText uses: em = UiCanvas::fontEm(pointSize, sy) per range.
// FUN_008f9860 (Text.WrapText). Per character: CR ends the line (an LF after it skipped too), LF ends it; a character no range of the
// font holds (FUN_008ce6c0 none) is passed over; else its glyph (FUN_006aef90 -> the data FUN_006af200 builds: + 0x18 the advance
// A + B + C, + 0x20 abcC) and the kerning with the next character (FUN_006af120; 0 for the last).
// It fits when kern - C + line width + advance <= the width: a break is noted after it (lineBreakBetween, not for the first character of
// the text or the last) - the width since the break restarting at 0 -, and kern + advance is added to both widths. It does not fit: on an
// empty line the wrap ends there (no more lines); without a break in the line the line ends before the character; else it ends after
// the break and the next line starts with the width since the break (that includes the break character - as the exe counts it). The
// character is then tried again. A non-empty rest is the last line. Glyph A / B / C come from FreeType here: A = the bitmap's left,
// B = its width, C = advance - A - B (GDI's GetCharABCWidthsW in the exe; APPROXIMATE as the advances are).
bool UiCanvas::editHighlight(const tr::XmlNode* box, int32_t& x, int32_t& y, int32_t& width, int32_t& height) const {
    const auto it = highlightPlaces_.find(box);
    if (it == highlightPlaces_.end()) return false;
    x = it->second.x;
    y = it->second.y;
    width = it->second.width;
    height = it->second.height;
    return true;
}

int32_t UiCanvas::glyphAdvance(const std::string& fontId, char16_t c, float sy) {
    const tr::UiFont* f = ui_ ? ui_->font(fontId) : nullptr;
    if (!f || f->ranges.empty()) return 0;
    const tr::UiFontRange* range = f->rangeFor(c);
    Face* rf = range ? face(range->face) : nullptr;
    if (!rf) return 0;
    const int32_t em = UiCanvas::fontEm(range->pointSize, sy);
    return rf->glyph(em, c, range->bold, range->shadow).advance;
}

std::vector<std::pair<int, int>> UiCanvas::wrapLines(const std::string& fontId, const std::u16string& text, int widthPixels, float sy) {
    std::vector<std::pair<int, int>> lines;
    const tr::UiFont* f = ui_ ? ui_->font(fontId) : nullptr;
    if (!f || f->ranges.empty()) return lines;
    auto emOf = [sy](const tr::UiFontRange& r) { return UiCanvas::fontEm(r.pointSize, sy); };
    const int n = int(text.size());
    int start = 0, i = 0, brk = 0, lineWidth = 0, sinceBreak = 0;
    while (i < n) {
        const char16_t c = text[size_t(i)];
        if (c == u'\r' || c == u'\n') {
            lines.push_back({start, i - start});
            lineWidth = sinceBreak = 0;
            ++i;
            if (c == u'\r' && i < n && text[size_t(i)] == u'\n') ++i;
            start = brk = i;
            continue;
        }
        const tr::UiFontRange* range = nullptr;
        for (const tr::UiFontRange& r : f->ranges)
            if (r.begin <= c && c <= r.end) {
                range = &r;
                break;
            }
        Face* rf = range ? face(range->face) : nullptr;
        if (range && !rf) rf = face(f->ranges.front().face);   // a face without a file here: the first one's (as drawing does)
        if (!rf) {
            ++i;
            continue;
        }
        const int32_t em = emOf(*range);
        const Face::Glyph& g = rf->glyph(em, c, range->bold, range->shadow);
        // The glyph data FUN_008f9860 reads (gfxFontImpl::AddGlyph 0x006af200 builds it: texture, 4 UV floats, + 0x14 cell, + 0x18
        // advance A + B + C, + 0x1c A, + 0x20 C, + 0x24 max(A, 0)): it fits when kern - C + line width + advance <= the width - the line up
        // to the end of the glyph's ink; kern + advance is added. (Read before 2026-11-07 as cell width and A, which wrapped titles retail
        // fits on one line: "Waypoint: Tampeii Settlement" in Euro_11_BS is 316 - 1 = 315 px wide, the map tooltip's title 315.)
        const int b = g.width - (range->shadow ? 1 : 0), cc = g.advance - g.originX - b;
        const int cell = g.advance;
        const bool last = i + 1 >= n;
        const int kern = last ? 0 : rf->kern(em, c, text[size_t(i + 1)]);
        // OPENRASA_WRAPLOG=1: each character's metrics as the wrap counts them (comparison with GDI's GetCharABCWidths).
        static const bool wrapLog = std::getenv("OPENRASA_WRAPLOG") != nullptr;
        if (wrapLog)
            std::printf("[wrap] %s em %d U+%04X A %d B %d C %d adv %d kern %d line %d / %d\n", fontId.c_str(), em, unsigned(c), g.originX, b, cc,
                        cell, kern, lineWidth, widthPixels);
        if (kern - cc + lineWidth + cell <= widthPixels) {
            if (i > 0 && !last && engine::ui::lineBreakBetween(c, text[size_t(i + 1)])) {
                sinceBreak = 0;
                brk = i;
            }
            lineWidth += kern + cell;
            sinceBreak += kern + cell;
            ++i;
            continue;
        }
        if (lineWidth == 0) return lines;
        if (brk == start) {
            lines.push_back({start, i - start});
            lineWidth = sinceBreak = 0;
            start = brk = i;
        } else {
            lines.push_back({start, brk - start + 1});
            lineWidth = sinceBreak;
            start = brk = brk + 1;
        }
    }
    if (i != start) lines.push_back({start, i - start});
    return lines;
}

engine::ui::RichMeasure UiCanvas::richMeasure(float sy) {
    engine::ui::RichMeasure m;
    auto fontOf = [this](const std::string& id) -> const tr::UiFont* {
        const tr::UiFont* f = ui_ ? ui_->font(id) : nullptr;
        if (!f && ui_) f = ui_->font("Default");
        if (!f && ui_ && !ui_->fonts.empty()) f = &ui_->fonts.begin()->second;
        return f && !f->ranges.empty() ? f : nullptr;
    };
    auto emOf = [sy](const tr::UiFontRange& r) { return UiCanvas::fontEm(r.pointSize, sy); };
    m.advance = [this, fontOf, emOf](const std::string& id, char16_t c) -> int {
        const tr::UiFont* f = fontOf(id);
        if (!f) return 0;
        const tr::UiFontRange* range = f->rangeFor(c);
        Face* rf = range ? face(range->face) : nullptr;
        if (!rf) {
            range = &f->ranges.front();
            rf = face(range->face);
        }
        if (!rf) return 0;
        // The layout (FUN_008fc410) reads the glyph data's + 0x18 = the advance A + B + C (gfxFontImpl::AddGlyph's record: + 0x14 is the
        // cell width). OpenRasa 7ddead3 had read it as the cell width. Glyph metrics from FreeType (APPROXIMATE against GDI's).
        return rf->glyph(emOf(*range), c, range->bold, range->shadow).advance;
    };
    // MultiFont + 0x10 (FUN_006b3f90: the largest tmHeight of its ranges' fonts) x UIManager + 0x90 (logical / screen), truncated.
    const float logicalPerPixel = 1.0f / sy;
    m.lineHeight = [this, fontOf, emOf, logicalPerPixel](const std::string& id) -> int {
        const tr::UiFont* f = fontOf(id);
        if (!f) return 0;
        int32_t tallest = 0;
        for (const tr::UiFontRange& r : f->ranges)
            if (Face* rf = face(r.face)) tallest = std::max(tallest, rf->cellHeight(emOf(r)));
        return int(float(tallest) * logicalPerPixel);
    };
    m.namedColour = [this](const std::string& name, uint32_t& argb) -> bool {
        if (!ui_) return false;
        const auto found = ui_->colours.find(name);
        if (found == ui_->colours.end()) return false;
        const tr::UiColour* c = &found->second;
        argb = uint32_t(c->a) << 24 | uint32_t(c->r) << 16 | uint32_t(c->g) << 8 | uint32_t(c->b);
        return true;
    };
    return m;
}

// RichText (RichText.h): the markup laid out as the game does, each run drawn as a Text at its place. A run's colour: its style's
// alpha 0 -> the render context colour (the widget's colour x its parent's), else the style's rgb with the context's alpha
// (FUN_008e5c10). UNVERIFIED: a run's vertical placement follows the Text rule (the em box at the run's y); kerning as drawText's.
void UiCanvas::drawRichText(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target) {
    const std::string s = text(widget);
    if (s.empty()) return;
    std::u16string markup;
    for (uint32_t cp : codepoints(s)) markup += char16_t(cp < 0x10000 ? cp : 0xFFFD);
    uint8_t own[4];
    ownColour(widget, own);
    float context[4];
    colourOf(widget, context);
    const uint32_t ownArgb = uint32_t(own[3]) << 24 | uint32_t(own[0]) << 16 | uint32_t(own[1]) << 8 | uint32_t(own[2]);
    const engine::ui::RichMeasure measure = richMeasure(target.sy);
    const engine::ui::RichLayout layout =
        engine::ui::layoutRichText(markup, widget.attribute("fontId"), ownArgb, rect.width(), target.sx, measure);
    const Rect px = target.pixels(rect);
    uint8_t savedContext[4];
    std::copy(context_, context_ + 4, savedContext);
    std::fill(context_, context_ + 4, uint8_t(255));
    auto byteOf = [](float f) { return std::to_string(int(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f))); };
    for (const engine::ui::RichRunPlace& place : engine::ui::placeRichRuns(layout, measure)) {
        const engine::ui::RichLayout::Run& run = layout.runs[size_t(place.run)];
        if (run.length <= 0) continue;
        const engine::ui::RichLayout::Style& style = layout.styleAt(run.start);
        std::string runText;
        for (int k = 0; k < run.length; ++k) {
            const uint32_t c = layout.text[size_t(run.start + k)];
            if (c < 0x80) runText += char(c);
            else if (c < 0x800) { runText += char(0xC0 | (c >> 6)); runText += char(0x80 | (c & 0x3F)); }
            else { runText += char(0xE0 | (c >> 12)); runText += char(0x80 | ((c >> 6) & 0x3F)); runText += char(0x80 | (c & 0x3F)); }
        }
        tr::XmlNode line;
        line.tag = "Text";
        line.attributes = {{"fontId", style.font}};
        float rgba[4] = {context[0], context[1], context[2], context[3]};
        if ((style.colour >> 24) != 0) {
            rgba[0] = float((style.colour >> 16) & 0xff) / 255.0f;
            rgba[1] = float((style.colour >> 8) & 0xff) / 255.0f;
            rgba[2] = float(style.colour & 0xff) / 255.0f;
        }
        line.attributes.emplace_back("colorR", byteOf(rgba[0]));
        line.attributes.emplace_back("colorG", byteOf(rgba[1]));
        line.attributes.emplace_back("colorB", byteOf(rgba[2]));
        line.attributes.emplace_back("colorA", byteOf(rgba[3]));
        if (widget.hasAttribute("effect")) line.attributes.emplace_back("effect", widget.attribute("effect"));
        textOverrides_[&line] = runText;
        // The run's pixels: the widget's left + the run's x (pixels), its top + its y (logical, scaled).
        const Rect at{rect.x0, rect.y0 + place.yLogical, rect.x0 + 100000, rect.y0 + place.yLogical + 100000};
        textShiftX_ = px.x0 + place.xPixels - target.pixels(at).x0;
        ++noCacheDepth_;   // a temporary node
        drawText(line, at, intersect(clip, rect), target);
        --noCacheDepth_;
        textShiftX_ = 0;
        textOverrides_.erase(&line);
    }
    std::copy(savedContext, savedContext + 4, context_);
}

// Widget slot 73 (FUN_0089d3c0): the colour property, x the widget animation's colour when it has one (FUN_008b7280).
void UiCanvas::ownColour(const tr::XmlNode& widget, uint8_t rgba[4]) const {
    float f[4];
    const auto o = buttonOverrides_.find(&widget);
    const NodeInfo& in = info(widget);
    if (o != buttonOverrides_.end() && o->second.colour) std::copy(o->second.rgba, o->second.rgba + 4, f);
    else std::copy(in.colour, in.colour + 4, f);
    for (int c = 0; c < 4; ++c) rgba[c] = uint8_t(std::lround(std::clamp(f[c], 0.0f, 1.0f) * 255.0f));
    AnimFrame frame;
    if (sampleAnimation(in.widgetAnimName, false, frame, &widget) && frame.colour)
        for (int c = 0; c < 4; ++c) rgba[c] = multiplyColour(frame.rgba[c], rgba[c]);
}

// What a widget draws with (Bitmap render FUN_008f5300, the composite's FUN_008aee70): its colour x the render context colour, which
// every composite multiplies its own colour into for its children - so a container's colour (alpha 0 on some HUD groups) reaches them.
void UiCanvas::colourOf(const tr::XmlNode& widget, float rgba[4]) const {
    uint8_t own[4];
    ownColour(widget, own);
    for (int c = 0; c < 4; ++c) rgba[c] = float(multiplyColour(own[c], context_[c])) / 255.0f;
}

// Button states (tabula_rasa.exe, decompiled 2026-10-15). A push button (PushButton; ScrollUpBtn / ScrollDownBtn, Thumb, Track and Entry
// load the same attributes; loader FUN_008e2fc0) owns a Bitmap and a Text child (the export's children are those, dumped in the
// state they had: Up for all but 27 Disabled ones) and applies its state to them (FUN_008e42f0): texture (FUN_008e4460), text
// colour (FUN_008e4360, SetColor) and text anchor offset (FUN_008e43e0, SetAnchorOffset), each chosen as: not enabled -> Disabled;
// pushed (+0x139) -> Down; highlighted (+0x138) -> Highlighted; else Up (texture attributes Up / Highlighted / Down /
// DisabledTextureDef; colour and offset Up / Highlight / Down / DisabledTextColor / TextAnchorOffset). Defaults (constructor
// FUN_008e2b20): textures "", colours 0xFFFFFFFF, offsets (0, 0) - so a state without an attribute still sets white / (0, 0).
// A toggle button (Button, ToggleButton; FUN_008e7c30) owns a Bitmap and chooses its texture (FUN_008e8be0): not enabled ->
// DownDisabled if on, else UpDisabled; pressed (+0x18b) -> Pressed; highlighted (+0x18a) -> DownHighlighted / UpHighlighted; else
// Down / Up.
void UiCanvas::applyButtonState(const tr::XmlNode& button) {
    if ((options_.buttonState == 0 && !options_.interactiveButtons) || !isButton(button)) return;
    const tr::XmlNode* bitmap = nullptr;
    const tr::XmlNode* textNode = nullptr;
    for (const tr::XmlNode& c : button.children) {
        if (!bitmap && c.tag == "Bitmap") bitmap = &c;
        if (!textNode && c.tag == "Text") textNode = &c;
    }
    int state = options_.buttonState;                       // 1 up, 2 highlighted, 3 down, 4 disabled, 5 pressed
    const bool toggle = button.tag == "Button" || button.tag == "ToggleButton";
    bool on = options_.toggledOn;
    if (options_.interactiveButtons) {
        // enabled defaults to false: the Widget constructor (FUN_0089c280) zeroes the flag (+ 0x56, SetEnabled / IsEnabled = slots 114 / 115)
        // and the loader sets it only when the attribute is present (the export's 22 push buttons without it are exactly its Disabled ones).
        const bool hovered = options_.hoveredButton == &button;
        state = button.attribute("enabled") != "true" ? 4 : hovered && options_.pushedButton == &button ? (toggle ? 5 : 3) : hovered ? 2 : 1;
        on = options_.toggledButtons && options_.toggledButtons->count(&button) > 0;
    }
    if (toggle) {
        std::string def;
        if (state == 4) def = button.attribute(on ? "DownDisabledTextureDef" : "UpDisabledTextureDef");
        else if (state == 5) def = button.attribute("PressedTextureDef");
        else if (state == 2) def = button.attribute(on ? "DownHighlightedTextureDef" : "UpHighlightedTextureDef");
        else def = button.attribute(on ? "DownTextureDef" : "UpTextureDef");
        if (bitmap && !def.empty()) {
            ButtonOverride& o = buttonOverrides_[bitmap];
            o.texture = true;
            o.textureDef = def;
        }
        return;
    }
    const char* texture = state == 4 ? "DisabledTextureDef" : state == 3 || state == 5 ? "DownTextureDef" : state == 2 ? "HighlightedTextureDef" : "UpTextureDef";
    const char* prefix = state == 4 ? "Disabled" : state == 3 || state == 5 ? "Down" : state == 2 ? "Highlight" : "Up";
    // An empty state texture (the constructor's "") leaves the bitmap's own texture: the game draws the DiscordBtn's logo (its Bitmap's
    // textureDef) although the button names no state textures. (UIBitmap::SetTextureByDef, FUN_008f4840, asserts on a name the
    // definitions lack; how "" gets past it is not traced - this follows the game's visible result.)
    if (bitmap && !button.attribute(texture).empty()) {
        ButtonOverride& o = buttonOverrides_[bitmap];
        o.texture = true;
        o.textureDef = button.attribute(texture);
    }
    if (textNode) {
        ButtonOverride& o = buttonOverrides_[textNode];
        o.colour = true;
        const std::string p = prefix;
        // Missing channels keep the constructor's 0xFFFFFFFF (opaque white), so a state without the attribute still sets white.
        for (int c = 0; c < 4; ++c) o.rgba[c] = float(intAttribute(button, (p + "TextColor" + "RGBA"[c]).c_str(), 255)) / 255.0f;
        o.offset = true;
        o.offsetX = intAttribute(button, (p + "TextAnchorOffsetX").c_str());
        o.offsetY = intAttribute(button, (p + "TextAnchorOffsetY").c_str());
    }
}

// Progress bars (tabula_rasa.exe uidrawables, decompiled 2026-10-15): the loader FUN_008b3ee0 reads orientation (leftToRight 1, rightToLeft
// 2, bottomToTop 4, topToBottom 8; default 1), style (reveal 0, stretch 1, anything else - "slide" - 2; default 1) and percentComplete
// (float, default 0), owns a Bitmap (the export's Filler child) and lays it out at load and on every change (FUN_008b4440), f = trunc(w x p)
// or trunc(h x p):
// - reveal (FUN_008b44b0): the filler at the bar's size and (0, 0); the bar clips its children to the filled part (right = left + f,
//   left = right - f, top = bottom - f, bottom = top + f).
// - stretch (FUN_008b45b0): the filler sized (f, h) / (w, f), at (0, 0), or (w - f, 0) / (0, h - f) for rightToLeft / bottomToTop.
// - slide (FUN_008b4710): the filler at the bar's size, offset (f - w, 0), (w - f, 0), (0, h - f), (0, f - h).
// The other two clip to the whole bar. The export's Filler children are this layout at the exported percentComplete.
void UiCanvas::applyProgress(const tr::XmlNode& bar) {
    if (bar.tag != "ProgressBar") return;
    const tr::XmlNode* filler = nullptr;
    for (const tr::XmlNode& c : bar.children)
        if (c.tag == "Filler" || c.tag == "Bitmap") { filler = &c; break; }
    const std::string o = bar.attribute("orientation"), st = bar.attribute("style");
    const int orientation = o.empty() || o == "leftToRight" ? 1 : o == "rightToLeft" ? 2 : o == "bottomToTop" ? 4 : 8;
    const int style = st.empty() || st == "stretch" ? 1 : st == "reveal" ? 0 : 2;
    float p = options_.progress >= 0 ? options_.progress : 0.0f;
    if (options_.progress < 0 && bar.hasAttribute("percentComplete")) p = std::strtof(bar.attribute("percentComplete").c_str(), nullptr);
    const int32_t w = intAttribute(bar, "sizeW"), h = intAttribute(bar, "sizeH");
    const int32_t fw = int32_t(float(w) * p), fh = int32_t(float(h) * p);     // truncated, as _ftol
    Rect clipRect{0, 0, w, h};
    if (!filler) {
        childClips_[&bar] = clipRect;
        return;
    }
    ButtonOverride& f = buttonOverrides_[filler];
    f.size = true;
    f.offset = true;
    f.width = w;
    f.height = h;
    f.offsetX = 0;
    f.offsetY = 0;
    if (style == 0) {
        if (orientation == 1) clipRect.x1 = fw;
        else if (orientation == 2) clipRect.x0 = w - fw;
        else if (orientation == 4) clipRect.y0 = h - fh;
        else clipRect.y1 = fh;
    } else if (style == 1) {
        if (orientation == 1 || orientation == 2) f.width = fw;
        else f.height = fh;
        if (orientation == 2) f.offsetX = w - fw;
        if (orientation == 4) f.offsetY = h - fh;
    } else {
        if (orientation == 1) f.offsetX = fw - w;
        else if (orientation == 2) f.offsetX = w - fw;
        else if (orientation == 4) f.offsetY = h - fh;
        else f.offsetY = fh - h;
    }
    childClips_[&bar] = clipRect;
}

// Scroll bars (scrollbar.cpp; loader FUN_008d6ce0, decompiled 2026-10-15): own two arrow PushButtons, a Track and a Thumb (the export's
// children). Range min..max (FUN_008d7950), value clamped to min..max - pageSize + 1 (FUN_008d79e0); the thumb (FUN_008d84f0, anchors
// top / left, FUN_008d8410): ratio = (value - min) / ((max - min + 1) - pageSize) (0 when the range is smaller than the page; 0 / 0 when
// equal - NaN, _ftol 0x80000000, as one exported scroll bar shows), vertical (the default) offset (0, top button height +
// trunc((track height - thumb height) x ratio)), horizontal (left button width + trunc((track width - thumb width) x ratio), 0). All 63
// exported thumbs match. ("slide" stores the thumb's centre in a Track field, +0xb4 - its use is not traced.)
// Sliders (Slider, Selector, RangeTarget; loader FUN_008da640 not traced in detail): offset (trunc((track width - thumb width) x
// (value - min) / (max - min)), (track height - thumb height) / 2) fits all 11 exported thumbs exactly.
void UiCanvas::applyScroll(const tr::XmlNode& bar) {
    float ratio = options_.scroll;
    if (options_.scrollValues) {
        const auto own = options_.scrollValues->find(&bar);
        if (own != options_.scrollValues->end()) ratio = own->second;
    }
    if (ratio < 0 || !bar.hasAttribute("maxRange")) return;
    const tr::XmlNode* thumb = nullptr;
    const tr::XmlNode* track = nullptr;
    const tr::XmlNode* up = nullptr;
    const tr::XmlNode* down = nullptr;
    for (const tr::XmlNode& c : bar.children) {
        if (c.tag == "Thumb") thumb = &c;
        else if (c.tag == "Track") track = &c;
        else if (c.tag == "ScrollUpBtn") up = &c;
        else if (c.tag == "ScrollDownBtn") down = &c;
    }
    if (!thumb) return;
    const int32_t trackW = track ? intAttribute(*track, "sizeW") : intAttribute(bar, "sizeW");
    const int32_t trackH = track ? intAttribute(*track, "sizeH") : intAttribute(bar, "sizeH");
    const int32_t thumbW = intAttribute(*thumb, "sizeW"), thumbH = intAttribute(*thumb, "sizeH");
    const float r = std::clamp(ratio, 0.0f, 1.0f);
    ButtonOverride& o = buttonOverrides_[thumb];
    o.offset = true;
    if (bar.tag == "ScrollBar") {
        // The chosen position stands for value = min + r x (range - page), i.e. the same ratio.
        if (bar.attribute("style") == "horizontal") {
            o.offsetX = (down ? intAttribute(*down, "sizeW") : 0) + int32_t(float(trackW - thumbW) * r);
            o.offsetY = 0;
        } else {
            o.offsetX = 0;
            o.offsetY = (up ? intAttribute(*up, "sizeH") : 0) + int32_t(float(trackH - thumbH) * r);
        }
    } else {
        o.offsetX = int32_t(float(trackW - thumbW) * r);
        o.offsetY = (trackH - thumbH) / 2;
    }
}

void UiCanvas::drawWidget(const tr::XmlNode& widget, std::vector<int32_t>& path, const Placed& parent, const Rect& clip, bool branchRoot,
                          Target& target) {
    const NodeInfo& in = info(widget);
    if (!branchRoot && !options_.showHidden && in.hidden) {
        const auto shown = buttonOverrides_.find(&widget);
        if (shown == buttonOverrides_.end() || !shown->second.show) return;
    }
    applyButtonState(widget);
    applyProgress(widget);
    applyScroll(widget);
    const Placed placed = place(widget, parent);
    const Rect rect = placed.rect;
    std::copy(parent.colour, parent.colour + 4, context_);
    parentRect_ = parent.rect;
    bool visual = false;
    if (in.hasTextureDef || in.hasTextureFile) {
        drawTexture(widget, rect, clip, target);
        visual = true;
    } else if (widget.tag == "ColoredRect") {
        drawColour(widget, rect, clip, target);
        visual = true;
    } else if (widget.tag == "SceneView" && options_.quads) {
        // OpenRasa: a SceneView's 3D view, in its place among the widgets (the engine renders it into the quad's texture).
        const Rect area = intersect(target.pixels(rect), target.pixels(clip));
        if (area.width() > 0 && area.height() > 0) {
            Quad q;
            q.scene = &widget;
            q.rect = target.pixels(rect);
            q.clip = area;
            options_.quads->push_back(q);
            visual = true;
        }
    } else if (widget.tag == "Movie" && options_.quads) {
        // OpenRasa: a Movie's picture (the engine fits it into the widget and draws the decoded frame).
        const Rect area = intersect(target.pixels(rect), target.pixels(clip));
        if (area.width() > 0 && area.height() > 0) {
            Quad q;
            q.movie = &widget;
            q.rect = q.area = target.pixels(rect);
            q.clip = area;
            options_.quads->push_back(q);
            visual = true;
        }
    } else if (widget.tag == "RichText") {
        drawRichText(widget, rect, clip, target);
        visual = !text(widget).empty();
    } else if (widget.tag == "Text") {
        drawText(widget, rect, clip, target);
        visual = !text(widget).empty();
    }
    if (widget.tag == "EditBox" || widget.tag == "PasswordBox") layoutEdit(widget, rect, clip, target);
    drawn_.push_back(DrawnWidget{path, rect, visual});
    // clipChildren defaults to true: the WidgetComposite constructor (FUN_008ae750) sets + 0xB0 = 1 and its loader (FUN_008ae9c0) only
    // overwrites it when the attribute is present (the export writes only "false"); the composite render (FUN_008aee70) clips its
    // children to its own rectangle when it is set. Only composites have children.
    Rect childClip = in.clipChildren ? intersect(clip, rect) : clip;
    if (const auto cc = childClips_.find(&widget); cc != childClips_.end())
        childClip = intersect(childClip, Rect{rect.x0 + cc->second.x0, rect.y0 + cc->second.y0, rect.x0 + cc->second.x1, rect.y0 + cc->second.y1});
    Options::QuadCache* cache = options_.quads && options_.quadCache && path.size() + 1 >= options_.quadCache->minDepth && path.size() + 1 <= options_.quadCache->maxDepth ? options_.quadCache : nullptr;
    for (size_t k = 0; k < widget.children.size(); ++k) {
        path.push_back(int32_t(k));
        const tr::XmlNode& child = widget.children[k];
        if (cache) {
            Options::QuadCache::Entry& e = cache->entries[&child];
            if (e.cacheable < 0) e.cacheable = holdsEditBox(child) ? 0 : 1;
            if (e.valid && e.cacheable == 1) {
                options_.quads->insert(options_.quads->end(), e.quads.begin(), e.quads.end());
                // The replayed quads' images stay alive: a text image unused for 300 renders is dropped, and a cached quad still points
                // at it (a freed image read by UiGpu::prepare crashed the client).
                for (const Quad& q : e.quads)
                    for (const Image* image : {q.image, q.mask, q.mouseMask})
                        if (image) image->lastUsed = renderCount_;
                drawn_.insert(drawn_.end(), e.drawn.begin(), e.drawn.end());
                ++cache->hits;
                path.pop_back();
                continue;
            }
            const size_t q0 = options_.quads->size(), d0 = drawn_.size();
            drawWidget(child, path, placed, childClip, false, target);
            if (e.cacheable == 1) {
                e.quads.assign(options_.quads->begin() + std::ptrdiff_t(q0), options_.quads->end());
                e.drawn.assign(drawn_.begin() + std::ptrdiff_t(d0), drawn_.end());
                e.valid = true;
            }
            ++cache->misses;
        } else {
            drawWidget(child, path, placed, childClip, false, target);
        }
        path.pop_back();
    }
    if (widget.tag == "EditBox" || widget.tag == "PasswordBox") drawEditText(widget, rect, childClip, target);
}

bool UiCanvas::holdsEditBox(const tr::XmlNode& n) {
    if (n.tag == "EditBox" || n.tag == "PasswordBox" || n.tag == "MultiLineEditBox") return true;
    for (const tr::XmlNode& c : n.children)
        if (holdsEditBox(c)) return true;
    return false;
}

// Edit boxes (tabula_rasa.exe editbox.cpp, traced 2026-10-20): EditBox (constructor FUN_008ce7a0, loader FUN_008cebb0) owns a
// Caret (+ 0xf0) and a Highlight (+ 0xf4, selection) Bitmap; the loader sizes the caret 3 x the font's height and hides it; while the
// box has the keyboard (+ 0x18c) its update (FUN_008cffd0) toggles the caret every 500 ms (focus and typing show it, the timer at 0). Its render
// (vtable + 0x1b8, FUN_008cfb70; WidgetComposite's is FUN_008aee70) draws the composite (children) first, then the text: the prepared
// runs (FUN_008d2710) at x = the width of a prefix string (+ 0x11c, set by scripts - empty here) and y = (height - font height) / 2,
// in TextColor (+ 0xdc; default 0xFFFFFFFF, DAT_00d08b54 - no exported box sets it) and TextColorHighlighted for selected runs; the
// caret is placed (Widget slot + 0x148) at (width of the text before it + the prefix, (height - font height) / 2). A PasswordBox draws
// drawableChar for every character. UNVERIFIED: the font height is taken as the em (the game reads the font's + 0x10 x the UI scale),
// the text colour is multiplied by the render context like a Text's, the scrolling rule.
bool UiCanvas::editText(const tr::XmlNode& box, std::string& text) const {
    text.clear();
    if (options_.editTexts) {
        const auto it = options_.editTexts->find(&box);
        if (it != options_.editTexts->end()) text = it->second;
    }
    if (box.tag == "PasswordBox" && !text.empty()) {
        const uint32_t cp = uint32_t(std::strtoul(box.attribute("drawableChar").c_str(), nullptr, 10));
        std::string glyph;
        auto put = [&glyph](uint32_t c) {
            if (c < 0x80) glyph += char(c);
            else if (c < 0x800) { glyph += char(0xC0 | (c >> 6)); glyph += char(0x80 | (c & 0x3F)); }
            else { glyph += char(0xE0 | (c >> 12)); glyph += char(0x80 | ((c >> 6) & 0x3F)); glyph += char(0x80 | (c & 0x3F)); }
        };
        put(cp ? cp : '*');
        const size_t n = codepoints(text).size();
        text.clear();
        for (size_t k = 0; k < n; ++k) text += glyph;
    }
    return !text.empty() || options_.focusedEdit == &box;
}

// The width of a string in the box's font, pixels (the game measures each run apart, FUN_008f9d70).
int32_t UiCanvas::measureEdit(const tr::XmlNode& box, const Rect& rect, const Rect& clip, Target& target, const std::string& text) {
    if (text.empty()) return 0;
    tr::XmlNode line;
    line.tag = "Text";
    line.attributes = {{"fontId", box.attribute("fontId")}, {"vertAlign", "center"}};
    textOverrides_[&line] = text;
    measureOnly_ = scrollTail_ = true;
    ++noCacheDepth_;   // a temporary node
    drawText(line, Rect{rect.x0, rect.y0, rect.x0 + 100000, rect.y1}, clip, target);
    --noCacheDepth_;
    measureOnly_ = scrollTail_ = false;
    textOverrides_.erase(&line);
    return measuredWidth_;
}

namespace {
std::string utf8Of(const std::vector<uint32_t>& cps, size_t from, size_t to) {
    std::string s;
    for (size_t k = from; k < to && k < cps.size(); ++k) {
        const uint32_t c = cps[k];
        if (c < 0x80) s += char(c);
        else if (c < 0x800) { s += char(0xC0 | (c >> 6)); s += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { s += char(0xE0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F)); }
        else { s += char(0xF0 | (c >> 18)); s += char(0x80 | ((c >> 12) & 0x3F)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F)); }
    }
    return s;
}
}  // namespace

// EditBox layout (FUN_008d2710, traced 2026-10-20). The visible window [+ 0x184, + 0x188) follows the caret (+ 0x144): a caret before the
// window's start becomes the start and the end is worked out from it (vtable + 0x20c); one past the end (or an end past the text)
// becomes the end and the start is worked out back from it (+ 0x208) - both (FUN_008d2570 / FUN_008d23a0) take the longest run of whole
// characters from the fixed end that fits the box's width (+ 0x13c) less the prefix's. The window's text is laid out as runs (+ 0x134, 0x30 bytes: string, highlighted + 0x1c, x / y): without a
// selection (+ 0x13c == + 0x140) one run and the Highlight (+ 0xf4) hidden; with one, the text before it, the selection clipped to the
// window (highlighted), then the rest; the Highlight is shown, placed at the selection's run (Widget + 0x148) and sized to it (+ 0x128:
// the run's width x the font height, from FUN_008f9d70). The caret goes to (width of the window's text before it + the prefix,
// (height - font height) / 2).
void UiCanvas::layoutEdit(const tr::XmlNode& box, const Rect& rect, const Rect& clip, Target& target) {
    const tr::XmlNode* caret = nullptr;
    const tr::XmlNode* highlight = nullptr;
    for (const tr::XmlNode& c : box.children) {
        if (c.tag == "Caret") caret = &c;
        if (c.tag == "Highlight") highlight = &c;
    }
    std::string shown;
    editText(box, shown);
    const std::vector<uint32_t> cps = codepoints(shown);
    const size_t n = cps.size();
    const bool focused = options_.focusedEdit == &box;
    const size_t caretAt = focused ? std::min(options_.caretIndex, n) : n;
    size_t selLo = focused ? std::min(std::min(options_.selectionStart, options_.selectionEnd), n) : 0;
    size_t selHi = focused ? std::min(std::max(options_.selectionStart, options_.selectionEnd), n) : 0;
    const float sx = std::max(target.sx, 0.01f);
    const int32_t boxWidth = int32_t(float(rect.width()) * sx);
    // Prefix widths from a start character (pixels).
    auto widthBetween = [&](size_t from, size_t to) { return measureEdit(box, rect, clip, target, utf8Of(cps, from, to)); };
    auto endFrom = [&](size_t start) {
        size_t end = start;
        while (end < n && widthBetween(start, end + 1) <= boxWidth) ++end;
        return end;
    };
    auto startBack = [&](size_t end) {
        size_t start = end;
        while (start > 0 && widthBetween(start - 1, end) <= boxWidth) --start;
        return start;
    };
    size_t first = 0;
    if (options_.editWindows) {
        const auto it = options_.editWindows->find(&box);
        if (it != options_.editWindows->end()) first = std::min(it->second, n);
    }
    size_t last = endFrom(first);
    if (caretAt < first) {
        first = caretAt;
        last = endFrom(first);
    } else if (caretAt > last) {
        last = caretAt;
        first = startBack(last);
    } else if (last < n && first > 0 && widthBetween(first - 1, last) <= boxWidth) {
        first = startBack(last);               // room again (text removed): show more of what is before
    }
    if (options_.editWindows) (*options_.editWindows)[&box] = first;
    EditLayout& layout = editLayouts_[&box];
    layout = EditLayout{};
    layout.left = rect.x0;
    layout.first = first;
    for (size_t k = first; k <= last; ++k) layout.edges.push_back(rect.x0 + int32_t(float(widthBetween(first, k)) / sx));
    auto logicalX = [&](size_t k) { return layout.edges[std::clamp(k, first, last) - first] - rect.x0; };
    selLo = std::clamp(selLo, first, last);
    selHi = std::clamp(selHi, first, last);
    if (selLo >= selHi) {
        if (first < last) layout.runs.push_back(EditRun{utf8Of(cps, first, last), 0, false});
    } else {
        if (first < selLo) layout.runs.push_back(EditRun{utf8Of(cps, first, selLo), 0, false});
        layout.runs.push_back(EditRun{utf8Of(cps, selLo, selHi), logicalX(selLo), true});
        if (selHi < last) layout.runs.push_back(EditRun{utf8Of(cps, selHi, last), logicalX(selHi), false});
    }
    measureEdit(box, rect, clip, target, " ");
    const int32_t emLogical = std::max(1, int32_t(std::lround(float(measuredEm_) / std::max(target.sy, 0.01f))));
    if (highlight) {
        ButtonOverride& o = buttonOverrides_[highlight];
        o = ButtonOverride{};
        if (focused && selLo < selHi) {
            o.show = true;
            o.offset = true;
            o.offsetX = logicalX(selLo);
            o.offsetY = (rect.height() - emLogical) / 2;
            o.size = true;
            o.width = std::max(1, logicalX(selHi) - logicalX(selLo));
            o.height = emLogical;
            highlightPlaces_[&box] = HighlightPlace{o.offsetX, o.offsetY, o.width, o.height};
        }
    }
    if (!caret || !focused || !options_.caretVisible) return;
    ButtonOverride& o = buttonOverrides_[caret];
    o.show = true;
    o.offset = true;
    o.offsetX = logicalX(caretAt);
    o.offsetY = (rect.height() - emLogical) / 2;
    o.size = true;
    o.width = 3;
    o.height = emLogical;
}

// The runs, in TextColor (+ 0xdc) or, for the selection, TextColorHighlighted (+ 0xe0), each x the render context's colour
// (FUN_008cfb70 multiplies both by the context with FUN_008b7280 and passes the highlighted one for runs with + 0x1c set).
void UiCanvas::drawEditText(const tr::XmlNode& box, const Rect& rect, const Rect& clip, Target& target) {
    const auto it = editLayouts_.find(&box);
    if (it == editLayouts_.end()) return;
    for (const EditRun& run : it->second.runs) {
        tr::XmlNode line;
        line.tag = "Text";
        line.attributes = {{"fontId", box.attribute("fontId")}, {"vertAlign", "center"}};
        const std::string colour = run.highlighted ? "TextColorHighlighted" : "TextColor";
        for (const char* c : {"R", "G", "B", "A"})
            if (box.hasAttribute(colour + c)) line.attributes.emplace_back(std::string("color") + c, box.attribute(colour + c));
        textOverrides_[&line] = run.text;
        scrollTail_ = true;
        ++noCacheDepth_;   // a temporary node
        drawText(line, Rect{rect.x0 + run.x, rect.y0, rect.x1 + 100000, rect.y1}, intersect(clip, rect), target);
        --noCacheDepth_;
        scrollTail_ = false;
        textOverrides_.erase(&line);
    }
}

size_t UiCanvas::editIndexAt(const tr::XmlNode* box, int32_t x) const {
    const auto it = editLayouts_.find(box);
    if (it == editLayouts_.end() || it->second.edges.empty()) return 0;
    const std::vector<int32_t>& edges = it->second.edges;
    size_t best = 0;
    for (size_t k = 1; k < edges.size(); ++k)
        if (std::abs(edges[k] - x) < std::abs(edges[best] - x)) best = k;
    return it->second.first + best;
}

void UiCanvas::fillBackground(Target& target) const {
    const int32_t screenW = target.width, screenH = target.height;
    const Options& options = options_;
    target.rgb.assign(size_t(screenW) * size_t(screenH) * 3, 0.0f);
    const bool imageBackground = options.backgroundRgba && options.backgroundRgba->size() == size_t(screenW) * size_t(screenH) * 4;
    for (int32_t y = 0; y < screenH; ++y)
        for (int32_t x = 0; x < screenW; ++x) {
            float* p = &target.rgb[(size_t(y) * size_t(screenW) + size_t(x)) * 3];
            if (imageBackground) {
                const uint8_t* b = &(*options.backgroundRgba)[(size_t(y) * size_t(screenW) + size_t(x)) * 4];
                p[0] = float(b[0]) / 255.0f; p[1] = float(b[1]) / 255.0f; p[2] = float(b[2]) / 255.0f;
                continue;
            }
            float g = 0;
            if (options.background == 1) g = 0.25f;
            else if (options.background == 2) g = ((x / 16 + y / 16) & 1) ? 0.30f : 0.20f;
            p[0] = p[1] = p[2] = g;
        }
}

void UiCanvas::finish(const Target& target, std::vector<uint8_t>& rgba) const {
    const int32_t screenW = target.width, screenH = target.height;
    const size_t count = size_t(screenW) * size_t(screenH);
    rgba.resize(count * 4);
    for (size_t k = 0; k < count; ++k) {
        for (int c = 0; c < 3; ++c) rgba[k * 4 + size_t(c)] = uint8_t(std::lround(std::clamp(target.rgb[k * 3 + size_t(c)], 0.0f, 1.0f) * 255.0f));
        rgba[k * 4 + 3] = 255;
    }
}

void UiCanvas::renderDetached(const tr::XmlNode& widget, int32_t pixelWidth, int32_t pixelHeight, float scale, const Options& options,
                              const std::string& sampleText, std::vector<uint8_t>& rgba) {
    detachedRoot_ = &widget;                // paths of this render (and the queries after it) start at the widget
    nodeCache_.clear();                     // the caller's nodes: not kept
    ++noCacheDepth_;
    struct Leave { int& d; ~Leave() { --d; } } leave{noCacheDepth_};
    drawn_.clear();
    editLayouts_.clear();
    options_ = options;
    options_.screenWidth = options_.screenHeight = 0;
    options_.backgroundRgba = nullptr;
    animating_ = false;
    buttonOverrides_.clear();
    childClips_.clear();
    if (scale <= 0) scale = 1;
    pixelWidth_ = std::max(pixelWidth, 1);
    pixelHeight_ = std::max(pixelHeight, 1);
    width_ = std::max(int32_t(std::lround(float(pixelWidth_) / scale)), 1);
    height_ = std::max(int32_t(std::lround(float(pixelHeight_) / scale)), 1);
    Target target;
    target.width = pixelWidth_;
    target.height = pixelHeight_;
    target.sx = target.sy = scale;
    fillBackground(target);
    Placed frame;
    frame.rect = Rect{0, 0, width_, height_};
    frame.w = width_;
    frame.h = height_;
    sampleNode_ = &widget;
    sampleText_ = sampleText;
    std::vector<int32_t> walk;
    drawWidget(widget, walk, frame, frame.rect, true, target);
    sampleNode_ = nullptr;
    finish(target, rgba);
}

uint32_t UiCanvas::animationLength(const std::string& name) const {
    const auto it = animations_.find(name);
    return it == animations_.end() ? 0 : it->second.length;
}

long UiCanvas::sampledAnimationTime(const std::string& name, bool bitmap, double time) const {
    if (time < 0 || name.empty()) return -1;
    const auto it = animations_.find(name);
    if (it == animations_.end() || it->second.bitmap != bitmap || it->second.length == 0) return -1;
    const Animation& a = it->second;
    uint32_t t = uint32_t(float(time));
    if (a.playback == 1) t %= a.length;
    else if (a.playback == 2) {
        t %= a.length * 2;
        if (t > a.length) t = a.length * 2 - t;
    } else if (t > a.length) t = a.length;   // every track holds its last key from here on
    return long(t);
}

std::vector<UiCanvas::DrawnWidget> UiCanvas::render(const std::vector<int32_t>& path, const Options& options, std::vector<uint8_t>& rgba) {
    detachedRoot_ = nullptr;
    drawn_.clear();
    editLayouts_.clear();
    options_ = options;
    animating_ = false;
    buttonOverrides_.clear();
    childClips_.clear();
    // The logical size is the exe's (FUN_004cd8c0: 1280 x 960); the export's Definitions/LogicalSize is not read by the game.
    const int32_t logicalW = 1280, logicalH = 960;
    // The root window's logical size (tabula_rasa.exe FUN_00896220, sizes from FUN_004cd8c0's call of the UI manager's init
    // FUN_00895430): 1706 x 960 when the screen's width / height exceeds 1.5 (double at 0x00c2b570), else 1280 x 960; the screen
    // scale is screen / logical per axis (FUN_00897190).
    const int32_t screenW = options.screenWidth > 0 ? options.screenWidth : logicalW;
    const int32_t screenH = options.screenHeight > 0 ? options.screenHeight : logicalH;
    width_ = options.screenWidth > 0 && double(screenW) / double(screenH) > 1.5 ? 1706 : logicalW;
    height_ = logicalH;
    pixelWidth_ = screenW;
    pixelHeight_ = screenH;
    Target target;
    target.width = screenW;
    target.height = screenH;
    target.sx = float(screenW) / float(width_);
    target.sy = float(screenH) / float(height_);
    ++renderCount_;
    if (options.quadCache) options.quadCache->hits = options.quadCache->misses = 0;
    if (options.quads) {
        options.quads->clear();
        for (auto it = textImages_.begin(); it != textImages_.end();)
            it = renderCount_ - std::max(it->second.lastUsed, it->second.image ? it->second.image->lastUsed : 0) > 300 ? textImages_.erase(it)
                                                                                                                    : std::next(it);
    } else {
        fillBackground(target);
    }
    const Rect screen{0, 0, width_, height_};
    const tr::XmlNode* n = node(path);
    if (n && !path.empty()) {
        std::vector<int32_t> parentPath(path.begin(), path.end() - 1);
        std::vector<int32_t> walk = path;
        drawWidget(*n, walk, placedAt(parentPath), screen, true, target);
    } else if (n) {                            // the <Widgets> element itself: every root widget
        const Placed root = placedAt({});
        for (size_t k = 0; k < n->children.size(); ++k) {
            std::vector<int32_t> walk{int32_t(k)};
            drawWidget(n->children[k], walk, root, screen, false, target);
        }
    }
    if (options.quads) {
        rgba.clear();
        return drawn_;
    }
    finish(target, rgba);
    return drawn_;
}

const tr::XmlNode* UiCanvas::thumbAt(int32_t x, int32_t y) const {
    for (auto it = drawn_.rbegin(); it != drawn_.rend(); ++it) {
        const tr::XmlNode* n = node(it->path);
        if (n && n->tag == "Thumb" && it->rect.contains(x, y)) return n;
    }
    return nullptr;
}

bool UiCanvas::drawnRect(const tr::XmlNode* widget, Rect& rect) const {
    for (const DrawnWidget& d : drawn_)
        if (node(d.path) == widget) {
            rect = d.rect;
            return true;
        }
    return false;
}

const tr::XmlNode* UiCanvas::parentOf(const tr::XmlNode* child) const {
    for (const DrawnWidget& d : drawn_)
        if (node(d.path) == child && !d.path.empty()) {
            std::vector<int32_t> up = d.path;
            up.pop_back();
            return up.empty() ? nullptr : node(up);
        }
    return nullptr;
}

float UiCanvas::thumbRatioAt(const tr::XmlNode& bar, int32_t x, int32_t y) const {
    // The bar's drawn rectangle and the same sizes applyScroll uses: thumb offset = lead (+ arrow button) + trunc(travel x r).
    Rect barRect{0, 0, 0, 0};
    bool found = false;
    for (const DrawnWidget& d : drawn_)
        if (node(d.path) == &bar) {
            barRect = d.rect;
            found = true;
            break;
        }
    if (!found) return 0.0f;
    const tr::XmlNode *thumb = nullptr, *track = nullptr, *up = nullptr, *down = nullptr;
    for (const tr::XmlNode& c : bar.children) {
        if (c.tag == "Thumb") thumb = &c;
        else if (c.tag == "Track") track = &c;
        else if (c.tag == "ScrollUpBtn") up = &c;
        else if (c.tag == "ScrollDownBtn") down = &c;
    }
    if (!thumb) return 0.0f;
    const int32_t trackW = track ? intAttribute(*track, "sizeW") : intAttribute(bar, "sizeW");
    const int32_t trackH = track ? intAttribute(*track, "sizeH") : intAttribute(bar, "sizeH");
    const int32_t thumbW = intAttribute(*thumb, "sizeW"), thumbH = intAttribute(*thumb, "sizeH");
    const bool vertical = bar.tag == "ScrollBar" && bar.attribute("style") != "horizontal";
    if (vertical) {
        const int32_t lead = barRect.y0 + (up ? intAttribute(*up, "sizeH") : 0);
        return trackH > thumbH ? std::clamp(float(y - lead) / float(trackH - thumbH), 0.0f, 1.0f) : 0.0f;
    }
    const int32_t lead = barRect.x0 + (bar.tag == "ScrollBar" && down ? intAttribute(*down, "sizeW") : 0);
    return trackW > thumbW ? std::clamp(float(x - lead) / float(trackW - thumbW), 0.0f, 1.0f) : 0.0f;
}

// Scroll bar presses (tabula_rasa.exe scrollbar.cpp, traced 2026-10-20). The loader (FUN_008d6ce0) keeps its parts at + 0xcc
// LesserButton / ScrollDownBtn, + 0xc8 GreaterButton / ScrollUpBtn, + 0xd0 Track, + 0xd4 Thumb; the press handler (0x008d7cd0)
// on ScrollDownBtn sets value + 1, on ScrollUpBtn max(min, value - 1), on the Track (FUN_008d8690) value - 1 when the mouse is
// before the thumb along the bar (x for horizontal, style 0; y for vertical) and value + 1 when after it, nothing on the thumb;
// setValue (FUN_008d79e0) clamps to min .. max - pageSize + 1 (min when that is not above min). The control held keeps firing
// (FUN_008d7c00): elapsed ms summed, past 599 it fires again and 100 is taken off - repeats at 600, 700, 800 ... ms, the track
// re-testing the mouse against the thumb each time. Sliders are not handled (their loader is not read).
bool UiCanvas::scrollStepAt(int32_t x, int32_t y, const tr::XmlNode*& bar, int& step) const {
    bar = nullptr;
    step = 0;
    for (auto it = drawn_.rbegin(); it != drawn_.rend(); ++it) {
        if (!it->rect.contains(x, y) || it->path.empty()) continue;
        const tr::XmlNode* n = node(it->path);
        if (!n || (n->tag != "ScrollUpBtn" && n->tag != "ScrollDownBtn" && n->tag != "Track" && n->tag != "Thumb")) continue;
        std::vector<int32_t> up = it->path;
        up.pop_back();
        const tr::XmlNode* parent = node(up);   // {} = the root (a detached render's widget)
        if (!parent || parent->tag != "ScrollBar") continue;
        bar = parent;
        if (n->tag == "ScrollDownBtn") step = 1;
        else if (n->tag == "ScrollUpBtn") step = -1;
        else if (n->tag == "Track") {
            const tr::XmlNode* thumb = nullptr;
            for (const tr::XmlNode& c : parent->children)
                if (c.tag == "Thumb") thumb = &c;
            Rect t{0, 0, 0, 0};
            if (!thumb || !drawnRect(thumb, t)) return true;
            const bool horizontal = parent->attribute("style") == "horizontal";
            const int32_t at = horizontal ? x : y, low = horizontal ? t.x0 : t.y0, high = horizontal ? t.x1 : t.y1;
            step = at < low ? -1 : at >= high ? 1 : 0;
        }
        return true;
    }
    return false;
}

float UiCanvas::scrollRatioStepped(const tr::XmlNode& bar, float ratio, int step) {
    const int32_t minimum = intAttribute(bar, "minRange"), maximum = intAttribute(bar, "maxRange"), page = intAttribute(bar, "pageSize");
    const int32_t top = maximum - page + 1;                        // setValue's upper limit
    if (top <= minimum) return 0.0f;
    const float span = float(top - minimum);                        // (max - min + 1) - pageSize, the thumb ratio's denominator
    if (ratio < 0) ratio = std::clamp(float(intAttribute(bar, "currentValue") - minimum) / span, 0.0f, 1.0f);   // as exported
    int32_t value = minimum + int32_t(std::lround(std::clamp(ratio, 0.0f, 1.0f) * span)) + step;
    value = std::clamp(value, minimum, top);
    return float(value - minimum) / span;
}

// Sliders (slider.cpp, traced 2026-10-20): the loader (FUN_008da640) keeps + 0xc4 Track and + 0xc8 Thumb (no arrow buttons); style
// "horizontal" = 0 (+ 0xc0), min + 0xe0, max + 0xe4, value + 0xec. A press on the Track (FUN_008db250) sets value = min + trunc((max - min)
// x f + 0.5) with f = (mouse - track start) / track length along the slider's axis, clamped to min .. max by setValue (FUN_008db030); the
// thumb ratio is (value - min) / (max - min) (FUN_008db630). The mouse wheel (both kinds: scroll bar FUN_008d7ca0, slider FUN_008db220)
// adds the event's delta to the value; the UI makes it -(WM_MOUSEWHEEL delta / 120) (FUN_00902870: one notch up = -1).
bool UiCanvas::sliderPressAt(int32_t x, int32_t y, const tr::XmlNode*& slider, float& ratio) const {
    slider = nullptr;
    for (auto it = drawn_.rbegin(); it != drawn_.rend(); ++it) {
        if (!it->rect.contains(x, y) || it->path.empty()) continue;
        const tr::XmlNode* n = node(it->path);
        if (!n || n->tag != "Track") continue;
        std::vector<int32_t> up = it->path;
        up.pop_back();
        const tr::XmlNode* parent = node(up);   // {} = the root (a detached render's widget)
        if (!parent || parent->tag == "ScrollBar" || !parent->hasAttribute("maxRange")) continue;
        const int32_t minimum = intAttribute(*parent, "minRange"), maximum = intAttribute(*parent, "maxRange");
        const bool vertical = parent->attribute("style") == "vertical";
        const int32_t start = vertical ? it->rect.y0 : it->rect.x0, length = vertical ? it->rect.height() : it->rect.width();
        if (length <= 0 || maximum <= minimum) return false;
        const float f = float((vertical ? y : x) - start) / float(length);
        const int32_t value = std::clamp(minimum + int32_t(float(maximum - minimum) * f + 0.5f), minimum, maximum);
        slider = parent;
        ratio = float(value - minimum) / float(maximum - minimum);
        return true;
    }
    return false;
}

const tr::XmlNode* UiCanvas::wheelTargetAt(int32_t x, int32_t y) const {
    for (auto it = drawn_.rbegin(); it != drawn_.rend(); ++it) {
        if (!it->rect.contains(x, y)) continue;
        for (std::vector<int32_t> p = it->path;; p.pop_back()) {
            const tr::XmlNode* n = node(p);
            if (n && n->hasAttribute("maxRange") && (n->tag == "ScrollBar" || n->tag == "Slider" || n->tag == "Selector" || n->tag == "RangeTarget"))
                return n;
            if (p.empty()) break;
        }
    }
    return nullptr;
}

float UiCanvas::sliderRatioStepped(const tr::XmlNode& slider, float ratio, int step) {
    const int32_t minimum = intAttribute(slider, "minRange"), maximum = intAttribute(slider, "maxRange");
    if (maximum <= minimum) return 0.0f;
    const float span = float(maximum - minimum);
    if (ratio < 0) ratio = std::clamp(float(intAttribute(slider, "currentValue") - minimum) / span, 0.0f, 1.0f);
    const int32_t value = std::clamp(minimum + int32_t(std::lround(std::clamp(ratio, 0.0f, 1.0f) * span)) + step, minimum, maximum);
    return float(value - minimum) / span;
}

const tr::XmlNode* UiCanvas::buttonAt(int32_t x, int32_t y) const {
    // The mouse reaches the last drawn pickable widget under it (pickable defaults to false: Widget constructor FUN_0089c280 + 0x55,
    // set by the loader only when present). Composites take the mouse themselves only with mouseCapture (default false, + 0xB1 in
    // FUN_008ae750) - UNVERIFIED: the engine's hit test is not traced; without this an empty button group drawn over another
    // (LogOutWindow's QuitButtons / LogoutButtons) would hide its buttons.
    std::vector<int32_t> path;
    for (auto it = drawn_.rbegin(); it != drawn_.rend() && path.empty(); ++it) {
        const tr::XmlNode* n = node(it->path);
        if (!n || !it->rect.contains(x, y) || n->attribute("pickable") != "true") continue;
        if (!n->children.empty() && !isButton(*n) && n->attribute("mouseCapture") != "true") continue;
        path = it->path;
    }
    while (!path.empty()) {
        const tr::XmlNode* n = node(path);
        if (n && isButton(*n)) return n;
        path.pop_back();
    }
    return nullptr;
}

std::vector<int32_t> UiCanvas::pick(int32_t x, int32_t y) const {
    for (auto it = drawn_.rbegin(); it != drawn_.rend(); ++it)
        if (it->visual && it->rect.contains(x, y)) return it->path;
    const DrawnWidget* best = nullptr;
    for (const DrawnWidget& d : drawn_)
        if (d.rect.contains(x, y) && (!best || d.path.size() >= best->path.size())) best = &d;
    return best ? best->path : std::vector<int32_t>{};
}
