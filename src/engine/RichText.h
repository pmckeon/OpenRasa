// RichText.h - the game's RichText markup and layout (RichUIText, richtextlayout.cpp; traced 2026-10-26 in tabula_rasa.exe).
//
// A RichText widget's text is markup (chat lines: "<a action = 'PlayerNameLink:Doe'>Doe</a> says: ..."), parsed by FUN_008ffc30
// into a RichTextLayout (FUN_008fc120 .. FUN_008fc330) when the widget is drawn or measured after a change (FUN_008e52d0):
// - tags (FUN_009002f0, names case-insensitive, table 0x00bc4f28): a (link: attributes action / href), br, font (color / fg, face),
//   p (align: left / center / right / justify), var (src: a variable map, empty for widgets - nothing); body, h1 - h6, em, strong
//   and img are recognised and ignored; "</tag>" closes; any other "<" is text. Attributes (FUN_009003b0): name (alphanumeric,
//   table 0x00bc4fa0), spaces, "=", a value in ' or " or up to a space, / or >.
// - entities (FUN_009005a0, exact names): &nbsp; &quot; &amp; &lt; &gt;; an unknown one is dropped; "&" without a later ";" is text.
// - text runs up to the next & or <.
// The layout appends character by character (FUN_008fc410): CR / LF / TAB become spaces, spaces at a line start are dropped; widths
// are the glyphs' advances in screen pixels against the widget width x the screen scale; a word that no longer fits moves to the next
// line (a word longer than the line is split); words end after a space, "-" or CJK character (with the kinsoku tables FUN_008f95c0 /
// FUN_008f9610). Line heights are logical units (the font's tmHeight x logical / screen, truncated).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace engine::ui {

struct RichMeasure {
    // A glyph's width in screen pixels as the layout counts it - the glyph record's cell width (+ 0x18), in the font that holds the
    // code point (the font's range for it).
    std::function<int(const std::string& font, char16_t c)> advance;
    // The font's line height in logical units: the largest tmHeight of its ranges (pixels, MultiFont + 0x10, FUN_006b3f90) x logical /
    // screen (FUN_008fa790), truncated.
    std::function<int(const std::string& font)> lineHeight;
    // A colour definition by name (UIManager slot 6, FUN_008969c0), ARGB.
    std::function<bool(const std::string& name, uint32_t& argb)> namedColour;
};

struct RichLayout {
    std::u16string text;                  // the characters laid out (RichUIText + 0xac)
    struct Run {                          // RichUIText::TextData (+ 0xc8): a stretch of one style on one line
        int start = 0, length = 0;
    };
    struct Line {                         // + 0xd8, 0x14 bytes; the last is a sentinel (firstRun 0x7fffffff)
        int firstRun = 0;
        int x = 0;                        // screen pixels (alignment)
        int y = 0, height = 0;            // logical units
        float justify = 0;                // extra pixels per break (justify), UNVERIFIED how the render uses it
    };
    struct Style {                        // + 0xe8, 0x24 bytes: from character start on
        int start = 0;
        std::string font;
        uint32_t colour = 0x00FFFFFF;     // ARGB; alpha 0 = the widget's render colour
    };
    struct Link {                         // + 0xf8, 0x24 bytes
        std::u16string data;              // the action / href value: eMessageHTMLLink's linkData
        int start = 0, length = 0;
    };
    std::vector<Run> runs;
    std::vector<Line> lines;
    std::vector<Style> styles;
    std::vector<Link> links;
    int realLines() const { return lines.empty() ? 0 : int(lines.size()) - 1; }
    // GetTextHeight / ResizeToTextHeight (FUN_008e5410): the last line's y + height (0 for fewer than one line).
    int height() const;
    // The render draws lines from the first line's y (FUN_008e5020).
    int firstY() const { return realLines() >= 1 ? lines[0].y : 0; }
    const Style& styleAt(int position) const;    // FUN_008e5b20: the last style starting at or before the position
    const Link* linkAt(int position) const;      // FUN_008e5bb0
};

// The runs where the render puts them (FUN_008e5020 -> run slot 0 FUN_008e5c10): each real line from the first line's y, its runs
// (firstRun up to the next line's) one after another from the line's x; a run's text at the line's top + its height - the run's
// font's line height (the bottoms line up). The justify spacing is not applied (UNVERIFIED how the render uses it).
struct RichRunPlace {
    int run = 0;
    int xPixels = 0, widthPixels = 0;     // from the widget's left, screen pixels
    int yLogical = 0, heightLogical = 0;  // from the widget's top
};
std::vector<RichRunPlace> placeRichRuns(const RichLayout& layout, const RichMeasure& measure);

// widthLogical: the widget's width; widgetColour: its own colour (+ 0x4c, ARGB), which links take (FUN_008ffdf0).
RichLayout layoutRichText(const std::u16string& markup, const std::string& font, uint32_t widgetColour, int widthLogical, float screenScaleX,
                          const RichMeasure& measure);

// The word-break test the layouts share (RichText FUN_008fc410, Text.WrapText FUN_008f9860): a line may end after c, followed by next,
// when (next may start a line - CJK, not closing punctuation, FUN_008f97d0 - and c is not opening punctuation) or (a line may end after c
// - space, "-" or CJK, not opening punctuation, FUN_008f9810 - and next is not closing punctuation).
bool lineBreakBetween(char16_t c, char16_t next);

}  // namespace engine::ui
