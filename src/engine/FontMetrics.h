// FontMetrics.h - text sizes for the UI's layout methods (GetTextWidth, GetTextHeight, ResizeToTextHeight ...), measured from the
// game's TrueType files (data\misc\f1.lff "Conduit ITC Light", f2.lff "EurostileTRegExt", named by the export's LoadedFontFile).
//
// The game rasterises with GDI at screen pixels (gfxFontImpl.cpp, FUN_006ae9e0; see RasaEdit's notes "Text rendering"): em =
// trunc(pointSize x 4 / 3) at screen scale 1, lines tmHeight = round(ascent) + round(descent) apart (OS/2 usWinAscent /
// usWinDescent), advances from the glyphs' metrics. Here: unhinted advances from hmtx scaled to the em and rounded per glyph - GDI's
// hinted widths can differ by a pixel per glyph (APPROXIMATE). Faces without a file here (the system's "MS UI Gothic") use the
// first file's metrics (APPROXIMATE).
#pragma once

#include <string>
#include <vector>

namespace engine::ui {

struct TextSize {
    int width = 0, height = 0;    // logical pixels
    int lineHeight = 0;
};

// Loads the font files (once; the game folder is the working directory).
void loadFontFiles(const std::vector<std::string>& files);
// The size of text in a font of the export ("A_Conduit_10" or "Fonts.A_Conduit_10"); wrapWidth > 0 wraps at spaces.
TextSize measureText(const std::string& fontId, const std::u16string& text, int wrapWidth);

}  // namespace engine::ui
