// Strata - themed popup menus
//
// Strata's own menus (the ⋯ menu, command bar, tabs, sidebar, folder background …) are built with the
// normal AppendMenuW calls and shown with ThemedMenu() instead of TrackPopupMenuEx: same items, same
// return value, drawn in Strata's theme (rounded card, accent hover, check marks, icons, quiet shortcuts).
// The right-click menu on files stays the Windows one, because shell extensions (7-Zip, …) live there.
#pragma once
#include "common.h"

// Like TrackPopupMenuEx(menu, TPM_RETURNCMD | flags, x, y, owner): returns the chosen command id, or 0.
// Supports TPM_RIGHTALIGN / TPM_BOTTOMALIGN / TPM_CENTERALIGN. `avoid` (screen coords) is kept clear,
// e.g. the button the menu drops from.
UINT ThemedMenu(HMENU menu, UINT flags, int x, int y, HWND owner, const RECT* avoid = nullptr);

// Optional icon for an item (a Segoe Fluent/MDL2 glyph), shown in the menu's icon column.
void MenuIcon(HMENU menu, UINT id, wchar_t glyph);
