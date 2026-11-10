// Strata - batch rename with live preview (F2 with several items selected)
#pragma once
#include "common.h"

// Shows the batch rename dialog. Returns the (old path, new path) pairs the user confirmed (may be empty).
std::vector<std::pair<std::wstring, std::wstring>> BatchRenameDialog(HWND owner, const std::vector<std::wstring>& paths);
// Renames in two passes (through temporary names) so swaps like a<->b work. Returns what was renamed.
std::vector<std::pair<std::wstring, std::wstring>> BatchRenameApply(const std::vector<std::pair<std::wstring, std::wstring>>& plan,
                                                                     std::wstring* error);
