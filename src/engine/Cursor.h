// Cursor.h - the mouse cursors (traced 2026-10-25 in tabula_rasa.exe).
//
// The game's cursors are resources of tabula_rasa.exe: the GameUIManager's set-up (FUN_0051f130 -> FUN_00520e90) loads six with
// LoadImageA(instance, id, IMAGE_CURSOR, 32, 32, LR_SHARED) and names them - "Pointer" 115 (the green arrow), "TextEntry" 116 (an
// I-beam), "Use" 120, "Buy" 122, "Sell" 123, "Trade" 124 (the arrow with an icon) - then pushes "Pointer". All six are 32 x 32,
// 24-bit colour with a 1-bit mask, no pixel that inverts the screen. OpenRasa reads them from the game folder's tabula_rasa.exe (its PE
// resources) and makes SDL cursors of them; a cursor it cannot give (no exe, or an unreadable resource) is one of the system's: the
// arrow for Pointer, the I-beam for TextEntry, the hand for Use / Buy / Sell / Trade.
//
// The cursor stack (the UI manager + 0x4C, a std::deque of names; FUN_008a3b60 / FUN_008a3cc0 / FUN_008a3c40): the shown cursor is the
// front. Push(name) ("" ignored): every occurrence of the name except the back element is removed (nothing is removed while fewer
// than two remain), then it is put at the front unless the back element is that name; the front is shown. Pop(name): the same removal,
// then the front is shown. A name with no cursor leaves the cursor as it was. "Pointer", pushed first, stays at the back.
// Who pushes: the scripts (GameUIManager.PushCursor / PopCursor 0x00520360 / 0x00520450: "Use", "Trade", "Sell", "Buy"), and edit
// boxes - "TextEntry" when the mouse enters an enabled one (FUN_008d12b0), removed when it leaves (FUN_008d1340), when the box is hidden
// (FUN_008cf750) or destroyed (FUN_008ce9e0).
#pragma once

#include <string>

namespace engine::cursor {

void load(const std::string& gameFolder);   // reads the cursors and pushes "Pointer" (FUN_00520e90); after SDL_Init
void push(const std::string& name);
void pop(const std::string& name);
void setShown(bool shown);                  // UIManager.ShowMousePointer
void shutdown();

}  // namespace engine::cursor
