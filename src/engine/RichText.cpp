// RichText.cpp - see RichText.h.
#include "RichText.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace engine::ui {

int RichLayout::height() const {
    // FUN_008e5410: more than one record (the sentinel counts) -> the last real line's y + height.
    if (lines.size() <= 1) return 0;
    const Line& last = lines[lines.size() - 2];
    return last.y + last.height;
}

const RichLayout::Style& RichLayout::styleAt(int position) const {
    static const Style none;
    if (styles.empty()) return none;
    size_t found = 0;
    for (size_t i = 0; i < styles.size() && styles[i].start <= position; ++i) found = i;
    return styles[found];
}

const RichLayout::Link* RichLayout::linkAt(int position) const {
    for (const Link& l : links)
        if (l.start <= position && position < l.start + l.length) return &l;
    return nullptr;
}

namespace {

// Kinsoku: no line break before these (FUN_008f9610) ...
bool closingPunctuation(char16_t c) {
    switch (c) {
        case 0x20: case 0x2d: case 0x2019: case 0x201d: case 0x2026: case 0x3001: case 0x3002: case 0x3005: case 0x3009: case 0x300b:
        case 0x300d: case 0x300f: case 0x3011: case 0x3015: case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049:
        case 0x3063: case 0x3083: case 0x3085: case 0x3087: case 0x308e: case 0x309d: case 0x309e: case 0x30a1: case 0x30a3:
        case 0x30a5: case 0x30a7: case 0x30a9: case 0x30c3: case 0x30e3: case 0x30e5: case 0x30e7: case 0x30ee: case 0x30f5:
        case 0x30f6: case 0x30fb: case 0x30fc: case 0x30fd: case 0x30fe: case 0xff01: case 0xff09: case 0xff0c: case 0xff0e:
        case 0xff1a: case 0xff1b: case 0xff1f: case 0xff3d: case 0xff5d:
            return true;
        default:
            return false;
    }
}
// ... and none after these (FUN_008f95c0).
bool openingPunctuation(char16_t c) {
    switch (c) {
        case 0x2018: case 0x201c: case 0x3008: case 0x300a: case 0x300c: case 0x300e: case 0x3010: case 0x3014: case 0xff08:
        case 0xff3b: case 0xff5b:
            return true;
        default:
            return false;
    }
}
bool cjk(char16_t c) { return (c >= 0x3000 && c < 0xa000) || (c >= 0xf900 && c < 0xfb00) || (c >= 0xff00 && c < 0xfff0); }
// FUN_008f97d0: a line may break before c.
bool breakBefore(char16_t c) { return !closingPunctuation(c) && cjk(c); }
// FUN_008f9810: a line may break after c.
bool breakAfter(char16_t c) { return !openingPunctuation(c) && (c == ' ' || c == '-' || cjk(c)); }

bool space(char16_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool nameCharacter(char16_t c) { return c != 0 && c < 0x80 && std::iswalnum(wint_t(c)); }

bool equalNoCase(const std::u16string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::towlower(wint_t(a[i])) != std::towlower(wint_t(uint8_t(b[i])))) return false;
    return i == a.size() && !b[i];
}

std::string narrow(const std::u16string& s) {
    // std::ctype<wchar_t>::narrow with '@' for what does not narrow (FUN_008fbdf0).
    std::string out;
    for (char16_t c : s) out += c < 0x80 ? char(c) : '@';
    return out;
}

class Layout {
public:
    Layout(const std::string& font, uint32_t widgetColour, int widthLogical, float scaleX, const RichMeasure& m)
        : measure_(m), widgetColour_(widgetColour) {
        // FUN_008fc120: width = the widget's width x the screen scale (UIManager + 0x84), colour 0x00FFFFFF, the widget's font, a
        // style record from character 0, the line height preset to the font's.
        width_ = int(float(widthLogical) * scaleX);
        style_.font = font;
        style_.colour = 0x00FFFFFF;
        out_.styles.push_back(RichLayout::Style{0, style_.font, style_.colour});
        lineHeight_ = m.lineHeight(style_.font);
    }

    // FUN_008ffc30.
    void parse(const std::u16string& markup) {
        s_.assign(markup.begin(), markup.end());
        s_.push_back(0);
        size_t p = 0;
        bool more = true;
        while (more) {
            const char16_t c = s_[p];
            if (c == '<') {
                ++p;
                bool open = true;
                const size_t afterBracket = p;
                const int tag = tagName(p, open);
                if (tag == 15) {
                    p = afterBracket;
                    append(std::u16string(1, u'<'));
                    continue;
                }
                switch (tag) {
                    case 0: anchor(p, open); break;
                    case 1: if (open) lineBreak(); break;
                    case 3: font(p, open); break;
                    case 13: paragraph(p, open); break;
                    case 14: break;              // var: its src names an entry of the widget's variable map, empty (FUN_008e52d0)
                    default: break;              // body, h1 - h6, em, strong, img
                }
                while (s_[p] != 0 && s_[p] != '>') ++p;
                if (s_[p] == '>') ++p;
            } else if (c == '&') {
                ++p;
                append(entity(p));
            } else {
                // FUN_00900660: up to the next & or <; none -> the rest, and the parse ends.
                size_t q = p;
                while (s_[q] != 0 && s_[q] != '&' && s_[q] != '<') ++q;
                append(std::u16string(s_.begin() + std::ptrdiff_t(p), s_.begin() + std::ptrdiff_t(q)));
                if (s_[q] == 0) more = false;
                p = q;
            }
        }
    }

    // FUN_008fc330: the last line ends, a sentinel line and style.
    RichLayout finish() {
        commitWord();
        endLine(true);
        RichLayout::Line sentinel;
        sentinel.firstRun = 0x7fffffff;
        out_.lines.push_back(sentinel);
        return std::move(out_);
    }

private:
    const RichMeasure& measure_;
    uint32_t widgetColour_;
    std::vector<char16_t> s_;
    RichLayout out_;
    struct StyleState {
        std::string font;
        uint32_t colour = 0x00FFFFFF;
    };
    // The layout's fields (FUN_008fc120): + 4 width, + 8 committed width, + 0xC line height, + 0x10 / + 0x14 the word not yet
    // committed (pixels / characters), + 0x18 characters given to runs, + 0x1C runs at the last commit, + 0x20 y, + 0x24 breaks,
    // + 0x28 at a line's start, + 0x29 style changed, + 0x2A a link to open, + 0x2C the current run, + 0x30 style, + 0x54 link,
    // + 0x78 alignment, + 0xA4 line started, + 0xA5 in a link.
    int width_ = 0, committed_ = 0, lineHeight_ = 0, wordWidth_ = 0, wordChars_ = 0, assigned_ = 0, runsAtCommit_ = 0, y_ = 0, breaks_ = 0;
    bool atLineStart_ = true, styleChanged_ = false, linkPending_ = false, lineStarted_ = false, inLink_ = false;
    int run_ = -1;
    StyleState style_;
    std::u16string linkData_;
    int align_ = 0;
    std::vector<StyleState> styleStack_;
    std::vector<int> alignStack_;

    // FUN_009002f0: "/"? spaces, the name (alphanumeric ASCII), looked up ignoring case; 15 = not a tag.
    int tagName(size_t& p, bool& open) {
        size_t q = p;
        open = true;
        if (s_[q] == '/') {
            ++q;
            open = false;
        }
        while (space(s_[q])) ++q;
        const size_t start = q;
        while (nameCharacter(s_[q])) ++q;
        const std::u16string name(s_.begin() + std::ptrdiff_t(start), s_.begin() + std::ptrdiff_t(q));
        static const std::pair<int, const char*> tags[] = {{0, "a"},  {1, "br"},  {2, "body"},  {12, "img"}, {3, "font"},
                                                           {4, "h1"}, {5, "h2"},  {6, "h3"},    {7, "h4"},   {8, "h5"},
                                                           {9, "h6"}, {10, "em"}, {11, "strong"}, {13, "p"}, {14, "var"}};
        for (const auto& [id, text] : tags)
            if (equalNoCase(name, text)) {
                p = q;
                return id;
            }
        return 15;
    }

    // FUN_009003b0: one attribute; false at ">" or without "=" (or an unclosed quote).
    bool attribute(size_t& p, int& id, std::u16string& value) {
        while (space(s_[p])) ++p;
        if (s_[p] == '>') return false;
        const size_t start = p;
        while (nameCharacter(s_[p])) ++p;
        const std::u16string name(s_.begin() + std::ptrdiff_t(start), s_.begin() + std::ptrdiff_t(p));
        static const char* attributes[] = {"action", "align", "color", "face", "fg", "href", "src"};
        id = 7;
        for (int i = 0; i < 7; ++i)
            if (equalNoCase(name, attributes[i])) {
                id = i;
                break;
            }
        while (space(s_[p])) ++p;
        if (s_[p] != '=') return false;
        ++p;
        while (space(s_[p])) ++p;
        size_t from = p, to = p, next = p;
        if (s_[p] == '\'' || s_[p] == '"') {
            const char16_t quote = s_[p];
            from = p + 1;
            to = from;
            while (s_[to] != 0 && s_[to] != quote) ++to;
            if (s_[to] == 0) return false;
            next = to + 1;
        } else {
            while (s_[to] != 0 && !std::iswspace(wint_t(s_[to])) && s_[to] != '/' && s_[to] != '>') ++to;
            next = to;
        }
        value.assign(s_.begin() + std::ptrdiff_t(from), s_.begin() + std::ptrdiff_t(to));
        p = next;
        return true;
    }

    // FUN_009005a0.
    std::u16string entity(size_t& p) {
        size_t semi = p;
        while (s_[semi] != 0 && s_[semi] != ';') ++semi;
        if (s_[semi] == 0) return u"&";
        const std::u16string name(s_.begin() + std::ptrdiff_t(p), s_.begin() + std::ptrdiff_t(semi));
        p = semi + 1;
        if (name == u"nbsp") return std::u16string(1, char16_t(0xa0));
        if (name == u"quot") return u"\"";
        if (name == u"amp") return u"&";
        if (name == u"lt") return u"<";
        if (name == u"gt") return u">";
        return u"";
    }

    // FUN_00900700: "#" + RGB / RGBA / RRGGBB / RRGGBB? / RRGGBBAA hex digits (without "#" when every character is one), else a
    // colour definition's name; other lengths white. ARGB.
    uint32_t colour(const std::u16string& v) {
        std::u16string digits;
        if (!v.empty() && v[0] == '#') {
            digits = v.substr(1);
        } else {
            for (char16_t c : v)
                if (!std::iswxdigit(wint_t(c))) {
                    uint32_t argb = 0xFFFFFFFF;
                    // An unknown name looks up the definitions' entry DAT_00d4c920 (not read): white here (UNVERIFIED).
                    if (measure_.namedColour && measure_.namedColour(narrow(v), argb)) return argb;
                    return 0xFFFFFFFF;
                }
            digits = v;
        }
        auto nibble = [](char16_t c) -> int { return c >= 0x61 ? c - 0x57 : c >= 0x41 ? c - 0x37 : c - 0x30; };
        auto twice = [&](char16_t c) -> uint32_t { const int n = nibble(c); return uint8_t(uint8_t(n << 4) | uint8_t(n)); };
        auto pair = [&](char16_t hi, char16_t lo) -> uint32_t { return uint8_t(nibble(hi) * 16 + nibble(lo)); };
        const size_t n = digits.size();
        if (n == 3 || n == 4) {
            const uint32_t a = n == 4 ? twice(digits[3]) : 0xff;
            return a << 24 | twice(digits[0]) << 16 | twice(digits[1]) << 8 | twice(digits[2]);
        }
        if (n >= 6 && n <= 8) {
            const uint32_t a = n == 8 ? pair(digits[6], digits[7]) : 0xff;
            return a << 24 | pair(digits[0], digits[1]) << 16 | pair(digits[2], digits[3]) << 8 | pair(digits[4], digits[5]);
        }
        return 0xFFFFFFFF;
    }

    void pushStyle() { styleStack_.push_back(style_); }   // FUN_008fd590
    void popStyle() {                                     // FUN_008fcc30
        if (styleStack_.empty()) return;
        style_ = styleStack_.back();
        styleStack_.pop_back();
        styleChanged_ = true;
    }

    // <a> (FUN_008ffdf0): opens a link with the widget's own colour; </a> ends it.
    void anchor(size_t& p, bool open) {
        if (!open) {
            if (inLink_ && !linkPending_) {
                out_.links.back().length = int(out_.text.size()) - out_.links.back().start;
                inLink_ = false;
            }
            popStyle();
            return;
        }
        pushStyle();
        if (!inLink_) {
            linkPending_ = true;
            inLink_ = true;
        }
        style_.colour = widgetColour_;
        styleChanged_ = true;
        int id;
        std::u16string value;
        while (attribute(p, id, value))
            if (id == 0 || id == 5) linkData_ = value;
    }

    // <font> (FUN_008fffb0): color / fg, face.
    void font(size_t& p, bool open) {
        if (!open) {
            popStyle();
            return;
        }
        pushStyle();
        int id;
        std::u16string value;
        while (attribute(p, id, value)) {
            if (id == 2 || id == 4) {
                style_.colour = colour(value);
                styleChanged_ = true;
            } else if (id == 3) {
                style_.font = narrow(value);   // FUN_008fc960
                styleChanged_ = true;
            }
        }
    }

    // <p> (FUN_00900110): a new paragraph a third of the last line's height down; align.
    void paragraph(size_t& p, bool open) {
        if (!open) {
            // FUN_008fc7b0 (EndPara).
            commitWord();
            endLine(true);
            if (realLinesSoFar() > 0) y_ += out_.lines.back().height / 3;
            if (!alignStack_.empty()) {
                align_ = alignStack_.back();
                alignStack_.pop_back();
            }
            return;
        }
        // FUN_008fc740.
        alignStack_.push_back(align_);
        if (wordWidth_ + committed_ != 0) {
            commitWord();
            endLine(true);
        }
        if (realLinesSoFar() > 0) y_ += out_.lines.back().height / 3;
        int id;
        std::u16string value;
        while (attribute(p, id, value))
            if (id == 1) {
                // FUN_008fbf90.
                align_ = equalNoCase(value, "center") ? 1 : equalNoCase(value, "right") ? 2 : equalNoCase(value, "justify") ? 3 : 0;
            }
    }

    int realLinesSoFar() const { return int(out_.lines.size()); }

    // <br> (FUN_008fc8a0): ends a started line; otherwise an empty line of the last line's height (else the font's).
    void lineBreak() {
        if (lineStarted_) {
            commitWord();
            endLine(true);
            return;
        }
        if (!out_.lines.empty()) y_ += out_.lines.back().height;
        else y_ += measure_.lineHeight(style_.font);
    }

    // FUN_008fccd0.
    void commitWord() {
        if (run_ >= 0) out_.runs[size_t(run_)].length += wordChars_;
        assigned_ += wordChars_;
        committed_ += wordWidth_;
        wordWidth_ = wordChars_ = 0;
        runsAtCommit_ = int(out_.runs.size());
    }

    // FUN_008fcdf0.
    void endLine(bool paragraphEnd) {
        if (!lineStarted_) return;
        RichLayout::Line& line = out_.lines.back();
        // Trailing spaces leave the line's last committed run.
        if (runsAtCommit_ - line.firstRun > 0) {
            RichLayout::Run& r = out_.runs[size_t(runsAtCommit_ - 1)];
            while (r.length > 0 && out_.text[size_t(r.start + r.length - 1)] == ' ') {
                --r.length;
                --breaks_;
            }
        }
        if (align_ == 1) line.x += int(uint32_t(width_ - committed_) >> 1);
        else if (align_ == 2) line.x += width_ - committed_;
        else if (align_ == 3 && !paragraphEnd && breaks_ != 0) line.justify = float(width_ - committed_) / float(breaks_);
        line.height = lineHeight_ != 0 ? lineHeight_ : measure_.lineHeight(style_.font);
        breaks_ = 0;
        y_ += line.height;
        committed_ = 0;
        lineHeight_ = 0;
        atLineStart_ = true;
        lineStarted_ = false;
    }

    // FUN_008fcd20.
    void newLine() {
        RichLayout::Line line;
        line.firstRun = runsAtCommit_;
        line.y = y_;
        out_.lines.push_back(line);
        out_.runs.push_back(RichLayout::Run{assigned_, 0});
        run_ = int(out_.runs.size()) - 1;
        lineHeight_ = std::max(lineHeight_, measure_.lineHeight(style_.font));
        lineStarted_ = true;
    }

    // FUN_008fc650: each character with the next one (the last with itself).
    void append(const std::u16string& piece) {
        for (size_t i = 0; i < piece.size(); ++i) {
            const bool last = i + 1 == piece.size();
            addCharacter(piece[i], last ? piece[i] : piece[i + 1], last);
        }
    }

    // FUN_008fc410.
    void addCharacter(char16_t c, char16_t next, bool last) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (c == ' ' && atLineStart_) return;
        const int advance = measure_.advance(style_.font, c);
        if (width_ < advance) width_ = advance;
        if (committed_ + wordWidth_ + advance > width_) {
            if (wordWidth_ + advance > width_) commitWord();
            endLine(false);
        }
        if (atLineStart_) {
            atLineStart_ = false;
            newLine();
        }
        if (styleChanged_) {
            styleChanged_ = false;
            RichLayout::Style st;
            st.start = int(out_.text.size());
            st.font = style_.font;
            st.colour = style_.colour;
            out_.styles.push_back(st);
            if (run_ >= 0) out_.runs[size_t(run_)].length += wordChars_;
            assigned_ += wordChars_;
            wordChars_ = 0;
            out_.runs.push_back(RichLayout::Run{assigned_, 0});
            run_ = int(out_.runs.size()) - 1;
            lineHeight_ = std::max(lineHeight_, measure_.lineHeight(style_.font));
        }
        if (linkPending_) {
            linkPending_ = false;
            out_.links.push_back(RichLayout::Link{linkData_, int(out_.text.size()), 0});
        }
        out_.text.push_back(c);
        wordWidth_ += advance;
        ++wordChars_;
        if ((!last && breakBefore(next) && !openingPunctuation(c)) || (breakAfter(c) && !closingPunctuation(next))) {
            ++breaks_;
            commitWord();
        }
    }
};

}  // namespace

std::vector<RichRunPlace> placeRichRuns(const RichLayout& layout, const RichMeasure& measure) {
    std::vector<RichRunPlace> out;
    const int lines = layout.realLines(), top = layout.firstY();
    for (int i = 0; i < lines; ++i) {
        const RichLayout::Line& line = layout.lines[size_t(i)];
        const int end = std::min(layout.lines[size_t(i) + 1].firstRun, int(layout.runs.size()));
        int x = line.x;
        for (int r = line.firstRun; r < end; ++r) {
            const RichLayout::Run& run = layout.runs[size_t(r)];
            const RichLayout::Style& style = layout.styleAt(run.start);
            int width = 0;
            for (int k = 0; k < run.length; ++k) width += measure.advance(style.font, layout.text[size_t(run.start + k)]);
            RichRunPlace place;
            place.run = r;
            place.xPixels = x;
            place.widthPixels = width;
            place.heightLogical = measure.lineHeight(style.font);
            place.yLogical = line.y - top + line.height - place.heightLogical;
            out.push_back(place);
            x += width;
        }
    }
    return out;
}

RichLayout layoutRichText(const std::u16string& markup, const std::string& font, uint32_t widgetColour, int widthLogical, float screenScaleX,
                          const RichMeasure& measure) {
    Layout layout(font, widgetColour, widthLogical, screenScaleX, measure);
    layout.parse(markup);
    return layout.finish();
}

bool lineBreakBetween(char16_t c, char16_t next) {
    return (breakBefore(next) && !openingPunctuation(c)) || (breakAfter(c) && !closingPunctuation(next));
}

}  // namespace engine::ui
