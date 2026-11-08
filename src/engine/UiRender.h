// UiRender.h - draws the live UI (the widget tree the scripts change) with UiCanvas, as GPU quads (RasaEdit's CPU renderer of
// the UI export, which reproduces the game's layout, texture regions, UI effects' blending and GDI-like text).
//
// Each render takes a snapshot of the live tree as export elements: a widget's own element (its attributes as loaded) with the state
// the scripts change written over it - visibility, anchors and offset, size, colour, texture definition, font, bar and range values,
// uiid - and the text each Text / RichText shows (set by a script, else its textId's string through CPPLookup). Hidden widgets'
// children are left out. Live state besides: button highlight / pushed / toggled, edit boxes' text, focus and caret, each widget's
// animation clocks.
#pragma once

#include "UiCanvas.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

namespace engine::ui {

struct Widget;

// The visible UI at pixelWidth x pixelHeight (the screen; the logical root is 1280 x 960, 1706 x 960 wider than 3:2) as GPU quads
// (render::UiGpu draws them). Returns false - quads left as they were - when nothing changed since the last call at this size; the
// quads' images stay valid until the next call. The game folder is the working directory.
bool renderUi(int pixelWidth, int pixelHeight, std::vector<UiCanvas::Quad>& quads);
// The 3D views of the SceneView widgets among the quads (scene::SceneWidget), rendered into their textures (quad.texture); call each
// frame before the UI pass, dt seconds after the last.
void renderScenes(SDL_GPUCommandBuffer* commands, SDL_GPUDevice* device, std::vector<UiCanvas::Quad>& quads, float dt);
// A RichText widget's layout as the game makes it (RichText.h), with the drawn glyphs' metrics at the current screen size; places
// receives the runs' positions when given. For GetTextHeight / ResizeToTextHeight and the link clicks.
RichLayout richTextLayout(Widget* w, RichMeasure* measureOut = nullptr);
float uiScaleX();   // screen pixels per logical unit across
// Text.WrapText (FUN_008f9860): text broken into the widget's width (GetSize x the screen scale, truncated) in its font.
std::vector<std::pair<int, int>> wrapTextLines(const Widget* w, const std::u16string& text);
// A character's advance in logical units in the range of the font that holds it (the canvas's faces, system fonts included).
// Measured at the screen's scale (as it is drawn) and given back in logical units.
float glyphAdvanceLogical(const std::string& fontId, char16_t c);
int textLayoutHeight(const Widget* w, const std::u16string& text);   // UIText::GetTextHeight (lines x the font's tallest line height)
// An edit box's hit test (vtable + 0x204, FUN_008d21b0): the character boundary nearest the logical x in the box's visible text as it
// was last drawn; false when the box was not drawn (then the caller keeps its own rule).
bool editIndexAt(const Widget* w, int logicalX, size_t& index);
// The edit box's Highlight as the last drawing placed it on the selection (screen rectangle, logical); false when it never was.
bool editHighlightRect(const Widget* w, int& left, int& top, int& right, int& bottom);
// The same UI state composited on the CPU (UiCanvas's own path): test comparisons with the GPU frame.
void renderUiCpu(int pixelWidth, int pixelHeight, std::vector<uint8_t>& rgba);

}  // namespace engine::ui
